// main/game_darkroom/dr_util.h —— CRC32 与确定性小随机数,供存档校验与事件概率使用。
// 纯 C99,主机可测;不依赖 ESP-IDF(设备侧 NVS 读写由适配层提供)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 标准 CRC-32 (IEEE 802.3, 多项式 0xEDB88320),查表法。
uint32_t dr_crc32(const void *data, size_t len);

// 数量紧凑化(资源格 46px 宽,5 位数字起必被 LONG_DOT 截断):
//   <10000 原样("9876") / <99500 一位小数 k("12.3k",上溢按 100k)
//   <999500 整 k("123k") / 更大 0.1M 精度("1.2M")
// 输出恒 ≤4 字符,"木 99.9k" = 44.7px 恰好放进 46px 资源格。
void dr_fmt_compact(char *out, size_t n, uint32_t v);

// xorshift32 确定性随机:同一种子同一序列,保证事件概率/地图生成可复现。
// 种子不可为 0;内部若遇 0 会换成 0xDEADBEEF。
typedef struct {
    uint32_t s;
} dr_rng_t;

void     dr_rng_seed(dr_rng_t *r, uint32_t seed);
uint32_t dr_rng_next(dr_rng_t *r);                 // [0, 2^32)
uint32_t dr_rng_below(dr_rng_t *r, uint32_t n);    // [0, n)
bool     dr_rng_chance(dr_rng_t *r, uint16_t permille);  // 千分比概率

#ifdef __cplusplus
}
#endif
