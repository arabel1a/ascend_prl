/*
 * k1 frontend — k1pool's OBJECT protocol (kryptex-style), per the pool operator (2026-06-15).
 *
 * The EU endpoint (eu.pearl.k1pool.com:5566) speaks an object dialect, NOT the AlphaPool array
 * dialect we first reverse-engineered. Array-mode authorize returns result:true but lands in a
 * legacy mode that SILENTLY DROPS submits. The correct flow (operator-confirmed):
 *   - mining.authorize params is an OBJECT: {"wallet":<addr>,"worker":<rig>,"agent":<miner/ver>}
 *   - mining.notify params is an OBJECT: {header,height,job_id:"<hex8>_2097152",
 *                                         target:"<64 nibbles, 53 zero bits>",cert_version:2}
 *   - rank/shape are MINER-CHOSEN (no set_mining_params) and encoded in the PlainProof; the pool
 *     reads them out (so our fixed r512/k8192/16384² is fine — init_params sets it).
 *   - adjusted target = pool_target(BE) * tile_elems * rounded_k  (like kryptex; no PRL_K1_FACTOR).
 *   - mining.submit params is an OBJECT: {wallet,worker,job_id,plain_proof}.
 */
#include "pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef K
#define K 8192
#endif
#ifndef RANK
#define RANK 512
#endif
#ifndef MDIM
#define MDIM 16384
#endif

#ifndef K1_AGENT
#define K1_AGENT "ascend_prl/0.1"
#endif

static void k1_init_params(mining_params_t *mp) {
    /* miner-chosen; must match the linked kernel .so (K/RANK). Standard [0,32] / [0..63]. */
    mp->m = MDIM; mp->n = MDIM; mp->k = K; mp->rank = RANK;
    mp->rows[0] = 0; mp->rows[1] = 32; mp->nrows = 2;
    for (size_t i = 0; i < 64; i++) mp->cols[i] = i;
    mp->ncols = 64; mp->have = 1;
    printf("[stratum] k1: rank=%ld k=%ld %ldx%ld (miner-chosen, object protocol)\n",
           mp->rank, mp->k, mp->m, mp->n);
}

static int k1_open(pool_conn_t *c, const char *host, int port,
                   const char *addr, const char *worker, const char *pass, mining_params_t *mp) {
    (void)pass;
    if (pool_connect(c, host, port)) return -1;
    char line[LINE];
    snprintf(line, LINE, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[\"%s\"]}", K1_AGENT);
    pool_send_line(c, line);
    /* OBJECT authorize (operator-specified): {wallet, worker, agent} */
    snprintf(line, LINE, "{\"id\":3,\"method\":\"mining.authorize\",\"params\":"
             "{\"wallet\":\"%s\",\"worker\":\"%s\",\"agent\":\"%s\"}}", addr, worker, K1_AGENT);
    pool_send_line(c, line);
    pool_start_reader(c, mp);
    return 0;
}

static void k1_handle_notify(pool_conn_t *c, const char *line) {
    /* params is an OBJECT: {header, height, job_id, target, cert_version} */
    char jid[JOBLEN], hdr[2 * HDRLEN], tgt[80];
    if (jstr(line, "header", hdr, sizeof hdr)) return;
    if (jstr(line, "job_id", jid, sizeof jid)) return;
    long height = (long)jnum(line, "height", 0);
    uint8_t ptarget[32]; int have_t = 0;
    if (!jstr(line, "target", tgt, sizeof tgt)) {
        memset(ptarget, 0, 32);
        int tn = hex2bin(tgt, ptarget, 32);     /* up to 64 nibbles, big-endian */
        if (tn > 0) {
            if (tn < 32) { memmove(ptarget + (32 - tn), ptarget, tn);   /* right-align BE */
                           memset(ptarget, 0, 32 - tn); }
            have_t = 1;
        }
    }
    pthread_mutex_lock(&job_mu);
    strncpy(c->job.job_id, jid, JOBLEN - 1);
    c->job.header_len = (size_t)hex2bin(hdr, c->job.header, HDRLEN);
    c->job.height = height;
    if (have_t) { memcpy(c->job.ptarget, ptarget, 32); c->job.have_target = 1; }
    c->job.have = 1;
    pthread_mutex_unlock(&job_mu);
    printf("[stratum]%s job %s height=%ld%s\n", c->tag, jid, height, have_t ? " (target set)" : "");
}

static void k1_dispatch(pool_conn_t *c, const char *line, mining_params_t *mp) {
    (void)mp;
    if (getenv("PRL_RAW")) fprintf(stderr, "[raw<]%s %.500s\n", c->tag, line);
    char meth[48] = "";
    jstr(line, "method", meth, sizeof meth);
    if (!strcmp(meth, "mining.notify")) k1_handle_notify(c, line);
    else if (!strcmp(meth, "mining.set_difficulty")) {
        const char *p = jfind(line, "params");
        if (p && *p == '[') c->job.difficulty = atof(p + 1);
        printf("[stratum]%s difficulty %.0f\n", c->tag, c->job.difficulty);
    } else if (strstr(line, "\"result\"")) pool_handle_result(c, line);
}

static void k1_build_target(const job_t *J, int tile_h, long rounded_k, uint32_t out[8]) {
    target_from_be(J->ptarget, (long)tile_h * 64 * rounded_k, out);
}

static int k1_submit_prefix(pool_conn_t *c, const char *addr, const char *worker,
                            const char *job_id, char *msg, size_t cap, const char **tail) {
    /* OBJECT submit: {wallet, worker, job_id, plain_proof} */
    *tail = "\"}}";
    return snprintf(msg, cap, "{\"id\":%d,\"method\":\"mining.submit\",\"params\":"
                    "{\"wallet\":\"%s\",\"worker\":\"%s\",\"job_id\":\"%s\",\"plain_proof\":\"",
                    ++c->msg_id, addr, worker, job_id);
}

const pool_frontend_t POOL = {
    .name = "k1",
    .miner_chosen_params = 1,            /* object protocol: no set_mining_params; miner picks shape */
    .init_params = k1_init_params,
    .open = k1_open,
    .dispatch = k1_dispatch,
    .build_target = k1_build_target,
    .submit_prefix = k1_submit_prefix,
};
