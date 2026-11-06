/* Medium-size non-inline-declared function called from several hot sites: -finline-functions. */
#include "../common.h"
#define N 2048
static uint32_t tab[256], in[N];
static uint32_t step(uint32_t h, uint32_t x) {
    h ^= x; h = (h << 5) | (h >> 27); h += tab[h & 255];
    h ^= (x >> 3) * 0x9e3779b1u; h = (h << 7) | (h >> 25); h += tab[(h >> 8) & 255];
    h *= 0x85ebca6bu; h ^= h >> 15; h += tab[(h >> 16) & 255];
    return h ^ (x << 11);
}
int main(void) {
    uint32_t seed = 44u;
    for (int i = 0; i < 256; i++) tab[i] = rng_next(&seed);
    for (int i = 0; i < N; i++) in[i] = rng_next(&seed);
    uint32_t h = 1, g = 2;
    ROI_BEGIN();
    for (int r = 0; r < 24; r++)
        for (int i = 0; i < N; i++) { h = step(h, in[i]); g = step(g, in[N - 1 - i] + h); }
    ROI_END();
    print_checksum(((uint64_t)h << 32) | g); return 0;
}
