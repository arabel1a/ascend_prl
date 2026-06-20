/*
 * Optional gzip transform for the kryptex type:"v2" submit path.
 *
 * The proof FFI (libpearl_proof) emits base64(bincode) ~136KB. When the kryptex pool negotiated
 * gzip (conn->gzip, see src/pools/kryptex.c), the engine pipes that base64 through here to get
 * base64(gzip(bincode)) ~15-16KB. We base64-DECODE the FFI output back to the raw bincode, gzip
 * it, then base64-encode — so the .so stays untouched (no Rust rebuild needed on the miner box).
 *
 * Compression is real zlib (gzip container, windowBits=31) via libz.so.1. The build box has the
 * runtime lib but no zlib.h, so the minimal ABI is declared here and linked with -l:libz.so.1.
 * zlib's z_stream layout + the version[0]/stream_size handshake are stable across all 1.x.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/* ---- minimal zlib ABI (no <zlib.h> on the build box) ---- */
typedef struct {
    const unsigned char *next_in; unsigned int avail_in; unsigned long total_in;
    unsigned char *next_out; unsigned int avail_out; unsigned long total_out;
    const char *msg; void *state;
    void *(*zalloc)(void *, unsigned, unsigned); void (*zfree)(void *, void *); void *opaque;
    int data_type; unsigned long adler; unsigned long reserved;
} z_stream_min;
extern int deflateInit2_(z_stream_min *, int level, int method, int windowBits, int memLevel,
                         int strategy, const char *version, int stream_size);
extern int deflate(z_stream_min *, int flush);
extern int deflateEnd(z_stream_min *);
#define Z_FINISH       4
#define Z_OK           0
#define Z_STREAM_END   1
#define Z_DEFLATED     8
#define ZLIB_VERSION   "1"   /* deflateInit2_ only compares version[0]; "1" matches every zlib 1.x */

/* gzip `in` (n bytes) into `out` (cap). gzip container (windowBits=15+16). Returns len or -1. */
static ssize_t gz_compress(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    z_stream_min s;
    memset(&s, 0, sizeof s);
    if (deflateInit2_(&s, 6, Z_DEFLATED, 31, 8, 0, ZLIB_VERSION, (int)sizeof s) != Z_OK) return -1;
    s.next_in = in;   s.avail_in  = (unsigned)n;
    s.next_out = out; s.avail_out = (unsigned)cap;
    int r = deflate(&s, Z_FINISH);
    size_t outlen = s.total_out;
    deflateEnd(&s);
    return (r == Z_STREAM_END) ? (ssize_t)outlen : -1;
}

/* ---- standard base64 (matches Rust base64 STANDARD: +/ alphabet, '=' padding) ---- */
static const char B64E[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static ssize_t b64_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    if (((n + 2) / 3) * 4 > cap) return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned)in[i + 2];
        out[o++] = B64E[(v >> 18) & 63];
        out[o++] = B64E[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? B64E[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? B64E[v & 63] : '=';
    }
    return (ssize_t)o;
}

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;   /* '=' / whitespace / anything else */
}

static ssize_t b64_decode(const char *in, size_t n, uint8_t *out, size_t cap) {
    int q[4], qi = 0; size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == '=') break;
        int v = b64_val((unsigned char)in[i]);
        if (v < 0) continue;                 /* skip newlines / stray bytes */
        q[qi++] = v;
        if (qi == 4) {
            if (o + 3 > cap) return -1;
            out[o++] = (uint8_t)((q[0] << 2) | (q[1] >> 4));
            out[o++] = (uint8_t)(((q[1] & 15) << 4) | (q[2] >> 2));
            out[o++] = (uint8_t)(((q[2] & 3) << 6) | q[3]);
            qi = 0;
        }
    }
    if (qi >= 2) {                            /* trailing 2-3 chars => 1-2 bytes */
        if (o + (size_t)(qi - 1) > cap) return -1;
        out[o++] = (uint8_t)((q[0] << 2) | (q[1] >> 4));
        if (qi >= 3) out[o++] = (uint8_t)(((q[1] & 15) << 4) | (q[2] >> 2));
    }
    return (ssize_t)o;
}

/* base64(bincode) -> base64(gzip(bincode)). Writes into `out` (cap), returns its length or -1. */
ssize_t gzip_proof_b64(const char *in_b64, size_t in_len, uint8_t *out, size_t cap) {
    size_t bin_cap = (in_len / 4) * 3 + 4;
    uint8_t *bin = malloc(bin_cap);
    if (!bin) return -1;
    ssize_t bn = b64_decode(in_b64, in_len, bin, bin_cap);
    if (bn < 0) { free(bin); return -1; }
    size_t gz_cap = (size_t)bn + (size_t)bn / 2 + 256;   /* deflate worst case + gzip framing */
    uint8_t *gz = malloc(gz_cap);
    if (!gz) { free(bin); return -1; }
    ssize_t gn = gz_compress(bin, (size_t)bn, gz, gz_cap);
    free(bin);
    if (gn < 0) { free(gz); return -1; }
    ssize_t on = b64_encode(gz, (size_t)gn, (char *)out, cap);
    free(gz);
    return on;
}
