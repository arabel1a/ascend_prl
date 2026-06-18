/* pthread implementation of blake3's parallel split hook. blake3.c (compiled -DBLAKE3_USE_TBB)
 * calls this for each left/right subtree split; we run the LEFT subtree on a helper thread and
 * the RIGHT in-place, bounded by a global concurrency budget. blake3's own tree logic is
 * unchanged => digest is bit-identical to the serial path; only WHERE the work runs changes.
 * Only large subtrees are threaded (small ones run serial to avoid pthread overhead). */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

extern size_t blake3_compress_subtree_wide(const uint8_t *input, size_t input_len,
                                           const uint32_t key[8], uint64_t chunk_counter,
                                           uint8_t flags, uint8_t *out, bool use_tbb);

#ifndef PRL_B3_CAP
#define PRL_B3_CAP 64            /* max concurrent helper threads */
#endif
#ifndef PRL_B3_MIN
#define PRL_B3_MIN (1u << 20)    /* only thread subtrees >= 1 MB */
#endif

static int g_budget = 0;
static pthread_mutex_t g_bmu = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    const uint8_t *input; size_t input_len; const uint32_t *key;
    uint64_t chunk_counter; uint8_t flags; uint8_t *cvs; size_t *n; bool use_tbb;
} sub_arg;

static void *sub_thread(void *a) {
    sub_arg *s = (sub_arg *)a;
    *s->n = blake3_compress_subtree_wide(s->input, s->input_len, s->key,
                                         s->chunk_counter, s->flags, s->cvs, s->use_tbb);
    return 0;
}

void blake3_compress_subtree_wide_join_tbb(
    const uint32_t key[8], uint8_t flags, bool use_tbb,
    const uint8_t *l_input, size_t l_input_len, uint64_t l_chunk_counter,
    uint8_t *l_cvs, size_t *l_n,
    const uint8_t *r_input, size_t r_input_len, uint64_t r_chunk_counter,
    uint8_t *r_cvs, size_t *r_n)
{
    int spawn = 0;
    if (use_tbb && l_input_len >= PRL_B3_MIN) {
        pthread_mutex_lock(&g_bmu);
        if (g_budget < PRL_B3_CAP) { g_budget++; spawn = 1; }
        pthread_mutex_unlock(&g_bmu);
    }
    if (spawn) {
        sub_arg la = { l_input, l_input_len, key, l_chunk_counter, flags, l_cvs, l_n, use_tbb };
        pthread_t t;
        if (pthread_create(&t, 0, sub_thread, &la) == 0) {
            *r_n = blake3_compress_subtree_wide(r_input, r_input_len, key,
                                                r_chunk_counter, flags, r_cvs, use_tbb);
            pthread_join(t, 0);
            pthread_mutex_lock(&g_bmu); g_budget--; pthread_mutex_unlock(&g_bmu);
            return;
        }
        pthread_mutex_lock(&g_bmu); g_budget--; pthread_mutex_unlock(&g_bmu);  /* create failed -> serial */
    }
    *l_n = blake3_compress_subtree_wide(l_input, l_input_len, key, l_chunk_counter, flags, l_cvs, use_tbb);
    *r_n = blake3_compress_subtree_wide(r_input, r_input_len, key, r_chunk_counter, flags, r_cvs, use_tbb);
}
