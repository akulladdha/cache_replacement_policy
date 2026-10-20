/*
 * phases.c -- alternate between a working set that thrashes the LLC and one
 * that fits in it.
 *
 * Usage: phases <big_kb> <small_kb> [steps_per_phase] [phases]
 *
 * Each phase is a pointer chase over its own randomly permuted list, built
 * the same way as in chase.c. Even phases chase the big list, odd phases the
 * small one.
 *
 * The two phases want opposite insertion policies:
 *
 *   - The big list is a little larger than the cache. That is the regime
 *     where SRRIP thrashes and BRRIP wins, because BRRIP keeps a stable
 *     fraction of the list resident instead of cycling all of it through.
 *   - The small list fits. SRRIP makes it resident within a lap. BRRIP is
 *     slow to do so, because it inserts almost every new line as the next
 *     eviction candidate, so a working set that has just changed takes many
 *     laps to build back up.
 *
 * That was the design intent: a workload where any static choice is wrong
 * for half the run, so a policy that switches at run time could beat both.
 *
 * The measurement did not bear out the second half. BRRIP rebuilds the
 * small list quickly enough that it loses almost nothing in the fitting
 * phase, so BRRIP wins outright at every phase length and DRRIP, correctly,
 * runs BRRIP in about 95 percent of follower insertions. Kept in the sweep
 * because it is still a useful result: a phase change alone does not create
 * a regime where SRRIP wins. The fitting phase has to contain something
 * BRRIP is actually bad at, such as mixed.c's hot set under a streaming
 * scan. NOTES.md records an exploratory run of exactly that.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define LINE_BYTES 64

struct node {
    struct node *next;
    char pad[LINE_BYTES - sizeof(struct node *)];
};

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

static uint64_t
rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* Allocate ws_kb of line-sized nodes and link them into one random cycle
 * with a Sattolo shuffle, exactly as chase.c does. */
static struct node *
build_list(size_t ws_kb)
{
    size_t n_nodes = (ws_kb * 1024) / sizeof(struct node);
    if (n_nodes < 2) {
        n_nodes = 2;
    }

    struct node *nodes = (struct node *)malloc(n_nodes * sizeof(struct node));
    size_t *order = (size_t *)malloc(n_nodes * sizeof(size_t));
    if (nodes == NULL || order == NULL) {
        fprintf(stderr, "phases: allocation of %zu nodes failed\n", n_nodes);
        exit(1);
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
    for (size_t i = 0; i < n_nodes; i++) {
        nodes[order[i]].next = &nodes[order[(i + 1) % n_nodes]];
    }
    free(order);
    return nodes;
}

int
main(int argc, char **argv)
{
    size_t big_kb = 1280;
    size_t small_kb = 768;
    size_t steps = 400000;
    size_t phases = 8;

    if (argc > 1) {
        big_kb = (size_t)strtoul(argv[1], NULL, 10);
    }
    if (argc > 2) {
        small_kb = (size_t)strtoul(argv[2], NULL, 10);
    }
    if (argc > 3) {
        steps = (size_t)strtoul(argv[3], NULL, 10);
    }
    if (argc > 4) {
        phases = (size_t)strtoul(argv[4], NULL, 10);
    }

    struct node *big = build_list(big_kb);
    struct node *small = build_list(small_kb);

    /* Each list keeps its own position across phases, so returning to a
     * list resumes the same cycle rather than restarting it. */
    struct node *p_big = big;
    struct node *p_small = small;

    for (size_t ph = 0; ph < phases; ph++) {
        if (ph % 2 == 0) {
            for (size_t i = 0; i < steps; i++) {
                p_big = p_big->next;
            }
        } else {
            for (size_t i = 0; i < steps; i++) {
                p_small = p_small->next;
            }
        }
    }

    printf("phases: big=%zuKB small=%zuKB steps=%zu phases=%zu final=%ld/%ld\n",
           big_kb, small_kb, steps, phases,
           (long)(p_big - big), (long)(p_small - small));

    free(big);
    free(small);
    return 0;
}
