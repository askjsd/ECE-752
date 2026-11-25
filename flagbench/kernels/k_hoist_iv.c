/* Small/smooth kernel: LICM, GCSE, IVOPTS, and predictive commoning.
 *
 * A 2D fixed-point grid filter embeds loop-invariant scale/offset expressions,
 * repeated polynomial index calculations, and sliding-window neighbor reuse.
 * Compiler passes (loop-im, gcse, ivopts, predictive-commoning, vectorize)
 * act synergistically without register-spill cliffs, giving a smooth funnel
 * to the global optimum under small search budgets.
 */
#include "common.h"

#define ROWS 96
#define COLS 96
#define ROUNDS 36

static int32_t grid_a[ROWS * COLS];
static int32_t grid_b[ROWS * COLS];
static int32_t row_weights[ROWS];
static int32_t col_bias[COLS];

static void step_grid(
    int32_t * __restrict dst,
    const int32_t * __restrict src,
    const int32_t * __restrict r_w,
    const int32_t * __restrict c_b,
    int32_t alpha,
    int32_t beta,
    int32_t round_id)
{
    for (int r = 1; r < ROWS - 1; ++r) {
        for (int c = 1; c < COLS - 1; ++c) {
            /* Deliberate loop-invariant expressions & repeated 2D indexing */
            int32_t scale = ((alpha * 17 + beta * 31) ^ (round_id * 13)) & 255;
            int32_t r_gain = (r_w[r] * scale + (alpha << 3)) >> 4;
            /* Keep the fixed-point diffusion stable for the full run. */
            int32_t shift_amt = ((alpha + beta) & 3) + 13;

            int32_t center = src[r * COLS + c];
            int32_t left   = src[r * COLS + (c - 1)];
            int32_t right  = src[r * COLS + (c + 1)];
            int32_t up     = src[(r - 1) * COLS + c];
            int32_t down   = src[(r + 1) * COLS + c];

            int32_t lap = (left + right + up + down - (center * 4));
            int32_t val = center + ((lap * r_gain) >> shift_amt) +
                          (c_b[c] >> 2);
            dst[r * COLS + c] = val - (val >> 6);
        }
    }
}

int main(void)
{
    uint32_t seed = 0x9E3779B9u;
    for (int i = 0; i < ROWS * COLS; ++i) {
        grid_a[i] = (int32_t)(rng_next(&seed) & 4095u) - 2048;
        grid_b[i] = grid_a[i];
    }
    for (int r = 0; r < ROWS; ++r) {
        row_weights[r] = (int32_t)(rng_next(&seed) & 63u) + 8;
    }
    for (int c = 0; c < COLS; ++c) {
        col_bias[c] = (int32_t)(rng_next(&seed) & 127u) - 64;
    }

    ROI_BEGIN();
    for (int round = 0; round < ROUNDS; ++round) {
        step_grid(grid_b, grid_a, row_weights, col_bias, 19, 7, round);
        step_grid(grid_a, grid_b, row_weights, col_bias, 23, 11, round + 1);
    }
    ROI_END();

    uint64_t checksum = 1469598103934665603ull;
    for (int i = 0; i < ROWS * COLS; ++i) {
        checksum = (checksum ^ (uint32_t)grid_a[i]) * 1099511628211ull;
    }
    print_checksum(checksum);
    return 0;
}
