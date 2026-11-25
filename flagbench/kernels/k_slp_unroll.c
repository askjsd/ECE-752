/* Small/smooth optimization kernel: SLP vectorization + fixed-tap unrolling.
 *
 * Two independent hot loops with additive flag sensitivities:
 *   1. 4-lane tuple affine mixing (targets SLP vectorization & store merging).
 *   2. Fixed 8-tap FIR filter over a 1D signal (loop unrolling and SIMD).
 * Because the two stages do not compete for registers or cache capacity,
 * flag improvements compose monotonically so low-budget searches reach the
 * global optimum directly.
 */
#include "common.h"

#define TUPLES 4096
#define SIGNAL_LEN 4096
#define TAPS 8
#define ROUNDS 48

static float px_r[TUPLES];
static float px_g[TUPLES];
static float px_b[TUPLES];
static float px_a[TUPLES];

static float sig_in[SIGNAL_LEN + TAPS];
static float sig_out[SIGNAL_LEN];
static const float fir_coeff[TAPS] = {
    0.0625f, 0.125f, 0.1875f, 0.25f,
    0.1875f, 0.125f, 0.03125f, 0.03125f
};

int main(void)
{
    uint32_t seed = 0x1A2B3C4Du;
    for (int i = 0; i < TUPLES; ++i) {
        px_r[i] = (float)(rng_next(&seed) & 1023u) * (1.0f / 1024.0f);
        px_g[i] = (float)(rng_next(&seed) & 1023u) * (1.0f / 1024.0f);
        px_b[i] = (float)(rng_next(&seed) & 1023u) * (1.0f / 1024.0f);
        px_a[i] = (float)(rng_next(&seed) & 255u) * (1.0f / 256.0f);
    }
    for (int i = 0; i < SIGNAL_LEN + TAPS; ++i) {
        sig_in[i] = (float)(rng_next(&seed) & 511u) * (1.0f / 512.0f);
    }

    ROI_BEGIN();
    for (int round = 0; round < ROUNDS; ++round) {
        /* Stage A: 4-way independent tuple math -> SLP vectorization */
        for (int i = 0; i < TUPLES; ++i) {
            float alpha = px_a[i];
            px_r[i] = px_r[i] * 0.99609375f + alpha * 0.015625f;
            px_g[i] = px_g[i] * 0.99218750f + alpha * 0.031250f;
            px_b[i] = px_b[i] * 0.98828125f + alpha * 0.046875f;
            px_a[i] = alpha * 0.998046875f + 0.0009765625f;
        }

        /* Stage B: Fixed-trip 8-tap FIR -> loop unrolling + SIMD */
        for (int i = 0; i < SIGNAL_LEN; ++i) {
            float acc = 0.0f;
            for (int t = 0; t < TAPS; ++t) {
                acc += sig_in[i + t] * fir_coeff[t];
            }
            sig_out[i] = acc;
        }

        for (int i = 0; i < SIGNAL_LEN; ++i) {
            sig_in[i] = sig_out[i] * 0.9921875f + 0.00390625f;
        }
    }
    ROI_END();

    uint64_t checksum = 1469598103934665603ull;
    for (int i = 0; i < TUPLES; ++i) {
        union
        {
            float f;
            uint32_t u;
        } br = {px_r[i]};
        union
        {
            float f;
            uint32_t u;
        } bg = {px_g[i]};
        union
        {
            float f;
            uint32_t u;
        } bb = {px_b[i]};
        union
        {
            float f;
            uint32_t u;
        } ba = {px_a[i]};
        checksum = (checksum ^ br.u) * 1099511628211ull;
        checksum = (checksum ^ bg.u) * 1099511628211ull;
        checksum = (checksum ^ bb.u) * 1099511628211ull;
        checksum = (checksum ^ ba.u) * 1099511628211ull;
    }
    for (int i = 0; i < SIGNAL_LEN; ++i) {
        union
        {
            float f;
            uint32_t u;
        } bs = {sig_out[i]};
        checksum = (checksum ^ bs.u) * 1099511628211ull;
    }
    print_checksum(checksum);
    return 0;
}
