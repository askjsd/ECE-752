/* Reduction + prefix sum over an array larger than L2. */
#include "common.h"
#define N 131072
static int32_t arr[N];
int main(void) {
    uint32_t seed = 55555u;
    for (int i = 0; i < N; i++) arr[i] = (int32_t)(rng_next(&seed) & 0xffff) - 32768;
    int64_t total = 0;
    ROI_BEGIN();
    for (int i = 0; i < N; i++) total += arr[i];          /* reduction */
    int32_t run = 0;
    for (int i = 0; i < N; i++) { run += arr[i]; arr[i] = run; } /* prefix sum */
    for (int i = 0; i < N; i++) total ^= arr[i];          /* second reduction */
    ROI_END();
    print_checksum((uint64_t)total ^ (uint64_t)(uint32_t)arr[N - 1]);
    return 0;
}
