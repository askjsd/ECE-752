/* Column-order nested loops: -floop-interchange swaps them to unit stride. */
#include "../common.h"
#define R 96
#define C 96
static int32_t m[R][C], o[R][C];
int main(void) {
    uint32_t seed = 66u;
    for (int i = 0; i < R; i++) for (int j = 0; j < C; j++) m[i][j] = (int32_t)(rng_next(&seed) & 0xff);
    ROI_BEGIN();
    for (int r = 0; r < 12; r++)
        for (int j = 0; j < C; j++)
            for (int i = 0; i < R; i++)
                o[i][j] = o[i][j] + m[i][j] * 3 + (m[i][j] >> 1);
    ROI_END();
    uint64_t c = 0; for (int i = 0; i < R; i++) for (int j = 0; j < C; j++) c = c * 131 + (uint32_t)o[i][j];
    print_checksum(c); return 0;
}
