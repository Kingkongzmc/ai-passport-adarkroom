// main/game_darkroom/dr_util.c —— CRC32 与 xorshift32 实现。
#include "dr_util.h"

uint32_t dr_crc32(const void *data, size_t len) {
    static uint32_t table[256];
    static int table_ready = 0;
    if (!table_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        table_ready = 1;
    }
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++)
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void dr_rng_seed(dr_rng_t *r, uint32_t seed) {
    r->s = (seed == 0u) ? 0xDEADBEEFu : seed;
}

uint32_t dr_rng_next(dr_rng_t *r) {
    uint32_t x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = (x == 0u) ? 0xDEADBEEFu : x;
    return r->s;
}

uint32_t dr_rng_below(dr_rng_t *r, uint32_t n) {
    if (n == 0u) return 0;
    // 取高 32 位乘法避免低位模偏差:floor(next * n / 2^32)
    return (uint32_t)(((uint64_t)dr_rng_next(r) * (uint64_t)n) >> 32);
}

bool dr_rng_chance(dr_rng_t *r, uint16_t permille) {
    if (permille == 0) return false;
    if (permille >= 1000) return true;
    return dr_rng_below(r, 1000u) < (uint32_t)permille;
}
