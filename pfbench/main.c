/* pfbench driver: pfbench <kernel> [--option=value ...] */
#include "pfb.h"
#include <string.h>

void pfb_common_setup(void);

static const kernel_t KERNELS[] = {
    {"chase",           k_chase,           "pointer",   "random linked-list traversal (HW prefetch only)"},
    {"jump",            k_jump,            "pointer",   "random linked list, SW prefetch of the next k nodes, d hops ahead"},
    {"gather",          k_gather,          "indirect",  "val += A[B[i]] (+ optional SW prefetch)"},
    {"scatter",         k_scatter,         "indirect",  "A[B[i]] = val (+ optional SW prefetchw)"},
    {"gather2",         k_gather2,         "indirect",  "val += A[B[C[i]]] (+ optional staggered SW prefetch)"},
    {"stride",          k_stride,          "stride",    "constant stride (control)"},
    {"stride_overflow", k_stride_overflow, "stride",    "stride = 2^n + alias, overflows an n-bit stride field"},
    {"stride_alt",      k_stride_alt,      "stride",    "alternating stride magnitudes (+s1, +s2, ...) from one load PC"},
    {"stride_zigzag",   k_stride_zigzag,   "stride",    "alternating direction (+fwd, -back, ...) with net forward progress"},
    {"bursts",          k_bursts,          "stride",    "short strided bursts separated by random single-use jumps"},
    {"stream",          k_stream,          "stream",    "forward sequential stream (control)"},
    {"stream_rev",      k_stream_rev,      "stream",    "backward sequential stream (descending addresses)"},
    {"stream_holes",    k_stream_holes,    "stream",    "sequential stream that skips hole lines (periodic or random)"},
    {"phase",           k_phase,           "pollution", "random -> stream -> random ... phases (FDP throttling test)"},
    {"pollute",         k_pollute,         "pollution", "hot set in L1/L2 + single-use cold traffic (pollution test)"},
    {"late",            k_late,            "timeliness","memory-bound stream with minimal compute (distance tuning)"},
    {"filllevel",       k_filllevel,       "placement", "L1/L2/L3-reuse regions + one-shot region (fill-level test)"},
};
#define NK (int)(sizeof KERNELS / sizeof KERNELS[0])

static void usage(const char *prog) {
    printf("usage: %s <kernel> [--option=value ...]   (%s <kernel> --help for its options)\n\nkernels:\n", prog, prog);
    for (int i = 0; i < NK; i++) printf("  %-16s %-10s %s\n", KERNELS[i].name, KERNELS[i].group, KERNELS[i].desc);
    printf("\ncommon options: --line=64 --l1-lines=512 --l2-lines=4096 --l3-lines=16384\n"
           "                --big-lines=16*L1 --seed=1 --quiet=0 --perf=auto|off --pmu-raw=name=0xcfg,...\n");
}

int main(int argc, char **argv) {
    if (argc < 2 || argv[1][0] == '-') { usage(argv[0]); return argc < 2 ? 2 : 0; }
    const kernel_t *k = NULL;
    for (int i = 0; i < NK; i++) if (!strcmp(argv[1], KERNELS[i].name)) k = &KERNELS[i];
    if (!k) { fprintf(stderr, "unknown kernel '%s'\n\n", argv[1]); usage(argv[0]); return 2; }

    G.kernel = argv[1];
    cli_init(argc, argv);
    G.line  = cli_long("line", 64, "cache-line size in bytes");
    G.l1    = cli_long("l1-lines", 1L << 9, "L1D capacity in lines (sizes default working sets)");
    G.l2    = cli_long("l2-lines", 1L << 12, "L2 capacity in lines");
    G.l3    = cli_long("l3-lines", 1L << 14, "L3/LLC capacity in lines");
    G.seed  = (uint64_t)cli_long("seed", 1, "RNG seed (inputs are deterministic for a seed)");
    G.quiet = (int)cli_long("quiet", 0, "1 = suppress # info and REGION lines");
    G.big   = cli_long("big-lines", 16 * G.l1, "size of 'large' regions in lines (default 16x L1)");
    if (G.line < 8 || (G.line & (G.line - 1))) die("--line must be a power of two >= 8");
    if (G.seed == 0) G.seed = 1;
    pfb_common_setup();
    result_kv("line", "%ld", G.line);
    return k->fn();
}
