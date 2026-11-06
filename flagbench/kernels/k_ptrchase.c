/* Pointer chasing over a random cyclic list larger than L2: memory-latency bound (control kernel,
 * expected to be nearly flag-insensitive). */
#include "common.h"
#define N 16384
typedef struct node { struct node *next; uint32_t val; char pad[52]; } node_t; /* 64 B */
static node_t nodes[N];
static uint32_t perm[N];
int main(void) {
    uint32_t seed = 2468u;
    for (uint32_t i = 0; i < N; i++) perm[i] = i;
    for (uint32_t i = N - 1; i > 0; i--) { /* Fisher-Yates */
        uint32_t j = rng_next(&seed) % (i + 1);
        uint32_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
    for (uint32_t i = 0; i < N; i++) {
        nodes[perm[i]].next = &nodes[perm[(i + 1) % N]];
        nodes[perm[i]].val = rng_next(&seed);
    }
    node_t *p = &nodes[perm[0]];
    uint64_t sum = 0;
    ROI_BEGIN();
    for (int s = 0; s < 300000; s++) {
        sum += p->val;
        p = p->next;
    }
    ROI_END();
    print_checksum(sum ^ (uint64_t)(p - nodes));
    return 0;
}
