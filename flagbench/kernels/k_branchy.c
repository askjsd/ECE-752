/* Data-dependent branches: tiny bytecode interpreter over random input. */
#include "common.h"
#define LEN 65536
static uint8_t code[LEN];
int main(void) {
    uint32_t seed = 97531u;
    for (int i = 0; i < LEN; i++) code[i] = (uint8_t)rng_next(&seed);
    uint32_t acc = 1, x = 7, y = 3;
    ROI_BEGIN();
    for (int pass = 0; pass < 3; pass++) {
        for (int pc = 0; pc < LEN; pc++) {
            uint8_t op = code[pc];
            switch (op & 7) {
            case 0: acc += x; break;
            case 1: acc ^= (acc << 3) + y; break;
            case 2: x = x * 33u + (op >> 3); break;
            case 3: if (acc & 1) y += x; else y ^= acc; break;
            case 4: acc = (acc >> 1) | (acc << 31); break;
            case 5: if ((x ^ y) & 4) acc -= y; break;
            case 6: x += (op & 0x10) ? acc : y; break;
            default: y = (y << 1) ^ (op >> 4); break;
            }
        }
    }
    ROI_END();
    print_checksum(((uint64_t)acc << 32) ^ ((uint64_t)x << 16) ^ y);
    return 0;
}
