/* Loop whose body switches on the induction variable (i < k): -fsplit-loops splits it into two simple loops. */
#include "../common.h"
#define N 4096
static int32_t a[N], b[N];
int main(void) {
    uint32_t seed = 22u;
    for (int i = 0; i < N; i++) { a[i] = (int32_t)(rng_next(&seed) & 1023); b[i] = (int32_t)(rng_next(&seed) & 1023); }
    volatile int vk = 1000 + (int)(rng_next(&seed) & 1023);
    int k = vk;
    int64_t x = 0, y = 0;
    ROI_BEGIN();
    for (int r = 0; r < 60; r++)
        for (int i = 0; i < N; i++) {
            if (i < k) x += a[i] * 3 + (b[i] >> 1);
            else       y += (a[i] ^ b[i]) + 7;
        }
    ROI_END();
    print_checksum((uint64_t)x * 1000003u ^ (uint64_t)y); return 0;
}
