/* CRC32 (bitwise, no table): tight shift/xor loop, unrolling / scheduling sensitive. */
#include "common.h"
#define LEN 36000
static uint8_t buf[LEN];
int main(void) {
    uint32_t seed = 31337u;
    for (int i = 0; i < LEN; i++) buf[i] = (uint8_t)rng_next(&seed);
    uint32_t crc = 0xFFFFFFFFu;
    ROI_BEGIN();
    for (int i = 0; i < LEN; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    ROI_END();
    print_checksum(~crc);
    return 0;
}
