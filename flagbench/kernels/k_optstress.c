/* Focused compiler-optimization stress kernel.
 *
 * The hot loop performs independent floating-point updates across an array.
 * Loop and SLP vectorization should substantially reduce its instruction
 * count. Loop unrolling can provide a secondary effect. Fixed inputs and a
 * checksum make results comparable across compiler configurations.
 */
#include "common.h"

#define ELEMENTS 16384
#define ROUNDS 128

static float state[ELEMENTS];
static float input_a[ELEMENTS];
static float input_b[ELEMENTS];

int main(void)
{
    uint32_t seed = 0x51A7E5u;
    for (int i = 0; i < ELEMENTS; ++i) {
        state[i] = (float)(rng_next(&seed) & 1023u) * (1.0f / 1024.0f);
        input_a[i] = (float)(rng_next(&seed) & 255u) * (1.0f / 256.0f);
        input_b[i] = (float)(rng_next(&seed) & 255u) * (1.0f / 256.0f);
    }

    ROI_BEGIN();
    for (int round = 0; round < ROUNDS; ++round) {
        for (int i = 0; i < ELEMENTS; ++i) {
            state[i] = state[i] * 0.9991f
                + input_a[i] * 0.03125f
                - input_b[i] * 0.015625f;
        }
    }
    ROI_END();

    uint64_t checksum = 1469598103934665603ull;
    for (int i = 0; i < ELEMENTS; ++i) {
        union
        {
            float f;
            uint32_t u;
        } bits = {state[i]};
        checksum = (checksum ^ bits.u) * 1099511628211ull;
    }
    print_checksum(checksum);
    return 0;
}
