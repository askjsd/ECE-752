/* Runtime-stride loop: -fversion-loops-for-strides versions the loop for stride==1 (then vectorizable). */
#include "../common.h"
#define N 4096
static float a[N * 2], b[N * 2];
static __attribute__((noinline)) void axpy(float *x, const float *y, int n, int s) {
    for (int i = 0; i < n; i++) x[i * s] += 0.5f * y[i * s];
}
int main(void) {
    uint32_t seed = 77u;
    for (int i = 0; i < N * 2; i++) { a[i] = (float)(rng_next(&seed) & 255); b[i] = (float)(rng_next(&seed) & 255); }
    volatile int vs = 1; int s = vs;
    ROI_BEGIN();
    for (int r = 0; r < 60; r++) axpy(a, b, N, s);
    ROI_END();
    uint64_t c = 0; for (int i = 0; i < N * 2; i++) { union { float f; uint32_t u; } v = { a[i] }; c = c * 31 + v.u; }
    print_checksum(c); return 0;
}
