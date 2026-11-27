/* Pointer chasing on a randomized linked list.
 *
 *   chase   plain traversal, HW prefetching only. The list is one random cycle, so
 *           there is no address pattern: only a prefetcher that remembers the chain
 *           (a correlation / Markov prefetcher) can help, and only after it has seen
 *           the chain once. The ROI therefore holds two full traversals: the first
 *           trains the prefetcher (cover it with the simulator's warm-up, about
 *           --nodes x instructions-per-hop), the second is the measurement. How many
 *           next nodes it prefetches (its degree) and how far ahead it starts (its
 *           distance) are prefetcher settings, e.g. knobbed source=markov, level=L.
 *
 *   jump    the same list, but every node also stores pointers to upcoming nodes,
 *           so software can prefetch nodes it has not reached yet:
 *             --dist=d  the first prefetched node is d hops ahead (distance)
 *             --k=k     step size: how many next nodes are prefetched (degree)
 *           Every k hops the loop prefetches the k nodes d .. d+k-1 hops ahead, so
 *           each node is prefetched exactly once and k prefetches are in flight
 *           together. --swpf=0 walks the same nodes with no prefetching (baseline).
 *           The traversal itself still follows ->next: the stored pointers do not
 *           shorten the serial chain, they only let prefetches run ahead of it. */
#include "pfb.h"
#include <string.h>

typedef struct node { struct node *next; uint64_t val; struct node *ahead[]; } node_t;

/* n nodes of nb bytes linked into one random cycle (Sattolo). Returns the nodes in
 * traversal order through *order (caller frees). */
static uint8_t *build_list(long n, long nb, uint32_t **order) {
    uint8_t *mem = pfb_alloc((size_t)n * nb, "list");
#define NODE(i) ((node_t *)(mem + (size_t)(i) * nb))
    uint64_t s = G.seed;
    uint32_t *p = malloc(sizeof(uint32_t) * n);
    for (long i = 0; i < n; i++) p[i] = (uint32_t)i;
    for (long i = n - 1; i > 0; i--) {                  /* Sattolo: j in [0, i) */
        long j = (long)(xs64(&s) % (uint64_t)i);
        uint32_t t = p[i]; p[i] = p[j]; p[j] = t;
    }
    for (long i = 0; i < n; i++) { NODE(p[i])->next = NODE(p[(i + 1) % n]); NODE(i)->val = (uint64_t)i; }
#undef NODE
    *order = p;
    return mem;
}

static void list_params(long *n, long *nb, long *steps, long *warm, long nb_min) {
    *n     = cli_long("nodes", big_lines(), "number of nodes (default --big-lines)");
    *nb    = cli_long("node-bytes", G.line, "bytes per node; one line by default");
    *steps = cli_long("steps", 0, "hops in the ROI (0 = two full traversals)");
    *warm  = cli_long("warmup", 0, "hops before the ROI (not traced)");
    if (*n < 2) die("--nodes must be >= 2");
    if (*steps == 0) *steps = 2 * *n;
    if (*warm < 0) die("--warmup must be >= 0");
    if (*nb % 8 || *nb < nb_min) die("--node-bytes must be a multiple of 8 and >= %ld here", nb_min);
}

int k_chase(void) {
    long n, nb, steps, warm;
    list_params(&n, &nb, &steps, &warm, (long)sizeof(node_t));
    cli_done();
    uint32_t *order;
    uint8_t *mem = build_list(n, nb, &order);
    node_t *p = (node_t *)(mem + (size_t)order[0] * nb);
    free(order);
    result_kv("nodes", "%ld", n); result_kv("node_bytes", "%ld", nb); result_kv("warmup", "%ld", warm);

    uint64_t sum = 0;
    for (long i = 0; i < warm; i++) { sum += p->val; p = p->next; }
    pfb_roi_begin();
    for (long i = 0; i < steps; i++) {
        sum += p->val;
        p = p->next;                       /* serial dependency: next address = loaded value */
    }
    pfb_roi_end((uint64_t)steps, sum);
    return 0;
}

int k_jump(void) {
    long d   = cli_long("dist", 8, "hops ahead of the first prefetched node (prefetch distance)");
    long k   = cli_long("k", 1, "step size: how many next nodes to prefetch, every k hops (degree)");
    long sw  = cli_long("swpf", 1, "1 = software prefetch, 0 = same nodes, no prefetching");
    long loc = cli_long("locality", 3, "prefetch locality hint 0..3 (3=T0 ... 0=NTA)");
    long n, nb, steps, warm;
    list_params(&n, &nb, &steps, &warm, (long)sizeof(node_t) + 8);
    cli_done();
    if (d < 1 || k < 1) die("--dist and --k must be >= 1");
    long kmax = (nb - (long)sizeof(node_t)) / 8;
    if (k > kmax) die("--k=%ld needs %ld-byte nodes; pass --node-bytes=%ld", k,
                      (long)sizeof(node_t) + 8 * k, ((long)sizeof(node_t) + 8 * k + G.line - 1) / G.line * G.line);
    if (sw && !swpf_available()) die("this binary was built with -DPFB_NO_SWPF; use pfbench for --swpf=1");
    uint32_t *order;
    uint8_t *mem = build_list(n, nb, &order);
#define NODE(i) ((node_t *)(mem + (size_t)(i) * nb))
    for (long i = 0; i < n; i++)                        /* ahead[j] = node d+j hops ahead */
        for (long j = 0; j < k; j++) NODE(order[i])->ahead[j] = NODE(order[(i + d + j) % n]);
    node_t *p = NODE(order[0]);
#undef NODE
    free(order);
    result_kv("nodes", "%ld", n); result_kv("node_bytes", "%ld", nb); result_kv("warmup", "%ld", warm);
    result_kv("dist", "%ld", d); result_kv("k", "%ld", k); result_kv("swpf", "%ld", sw); result_kv("locality", "%ld", loc);

    uint64_t sum = 0;
    for (long i = 0; i < warm; i++) { sum += p->val; p = p->next; }
    pfb_roi_begin();
    if (!sw) {
        for (long i = 0; i < steps; i++) { sum += p->val; p = p->next; }
    } else {
        /* the batch counter starts at 0, so the first hop issues a batch */
#define JUMP_LOOP(L) { long c = 0;                                                    \
            for (long i = 0; i < steps; i++) {                                        \
                if (c == 0) { for (long j = 0; j < k; j++) PF(p->ahead[j], 0, L); c = k; } \
                c--;                                                                  \
                sum += p->val; p = p->next; } }
        SWPF_SWITCH(loc, JUMP_LOOP);
#undef JUMP_LOOP
    }
    pfb_roi_end((uint64_t)steps, sum);
    return 0;
}
