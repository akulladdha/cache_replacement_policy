/*
 * mixed.c -- a small reused hot set interleaved with a large streaming scan.
 *
 * Usage: mixed <stream_kb> [hot_kb] [phases]
 *
 * This is the case insertion policy is designed for. The hot array is small
 * enough to live in the LLC and is re-read every phase, so it is genuinely
 * worth keeping. The streaming array is large, is read once per phase, and
 * is never reused.
 *
 * Under a policy that inserts everything at high priority, the streaming
 * phase walks the whole cache and evicts the hot set on the way through, so
 * the next hot phase misses on data that was resident a moment earlier.
 * A policy that inserts streaming lines as evict-me-first leaves the hot set
 * alone. That is the whole argument for BRRIP-style insertion, and this
 * benchmark isolates it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int
main(int argc, char **argv)
{
    size_t stream_kb = 8192;
    size_t hot_kb = 64;
    size_t phases = 40;

    if (argc > 1) {
        stream_kb = (size_t)strtoul(argv[1], NULL, 10);
    }
    if (argc > 2) {
        hot_kb = (size_t)strtoul(argv[2], NULL, 10);
    }
    if (argc > 3) {
        phases = (size_t)strtoul(argv[3], NULL, 10);
    }

    size_t n_stream = (stream_kb * 1024) / sizeof(uint64_t);
    size_t n_hot = (hot_kb * 1024) / sizeof(uint64_t);
    if (n_stream < 1) {
        n_stream = 1;
    }
    if (n_hot < 1) {
        n_hot = 1;
    }

    uint64_t *big = (uint64_t *)malloc(n_stream * sizeof(uint64_t));
    uint64_t *hot = (uint64_t *)malloc(n_hot * sizeof(uint64_t));
    if (big == NULL || hot == NULL) {
        fprintf(stderr, "mixed: allocation failed\n");
        return 1;
    }

    for (size_t i = 0; i < n_stream; i++) {
        big[i] = i;
    }
    for (size_t i = 0; i < n_hot; i++) {
        hot[i] = i;
    }

    const size_t stride = 64 / sizeof(uint64_t);
    /* One slice of the big array per phase, so total streaming work stays
     * fixed as the array grows and the instruction count stays comparable. */
    size_t slice = n_stream / phases;
    if (slice < stride) {
        slice = stride;
    }

    uint64_t sum = 0;
    for (size_t p = 0; p < phases; p++) {
        /* Hot phase: re-read the small set several times. */
        for (size_t rep = 0; rep < 8; rep++) {
            for (size_t i = 0; i < n_hot; i += stride) {
                sum += hot[i];
            }
        }

        /* Streaming phase: read a fresh slice of the big array once. */
        size_t start = (p * slice) % n_stream;
        size_t end = start + slice;
        if (end > n_stream) {
            end = n_stream;
        }
        for (size_t i = start; i < end; i += stride) {
            sum += big[i];
        }
    }

    printf("mixed: stream=%zuKB hot=%zuKB phases=%zu sum=%llu\n",
           stream_kb, hot_kb, phases, (unsigned long long)sum);

    free(big);
    free(hot);
    return 0;
}
