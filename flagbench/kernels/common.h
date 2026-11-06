/* Shared helpers for flagbench kernels. Only the work between ROI_BEGIN()/ROI_END()
 * is measured; inputs are generated in-program from a fixed seed (no file I/O). */
#ifndef FLAGBENCH_COMMON_H
#define FLAGBENCH_COMMON_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <gem5/m5ops.h>

#ifdef NO_M5OPS /* reference/native builds without m5ops */
#define ROI_BEGIN() ((void)0)
#define ROI_END()   ((void)0)
#else
#define ROI_BEGIN() m5_work_begin(0, 0)
#define ROI_END()   m5_work_end(0, 0)
#endif

/* xorshift32, fixed seed -> deterministic inputs */
static inline uint32_t rng_next(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}
static inline void print_checksum(uint64_t c) { printf("CHECKSUM=%llu\n", (unsigned long long)c); }
#endif
