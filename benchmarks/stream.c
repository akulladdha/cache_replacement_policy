/*
 * stream.c -- repeated sequential scan of an array.
 *
 * Usage: stream <working_set_kb> [passes]
 *
 * This is the control. A sequential streaming scan has no reuse at any
 * distance the cache can exploit once the array exceeds capacity: every line
 * is touched once per pass and evicted long before the next pass reaches it
 * again. No replacement policy can fix that, so all three policies should
 * land on essentially the same miss rate here.
 *
 * Its job in the writeup is to establish the floor. If SRRIP and BRRIP
 * differ noticeably on stream.c, something is wrong with the experiment
 * rather than interesting about the policies.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/*
 * Prevent the compiler from merging the repeated passes. Summing the same
 * array `passes` times is a pure reduction, so at -O2 gcc will happily run
 * the loop once and multiply. That would leave this benchmark touching the
 * array a single time, which is not what it is supposed to measure. See the
 * longer note in mixed.c, where the same optimisation silently invalidated
 * an entire set of results.
 */
static inline void
barrier(void)
{
    __asm__ __volatile__("" : : : "memory");
}

int
main(int argc, char **argv)
{
    size_t ws_kb = 4096;
    size_t passes = 24;

    if (argc > 1) {
        ws_kb = (size_t)strtoul(argv[1], NULL, 10);
    }
    if (argc > 2) {
        passes = (size_t)strtoul(argv[2], NULL, 10);
    }

    size_t n = (ws_kb * 1024) / sizeof(uint64_t);
    if (n < 1) {
        n = 1;
    }

    uint64_t *a = (uint64_t *)malloc(n * sizeof(uint64_t));
    if (a == NULL) {
        fprintf(stderr, "stream: allocation of %zu elements failed\n", n);
        return 1;
    }

    for (size_t i = 0; i < n; i++) {
        a[i] = i;
    }

    /* Stride by one cache line rather than one element. Touching every
     * element would spend most of the run on L1 hits and dilute the L2
     * behaviour we are trying to measure. */
    const size_t stride = 64 / sizeof(uint64_t);

    uint64_t sum = 0;
    for (size_t p = 0; p < passes; p++) {
        for (size_t i = 0; i < n; i += stride) {
            sum += a[i];
        }
        barrier();
    }

    printf("stream: ws=%zuKB passes=%zu sum=%llu\n",
           ws_kb, passes, (unsigned long long)sum);

    free(a);
    return 0;
}
