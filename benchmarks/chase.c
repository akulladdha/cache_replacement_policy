/*
 * chase.c -- randomly permuted pointer chase over a working set of a
 * given size. This is the workhorse benchmark for the replacement-policy
 * sweep.
 *
 * Usage: chase <working_set_kb> [steps]
 *
 * The working set is laid out as an array of cache-line-sized nodes, so a
 * request for N KB touches exactly N KB of distinct cache lines. The nodes
 * are linked into a single random cycle (Sattolo shuffle), so the chase
 * visits every node once per lap and the hardware prefetcher cannot predict
 * the next address.
 *
 * `steps` is fixed by default and independent of the working set, so every
 * point in the sweep executes very nearly the same number of instructions.
 * That keeps MPKI comparable across working-set sizes.
 *
 * Reuse distance equals the number of nodes. Below the LLC capacity every
 * lap hits; above it, an LRU-like policy misses on every access because each
 * line is evicted before the chase laps back around to it. That is exactly
 * the thrashing regime where insertion policy decides the outcome.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define LINE_BYTES 64

struct node {
    struct node *next;
    /* Pad the node out to a full cache line so that one node occupies one
     * line and the requested working set maps 1:1 onto cache capacity. */
    char pad[LINE_BYTES - sizeof(struct node *)];
};

/* Small deterministic PRNG. Using our own keeps the shuffle identical across
 * libc versions, so runs are reproducible. */
static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

static uint64_t
rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

int
main(int argc, char **argv)
{
    size_t ws_kb = 1024;
    size_t steps = 1500000;

    if (argc > 1) {
        ws_kb = (size_t)strtoul(argv[1], NULL, 10);
    }
    if (argc > 2) {
        steps = (size_t)strtoul(argv[2], NULL, 10);
    }

    size_t n_nodes = (ws_kb * 1024) / sizeof(struct node);
    if (n_nodes < 2) {
        n_nodes = 2;
    }

    struct node *nodes = (struct node *)malloc(n_nodes * sizeof(struct node));
    if (nodes == NULL) {
        fprintf(stderr, "chase: allocation of %zu nodes failed\n", n_nodes);
        return 1;
    }

    /* Build the identity order, then Sattolo-shuffle it. Sattolo (as opposed
     * to Fisher-Yates) guarantees a single cycle of length n_nodes rather
     * than a set of shorter disjoint cycles, so the chase is certain to
     * touch the whole working set. */
    size_t *order = (size_t *)malloc(n_nodes * sizeof(size_t));
    if (order == NULL) {
        fprintf(stderr, "chase: allocation of order array failed\n");
        return 1;
    }
    for (size_t i = 0; i < n_nodes; i++) {
        order[i] = i;
    }
    for (size_t i = n_nodes - 1; i > 0; i--) {
        size_t j = (size_t)(rng_next() % i);
        size_t tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }

    /* Link the permuted order into a cycle. */
    for (size_t i = 0; i < n_nodes; i++) {
        nodes[order[i]].next = &nodes[order[(i + 1) % n_nodes]];
    }
    free(order);

    /* The chase itself. The dependent load chain means one outstanding miss
     * at a time, which is what makes this sensitive to the replacement
     * policy rather than to memory-level parallelism. */
    struct node *p = &nodes[0];
    for (size_t i = 0; i < steps; i++) {
        p = p->next;
    }

    /* Consume the result so the compiler cannot delete the loop. */
    printf("chase: ws=%zuKB nodes=%zu steps=%zu final=%ld\n",
           ws_kb, n_nodes, steps, (long)(p - nodes));

    free(nodes);
    return 0;
}
