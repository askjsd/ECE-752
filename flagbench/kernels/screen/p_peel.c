/* Inner loop with a small runtime trip count (3): -fpeel-loops / small-loop handling. */
#include "../common.h"
#define N 8192
static int32_t a[N + 8];
int main(void) {
    uint32_t seed = 55u;
    for (int i = 0; i < N + 8; i++) a[i] = (int32_t)(rng_next(&seed) & 0xfff);
    volatile int vm = 3; int m = vm;
    int64_t s = 0;
    ROI_BEGIN();
    for (int r = 0; r < 30; r++)
        for (int i = 0; i < N; i++) {
            int32_t t = 0;
            for (int j = 0; j < m; j++) t += a[i + j] * (j + 1);
            s += t;
        }
    ROI_END();
    print_checksum((uint64_t)s); return 0;
}
