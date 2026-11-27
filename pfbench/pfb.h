/* pfbench: microbenchmarks for hardware/software prefetcher evaluation.
 *
 * Shared definitions: target cache geometry, CLI options, RNG / single-use
 * permutations, aligned allocation, access macros, software-prefetch macro and
 * the ROI (region of interest) wrapper.
 */
#ifndef PFB_H
#define PFB_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- global configuration (set from the CLI in main.c) -------------------------- */
typedef struct {
    const char *kernel;
    long line;          /* cache-line size in bytes (default 64)           */
    long l1, l2, l3;    /* capacities in lines (default 2^9, 2^12, 2^14)   */
    uint64_t seed;
    int quiet;
    long big;           /* "large" region in lines (default 16x L1)       */
} pfb_cfg;
extern pfb_cfg G;

/* ---- CLI: every option is --name=value. Unknown options are an error. ----------- */
void        cli_init(int argc, char **argv);
long        cli_long(const char *name, long def, const char *help);
const char *cli_str(const char *name, const char *def, const char *help);
/* Call after all of a kernel's options are read: prints --help and exits, or
 * rejects unknown/misspelled options. */
void        cli_done(void);
int         cli_help_requested(void);

/* ---- diagnostics ------------------------------------------------------------------ */
#define info(...) do { if (!G.quiet) { fprintf(stderr, "# " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define warn(...) do { fprintf(stderr, "WARNING: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define die(...)  do { fprintf(stderr, "error: " __VA_ARGS__); fputc('\n', stderr); exit(2); } while (0)

/* ---- RNG and single-use permutations --------------------------------------------- */
static inline uint64_t xs64(uint64_t *s) {          /* xorshift64* */
    uint64_t x = *s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1DULL;
}
static inline int log2_ceil(uint64_t n) { int b = 0; while ((1ULL << b) < n) b++; return b; }

/* Bijection on [0, 2^bits): odd multiplies and xor-right-shifts are each bijective
 * modulo 2^bits. Used to visit lines in a random order with each line touched
 * exactly once (true single-use addresses, no index array needed). */
static inline uint64_t perm_bits(uint64_t x, int bits, uint64_t key) {
    if (bits == 0) return 0;
    const uint64_t m = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1);
    const int s = bits / 2 + 1;
    x = (x + key) & m;
    x = (x * 0x9E3779B97F4A7C15ULL) & m; x ^= x >> s;
    x = (x * 0xBF58476D1CE4E5B9ULL) & m; x ^= x >> s;
    x = (x * 0x94D049BB133111EBULL) & m; x ^= x >> s;
    return x;
}
/* Bijection on [0, n) by cycle-walking (expected < 2 rounds). */
static inline uint64_t perm_n(uint64_t i, uint64_t n, int bits, uint64_t key) {
    uint64_t x = i;
    do { x = perm_bits(x, bits, key); } while (x >= n);
    return x;
}

/* ---- memory ------------------------------------------------------------------------ */
/* 2 MiB-aligned, pre-touched (no first-touch faults inside the ROI). Prints a REGION line
 * with the address range so simulator misses can be binned per region. */
void *pfb_alloc(size_t bytes, const char *name);

/* Pattern-critical accesses go through volatile so the compiler keeps exactly one
 * scalar access per source access (no vectorisation, hoisting or merging). */
#define LD(p)     (*(const volatile uint64_t *)(p))
#define LD32(p)   (*(const volatile uint32_t *)(p))
#define ST(p, v)  (*(volatile uint64_t *)(p) = (v))

/* Compiler barrier on a value (keeps "work" loops from being folded). */
#define KEEP(x)   __asm__ volatile("" : "+r"(x))

/* ---- software prefetch -------------------------------------------------------------
 * __builtin_prefetch needs a constant locality, so kernels expand their loop once
 * per locality with SWPF_SWITCH. Locality: 3=T0 (all levels), 2=T1 (L2 and below),
 * 1=T2 (LLC), 0=NTA (non-temporal). Built with -DPFB_NO_SWPF the prefetch compiles
 * to nothing (the pfbench_noswpf binary). */
#ifdef PFB_NO_SWPF
#define PF(addr, rw, loc) ((void)(addr))
#else
#define PF(addr, rw, loc) __builtin_prefetch((const void *)(addr), (rw), (loc))
#endif
#define SWPF_SWITCH(loc, LOOP) do { switch (loc) { \
        case 0: LOOP(0); break; case 1: LOOP(1); break;   \
        case 2: LOOP(2); break; default: LOOP(3); break; } } while (0)
int swpf_available(void);

/* ---- ROI ---------------------------------------------------------------------------
 * pfb_roi_begin / pfb_roi_end are real (noinline) functions so a tracer can start at
 * the symbol, e.g.  trace_dr.sh -S pfb_roi_begin ... . Inside: m5 work markers when
 * built with -DPFB_M5, wall-clock time, and Linux perf counters when permitted.
 * pfb_roi_end prints one machine-readable "RESULT key=value ..." line. */
void pfb_roi_begin(void);
void pfb_roi_end(uint64_t accesses, uint64_t checksum);
extern volatile uint64_t pfb_sink;

/* "key=value" pairs appended to the RESULT line (kernel parameters). */
void result_kv(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ---- kernels ------------------------------------------------------------------------ */
typedef int (*kernel_fn)(void);
typedef struct { const char *name; kernel_fn fn; const char *group; const char *desc; } kernel_t;

#define K(n) int k_##n(void);
K(chase) K(jump) K(gather) K(scatter) K(gather2)
K(stride) K(stride_overflow) K(stride_alt) K(stride_zigzag) K(bursts)
K(stream) K(stream_rev) K(stream_holes)
K(phase) K(pollute) K(late) K(filllevel)
#undef K

/* Default size of a "large" region in lines: 16x L1 (8192 lines, 512 KiB). Every
 * kernel except filllevel is an L1-only study (run it with the L1-only ChampSim
 * config), so "large" only has to be far bigger than L1. */
static inline long big_lines(void) { return G.big; }
#endif
