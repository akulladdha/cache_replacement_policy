/*
 * mixed.c -- a reused hot set interleaved with a large streaming scan.
 *
 * Usage: mixed <stream_kb> [hot_kb] [phases] [reps]
 *
 * This is the case insertion policy is designed for. The hot array is small
 * enough to live in the LLC alongside everything else and is re-read `reps`
 * times every phase, so it is genuinely worth keeping. The streaming array
 * is larger than the LLC, is read once per phase, and is never reused.
 *
 * Under a policy that inserts every line at high priority, the streaming
 * phase walks the whole cache and evicts the hot set on the way through, so
 * the next hot phase misses on data that was resident a moment earlier. A
 * policy that inserts streaming lines as evict-me-first leaves the hot set
 * alone. That is the whole argument for BRRIP-style insertion.
 *
 * Sizing matters more than it looks. An earlier version streamed only a
 * slice of the big array per phase, and that slice was smaller than the
 * cache, so the hot set survived under every policy and all three came out
 * within half a percent of each other. The streaming phase has to exceed
 * capacity for the eviction pressure to exist at all. The defaults below put
 * the hot set at half the baseline 1 MiB L2 and stream twice capacity per
 * phase.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/*
 * Stop the compiler from merging repeated read passes.
 *
 * This is not a stylistic detail, it was a real bug. Re-reading the same
 * array N times to produce a sum is a pure reduction with no side effects,
 * so at -O2 gcc is entitled to collapse the N passes into one and multiply.
 * It does. The first version of this benchmark therefore generated a single
 * pass over the hot set per phase instead of `reps` passes, which means it
 * had no reuse to protect, which means every replacement policy scored
 * identically and the benchmark measured nothing.
 *
 * The empty asm with a memory clobber tells the compiler that memory may
 * have changed, so each pass has to actually re-read the array.
 */
static inline void
barrier(void)
{
    __asm__ __volatile__("" : : : "memory");
}

int
main(int argc, char **argv)
{
    size_t stream_kb = 2048;
    size_t hot_kb = 512;
    size_t phases = 10;
    size_t reps = 4;

    if (argc > 1) {
        stream_kb = (size_t)strtoul(argv[1], NULL, 10);
    }
    if (argc > 2) {
        hot_kb = (size_t)strtoul(argv[2], NULL, 10);
    }
    if (argc > 3) {
        phases = (size_t)strtoul(argv[3], NULL, 10);
    }
    if (argc > 4) {
        reps = (size_t)strtoul(argv[4], NULL, 10);
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

    /* Stride by a cache line. Touching every element would spend the run on
     * L1 hits and dilute the L2 behaviour being measured. */
    const size_t stride = 64 / sizeof(uint64_t);

    /* Precompute a scattered visiting order over the hot set's cache lines,
     * so the hot phase touches exactly the same lines as a sequential walk
     * but in an order the cache cannot exploit. Built once, outside the
     * timed phases, so it costs nothing per phase. */
    size_t n_hot_lines = n_hot / stride;
    if (n_hot_lines < 1) {
        n_hot_lines = 1;
    }
    size_t *hot_order = (size_t *)malloc(n_hot_lines * sizeof(size_t));
    if (hot_order == NULL) {
        fprintf(stderr, "mixed: allocation of hot order failed\n");
        return 1;
    }
    for (size_t k = 0; k < n_hot_lines; k++) {
        hot_order[k] = k * stride;
    }
    /* Sattolo shuffle, same deterministic PRNG idea as chase.c. */
    uint64_t rs = 0x2545f4914f6cdd1dULL;
    for (size_t k = n_hot_lines - 1; k > 0; k--) {
        rs ^= rs << 13;
        rs ^= rs >> 7;
        rs ^= rs << 17;
        size_t j = (size_t)(rs % k);
        size_t tmp = hot_order[k];
        hot_order[k] = hot_order[j];
        hot_order[j] = tmp;
    }

    uint64_t sum = 0;
    for (size_t p = 0; p < phases; p++) {
        /* Hot phase: re-read the small set several times. Every one of these
         * accesses after the first rep should be an L2 hit if the policy
         * managed to protect the hot set through the preceding scan. */
        for (size_t rep = 0; rep < reps; rep++) {
            /* Walk the hot set in a fixed pseudo-random order rather than
             * sequentially.
             *
             * A sequential walk turned out not to discriminate between
             * policies at all: with every line entering the set in index
             * order, LRU and SRRIP made bit-identical eviction choices on
             * this workload. Visiting the same lines in a scattered order
             * removes that structure while keeping the working set and the
             * reuse pattern exactly the same. It also defeats any residual
             * spatial locality the L1 could exploit. */
            for (size_t k = 0; k < n_hot_lines; k++) {
                sum += hot[hot_order[k]];
            }
            barrier();
        }

        /* Streaming phase: read the whole big array once. No reuse here at
         * all, so every line of it is pure eviction pressure. */
        for (size_t i = 0; i < n_stream; i += stride) {
            sum += big[i];
        }
        barrier();
    }

    printf("mixed: stream=%zuKB hot=%zuKB phases=%zu reps=%zu sum=%llu\n",
           stream_kb, hot_kb, phases, reps, (unsigned long long)sum);

    free(hot_order);
    free(big);
    free(hot);
    return 0;
}
