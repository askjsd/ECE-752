/* Timing shim for CARBS benchmarks (not shipped in the original repo).
 * Benchmarks call _entering()/_exiting() around the kernel; compiler_env.py
 * parses a "timing: <start_ns> <end_ns>" line from stderr. */
#include <stdio.h>
#include <time.h>

static long long start_ns;

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void _entering(void) { start_ns = now_ns(); }
void _exiting(void)  { fprintf(stderr, "timing: %lld %lld\n", start_ns, now_ns()); }
