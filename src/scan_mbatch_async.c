/* M-batch scan with ASYNC PoW: main thread drives submit/wait (NPU) and copies each transcript
 * out of the 2-slot pinned buffer into a ring; a persistent background PoW pool hashes whole
 * super-strips off a queue, OVERLAPPING the NPU pipeline (no per-super-strip broadcast sync).
 * hit_strip = superstrip*G + lane (global index).
 *
 * Lifecycle: workers live for the process. scan_full: reset state, feed jobs, drain (wait until
 * all ring slots returned, or a hit), return. Ring slot validity: the pinned buffer has only 2
 * slots, so each transcript is memcpy'd out (before submit(ss+2) overwrites) into ring[slot]. */
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "blake3.h"

#define R 128
#ifndef K
#define K 4096
#endif
#define WORDS 16
#define BN 64
#define MAXT 256
#define MAXSLOT 48

extern void pearl_strip_submit(const int8_t *a);
extern const uint32_t *pearl_strip_wait_ptr(void);
extern int pearl_mbatch(void);

static int le256(const uint8_t *h, const uint32_t *t) {
    const uint32_t *u = (const uint32_t *)h;
    for (int i = 7; i >= 0; i--) { if (u[i] > t[i]) return 0; if (u[i] < t[i]) return 1; }
    return 1;
}

static struct {
    pthread_mutex_t mu; pthread_cond_t cv_job, cv_free;
    uint32_t *ring[MAXSLOT]; int ring_ss[MAXSLOT];
    int nslot, nt, started;
    int q[MAXSLOT], qhead, qn;          /* FIFO of ready slots */
    int freelist[MAXSLOT], free_top;    /* stack of free slots */
    size_t superwords;
    const uint8_t *key; const uint32_t *target; int nbands, n_hi, G;
    volatile int hit_found; int hit_strip, hit_tile;
    pthread_t th[MAXT];
} A = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER };

static void *pow_async_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&A.mu);
        while (A.qn == 0) pthread_cond_wait(&A.cv_job, &A.mu);
        int slot = A.q[A.qhead]; A.qhead = (A.qhead + 1) % A.nslot; A.qn--;
        int ss = A.ring_ss[slot];
        pthread_mutex_unlock(&A.mu);

        if (!A.hit_found) {                          /* relaxed read: skip if a hit already exists */
            const size_t tstride = (size_t)A.nbands * WORDS * BN;
            const uint32_t *nat = A.ring[slot];
            int tiles = A.nbands * A.n_hi;
            uint32_t tr[WORDS]; uint8_t d[32];
            for (int g = 0; g < A.G && !A.hit_found; g++) {
                const uint32_t *base0 = nat + (size_t)g * tstride;
                for (int t = 0; t < tiles; t++) {
                    int hi = t / A.nbands, band = t % A.nbands;
                    const uint32_t *base = base0 + (size_t)band * WORDS * BN + hi;
                    for (int w = 0; w < WORDS; w++) tr[w] = base[w * BN];
                    blake3_hasher h;
                    blake3_hasher_init_keyed(&h, A.key);
                    blake3_hasher_update(&h, tr, sizeof tr);
                    blake3_hasher_finalize(&h, d, 32);
                    if (le256(d, A.target)) {
                        pthread_mutex_lock(&A.mu);
                        if (!A.hit_found) { A.hit_found = 1; A.hit_strip = ss * A.G + g; A.hit_tile = t; }
                        pthread_mutex_unlock(&A.mu);
                        break;
                    }
                }
            }
        }
        pthread_mutex_lock(&A.mu);
        A.freelist[A.free_top++] = slot;             /* return slot */
        pthread_cond_signal(&A.cv_free);
        pthread_mutex_unlock(&A.mu);
    }
}

int scan_full(const int8_t *a_noised, int num_strips,
              const uint8_t *key, const uint32_t *target,
              int nbands, int n_hi, int pow_threads,
              uint32_t *scratch, int *hit_strip, int *hit_tile) {
    (void)scratch;
    int G = pearl_mbatch(); if (G < 1) G = 1;
    int n_super = num_strips / G;
    size_t superwords = (size_t)G * nbands * WORDS * BN;
    int nt = pow_threads < 1 ? 1 : (pow_threads > MAXT ? MAXT : pow_threads);
    int nslot = nt + 8; if (nslot > MAXSLOT) nslot = MAXSLOT;

    pthread_mutex_lock(&A.mu);
    if (!A.started) {
        A.started = 1; A.nt = nt; A.nslot = nslot; A.superwords = superwords;
        for (int i = 0; i < nslot; i++) A.ring[i] = malloc(superwords * sizeof(uint32_t));
        for (int i = 0; i < nt; i++) pthread_create(&A.th[i], 0, pow_async_worker, 0);
    }
    A.key = key; A.target = target; A.nbands = nbands; A.n_hi = n_hi; A.G = G;
    A.qhead = A.qn = 0;
    A.free_top = A.nslot; for (int i = 0; i < A.nslot; i++) A.freelist[i] = i;
    A.hit_found = 0; A.hit_strip = A.hit_tile = -1;
    pthread_mutex_unlock(&A.mu);

    pearl_strip_submit(a_noised);
    for (int ss = 0; ss < n_super; ss++) {
        if (ss + 1 < n_super)
            pearl_strip_submit(a_noised + (size_t)(ss + 1) * G * R * K);
        const uint32_t *cur = pearl_strip_wait_ptr();      /* pinned slot ss%2 */
        pthread_mutex_lock(&A.mu);
        if (A.hit_found) { pthread_mutex_unlock(&A.mu);
            if (ss + 1 < n_super) pearl_strip_wait_ptr();  /* drain the 1 in-flight submit: keep g_sub==g_fin */
            break; }
        while (A.free_top == 0) pthread_cond_wait(&A.cv_free, &A.mu);   /* backpressure */
        int slot = A.freelist[--A.free_top];
        pthread_mutex_unlock(&A.mu);

        memcpy(A.ring[slot], cur, superwords * sizeof(uint32_t));   /* before submit(ss+2) overwrites */
        A.ring_ss[slot] = ss;
        pthread_mutex_lock(&A.mu);
        A.q[(A.qhead + A.qn) % A.nslot] = slot; A.qn++;
        pthread_cond_signal(&A.cv_job);
        pthread_mutex_unlock(&A.mu);
    }
    /* drain: wait until every slot is back (all queued + in-flight PoW done) */
    pthread_mutex_lock(&A.mu);
    while (A.free_top < A.nslot) pthread_cond_wait(&A.cv_free, &A.mu);
    int found = A.hit_found, hs = A.hit_strip, ht = A.hit_tile;
    pthread_mutex_unlock(&A.mu);

    if (found) { *hit_strip = hs; *hit_tile = ht; }
    return found ? 1 : 0;
}
