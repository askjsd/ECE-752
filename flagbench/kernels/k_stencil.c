/* Dense 5-point Jacobi stencil on a float grid: vectorization / unrolling / interchange sensitive. */
#include "common.h"
#include <string.h>
#define W 64
#define H 64
#define ITERS 140
static float a[H][W], b[H][W];
int main(void) {
    uint32_t seed = 12345u;
    for (int i = 0; i < H; i++)
        for (int j = 0; j < W; j++)
            a[i][j] = (float)(rng_next(&seed) & 1023u) * (1.0f / 64.0f);
    memcpy(b, a, sizeof a);
    ROI_BEGIN();
    for (int it = 0; it < ITERS; it++) {
        for (int i = 1; i < H - 1; i++)
            for (int j = 1; j < W - 1; j++)
                b[i][j] = 0.25f * (a[i-1][j] + a[i+1][j] + a[i][j-1] + a[i][j+1]);
        for (int i = 1; i < H - 1; i++)
            for (int j = 1; j < W - 1; j++)
                a[i][j] = b[i][j];
    }
    ROI_END();
    uint64_t c = 1469598103934665603ull;
    for (int i = 0; i < H; i++)
        for (int j = 0; j < W; j++) {
            union { float f; uint32_t u; } v = { a[i][j] };
            c = (c ^ v.u) * 1099511628211ull;
        }
    print_checksum(c);
    return 0;
}
