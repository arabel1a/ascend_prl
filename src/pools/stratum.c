/*
 * Shared stratum plumbing — pool-agnostic pieces used by every frontend:
 *   - the socket connect + line-buffered reader thread (one per pool_conn_t)
 *   - tiny JSON field extractors
 *   - the share-result (ACCEPTED/REJECTED) counter
 *   - the 256-bit target arithmetic (diff- and big-endian-derived adjusted targets)
 * The protocol-specific bits (handshake messages, notify shape, submit framing, which
 * target formula) live in the per-pool frontends and reach these via pool.h.
 */
#include "pool.h"
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

long shares_sub = 0, shares_acc = 0, shares_rej = 0;
pthread_mutex_t job_mu = PTHREAD_MUTEX_INITIALIZER;

void pool_conn_init(pool_conn_t *c, const char *tag) {
    c->fd = -1;
    c->dead = 1;
    c->msg_id = 0;
    c->tag = tag;
    c->gzip = 0;
    c->job.have = 0;
    pthread_mutex_init(&c->send_mu, 0);
}

void pool_send_line(pool_conn_t *c, const char *s) {
    pthread_mutex_lock(&c->send_mu);
    size_t len = strlen(s), off = 0;
    while (off < len) {
        ssize_t w = write(c->fd, s + off, len - off);
        if (w <= 0) { c->dead = 1; break; }
        off += (size_t)w;
    }
    if (!c->dead && write(c->fd, "\n", 1) != 1) c->dead = 1;
    pthread_mutex_unlock(&c->send_mu);
}

int pool_connect(pool_conn_t *c, const char *host, int port) {
    struct addrinfo hints = {0}, *ai;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16]; snprintf(ps, 16, "%d", port);
    if (getaddrinfo(host, ps, &hints, &ai)) return -1;
    c->fd = socket(ai->ai_family, SOCK_STREAM, 0);
    if (connect(c->fd, ai->ai_addr, ai->ai_addrlen)) { freeaddrinfo(ai); c->fd = -1; return -1; }
    freeaddrinfo(ai);
    int one = 1; setsockopt(c->fd, SOL_SOCKET, SO_KEEPALIVE, &one, 4);
    c->dead = 0;
    return 0;
}

/* ---- the reader thread: read newline-delimited frames, hand each to the frontend ---- */
typedef struct { pool_conn_t *c; mining_params_t *mp; } reader_arg_t;
static void *reader_thread(void *arg) {
    reader_arg_t *ra = arg;
    pool_conn_t *c = ra->c;
    static char buf[1 << 20];                 /* one reader runs per conn; static is fine */
    char *b = c->tag && c->tag[0] ? malloc(1 << 20) : buf;   /* dev conn gets its own buffer */
    size_t fill = 0;
    while (!c->dead) {
        ssize_t r = read(c->fd, b + fill, (1 << 20) - fill - 1);
        if (r <= 0) { c->dead = 1; break; }
        fill += (size_t)r;
        b[fill] = 0;
        char *nl;
        while ((nl = memchr(b, '\n', fill))) {
            *nl = 0;
            POOL.dispatch(c, b, ra->mp);
            size_t rest = fill - (size_t)(nl - b) - 1;
            memmove(b, nl + 1, rest);
            fill = rest;
        }
    }
    if (b != buf) free(b);
    free(ra);
    return 0;
}

void pool_start_reader(pool_conn_t *c, mining_params_t *mp) {
    reader_arg_t *ra = malloc(sizeof *ra);
    ra->c = c; ra->mp = mp;
    pthread_t t; pthread_create(&t, 0, reader_thread, ra); pthread_detach(t);
}

/* shared by every frontend's dispatch for result lines (submit acks + cosmetic handshake acks).
 * Space-tolerant (jfind skips ':'/spaces): an error present and not null => reject; otherwise a
 * boolean result of `true` => accept. (Pools send compact JSON; this just doesn't depend on it.) */
void pool_handle_result(pool_conn_t *c, const char *line) {
    const char *e = jfind(line, "error");
    int has_err = e && strncmp(e, "null", 4) != 0 && *e != '}' && *e != ',';
    if (has_err) {
        shares_rej++;
        printf("[stratum]%s REJECTED: %s\n", c->tag, line);
        return;
    }
    const char *r = jfind(line, "result");
    if (r && !strncmp(r, "true", 4)) {
        shares_acc++;
        printf("[stratum]%s ACCEPTED (%ld/%ld)\n", c->tag, shares_acc, shares_sub);
    }
}

/* ---- tiny JSON helpers ---- */
const char *jfind(const char *s, const char *key) {
    static char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(s, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ':' || *p == ' ') p++;
    return p;
}
int jstr(const char *s, const char *key, char *out, size_t cap) {
    const char *p = jfind(s, key);
    if (!p || *p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) out[i++] = *p++;
    out[i] = 0;
    return 0;
}
double jnum(const char *s, const char *key, double dflt) {
    const char *p = jfind(s, key);
    return p ? atof(p) : dflt;
}
int jarr_sz(const char *p, size_t *out, int cap) {
    int n = 0;
    if (*p != '[') return 0;
    p++;
    while (*p && *p != ']' && n < cap) {
        out[n++] = (size_t)strtol(p, (char **)&p, 10);
        while (*p == ',' || *p == ' ') p++;
    }
    return n;
}
int hex2bin(const char *h, uint8_t *out, size_t cap) {
    size_t n = strlen(h) / 2;
    if (n > cap) return -1;
    for (size_t i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); out[i] = (uint8_t)v; }
    return (int)n;
}

/* ---- target arithmetic ---- */
/* out = q (LE limbs) * factor, saturated to 2^256-1 on overflow */
void mul_limbs(const uint32_t q[8], uint64_t f, uint32_t out[8]) {
    unsigned __int128 carry = 0;
    uint32_t res[8];
    for (int i = 0; i < 8; i++) {
        carry += (unsigned __int128)q[i] * f;
        res[i] = (uint32_t)carry;
        carry >>= 32;
    }
    if (carry) memset(res, 0xff, 32);
    memcpy(out, res, 32);
}

/* adjusted target = floor(maxtgt/diff) * tile_elems*rounded_k  (AlphaPool/k1pool dialect) */
void target_from_diff(double diff, long tile_elems_rounded_k, uint32_t out[8]) {
    if (diff <= 0) { memset(out, 0xff, 32); return; }
    uint32_t maxt[8] = {0};        /* 0xFFFF * 2^208: limb6 high half (LE limbs) */
    maxt[6] = 0xFFFF0000;
    uint64_t d = (uint64_t)diff, rem = 0;
    uint32_t q[8];
    for (int i = 7; i >= 0; i--) {
        unsigned __int128 cur = ((unsigned __int128)rem << 32) | maxt[i];
        q[i] = (uint32_t)(cur / d);
        rem = (uint64_t)(cur % d);
    }
    mul_limbs(q, (uint64_t)tile_elems_rounded_k, out);
}

/* adjusted target = pool_target(BE 32B) * tile_elems*rounded_k  (kryptex object-notify dialect) */
void target_from_be(const uint8_t be[32], long tile_elems_rounded_k, uint32_t out[8]) {
    uint32_t q[8];
    for (int i = 0; i < 8; i++)     /* BE bytes -> LE limbs */
        q[i] = (uint32_t)be[31 - 4 * i] | ((uint32_t)be[30 - 4 * i] << 8)
             | ((uint32_t)be[29 - 4 * i] << 16) | ((uint32_t)be[28 - 4 * i] << 24);
    mul_limbs(q, (uint64_t)tile_elems_rounded_k, out);
}
