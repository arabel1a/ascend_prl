/*
 * Pearl miner CPU prep in C.
 *
 * prep_from_ab : caller supplies A (m*k int8) and B (n*k int8); computes
 *   keyed-blake3 merkle roots (root == keyed b3 of full chunk-aligned buffer),
 *   commitment, canonical noise and noised matrices
 *   A_noised (m*k) + B_T_noised (k*n).
 * prep_random : fills A/B with xoshiro256** values in [-63,63] first.
 */
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include "blake3.h"

#define DIGEST 32
#define ZERO_POINT 32
#define RANGE_MASK 63

/* ------------------------------------------------------------------ utils */

typedef struct { int t, nt; void *ctx; } span_t;

static void run_threads(int nt, void *(*fn)(void *), void *ctx, span_t *spans) {
    pthread_t th[256];
    if (nt > 256) nt = 256;
    for (int i = 0; i < nt; i++) { spans[i].t = i; spans[i].nt = nt; spans[i].ctx = ctx; }
    for (int i = 0; i < nt; i++) pthread_create(&th[i], 0, fn, &spans[i]);
    for (int i = 0; i < nt; i++) pthread_join(th[i], 0);
}

/* xoshiro256** seeded via splitmix64 */
typedef struct { uint64_t s[4]; } rng_t;
static uint64_t splitmix(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static void rng_seed(rng_t *r, uint64_t seed) { for (int i = 0; i < 4; i++) r->s[i] = splitmix(&seed); }
static inline uint64_t rotl64(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
static inline uint64_t rng_next(rng_t *r) {
    uint64_t *s = r->s, res = rotl64(s[1] * 5, 7) * 9, t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t; s[3] = rotl64(s[3], 45);
    return res;
}

/* keyed blake3 of message = int32[8]{ [slot]=1+idx } || seed32  (noise draw) */
static void draw_hash(uint32_t idx, const uint8_t *seed, const uint8_t *key,
                      int slot, uint8_t out[DIGEST]) {
    uint8_t msg[64] = {0};
    uint32_t v = idx + 1;
    memcpy(msg + slot * 4, &v, 4);          /* LE */
    memcpy(msg + 32, seed, 32);
    blake3_hasher h;
    blake3_hasher_init_keyed(&h, key);
    blake3_hasher_update(&h, msg, 64);
    blake3_hasher_finalize(&h, out, DIGEST);
}

/* ----------------------------------------------------- random A/B (int8) */

typedef struct { int8_t *buf; int64_t total; uint64_t seed; } fill_ctx;
static void *fill_worker(void *a) {
    span_t *sp = a; fill_ctx *c = sp->ctx;
    int64_t lo = c->total * sp->t / sp->nt, hi = c->total * (sp->t + 1) / sp->nt;
    rng_t r; rng_seed(&r, c->seed + 0x9E37 * (uint64_t)(sp->t + 1));
    int64_t i = lo;
    while (i < hi) {
        uint64_t v = rng_next(&r);
        for (int b = 0; b < 8 && i < hi; b++, v >>= 8)
            c->buf[i++] = (int8_t)((uint8_t)v % 127) - 63;
    }
    return 0;
}

/* ------------------------------------------------- merkle root (keyed b3) */

typedef struct { const int8_t *buf; int64_t len; const uint8_t *key; uint8_t *root; } mk_ctx;
static void *mk_worker(void *a) {
    mk_ctx *c = a;
    blake3_hasher h;
    blake3_hasher_init_keyed(&h, c->key);
#if defined(BLAKE3_USE_TBB)
    /* parallel tree-hash (identical digest). Needs the
     * pthread blake3_compress_subtree_wide_join_tbb (blake3_join.c) linked in. */
    blake3_hasher_update_tbb(&h, c->buf, (size_t)c->len);
#else
    blake3_hasher_update(&h, c->buf, (size_t)c->len);
#endif
    blake3_hasher_finalize(&h, c->root, DIGEST);
    return 0;
}

/* ----------------------------------------------------------- noise draws */

typedef struct { uint8_t *out; int64_t nbytes; const uint8_t *seed, *key; int slot; } unif_ctx;
static void *unif_worker(void *a) {
    span_t *sp = a; unif_ctx *c = sp->ctx;
    int64_t draws = (c->nbytes + DIGEST - 1) / DIGEST;
    int64_t lo = draws * sp->t / sp->nt, hi = draws * (sp->t + 1) / sp->nt;
    for (int64_t i = lo; i < hi; i++) {
        uint8_t d[DIGEST];
        draw_hash((uint32_t)i, c->seed, c->key, c->slot, d);
        int64_t off = i * DIGEST, n = c->nbytes - off; if (n > DIGEST) n = DIGEST;
        memcpy(c->out + off, d, (size_t)n);
    }
    return 0;
}
/* raw draw bytes -> int8 in [-32,31] */
typedef struct { uint8_t *raw; int8_t *out; int64_t n; } u2i_ctx;
static void *u2i_worker(void *a) {
    span_t *sp = a; u2i_ctx *c = sp->ctx;
    int64_t lo = c->n * sp->t / sp->nt, hi = c->n * (sp->t + 1) / sp->nt;
    for (int64_t i = lo; i < hi; i++) c->out[i] = (int8_t)((c->raw[i] & RANGE_MASK) - ZERO_POINT);
    return 0;
}

/* permutation pairs: line -> (first,second) over rank slots */
static void perm_pairs(const uint8_t *seed, const uint8_t *key, int64_t lines, int rank,
                       uint16_t *first, uint16_t *second) {
    int64_t draws = (lines * 4 + DIGEST - 1) / DIGEST;
    for (int64_t i = 0; i < draws; i++) {
        uint8_t d[DIGEST];
        draw_hash((uint32_t)i, seed, key, 1, d);
        for (int j = 0; j < 8; j++) {
            int64_t line = i * 8 + j; if (line >= lines) break;
            uint32_t u; memcpy(&u, d + j * 4, 4);
            uint32_t f = u & (uint32_t)(rank - 1);
            uint32_t s = f ^ (1u + (uint32_t)(((uint64_t)(rank - 1) * u) >> 32));
            first[line] = (uint16_t)f; second[line] = (uint16_t)s;
        }
    }
}

/* ------------------------------------------------------- noised matrices */

/* A_noised[r,j] = A[r,j] + EAL[r,fA[j]] - EAL[r,sA[j]]   (rows split) */
typedef struct { const int8_t *A, *EAL; int8_t *out; const uint16_t *f, *s;
                 int64_t m, k; int R; } an_ctx;
static void *an_worker(void *a) {
    span_t *sp = a; an_ctx *c = sp->ctx;
    int64_t lo = c->m * sp->t / sp->nt, hi = c->m * (sp->t + 1) / sp->nt;
    for (int64_t r = lo; r < hi; r++) {
        const int8_t *ar = c->A + r * c->k, *el = c->EAL + r * c->R;
        int8_t *o = c->out + r * c->k;
        for (int64_t j = 0; j < c->k; j++)
            o[j] = (int8_t)(ar[j] + el[c->f[j]] - el[c->s[j]]);
    }
    return 0;
}

/* Bt_noised[i,j] = B[j,i] + EBR[j,fB[i]] - EBR[j,sB[i]] ; blocked transpose */
typedef struct { const int8_t *B, *EBR; int8_t *out; const uint16_t *f, *s;
                 int64_t k, n; int R; } bn_ctx;
#define TB 64
static void *bn_worker(void *a) {
    span_t *sp = a; bn_ctx *c = sp->ctx;
    int64_t kb = (c->k + TB - 1) / TB;
    int64_t lo = kb * sp->t / sp->nt, hi = kb * (sp->t + 1) / sp->nt;
    for (int64_t ib = lo; ib < hi; ib++) {
        int64_t i0 = ib * TB, i1 = i0 + TB > c->k ? c->k : i0 + TB;
        for (int64_t j0 = 0; j0 < c->n; j0 += TB) {
            int64_t j1 = j0 + TB > c->n ? c->n : j0 + TB;
            for (int64_t j = j0; j < j1; j++) {
                const int8_t *bj = c->B + j * c->k, *ej = c->EBR + j * c->R;
                for (int64_t i = i0; i < i1; i++)
                    c->out[i * c->n + j] = (int8_t)(bj[i] + ej[c->f[i]] - ej[c->s[i]]);
            }
        }
    }
    return 0;
}

/* FUSED noise+transpose+PACK — write the kernel's bt layout directly.
 *   bt[((jp*NFOLD+p)*BN + m)*MM_K + ki] = Bt_noised[row=p*MM_K+ki, col=jp*BN+m]
 * with MM_K = R(=rank), MM_M = BN = 64, NFOLD = k/R, nbands = n/64.
 * Btn[i*n+j] = B[j,i] + EBR[j,f[i]] - EBR[j,s[i]] from bn_worker. */
#define BN_PACK 64
typedef struct { const int8_t *B, *EBR; int8_t *bt; const uint16_t *f, *s;
                 int64_t k, n; int R; } bnp_ctx;
static void *bn_pack_worker(void *a) {
    span_t *sp = a; bnp_ctx *c = sp->ctx;
    int64_t nbands = c->n / BN_PACK, NFOLD = c->k / c->R;
    int64_t lo = nbands * sp->t / sp->nt, hi = nbands * (sp->t + 1) / sp->nt;
    for (int64_t jp = lo; jp < hi; jp++)
        for (int64_t p = 0; p < NFOLD; p++)
            for (int64_t m = 0; m < BN_PACK; m++) {
                int64_t col = jp * BN_PACK + m;
                const int8_t *bcol = c->B + col * c->k;     /* B[col, :]  (row-major [n,k]) */
                const int8_t *ecol = c->EBR + col * c->R;   /* EBR[col, :] */
                int8_t *dst = c->bt + ((jp * NFOLD + p) * BN_PACK + m) * c->R;
                for (int64_t ki = 0; ki < c->R; ki++) {
                    int64_t row = p * c->R + ki;
                    dst[ki] = (int8_t)(bcol[row] + ecol[c->f[row]] - ecol[c->s[row]]);
                }
            }
    return 0;
}

/* ---------------------------------------------------------------- driver */

static void unif_int8(const uint8_t *seed, const uint8_t *key, int64_t n, int8_t *out,
                      int nt, span_t *sp) {
    unif_ctx uc = { (uint8_t *)out, n, seed, key, 0 };
    /* draw in place then remap (draw bytes == out bytes count) */
    run_threads(nt, unif_worker, &uc, sp);
    u2i_ctx ic = { (uint8_t *)out, out, n };
    run_threads(nt, u2i_worker, &ic, sp);
}

/* pack=0: Bt_out receives Bt_noised[k,n] (repacked later by the kernel's set_b).
 * pack=1: Bt_out receives the kernel's PACKED bt layout directly. Identical otherwise. */
static int prep_from_ab_impl(const int8_t *A, const int8_t *B, const uint8_t *key,
                 int64_t m, int64_t n, int64_t k, int R,
                 int8_t *A_noised, int8_t *Bt_out,
                 int8_t *EAL, int8_t *EBR,
                 uint8_t *rootA, uint8_t *rootB,
                 uint8_t *commitA, uint8_t *commitB,
                 int nt, int pack) {
    static span_t sp[256];
    if ((m * k) % 1024 || (n * k) % 1024) return -1;
    mk_ctx ma = { A, m * k, key, rootA }, mb = { B, n * k, key, rootB };
    pthread_t t1, t2;
    pthread_create(&t1, 0, mk_worker, &ma);
    pthread_create(&t2, 0, mk_worker, &mb);
    pthread_join(t1, 0); pthread_join(t2, 0);

    blake3_hasher h;
    blake3_hasher_init(&h); blake3_hasher_update(&h, key, 32);
    blake3_hasher_update(&h, rootB, 32); blake3_hasher_finalize(&h, commitB, 32);
    blake3_hasher_init(&h); blake3_hasher_update(&h, commitB, 32);
    blake3_hasher_update(&h, rootA, 32); blake3_hasher_finalize(&h, commitA, 32);

    static const uint8_t seedA[32] = "A_tensor", seedB[32] = "B_tensor";
    unif_int8(seedA, commitA, m * R, EAL, nt, sp);
    unif_int8(seedB, commitB, n * R, EBR, nt, sp);

    static uint16_t fA[1 << 16], sA[1 << 16], fB[1 << 16], sB[1 << 16];
    if (k > (1 << 16)) return -2;
    perm_pairs(seedA, commitA, k, R, fA, sA);
    perm_pairs(seedB, commitB, k, R, fB, sB);

    an_ctx ac = { A, EAL, A_noised, fA, sA, m, k, R };
    run_threads(nt, an_worker, &ac, sp);
    if (pack) {
        bnp_ctx bp = { B, EBR, Bt_out, fB, sB, k, n, R };
        run_threads(nt, bn_pack_worker, &bp, sp);    /* fused transpose+pack -> kernel bt layout */
    } else {
        bn_ctx bc = { B, EBR, Bt_out, fB, sB, k, n, R };
        run_threads(nt, bn_worker, &bc, sp);
    }
    return 0;
}

int prep_from_ab(const int8_t *A, const int8_t *B, const uint8_t *key,
                 int64_t m, int64_t n, int64_t k, int R,
                 int8_t *A_noised, int8_t *Bt_noised, int8_t *EAL, int8_t *EBR,
                 uint8_t *rootA, uint8_t *rootB, uint8_t *commitA, uint8_t *commitB, int nt) {
    return prep_from_ab_impl(A, B, key, m, n, k, R, A_noised, Bt_noised,
                             EAL, EBR, rootA, rootB, commitA, commitB, nt, 0);
}

static int prep_random_impl(uint64_t seed, int8_t *A, int8_t *B, const uint8_t *key,
                int64_t m, int64_t n, int64_t k, int R,
                int8_t *A_noised, int8_t *Bt_out, int8_t *EAL, int8_t *EBR,
                uint8_t *rootA, uint8_t *rootB, uint8_t *commitA, uint8_t *commitB,
                int nt, int pack) {
    static span_t sp[256];
    fill_ctx fa = { A, m * k, seed }, fb = { B, n * k, seed ^ 0xB0B0B0B0ULL };
    run_threads(nt, fill_worker, &fa, sp);
    run_threads(nt, fill_worker, &fb, sp);
    return prep_from_ab_impl(A, B, key, m, n, k, R, A_noised, Bt_out,
                             EAL, EBR, rootA, rootB, commitA, commitB, nt, pack);
}

int prep_random(uint64_t seed, int8_t *A, int8_t *B, const uint8_t *key,
                int64_t m, int64_t n, int64_t k, int R,
                int8_t *A_noised, int8_t *Bt_noised, int8_t *EAL, int8_t *EBR,
                uint8_t *rootA, uint8_t *rootB, uint8_t *commitA, uint8_t *commitB, int nt) {
    return prep_random_impl(seed, A, B, key, m, n, k, R, A_noised, Bt_noised,
                            EAL, EBR, rootA, rootB, commitA, commitB, nt, 0);
}
/* same as prep_random but Bt_noised receives the kernel's PACKED bt layout (skip repackBT). */
int prep_random_bt(uint64_t seed, int8_t *A, int8_t *B, const uint8_t *key,
                int64_t m, int64_t n, int64_t k, int R,
                int8_t *A_noised, int8_t *bt_packed, int8_t *EAL, int8_t *EBR,
                uint8_t *rootA, uint8_t *rootB, uint8_t *commitA, uint8_t *commitB, int nt) {
    return prep_random_impl(seed, A, B, key, m, n, k, R, A_noised, bt_packed,
                            EAL, EBR, rootA, rootB, commitA, commitB, nt, 1);
}

/* REUSE-B split. B-side (commitB, EBR, packed bt) depends only on (B,key) — fixed within
 * a job, so it is cached and the per-iter prep drops to the A-side.
 * prep_b_side: fill random B, rootB, commitB=blake3(key||rootB), EBR, perm fB/sB, pack -> bt.
 * prep_a_side: fill random A, rootA, commitA=blake3(commitB||rootA) [= PoW key], EAL, perm fA/sA,
 *              noised_A -> An. Different A -> different transcript -> a valid DISTINCT share. */
int prep_b_side(uint64_t seed, int8_t *B, const uint8_t *key,
                int64_t n, int64_t k, int R,
                int8_t *bt_packed, int8_t *EBR, uint8_t *rootB, uint8_t *commitB, int nt) {
    static span_t sp[256];
    if ((n * k) % 1024) return -1;
    if (k > (1 << 16)) return -2;
    fill_ctx fb = { B, n * k, seed };
    run_threads(nt, fill_worker, &fb, sp);
    mk_ctx mb = { B, n * k, key, rootB };
    pthread_t t; pthread_create(&t, 0, mk_worker, &mb); pthread_join(t, 0);
    blake3_hasher h;
    blake3_hasher_init(&h); blake3_hasher_update(&h, key, 32);
    blake3_hasher_update(&h, rootB, 32); blake3_hasher_finalize(&h, commitB, 32);
    static const uint8_t seedB[32] = "B_tensor";
    unif_int8(seedB, commitB, n * R, EBR, nt, sp);
    static uint16_t fB[1 << 16], sB[1 << 16];
    perm_pairs(seedB, commitB, k, R, fB, sB);
    bnp_ctx bp = { B, EBR, bt_packed, fB, sB, k, n, R };
    run_threads(nt, bn_pack_worker, &bp, sp);
    return 0;
}

int prep_a_side(uint64_t seed, int8_t *A, const uint8_t *key, const uint8_t *commitB,
                int64_t m, int64_t k, int R,
                int8_t *A_noised, int8_t *EAL, uint8_t *rootA, uint8_t *commitA, int nt) {
    static span_t sp[256];
    if ((m * k) % 1024) return -1;
    if (k > (1 << 16)) return -2;
    fill_ctx fa = { A, m * k, seed };
    run_threads(nt, fill_worker, &fa, sp);
    mk_ctx ma = { A, m * k, key, rootA };
    pthread_t t; pthread_create(&t, 0, mk_worker, &ma); pthread_join(t, 0);
    blake3_hasher h;
    blake3_hasher_init(&h); blake3_hasher_update(&h, commitB, 32);
    blake3_hasher_update(&h, rootA, 32); blake3_hasher_finalize(&h, commitA, 32);
    static const uint8_t seedA[32] = "A_tensor";
    unif_int8(seedA, commitA, m * R, EAL, nt, sp);
    static uint16_t fA[1 << 16], sA[1 << 16];
    perm_pairs(seedA, commitA, k, R, fA, sA);
    an_ctx ac = { A, EAL, A_noised, fA, sA, m, k, R };
    run_threads(nt, an_worker, &ac, sp);
    return 0;
}
