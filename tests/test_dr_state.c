// tests/test_dr_state.c —— 存档层主机测试:打包/校验/损坏检测/时钟回拨/离线钳制。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dr_rules.h"   // 仅用枚举常量(job/火焰档),不链接规则实现
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

static void test_load_is_side_effect_free(void) {
    // 回归(v1.1 修复):读档必须是纯读取,不得推进 saved_at_ts——
    // 离线间隔只能由 dr_rules_offline_settle 独占消费。设备端 dr_port_load
    // 曾在此顺手结算,导致后续 settle 看到 Δt=0 直接早退,真机离线收益
    // 从未生效(模拟器垫片与主机测试都绕开了设备胶水层,故全绿带病)。
    dr_game_t g, out;
    dr_game_init(&g, 7, 100000);
    dr_save_image_t img;
    dr_state_pack(&g, &img);

    assert(dr_state_load(&img, sizeof(img), &out));
    assert(out.saved_at_ts == 100000);            // 读档不动锚点
    assert(dr_offline_ticks(&out, 100600) == 60); // 间隔仍完整可结算
    assert(out.saved_at_ts == 100600);            // 结算方才推进锚点
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

static void test_v1_migration(void) {
    // 用 v1 布局压一张镜像(模拟已发布 v1 固件写的 NVS 存档),再走 dr_state_load
    dr_game_t g;
    dr_game_init(&g, 4242, 1000);
    g.res[DR_RES_WOOD] = 77;
    g.res[DR_RES_FUR] = 12;
    g.res_total[DR_RES_WOOD] = 300;
    g.population = 5;
    g.flags = 0x3;
    g.building_lv[2] = 3;
    g.job[DR_JOB_LUMBER] = 2;
    g.fire_lv = DR_FIRE_BURNING;
    // v1 没有的字段塞上"脏值":迁移后必须归零,证明新槽位是零填充而非穿帮
    g.res[DR_RES_LEATHER] = 999;
    g.res_total[DR_RES_LEATHER] = 999;
    g.armor_lv = 1;
    g.trap_bait_on = 1;

    uint8_t blob[512];
    size_t n = dr_state_pack_v1(&g, blob, sizeof(blob));
    assert(n == sizeof(dr_save_hdr_t) + 460u);   // v1 镜像 = 头 12 + 体 460
    dr_game_t out;
    assert(dr_state_load(blob, n, &out));
    assert(out.saved_at_ts == 1000);
    assert(out.res[DR_RES_WOOD] == 77 && out.res[DR_RES_FUR] == 12);
    assert(out.res_total[DR_RES_WOOD] == 300);
    assert(out.population == 5 && out.flags == 0x3);
    assert(out.building_lv[2] == 3 && out.job[DR_JOB_LUMBER] == 2);
    assert(out.fire_lv == DR_FIRE_BURNING);
    assert(out.res[DR_RES_LEATHER] == 0 && out.res_total[DR_RES_LEATHER] == 0);
    assert(out.armor_lv == 0 && out.trap_bait_on == 0);

    // 截断 / 篡改 / 垃圾头 → 拒绝(坏档不落进迁移路径)
    assert(!dr_state_load(blob, n - 1, &out));
    blob[sizeof(dr_save_hdr_t) + 8] ^= 0xFF;
    assert(!dr_state_load(blob, n, &out));
    assert(!dr_state_load("garbage!", 8, &out));
    assert(!dr_state_load(NULL, n, &out));

    // 当前版本镜像照常走 load(与 unpack 同口径)
    dr_save_image_t img;
    dr_state_pack(&g, &img);
    dr_game_t cur;
    assert(dr_state_load(&img, sizeof(img), &cur));
    assert(memcmp(&g, &cur, sizeof(g)) == 0);
}

int main(void) {
    test_crc_and_rng();
    test_pack_roundtrip();
    test_corruption_detected();
    test_version_gate();
    test_offline_ticks();
    test_load_is_side_effect_free();
    test_v1_migration();
    printf("test_dr_state: all passed\n");
    return 0;
}
