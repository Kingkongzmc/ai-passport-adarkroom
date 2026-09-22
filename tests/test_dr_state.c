// tests/test_dr_state.c —— 存档层主机测试:打包/校验/损坏检测/时钟回拨/离线钳制。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dr_state.h"
#include "dr_util.h"

static void test_pack_roundtrip(void) {
    dr_game_t g;
    dr_game_init(&g, 12345, 1000);
    g.res[DR_RES_WOOD] = 77;
    g.res_total[DR_RES_WOOD] = 300;
    g.population = 5;
    g.flags = 0x1;
    g.building_lv[2] = 3;

    dr_save_image_t img;
    dr_state_pack(&g, &img);

    dr_game_t out;
    uint16_t ver = 0;
    assert(dr_state_unpack(&img, &out, &ver));
    assert(ver == DR_SAVE_VERSION);
    assert(memcmp(&g, &out, sizeof(g)) == 0);
    assert(out.res[DR_RES_WOOD] == 77);
    assert(out.population == 5);
    assert(out.flags == 0x1);
}

static void test_corruption_detected(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    dr_save_image_t img;
    dr_state_pack(&g, &img);

    // 翻转 body 中一个字节 → CRC 必须失败
    ((uint8_t *)&img)[sizeof(dr_save_hdr_t) + 5] ^= 0xFF;
    dr_game_t out;
    uint16_t ver = 0;
    assert(!dr_state_unpack(&img, &out, &ver));

    // 魔数错误 → 失败
    dr_save_image_t img2;
    dr_state_pack(&g, &img2);
    img2.hdr.magic = 0x12345678;
    assert(!dr_state_unpack(&img2, &out, &ver));
}

static void test_version_gate(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    dr_save_image_t img;
    dr_state_pack(&g, &img);
    img.hdr.version = DR_SAVE_VERSION + 1;  // 未来版本拒绝
    // 重算 CRC 使版本检查成为唯一失败原因
    img.hdr.body_crc = dr_crc32(&img.body, sizeof(img.body));
    dr_game_t out;
    uint16_t ver = 0;
    assert(!dr_state_unpack(&img, &out, &ver));
}

static void test_offline_ticks(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);

    // 短离线:全速
    assert(dr_offline_ticks(&g, 1000 + 100) == 10);
    assert(g.saved_at_ts == 1100);

    // 恰好 8h 上限
    assert(dr_offline_ticks(&g, g.saved_at_ts + 8 * 3600) == 8 * 3600 / 10);

    // 16h:8h 全速 + 8h×25% = 10h 等效
    uint32_t t = dr_offline_ticks(&g, g.saved_at_ts + 16 * 3600);
    assert(t == (8 * 3600 + 8 * 3600 / 4) / 10);

    // 时钟回拨:0 且基准被拉平,不产生负数
    uint32_t now = g.saved_at_ts;
    assert(dr_offline_ticks(&g, now - 50) == 0);
    assert(g.saved_at_ts == now - 50);
    // 之后再正常前进,从新基准(回拨后的时间)起算:Δ = now+30 - (now-50) = 80s
    assert(dr_offline_ticks(&g, now + 30) == 8);
}

static void test_crc_and_rng(void) {
    // CRC-32 已知向量:"123456789" → 0xCBF43926
    assert(dr_crc32("123456789", 9) == 0xCBF43926u);

    // RNG 确定性与边界
    dr_rng_t r, r2;
    dr_rng_seed(&r, 42);
    dr_rng_seed(&r2, 42);
    for (int i = 0; i < 100; i++) assert(dr_rng_next(&r) == dr_rng_next(&r2));
    assert(dr_rng_below(&r, 1) == 0);
    // 概率边界
    dr_rng_seed(&r, 7);
    assert(!dr_rng_chance(&r, 0));
    assert(dr_rng_chance(&r, 1000));
    // 统计:50% 概率在 10000 次里应落在 45%~55%
    dr_rng_seed(&r, 99);
    int hits = 0;
    for (int i = 0; i < 10000; i++) hits += dr_rng_chance(&r, 500);
    assert(hits > 4500 && hits < 5500);
}

int main(void) {
    test_crc_and_rng();
    test_pack_roundtrip();
    test_corruption_detected();
    test_version_gate();
    test_offline_ticks();
    printf("test_dr_state: all passed\n");
    return 0;
}
