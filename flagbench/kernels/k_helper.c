/* Hot loop calling a small helper from the same TU: inlining / IPA sensitive. */
#include "common.h"
#define N 4096
static uint32_t data[N];
static uint32_t mix(uint32_t v, uint32_t k) {
    v ^= v >> 16; v *= 0x85ebca6bu; v ^= k;
    v ^= v >> 13; v *= 0xc2b2ae35u;
    return v ^ (v >> 16);
}
static uint32_t clampsum(uint32_t a, uint32_t b) { uint32_t s = a + b; return s < a ? 0xffffffffu : s; }
int main(void) {
    uint32_t seed = 424242u;
    for (int i = 0; i < N; i++) data[i] = rng_next(&seed);
    uint32_t acc = 0;
    ROI_BEGIN();
    for (int r = 0; r < 24; r++)
        for (int i = 0; i < N; i++)
            acc = clampsum(acc, mix(data[i], (uint32_t)r));
    ROI_END();
    print_checksum(acc);
    return 0;
}
