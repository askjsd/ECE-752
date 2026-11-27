/* Indirect array access: gather A[B[i]], scatter A[B[i]] = v, two-level A[B[C[i]]].
 *
 * A holds uint64 elements; index arrays hold uint32. Index values are uniformly
 * random, so A (and B for gather2) is touched at random lines while the index
 * stream itself is sequential (easy for a stream prefetcher).
 *
 * Software prefetch (--dist=D > 0), after Ainsworth & Jones (CGO'17):
 *   gather/scatter: prefetch &A[B[i+D]]   (B[i+D] is a demand load; B is streamed)
 *   gather2:        prefetch &B[C[i+2D]] and &A[B[C[i+D]]]   (staggered, so the B load
 *                   needed to form A's address was itself prefetched D iterations ago)
 * --pf-index=1 additionally prefetches the sequential index stream 2D ahead.
 * Index arrays are padded so the look-ahead needs no bounds check in the loop. */
#include "pfb.h"

typedef struct { long a_lines, n, dist, loc, pfidx, reps; } ind_p;

static ind_p ind_params(void) {
    ind_p p;
    p.a_lines = cli_long("a-lines", big_lines(), "size of A in lines (default --big-lines)");
    p.n       = cli_long("n", 0, "indirect accesses per repetition (0 = one per line of A)");
    p.reps    = cli_long("reps", 1, "repetitions of the index sweep inside the ROI");
    p.dist    = cli_long("dist", 0, "software prefetch distance in iterations (0 = HW only)");
    p.loc     = cli_long("locality", 3, "prefetch locality hint 0..3 (3=T0 ... 0=NTA)");
    p.pfidx   = cli_long("pf-index", 0, "1 = also prefetch the sequential index stream");
    return p;
}
static void ind_check(ind_p *p) {
    if (p->n == 0) p->n = p->a_lines;
    if (p->dist < 0) die("--dist must be >= 0");
    if (p->dist && !swpf_available()) die("this binary was built with -DPFB_NO_SWPF; use pfbench for --dist>0");
    result_kv("a_lines", "%ld", p->a_lines); result_kv("n", "%ld", p->n); result_kv("reps", "%ld", p->reps);
    result_kv("dist", "%ld", p->dist); result_kv("locality", "%ld", p->loc); result_kv("pf_index", "%ld", p->pfidx);
}
static uint32_t *rand_index(long n, long pad, uint64_t range, uint64_t *s, const char *name) {
    uint32_t *x = pfb_alloc(sizeof(uint32_t) * (size_t)(n + pad), name);
    for (long i = 0; i < n + pad; i++) x[i] = (uint32_t)(xs64(s) % range);
    return x;
}
static uint64_t *alloc_a(long lines) {
    long elems = lines * (G.line / 8);
    if (elems > 0xFFFFFFFFL) die("--a-lines too large for 32-bit indices");
    uint64_t *A = pfb_alloc((size_t)lines * G.line, "A");
    for (long i = 0; i < elems; i++) A[i] = (uint64_t)i;
    return A;
}

int k_gather(void) {
    ind_p p = ind_params();
    cli_done(); ind_check(&p);
    uint64_t s = G.seed;
    uint64_t *A = alloc_a(p.a_lines);
    const long pad = 2 * p.dist;
    uint32_t *B = rand_index(p.n, pad, (uint64_t)p.a_lines * (G.line / 8), &s, "B");
    const long n = p.n, d = p.dist;
    uint64_t sum = 0;
    pfb_roi_begin();
    for (long r = 0; r < p.reps; r++) {
        if (d == 0) {
            for (long i = 0; i < n; i++) sum += LD(&A[B[i]]);
        } else if (!p.pfidx) {
#define G_LOOP(L) for (long i = 0; i < n; i++) { PF(&A[B[i + d]], 0, L); sum += LD(&A[B[i]]); }
            SWPF_SWITCH(p.loc, G_LOOP);
#undef G_LOOP
        } else {
#define G_LOOP(L) for (long i = 0; i < n; i++) { PF(&B[i + 2 * d], 0, L); PF(&A[B[i + d]], 0, L); sum += LD(&A[B[i]]); }
            SWPF_SWITCH(p.loc, G_LOOP);
#undef G_LOOP
        }
    }
    pfb_roi_end((uint64_t)(n * p.reps), sum);
    return 0;
}

int k_scatter(void) {
    ind_p p = ind_params();
    cli_done(); ind_check(&p);
    uint64_t s = G.seed;
    uint64_t *A = alloc_a(p.a_lines);
    const long pad = 2 * p.dist;
    uint32_t *B = rand_index(p.n, pad, (uint64_t)p.a_lines * (G.line / 8), &s, "B");
    const long n = p.n, d = p.dist;
    pfb_roi_begin();
    for (long r = 0; r < p.reps; r++) {
        if (d == 0) {
            for (long i = 0; i < n; i++) ST(&A[B[i]], (uint64_t)i);
        } else if (!p.pfidx) {
#define S_LOOP(L) for (long i = 0; i < n; i++) { PF(&A[B[i + d]], 1, L); ST(&A[B[i]], (uint64_t)i); }
            SWPF_SWITCH(p.loc, S_LOOP);
#undef S_LOOP
        } else {
#define S_LOOP(L) for (long i = 0; i < n; i++) { PF(&B[i + 2 * d], 0, L); PF(&A[B[i + d]], 1, L); ST(&A[B[i]], (uint64_t)i); }
            SWPF_SWITCH(p.loc, S_LOOP);
#undef S_LOOP
        }
    }
    /* checksum outside the ROI */
    uint64_t sum = 0;
    for (long i = 0; i < n; i++) sum += A[B[i]];
    pfb_roi_end((uint64_t)(n * p.reps), sum);
    return 0;
}

int k_gather2(void) {
    ind_p p = ind_params();
    long b_lines = cli_long("b-lines", big_lines(), "size of the middle index array B in lines");
    cli_done(); ind_check(&p);
    result_kv("b_lines", "%ld", b_lines);
    uint64_t s = G.seed;
    uint64_t *A = alloc_a(p.a_lines);
    const long b_elems = b_lines * (G.line / 4);
    uint32_t *B = rand_index(b_elems, 0, (uint64_t)p.a_lines * (G.line / 8), &s, "B");
    const long pad = 2 * p.dist;
    uint32_t *C = rand_index(p.n, pad, (uint64_t)b_elems, &s, "C");
    const long n = p.n, d = p.dist;
    uint64_t sum = 0;
    pfb_roi_begin();
    for (long r = 0; r < p.reps; r++) {
        if (d == 0) {
            for (long i = 0; i < n; i++) sum += LD(&A[LD32(&B[C[i]])]);
        } else {
#define G2_LOOP(L) for (long i = 0; i < n; i++) {                                     \
                if (p.pfidx) PF(&C[i + 2 * d], 0, L);                                   \
                PF(&B[C[i + 2 * d]], 0, L);                                             \
                PF(&A[B[C[i + d]]], 0, L);                                              \
                sum += LD(&A[LD32(&B[C[i]])]); }
            SWPF_SWITCH(p.loc, G2_LOOP);
#undef G2_LOOP
        }
    }
    pfb_roi_end((uint64_t)(n * p.reps), sum);
    return 0;
}
