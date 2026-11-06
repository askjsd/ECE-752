/* Callee called with different constant arguments: -fipa-cp-clone specializes it per call site
 * (stride==1 clone becomes vectorizable / simpler addressing). */
#include "../common.h"
#define N 4096
static int32_t v[N];
__attribute__((noinline)) static int64_t work(int32_t *p, int n, int stride, int shift) {
    int64_t s = 0;
    for (int i = 0; i < n; i++) { int32_t t = p[i * stride]; t = (t << shift) + (t >> 2); p[i * stride] = t; s += t; }
    return s;
}
int main(void) {
    uint32_t seed = 33u;
    for (int i = 0; i < N; i++) v[i] = (int32_t)(rng_next(&seed) & 0xffff);
    int64_t acc = 0;
    ROI_BEGIN();
    for (int r = 0; r < 40; r++) {
        acc += work(v, N, 1, 1);
        acc += work(v, N / 2, 2, 2);
        acc ^= work(v, N / 4, 4, 1);
    }
    ROI_END();
    print_checksum((uint64_t)acc); return 0;
}
