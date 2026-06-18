/*
 * Pearl C miner — engine: prep/scan/proof main loop + the dev-fee time-slice + reuse-B.
 * Pool-AGNOSTIC: every wire-protocol detail lives behind the `POOL` frontend (src/pools/).
 * Each binary links exactly one frontend (kryptex.c -> ascend_prl_kryptex, k1pool.c ->
 * ascend_prl_k1pool, ...).
 *
 * usage: ascend_prl_<pool> <devid> <worker> <host> <port> <address> <password>
 */
#include "pools/pool.h"
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define R 128
/* ---- dev fee (disclosed; open-source) ----------------------------------------------------
 * DEV_FEE_PERMILLE/1000 of submitted shares are sent to DEV_FEE_ADDR (the rest go to the
 * miner's own wallet). It is announced at startup. The RATIO is a COMPILE-TIME flag, so the
 * shipped binary/Docker image carries it baked in; rebuild from source with
 * `make DEV_FEE_PERMILLE=0` to disable. Shares are bearer (wallet-independent proofs), so a fee
 * share is just a normally-mined share submitted under DEV_FEE_ADDR. */
#ifndef DEV_FEE_PERMILLE
#define DEV_FEE_PERMILLE 10              /* 10 per-mille = 1.0%. make DEV_FEE_PERMILLE=0 disables. */
#endif
#ifndef DEV_FEE_ADDR
#define DEV_FEE_ADDR "prl1p2skcz8kxn03p3j2hzaz4j687ewan8deju7lgvpswux9hkgavcz5s6v5p83"
#endif

/* ---- external module ABIs ---- */
extern int pearl_init(int dev, int nbands);
extern void pearl_set_b(const int8_t *b, int n);
/* pipelined B. set_b_slot (repack+async H2D into back buffer `slot`) runs in the prep
 * thread to OVERLAP the live scan; use_b flips the live buffer + resets flags on the main thread
 * between scans. Weak: fall back to the synchronous single-buffer set_b if the .so lacks them. */
extern void pearl_set_b_slot(const int8_t *b, int n, int slot) __attribute__((weak));
extern void pearl_use_b(int slot) __attribute__((weak));
/* best path — prep emits the packed bt layout (prep_random_bt) and set_bt_slot just H2Ds
 * it (no host repackBT). Gated together on pearl_set_bt_slot being present in the linked .so. */
extern void pearl_set_bt_slot(const int8_t *bt, int slot) __attribute__((weak));
extern int prep_random(uint64_t seed, int8_t *A, int8_t *B, const uint8_t *key,
                       int64_t m, int64_t n, int64_t k, int rank,
                       int8_t *An, int8_t *Btn, int8_t *EAL, int8_t *EBR,
                       uint8_t *rA, uint8_t *rB, uint8_t *cA, uint8_t *cB, int nt);
extern int prep_random_bt(uint64_t seed, int8_t *A, int8_t *B, const uint8_t *key,
                       int64_t m, int64_t n, int64_t k, int rank,
                       int8_t *An, int8_t *bt_packed, int8_t *EAL, int8_t *EBR,
                       uint8_t *rA, uint8_t *rB, uint8_t *cA, uint8_t *cB, int nt);
/* reuse-B split — B-side cached per job, A-side regenerated per iter. */
extern int prep_b_side(uint64_t seed, int8_t *B, const uint8_t *key,
                       int64_t n, int64_t k, int rank,
                       int8_t *bt_packed, int8_t *EBR, uint8_t *rootB, uint8_t *commitB, int nt);
extern int prep_a_side(uint64_t seed, int8_t *A, const uint8_t *key, const uint8_t *commitB,
                       int64_t m, int64_t k, int rank,
                       int8_t *An, int8_t *EAL, uint8_t *rootA, uint8_t *commitA, int nt);
extern int scan_full(const int8_t *an, int nstrips, const uint8_t *key, const uint32_t *tgt,
                     int nbands, int n_hi, int pow_threads, uint32_t *scratch,
                     int *hs, int *ht);
extern ssize_t build_proof_b64(const uint8_t *a, const uint8_t *b, size_t m, size_t n,
                               size_t k, size_t rank, const uint8_t *key,
                               const size_t *ar, size_t na, const size_t *bc, size_t nb,
                               uint8_t *out, size_t cap);
extern int hash_key(const uint8_t *hdr, size_t hl, size_t k, size_t rank,
                    const size_t *rp, size_t nr, const size_t *cp, size_t nc, uint8_t *out);

/* ---- connections + shared mining state ---- */
/* cont48h: dev-fee runs a SECOND connection authorized as DEV_FEE_ADDR, pre-warmed behind the
 * user mining. g_job points at the job the hot path mines (user_conn or dev_conn); during a dev
 * window the miner mines dev_conn's pre-warmed job and submits on the dev socket -> credited to
 * the dev. Both connections stay open; no reconnect, no "Job not found". */
static pool_conn_t user_conn;   /* the user wallet's connection */
static pool_conn_t dev_conn;    /* the dev-fee wallet's connection */
static job_t *g_job = &user_conn.job;   /* ACTIVE job for prep/scan (set by the scheduler) */
static mining_params_t mp;      /* miner-chosen (kryptex) or pool-dictated (k1pool) */
static long g_fee_sub = 0;      /* count of shares submitted during dev-fee windows */

/* connect + handshake the user connection, then wait (bounded) for the first job (+ params). */
static int do_handshake(const char *host, int port, const char *addr,
                        const char *worker, const char *pass) {
    user_conn.msg_id = 0; user_conn.job.have = 0;
    if (!POOL.miner_chosen_params) mp.have = 0;   /* pool re-pushes params on each connect */
    if (POOL.open(&user_conn, host, port, addr, worker, pass, &mp)) return -1;
    for (int i = 0; i < 300; i++) {               /* up to ~30s for the feed to start */
        int ok;
        pthread_mutex_lock(&job_mu); ok = user_conn.job.have; pthread_mutex_unlock(&job_mu);
        if (ok && mp.have) break;
        usleep(100000);
    }
    if (!(user_conn.job.have && mp.have)) { user_conn.dead = 1; if (user_conn.fd >= 0) close(user_conn.fd); return -1; }
    printf("[stratum] ready (pool=%s rank=%ld k=%ld %ldx%ld)\n",
           POOL.name, mp.rank, mp.k, mp.m, mp.n);
    return 0;
}

/* ---- dev fee: time-slice (cont48g) -----------------------------------------------------
 * The pool credits the AUTHORIZED connection wallet AND scopes job validity per-session (a
 * second connection gets "Job not found" for the main session's jobs — verified live). So the
 * only robust way to credit DEV_FEE_ADDR is to mine ITS OWN jobs: for DEV_FEE_PERMILLE/1000 of
 * the time the miner switches to the dev connection and mines its jobs, credited to the dev.
 * dev_secs_to_window() drives a CYCLE-second schedule. */
#if DEV_FEE_PERMILLE > 0
#ifndef DEV_FEE_CYCLE_S
#define DEV_FEE_CYCLE_S 5400        /* schedule period (s); dev window = CYCLE*PERMILLE/1000 (=54s @1%).
                                     * Larger cycle => fewer dev-conn open/close cycles and a window much
                                     * longer than a scan, so long scans still land dev shares cleanly.
                                     * Ratio unchanged. */
#endif
#ifndef DEV_FEE_PREOPEN_S
#define DEV_FEE_PREOPEN_S 8         /* warm the dev connection this many seconds before the window */
#endif
static int g_dev_now = 0;           /* NPU currently mining for the dev wallet? */
static time_t g_fee_anchor = 0;
static long dev_secs_to_window(void) {   /* 0 = inside window; else seconds until it opens */
    if (!g_fee_anchor) g_fee_anchor = time(0);
    long cyc = DEV_FEE_CYCLE_S, win = cyc * DEV_FEE_PERMILLE / 1000; if (win < 1) win = 1;
    long t = (long)((time(0) - g_fee_anchor) % cyc);
    return t < win ? 0 : cyc - t;
}
static int dev_open(const char *host, int port, const char *worker, const char *pass) {
    dev_conn.msg_id = 0; dev_conn.job.have = 0;
    if (POOL.open(&dev_conn, host, port, DEV_FEE_ADDR, worker, pass, &mp)) { dev_conn.dead = 1; return -1; }
    printf("[dev-fee] warming dev connection (%s)\n", DEV_FEE_ADDR);
    return 0;
}
static void dev_close(void) {
    if (dev_conn.fd >= 0) { dev_conn.dead = 1; close(dev_conn.fd); dev_conn.fd = -1; }
    dev_conn.job.have = 0;
}
#else
#define g_dev_now 0
#endif

/* ---- prep prefetch thread ---- */
typedef struct {
    int8_t *A, *B, *An, *Btn, *EAL, *EBR;
    uint8_t roots[4][32];
    uint8_t key[32];
    char job_id[JOBLEN];
    double diff;
    uint8_t ptarget[32];
    int slot;            /* this bundle's device B-buffer slot (0/1, = bun[] index) */
} bundle_t;
static void bundle_key(bundle_t *b);
/* Deferred prep timing: capture the job as LATE as possible (just before the overlapping
 * scan ends) so the prepped matrix binds to the freshest header, cutting depth-2 staleness.
 *   PRL_PREP_DELAY_MS set   -> static delay (0 disables)
 *   PRL_PREP_DELAY_MS unset -> ADAPTIVE: delay = last_scan - last_prep - margin (self-tunes) */
static volatile long g_last_scan_ms = 0, g_last_prep_ms = 0;

/* REUSE-B. B-side (B, packed bt, commitB, key) is fixed within a job, so it is built
 * once per job (or every PRL_BREUSE iters) on the MAIN thread (between scans, no race) and the
 * per-iter prep_worker does ONLY the A-side (hidden under the scan).
 * Gated by PRL_REUSE_B; off => full-prep path. */
static int g_reuse_b = 0;
static int8_t *g_EAL = 0;                 /* shared A-side noise scratch (m*rank) */
static struct {
    int8_t *B, *bt;
    int8_t *EBR;
    uint8_t key[32], commitB[32], rootB[32];
    char job_id[JOBLEN];
    double diff; uint8_t ptarget[32];
    int slot, valid; long since;
} bs;
/* (main thread, between scans) rebuild the B-side iff the job changed or the refresh interval
 * elapsed. Returns 1 if B was regenerated (caller must re-prep the current bundle's A-side so
 * its commitA matches the new commitB), else 0. */
static int ensure_bside(long N, long Kc, int rank, int prep_n) {
    long refresh = getenv("PRL_BREUSE") ? atol(getenv("PRL_BREUSE")) : 0;  /* 0 = job-change only */
    pthread_mutex_lock(&job_mu);
    int jobchg = !bs.valid || strncmp(bs.job_id, g_job->job_id, JOBLEN - 1);
    char jid[JOBLEN]; strncpy(jid, g_job->job_id, JOBLEN - 1); jid[JOBLEN - 1] = 0;
    double diff = g_job->difficulty; uint8_t pt[32]; memcpy(pt, g_job->ptarget, 32);
    static uint8_t hdr[HDRLEN]; size_t hl = g_job->header_len; memcpy(hdr, g_job->header, hl);
    pthread_mutex_unlock(&job_mu);
    if (!(jobchg || (refresh > 0 && bs.since >= refresh))) return 0;
    strncpy(bs.job_id, jid, JOBLEN - 1); bs.diff = diff; memcpy(bs.ptarget, pt, 32);
    hash_key(hdr, hl, (size_t)Kc, (size_t)rank, mp.rows, mp.nrows, mp.cols, mp.ncols, bs.key);
    uint64_t seed = ((uint64_t)rand() << 32) ^ (uint64_t)time(0) ^ 0xB5B5ULL;
    prep_b_side(seed, bs.B, bs.key, N, Kc, rank, bs.bt, bs.EBR, bs.rootB, bs.commitB, prep_n);
    if (pearl_set_bt_slot)     pearl_set_bt_slot(bs.bt, 0);          /* single device slot 0 */
    else if (pearl_set_b_slot) pearl_set_b_slot(bs.bt, (int)N, 0);
    bs.slot = 0; bs.valid = 1; bs.since = 0;
    return 1;
}

static void *prep_worker(void *p) {
    bundle_t *b = p;
    if (g_reuse_b) {  /* A-side only: B is cached in bs (built on the main thread) */
        int prep_n = getenv("PRL_PREP_THREADS") ? atoi(getenv("PRL_PREP_THREADS")) : 64;
        struct timespec pa, pb; clock_gettime(CLOCK_MONOTONIC, &pa);
        uint64_t seed = ((uint64_t)rand() << 32) ^ (uint64_t)time(0);
        strncpy(b->job_id, bs.job_id, JOBLEN - 1);
        b->diff = bs.diff; memcpy(b->ptarget, bs.ptarget, 32); memcpy(b->key, bs.key, 32);
        prep_a_side(seed, b->A, bs.key, bs.commitB, mp.m, mp.k, (int)mp.rank,
                    b->An, g_EAL, b->roots[0] /*rootA*/, b->roots[2] /*commitA = PoW key*/, prep_n);
        clock_gettime(CLOCK_MONOTONIC, &pb);
        g_last_prep_ms = (pb.tv_sec - pa.tv_sec) * 1000 + (pb.tv_nsec - pa.tv_nsec) / 1000000;
        return 0;
    }
    const char *ds = getenv("PRL_PREP_DELAY_MS");
    long delay_ms;
    if (ds) {
        delay_ms = atol(ds);
    } else {
        long margin = getenv("PRL_PREP_MARGIN_MS") ? atol(getenv("PRL_PREP_MARGIN_MS")) : 1000;
        delay_ms = g_last_scan_ms - g_last_prep_ms - margin;
        if (delay_ms < 0) delay_ms = 0;
    }
    if (delay_ms > 0) usleep((useconds_t)(delay_ms * 1000));
    bundle_key(b);
    struct timespec pa, pb;
    clock_gettime(CLOCK_MONOTONIC, &pa);
    uint64_t seed = ((uint64_t)rand() << 32) ^ (uint64_t)time(0);
    /* prep thread count env-overridable (PRL_PREP_THREADS, default 64). prep is
     * overlapped/hidden behind the scan, so it can run on FEWER cores; with NUMA pinning,
     * budget prep+PoW under one 32-core node — e.g. PRP=24 + POW=8. */
    int prep_n = getenv("PRL_PREP_THREADS") ? atoi(getenv("PRL_PREP_THREADS")) : 64;
    /* prefer the fused bt path (prep emits packed bt; set_bt_slot just H2Ds it). This
     * eliminates set_b's host repackBT. Both prep and the
     * device load run in THIS thread, overlapping the live scan (which reads the other slot). */
    if (pearl_set_bt_slot) {
        prep_random_bt(seed, b->A, b->B, b->key, mp.m, mp.n, mp.k, (int)mp.rank,
                       b->An, b->Btn, b->EAL, b->EBR,
                       b->roots[0], b->roots[1], b->roots[2], b->roots[3], prep_n);
    } else {
        prep_random(seed, b->A, b->B, b->key, mp.m, mp.n, mp.k, (int)mp.rank,
                    b->An, b->Btn, b->EAL, b->EBR,
                    b->roots[0], b->roots[1], b->roots[2], b->roots[3], prep_n);
    }
    clock_gettime(CLOCK_MONOTONIC, &pb);
    g_last_prep_ms = (pb.tv_sec - pa.tv_sec) * 1000 + (pb.tv_nsec - pa.tv_nsec) / 1000000;
    /* load this bundle's B into its OWN device slot here (overlaps the live scan on the other slot) */
    if (pearl_set_bt_slot)     pearl_set_bt_slot(b->Btn, b->slot);          /* b->Btn IS packed bt */
    else if (pearl_set_b_slot) pearl_set_b_slot(b->Btn, (int)mp.n, b->slot);
    return 0;
}
static void bundle_key(bundle_t *b) {
    pthread_mutex_lock(&job_mu);
    strncpy(b->job_id, g_job->job_id, JOBLEN - 1);
    b->diff = g_job->difficulty;
    memcpy(b->ptarget, g_job->ptarget, 32);
    hash_key(g_job->header, g_job->header_len, (size_t)mp.k, (size_t)mp.rank,
             mp.rows, mp.nrows, mp.cols, mp.ncols, b->key);
    pthread_mutex_unlock(&job_mu);
}

/* ---- coexistence: external pause/resume via signals -------------------------------------
 * Concurrent kernel execution by two tenants on one Ascend die is a likely crash, so coexistence
 * means yielding the WHOLE NPU on request. The miner is a PASSIVE endpoint here: an external
 * watcher (scripts/coexist_guard, a vLLM-side hook, or plain `kill`) decides WHEN to yield.
 *   SIGUSR1 -> pause: finish the in-flight scan, drain the overlapped prep H2D, then idle —
 *              launching NOTHING on the device. Once quiescent the miner ACKs by sending SIGUSR1
 *              back to the requester (siginfo si_pid), so the watcher KNOWS the die is free before
 *              it lets the other tenant proceed (the ACK is the "wait it stops" handshake).
 *   SIGUSR2 -> resume mining.
 * The pause is honored only at the loop's quiescent point (prep joined, nothing on the device), so
 * an ACK guarantees no kernel is in flight. Cross-PID-namespace (host watcher, containerized
 * miner): si_pid may be 0/untranslatable -> the miner skips the ACK and the watcher falls back to
 * its timeout. Default behavior is unchanged until a signal arrives. */
static volatile sig_atomic_t g_pause_req = 0;
static volatile sig_atomic_t g_ack_pid = 0;
static void on_pause_sig(int sig, siginfo_t *si, void *u) {
    (void)sig; (void)u;
    g_pause_req = 1;
    g_ack_pid = si ? (sig_atomic_t)si->si_pid : 0;
}
static void on_resume_sig(int sig, siginfo_t *si, void *u) {
    (void)sig; (void)si; (void)u;
    g_pause_req = 0;
}
/* Called at the loop's quiescent point. If a pause is pending, ACK the requester (the die is now
 * idle) and block — launching nothing — until SIGUSR2 clears it. */
static void coexist_gate(void) {
    if (!g_pause_req) return;
    pid_t ack = (pid_t)g_ack_pid;
    printf("[coexist] pause (req pid %d): NPU quiescent, idling until resume\n", (int)ack);
    if (ack > 0) kill(ack, SIGUSR1);          /* ACK: in-flight work drained, die is free */
    while (g_pause_req) {
        struct timespec ts = {0, 200L * 1000 * 1000};   /* 200ms; resume latency on SIGUSR2 */
        nanosleep(&ts, 0);
    }
    puts("[coexist] resume");
}

int main(int argc, char **argv) {
    if (argc < 7) { fprintf(stderr, "usage: %s dev worker host port addr pass\n", argv[0]); return 1; }
    int dev = atoi(argv[1]);
    const char *worker = argv[2], *host = argv[3], *addr = argv[5], *pass = argv[6];
    int port = atoi(argv[4]);
    signal(SIGPIPE, SIG_IGN);
    /* coexistence: SIGUSR1 = pause (ACK when quiescent), SIGUSR2 = resume. SA_RESTART so the
     * detached stratum reader's blocking read() isn't disturbed; flags are read at the gate. */
    { struct sigaction sa; memset(&sa, 0, sizeof sa);
      sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask);
      sa.sa_sigaction = on_pause_sig;  sigaction(SIGUSR1, &sa, 0);
      sa.sa_sigaction = on_resume_sig; sigaction(SIGUSR2, &sa, 0); }
    setvbuf(stdout, 0, _IOLBF, 0);
    g_reuse_b = getenv("PRL_REUSE_B") ? 1 : 0;   /* cache B-side, regen only A per iter */
    pool_conn_init(&user_conn, "");
    pool_conn_init(&dev_conn, " [dev]");
#if DEV_FEE_PERMILLE > 0
    printf("[dev-fee] This open-source build mines %.1f%% of the time for the developer\n"
           "[dev-fee]   wallet: %s\n"
           "[dev-fee]   To disable, rebuild from source: make DEV_FEE_PERMILLE=0\n",
           DEV_FEE_PERMILLE / 10.0, DEV_FEE_ADDR);
#endif

    /* miner-chosen pools (kryptex) set rank/shape locally (must match the linked .so K/RANK);
     * pool-dictated pools (k1pool) leave mp.have=0 until pearl.set_mining_params arrives. */
    POOL.init_params(&mp);
    printf("[stratum] pool=%s\n", POOL.name);

    while (do_handshake(host, port, addr, worker, pass)) { puts("[!] handshake failed, retry 10s"); sleep(10); }

    long M = mp.m, N = mp.n, Kc = mp.k;
    /* hash tile = nrows x 64 (rows_pattern len), HT per 64-row block = 64/nrows */
    int nbands = (int)(N / 64), nstrips = (int)(M / R);
    int tile_h = (int)mp.nrows, n_hi = R / tile_h, ht_per_block = 64 / tile_h;
    if (pearl_init(dev, nbands)) { puts("pearl_init failed"); return 2; }

    /* An is H2D'd to the NPU every strip — pin it (page-locked) so the async H2D overlaps.
     * Weak: falls back to malloc if the linked .so doesn't expose pearl_pinned_alloc. */
    extern void *pearl_pinned_alloc(size_t) __attribute__((weak));
    bundle_t bun[2];
    for (int i = 0; i < 2; i++) {
        bun[i].slot = i;   /* fixed device B-buffer slot per bundle */
        bun[i].A = malloc((size_t)M * Kc); bun[i].B = malloc((size_t)N * Kc);
        bun[i].An = pearl_pinned_alloc ? (int8_t *)pearl_pinned_alloc((size_t)M * Kc)
                                       : (int8_t *)malloc((size_t)M * Kc);
        bun[i].Btn = malloc((size_t)Kc * N);
        /* noise scratch is m*rank / n*rank (NOT m*R — rank != R when r>128, e.g. r=1024) */
        bun[i].EAL = malloc((size_t)M * mp.rank); bun[i].EBR = malloc((size_t)N * mp.rank);
    }
    uint32_t *scratch = malloc(2ull * nbands * 16 * 64 * 4);
    uint8_t *b64 = malloc(64 << 20);
    size_t rows_abs[64], cols_abs[64];

    if (g_reuse_b) {   /* shared B-side cache + A-side scratch */
        bs.B = malloc((size_t)N * Kc); bs.bt = malloc((size_t)Kc * N);
        bs.EBR = malloc((size_t)N * mp.rank); bs.valid = 0;
        g_EAL = malloc((size_t)M * mp.rank);
        ensure_bside(N, Kc, (int)mp.rank, 64);   /* build B for the first job before A-side prep */
        printf("[reuse-B] enabled (PRL_BREUSE=%s, B-side cached per job)\n",
               getenv("PRL_BREUSE") ? getenv("PRL_BREUSE") : "0=job-change-only");
    }
    pthread_t pt;
    pthread_create(&pt, 0, prep_worker, &bun[0]);   /* prep_worker self-captures the job (deferred) */
    long iter = 0;
    time_t t_start = time(0);
    while (1) {
#if DEV_FEE_PERMILLE > 0
        /* dev-fee: pre-warm the dev connection ahead of its window (behind user mining); enter the
         * window only once it holds a valid job (graceful, no "Job not found"); mine dev_conn's job
         * + submit on the dev socket during the window; switch back + close after. reuse-B re-binds
         * prep on the g_job change. (Requires reuse-B; without it a switch iter's bundle is stale.) */
        { long s2w = dev_secs_to_window();
          if (dev_conn.fd < 0 && s2w <= DEV_FEE_PREOPEN_S) dev_open(host, port, worker, pass);  /* warm ahead */
          if (s2w == 0 && !g_dev_now && !dev_conn.dead && dev_conn.job.have) {
              g_dev_now = 1; g_job = &dev_conn.job; printf("[dev-fee] window OPEN -> mining for dev\n"); }
          if ((s2w != 0 || dev_conn.dead) && g_dev_now) {
              g_dev_now = 0; g_job = &user_conn.job;
              printf("[dev-fee] window CLOSE -> mining for user (dev shares so far %ld)\n", g_fee_sub);
              dev_close(); }
          if (s2w > DEV_FEE_PREOPEN_S && dev_conn.fd >= 0 && !g_dev_now) dev_close(); }   /* idle: close */
#endif
        if (user_conn.dead) {
            puts("[!] (re)connecting");
            close(user_conn.fd);
#if DEV_FEE_PERMILLE > 0
            if (g_dev_now) { g_dev_now = 0; g_job = &user_conn.job; dev_close(); }   /* user conn died: drop dev win */
#endif
            while (do_handshake(host, port, addr, worker, pass)) sleep(10);
        }
        bundle_t *cur = &bun[iter % 2], *nxt = &bun[(iter + 1) % 2];
        struct timespec lj0; clock_gettime(CLOCK_MONOTONIC, &lj0);
        pthread_join(pt, 0);
        struct timespec lj1; clock_gettime(CLOCK_MONOTONIC, &lj1);   /* prep-join wait */
        /* coexist gate: prep (and its H2D) is now joined, so the device is quiescent. Honor a
         * pending pause HERE — before any H2D/scan launches — so an ACK means the die is truly free. */
        coexist_gate();
        if (g_reuse_b) {   /* rebuild B-side iff job changed (main thread, no race) */
            int prep_n = getenv("PRL_PREP_THREADS") ? atoi(getenv("PRL_PREP_THREADS")) : 64;
            if (ensure_bside(N, Kc, (int)mp.rank, prep_n)) {
                /* B changed -> cur->An used the old commitB; re-prep cur's A-side to match */
                uint64_t s = ((uint64_t)rand() << 32) ^ (uint64_t)time(0) ^ 0xA5A5ULL;
                prep_a_side(s, cur->A, bs.key, bs.commitB, mp.m, mp.k, (int)mp.rank,
                            cur->An, g_EAL, cur->roots[0], cur->roots[2], prep_n);
                strncpy(cur->job_id, bs.job_id, JOBLEN - 1);
                cur->diff = bs.diff; memcpy(cur->ptarget, bs.ptarget, 32); memcpy(cur->key, bs.key, 32);
            }
            bs.since++;
        }
        pthread_create(&pt, 0, prep_worker, nxt);   /* prep_worker self-captures freshest job (deferred) */

        time_t t0 = time(0);
        struct timespec sa; clock_gettime(CLOCK_MONOTONIC, &sa);   /* overlap window for adaptive prep */
        /* B was already repacked+H2D'd into cur->slot by prep_worker (overlapping the
         * previous scan). Just flip the live buffer + reset flags here (~us). Fallback: if the
         * .so lacks the split API, do the synchronous set_b on the critical path. */
        if (pearl_use_b) pearl_use_b(g_reuse_b ? 0 : cur->slot);
        else pearl_set_b(cur->Btn, (int)N);
        struct timespec sbb; clock_gettime(CLOCK_MONOTONIC, &sbb);   /* set_b+target done */
        uint32_t tgt[8];
        long rounded_k = (Kc / mp.rank) * mp.rank;
        /* the target depends only on this bundle's job snapshot (captured at prep time) */
        job_t jt; jt.difficulty = cur->diff; memcpy(jt.ptarget, cur->ptarget, 32);
        POOL.build_target(&jt, tile_h, rounded_k, tgt);
        int hs = -1, ht = -1;
        /* PoW hash-pool thread count: env-overridable (NUMA pinning wants ~32-cores/node, so
         * 16 hides the hash on an NPU-bound scan; 64 oversubscribes a pinned node). Default 64. */
        int pow_n = getenv("PRL_POW_THREADS") ? atoi(getenv("PRL_POW_THREADS")) : 64;
        int rc = scan_full(cur->An, nstrips, cur->roots[2], tgt, nbands, n_hi, pow_n,
                           scratch, &hs, &ht);
        struct timespec sb; clock_gettime(CLOCK_MONOTONIC, &sb);
        g_last_scan_ms = (sb.tv_sec - sa.tv_sec) * 1000 + (sb.tv_nsec - sa.tv_nsec) / 1000000;
        if (getenv("PRL_LOOP_PROF"))
            fprintf(stderr, "[loopprof] prep-join=%.0fms set_b+tgt=%.0fms scan_full=%.0fms\n",
                    (lj1.tv_sec-lj0.tv_sec)*1e3 + (lj1.tv_nsec-lj0.tv_nsec)/1e6,
                    (sbb.tv_sec-sa.tv_sec)*1e3 + (sbb.tv_nsec-sa.tv_nsec)/1e6,
                    (sb.tv_sec-sbb.tv_sec)*1e3 + (sb.tv_nsec-sbb.tv_nsec)/1e6);
        iter++;
        if (rc) {
            int hi = ht / nbands, wi = ht % nbands;
            long row = (long)hs * R + (long)(hi / ht_per_block) * 64 + hi % ht_per_block;
            long col = (long)wi * 64;
            for (size_t i = 0; i < mp.nrows; i++) rows_abs[i] = (size_t)row + mp.rows[i];
            for (size_t i = 0; i < mp.ncols; i++) cols_abs[i] = (size_t)col + mp.cols[i];
            ssize_t bl = build_proof_b64((uint8_t *)cur->A, (uint8_t *)(g_reuse_b ? bs.B : cur->B),
                                         (size_t)M, (size_t)N, (size_t)Kc, (size_t)mp.rank,
                                         cur->key, rows_abs, mp.nrows, cols_abs, mp.ncols,
                                         b64, 64 << 20);
            if (bl > 0) {
                /* submit on whichever connection is authorized for this window (dev or user) so
                 * the pool credits the right wallet; job_id is that session's current job. */
                pool_conn_t *sc = g_dev_now ? &dev_conn : &user_conn;
                const char *sub_addr = g_dev_now ? DEV_FEE_ADDR : addr;
                char *msg = malloc((size_t)bl + 512);
                const char *tail;
                int hl = POOL.submit_prefix(sc, sub_addr, worker, cur->job_id, msg, 512, &tail);
                memcpy(msg + hl, b64, (size_t)bl);
                strcpy(msg + hl + bl, tail);
                pool_send_line(sc, msg);
                if (getenv("PRL_RAW")) fprintf(stderr, "[raw>] %s\n", msg);
                free(msg);
                shares_sub++;
                if (g_dev_now) g_fee_sub++;
                printf("[✓]%s share row=%ld col=%ld job=%s (%ld submitted, %ld dev)\n",
                       g_dev_now ? " [dev]" : "", row, col, cur->job_id, shares_sub, g_fee_sub);
            } else printf("[!] proof build failed %zd\n", bl);
        }
        double el = difftime(time(0), t_start);
        printf("[*] iter %ld scan %.0fs%s | %.3f iter/s | shares %ld/%ld\n",
               iter, difftime(time(0), t0), rc ? " HIT" : "", iter / (el > 0 ? el : 1), shares_acc, shares_sub);
        { const char *mi = getenv("PRL_MAX_ITERS");   /* profiling: clean exit after N iters */
          if (mi && iter >= atol(mi)) { puts("[*] PRL_MAX_ITERS reached, exiting"); break; } }
    }
    return 0;
}
