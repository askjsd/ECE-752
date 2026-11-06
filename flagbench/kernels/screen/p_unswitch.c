/* Loop with a loop-invariant runtime branch: -funswitch-loops hoists it (and enables vectorizing each copy). */
#include "../common.h"
#define N 2048
static float a[N], b[N];
int main(void) {
    uint32_t seed = 11u;
    for (int i = 0; i < N; i++) { a[i] = (float)(rng_next(&seed) & 255); b[i] = (float)(rng_next(&seed) & 255); }
    volatile int vmode = (int)(rng_next(&seed) & 1) | 1; /* runtime value, loop invariant */
    int mode = vmode;
    ROI_BEGIN();
    for (int r = 0; r < 60; r++)
        for (int i = 0; i < N; i++) {
            if (mode) a[i] = a[i] * 0.5f + b[i];
            else      a[i] = a[i] - b[i] * 0.25f;
        }
    ROI_END();
    uint64_t c = 0; for (int i = 0; i < N; i++) { union { float f; uint32_t u; } v = { a[i] }; c = c * 31 + v.u; }
    print_checksum(c); return 0;
}
