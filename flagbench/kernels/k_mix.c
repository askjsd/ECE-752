/* Composite kernel: 8 phases, each sensitive to specific GCC flags, in ONE region of interest.
 * Goal: a flag space with (almost) no don't-cares, where the optimum needs specific flags ON and
 * others OFF, and where some flags conflict across phases (so the best setting is a trade-off).
 * Flags that matter (see flagbench/README.md for measured effects):
 *   stencil    : tree-loop-vectorize, tree-slp-vectorize, predictive-commoning, ivopts, unroll-loops
 *   reduce     : unroll-loops, unroll-all-loops, tree-loop-vectorize, schedule-insns, ivopts
 *   helper     : tracer (good), unroll-all-loops (bad)
 *   crc        : schedule-insns (bad)
 *   unswitch   : unswitch-loops (bad when on), loop-interchange
 *   ipacp      : ipa-cp-clone (good), tree-loop-vectorize, unroll-loops
 *   interchange: loop-interchange, unroll-all-loops
 *   stride     : version-loops-for-strides
 */
#include "common.h"
#include <string.h>

/* ---- phase data (initialised before the ROI) ---- */
#define SW 64
#define SH 64
static float sa[SH][SW], sb[SH][SW];
#define RN 40000
static int32_t rarr[RN];
#define HN 4096
static uint32_t hdata[HN];
#define CL 10000
static uint8_t cbuf[CL];
#define UN 2048
static float ua[UN], ub[UN];
#define PN 4096
static int32_t pv[PN];
#define IR 96
#define IC 96
static int32_t im[IR][IC], io[IR][IC];
#define TN 4096
static float ta[TN * 2], tb[TN * 2];

/* ---- phases ---- */
__attribute__((noinline)) static void ph_stencil(void) {
    for (int it = 0; it < 45; it++) {
        for (int i = 1; i < SH - 1; i++)
            for (int j = 1; j < SW - 1; j++)
                sb[i][j] = 0.25f * (sa[i-1][j] + sa[i+1][j] + sa[i][j-1] + sa[i][j+1]);
        for (int i = 1; i < SH - 1; i++)
            for (int j = 1; j < SW - 1; j++)
                sa[i][j] = sb[i][j];
    }
}
static int64_t g_total;
__attribute__((noinline)) static void ph_reduce(void) {
    int64_t total = 0;
    for (int i = 0; i < RN; i++) total += rarr[i];
    int32_t run = 0;
    for (int i = 0; i < RN; i++) { run += rarr[i]; rarr[i] = run; }
    for (int i = 0; i < RN; i++) total ^= rarr[i];
    g_total = total;
}
static uint32_t mix(uint32_t v, uint32_t k) {
    v ^= v >> 16; v *= 0x85ebca6bu; v ^= k; v ^= v >> 13; v *= 0xc2b2ae35u; return v ^ (v >> 16);
}
static uint32_t clampsum(uint32_t a, uint32_t b) { uint32_t s = a + b; return s < a ? 0xffffffffu : s; }
static uint32_t g_acc;
__attribute__((noinline)) static void ph_helper(void) {
    uint32_t acc = 0;
    for (int r = 0; r < 8; r++)
        for (int i = 0; i < HN; i++) acc = clampsum(acc, mix(hdata[i], (uint32_t)r));
    g_acc = acc;
}
static uint32_t g_crc;
__attribute__((noinline)) static void ph_crc(void) {
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < CL; i++) {
        crc ^= cbuf[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    g_crc = ~crc;
}
static int g_mode;
__attribute__((noinline)) static void ph_unswitch(void) {
    int mode = g_mode;
    for (int r = 0; r < 60; r++)
        for (int i = 0; i < UN; i++) {
            if (mode) ua[i] = ua[i] * 0.5f + ub[i];
            else      ua[i] = ua[i] - ub[i] * 0.25f;
        }
}
__attribute__((noinline)) static int64_t work(int32_t *p, int n, int stride, int shift) {
    int64_t s = 0;
    for (int i = 0; i < n; i++) { int32_t t = p[i * stride]; t = (t << shift) + (t >> 2); p[i * stride] = t; s += t; }
    return s;
}
static int64_t g_ipa;
__attribute__((noinline)) static void ph_ipacp(void) {
    int64_t acc = 0;
    for (int r = 0; r < 30; r++) {
        acc += work(pv, PN, 1, 1);
        acc += work(pv, PN / 2, 2, 2);
        acc ^= work(pv, PN / 4, 4, 1);
    }
    g_ipa = acc;
}
__attribute__((noinline)) static void ph_interchange(void) {
    for (int r = 0; r < 24; r++)
        for (int j = 0; j < IC; j++)
            for (int i = 0; i < IR; i++)
                io[i][j] = io[i][j] + im[i][j] * 3 + (im[i][j] >> 1);
}
static int g_stride;
__attribute__((noinline)) static void axpy(float *x, const float *y, int n, int s) {
    for (int i = 0; i < n; i++) x[i * s] += 0.5f * y[i * s];
}
__attribute__((noinline)) static void ph_stride(void) {
    int s = g_stride;
    for (int r = 0; r < 100; r++) axpy(ta, tb, TN, s);
}

static uint64_t fnv(uint64_t c, uint64_t v) { return (c ^ v) * 1099511628211ull; }
int main(void) {
    uint32_t seed = 0xC0FFEEu;
    for (int i = 0; i < SH; i++) for (int j = 0; j < SW; j++) sa[i][j] = (float)(rng_next(&seed) & 1023u) * (1.0f / 64.0f);
    memcpy(sb, sa, sizeof sa);
    for (int i = 0; i < RN; i++) rarr[i] = (int32_t)(rng_next(&seed) & 0xffff) - 32768;
    for (int i = 0; i < HN; i++) hdata[i] = rng_next(&seed);
    for (int i = 0; i < CL; i++) cbuf[i] = (uint8_t)rng_next(&seed);
    for (int i = 0; i < UN; i++) { ua[i] = (float)(rng_next(&seed) & 255); ub[i] = (float)(rng_next(&seed) & 255); }
    for (int i = 0; i < PN; i++) pv[i] = (int32_t)(rng_next(&seed) & 0xffff);
    for (int i = 0; i < IR; i++) for (int j = 0; j < IC; j++) im[i][j] = (int32_t)(rng_next(&seed) & 0xff);
    for (int i = 0; i < TN * 2; i++) { ta[i] = (float)(rng_next(&seed) & 255); tb[i] = (float)(rng_next(&seed) & 255); }
    volatile int vm = 1, vs = 1; g_mode = vm; g_stride = vs;

    ROI_BEGIN();
    ph_stencil(); ph_reduce(); ph_helper(); ph_crc(); ph_unswitch(); ph_ipacp(); ph_interchange(); ph_stride();
    ROI_END();

    uint64_t c = 1469598103934665603ull;
    for (int i = 0; i < SH; i++) for (int j = 0; j < SW; j++) { union { float f; uint32_t u; } v = { sa[i][j] }; c = fnv(c, v.u); }
    c = fnv(c, (uint64_t)g_total); c = fnv(c, g_acc); c = fnv(c, g_crc);
    for (int i = 0; i < UN; i++) { union { float f; uint32_t u; } v = { ua[i] }; c = fnv(c, v.u); }
    c = fnv(c, (uint64_t)g_ipa);
    for (int i = 0; i < IR; i++) for (int j = 0; j < IC; j++) c = fnv(c, (uint32_t)io[i][j]);
    for (int i = 0; i < TN * 2; i++) { union { float f; uint32_t u; } v = { ta[i] }; c = fnv(c, v.u); }
    print_checksum(c);
    return 0;
}
