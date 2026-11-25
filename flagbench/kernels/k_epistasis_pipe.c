/* Large/epistatic kernel: coupled inlining, IPA-CP, loop split, and SIMD.
 *
 * Designed for larger flag spaces (12-16+ flags) and higher search budgets:
 *   - Hot loops call small non-inline helper routines with compile-time mode
 *     and threshold parameters.
 *   - Unlocking vectorization requires a chain of prerequisite passes:
 *       1) function inlining (-finline-functions)
 *       2) interprocedural constant propagation (-fipa-cp-clone)
 *       3) loop splitting / unswitching (-fsplit-loops / -funswitch-loops)
 *       4) loop & SLP vectorization (-ftree-vectorize / -ftree-slp-vectorize)
 *   - Enabling unrolling/peeling alone bloats call-heavy loops into local
 *     optima traps; CARBS needs higher budget to learn the joint flag cluster.
 */
#include "common.h"

#define N_ELEMS 4096
#define ROUNDS 32

static float buf_x[N_ELEMS];
static float buf_y[N_ELEMS];
static float buf_z[N_ELEMS];
static float coeff_w[N_ELEMS];

static float helper_affine(float v, float w, float gain, float bias)
{
    return v * gain + w * bias;
}

static float helper_piecewise(
    float v, float threshold, float lo_scale, float hi_scale)
{
    if (v < threshold) {
        return v * lo_scale + 0.015625f;
    }
    return v * hi_scale - 0.0078125f;
}

static float helper_blend(float a, float b, int mode)
{
    if (mode == 0) {
        return a * 0.75f + b * 0.25f;
    } else if (mode == 1) {
        return a * 0.625f + b * 0.375f;
    }
    return a - b * 0.125f;
}

static void run_pipeline_pass(
    float * __restrict x,
    float * __restrict y,
    float * __restrict z,
    const float * __restrict w,
    int n,
    int split_point,
    int mode,
    float gain,
    float bias,
    float thresh)
{
    /* Phase 1: Split-index + mode-branched helper chain */
    for (int i = 0; i < n; ++i) {
        float base = helper_affine(x[i], w[i], gain, bias);
        float shaped;
        if (i < split_point) {
            shaped = helper_piecewise(base, thresh, 0.96875f, 0.9375f);
        } else {
            shaped = helper_piecewise(base, thresh * 1.5f, 0.90625f, 0.875f);
        }
        y[i] = helper_blend(shaped, z[i], mode);
    }

    /* Phase 2: Second-order recurrence coupled with helper dispatch */
    for (int i = 1; i < n - 1; ++i) {
        float d0 = helper_blend(y[i - 1], y[i], mode ^ 1);
        float d1 = helper_blend(y[i + 1], y[i], mode);
        z[i] = helper_affine(d0 + d1, w[i], 0.4921875f, 0.015625f);
    }
}

int main(void)
{
    uint32_t seed = 0xC0FFEE11u;
    for (int i = 0; i < N_ELEMS; ++i) {
        buf_x[i] = (float)(rng_next(&seed) & 1023u) * (1.0f / 1024.0f);
        buf_y[i] = (float)(rng_next(&seed) & 511u) * (1.0f / 512.0f);
        buf_z[i] = (float)(rng_next(&seed) & 255u) * (1.0f / 256.0f);
        coeff_w[i] = (float)(rng_next(&seed) & 127u) * (1.0f / 128.0f);
    }

    ROI_BEGIN();
    for (int round = 0; round < ROUNDS; ++round) {
        run_pipeline_pass(
            buf_x, buf_y, buf_z, coeff_w,
            N_ELEMS, N_ELEMS / 2, round & 1,
            0.984375f, 0.03125f, 0.5f);
        run_pipeline_pass(
            buf_z, buf_x, buf_y, coeff_w,
            N_ELEMS, (N_ELEMS * 3) / 4, (round + 1) & 1,
            0.9765625f, 0.0234375f, 0.375f);
    }
    ROI_END();

    uint64_t checksum = 1469598103934665603ull;
    for (int i = 0; i < N_ELEMS; ++i) {
        union
        {
            float f;
            uint32_t u;
        } bx = {buf_x[i]};
        union
        {
            float f;
            uint32_t u;
        } by = {buf_y[i]};
        union
        {
            float f;
            uint32_t u;
        } bz = {buf_z[i]};
        checksum = (checksum ^ bx.u) * 1099511628211ull;
        checksum = (checksum ^ by.u) * 1099511628211ull;
        checksum = (checksum ^ bz.u) * 1099511628211ull;
    }
    print_checksum(checksum);
    return 0;
}
