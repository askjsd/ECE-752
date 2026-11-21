/* Large/epistatic kernel: tiled micro-GEMM + fused reduction cliff.
 *
 * Designed for larger flag spaces (12-16+ flags) and higher search budgets:
 *   - Combines an 8x8 register-blocked matrix multiply-accumulate with a
 *     fused indirect bucket-histogram update inside the innermost loop, plus
 *     a strided transpose-mix pass.
 *   - Global optimum requires loop distribution (-ftree-loop-distribution) to
 *     split the loop-carried scalar histogram away from the vectorizable GEMM
 *     inner loop, paired with loop unrolling and SIMD vectorization.
 *   - Aggressive unrolling without loop distribution unrolls the indirect
 *     float-to-int histogram updates, causing register/uop pressure cliffs
 *     that trap greedy low-budget searches in local optima.
 */
#include "common.h"

#define MAT_N 48
#define TILE 8
#define BINS 32
#define ROUNDS 16

static float mat_a[MAT_N * MAT_N];
static float mat_b[MAT_N * MAT_N];
static float mat_c[MAT_N * MAT_N];
static uint32_t hist_bins[BINS];

int main(void)
{
    uint32_t seed = 0x7F4A7C15u;
    for (int i = 0; i < MAT_N * MAT_N; ++i) {
        mat_a[i] = (float)(rng_next(&seed) & 255u) * (1.0f / 256.0f);
        mat_b[i] = (float)(rng_next(&seed) & 255u) * (1.0f / 256.0f);
        mat_c[i] = 0.0f;
    }
    for (int b = 0; b < BINS; ++b) {
        hist_bins[b] = 1u;
    }

    ROI_BEGIN();
    for (int round = 0; round < ROUNDS; ++round) {
        /* Stage 1: Tiled GEMM + loop-carried indirect histogram dependency */
        for (int ii = 0; ii < MAT_N; ii += TILE) {
            for (int jj = 0; jj < MAT_N; jj += TILE) {
                for (int kk = 0; kk < MAT_N; kk += TILE) {
                    for (int i = ii; i < ii + TILE; ++i) {
                        for (int k = kk; k < kk + TILE; ++k) {
                            float a_ik = mat_a[i * MAT_N + k];
                            for (int j = jj; j < jj + TILE; ++j) {
                                float prod = a_ik * mat_b[k * MAT_N + j];
                                mat_c[i * MAT_N + j] += prod;
                                uint32_t bin = (uint32_t)(i * 7 + j * 11 +
                                                          k * 13 + round) &
                                               (BINS - 1);
                                hist_bins[bin] += (uint32_t)(prod * 32.0f);
                            }
                        }
                    }
                }
            }
        }

        /* Stage 2: Strided transpose-feedback & normalization */
        float inv_scale = 0.03125f;
        for (int i = 0; i < MAT_N; ++i) {
            for (int j = 0; j < MAT_N; ++j) {
                float c_ij = mat_c[i * MAT_N + j] * inv_scale;
                float b_ji = mat_b[j * MAT_N + i];
                mat_a[i * MAT_N + j] = b_ji * 0.875f + c_ij * 0.125f;
                mat_c[i * MAT_N + j] = c_ij * 0.5f;
            }
        }
    }
    ROI_END();

    uint64_t checksum = 1469598103934665603ull;
    for (int i = 0; i < MAT_N * MAT_N; ++i) {
        union
        {
            float f;
            uint32_t u;
        } ba = {mat_a[i]};
        union
        {
            float f;
            uint32_t u;
        } bc = {mat_c[i]};
        checksum = (checksum ^ ba.u) * 1099511628211ull;
        checksum = (checksum ^ bc.u) * 1099511628211ull;
    }
    for (int b = 0; b < BINS; ++b) {
        checksum = (checksum ^ hist_bins[b]) * 1099511628211ull;
    }
    print_checksum(checksum);
    return 0;
}
