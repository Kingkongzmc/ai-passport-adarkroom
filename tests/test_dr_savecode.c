// tests/test_dr_savecode.c —— 存档码主机测试:round-trip / CRC 篡改 / 截断 /
// RLE 边界(全零/全非零/128 游程)/ base32 非法字符 / expect_len。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dr_savecode.h"

// 填一个"像样"的档:零值段(建筑/职业高位)+ 小数值(资源)+ 杂项
static void make_game(dr_game_t *g) {
    memset(g, 0, sizeof(*g));
    g->saved_at_ts = 1700000000u;
    g->res[DR_RES_WOOD] = 12345u;
    g->res[DR_RES_FUR] = 678u;
    g->res[DR_RES_LEATHER] = 90u;
    g->res_total[DR_RES_WOOD] = 999999u;
    g->population = 24;
    g->job[0] = 3;
    g->job[1] = 5;
    g->building_lv[0] = 1;
    g->building_lv[2] = 4;
    g->building_lv[9] = 1;
    g->flags = 0x0000000000FF21C3ull;
    g->rng_seed_state = 42u;
    g->map_seed = 0xBEEFu;
    g->fire_lv = 3;
    g->temp_lv = 2;
    g->builder_lv = 4;
    g->weapon_lv = 2;
    g->armor_lv = 1;
    g->prestige_count = 2;
    g->ship_thrusters = 1;
    g->hero_hp = g->hero_hp_max = 10;
}

static void test_round_trip(void) {
    dr_game_t g, out;
    make_game(&g);
    char code[DR_CODE_MAX_CHARS + 1];
    size_t n = dr_savecode_export(&g, code, sizeof(code));
    printf("code len=%zu\n", n);
    assert(n > 0 && n <= DR_CODE_MAX_CHARS);
    for (size_t i = 0; i < n; i++)
        assert(strchr(DR_CODE_B32, code[i]) != NULL);   // 字母表闭集

    dr_code_core_t core;
    assert(dr_savecode_expect_len(code, n) == n);       // 预读总长自洽
    assert(dr_savecode_import(code, n, &core));
    dr_savecode_apply(&core, &out, 0x12345678u, 1700009999u);
    // 核心档字段逐项相等
    assert(memcmp(out.res, g.res, sizeof(g.res)) == 0);
    assert(out.population == g.population);
    assert(memcmp(out.job, g.job, sizeof(g.job)) == 0);
    assert(memcmp(out.building_lv, g.building_lv, sizeof(g.building_lv)) == 0);
    assert(out.flags == g.flags);
    assert(out.weapon_lv == g.weapon_lv && out.armor_lv == g.armor_lv);
    assert(out.fire_lv == g.fire_lv && out.temp_lv == g.temp_lv &&
           out.builder_lv == g.builder_lv);
    assert(out.prestige_count == g.prestige_count);
    // 丢弃字段按口径重置
    assert(out.saved_at_ts == 1700009999u);             // now,不触发离线
    assert(out.map_seed == (uint16_t)(0x12345678u >> 16));
    assert(out.rng_seed_state == 0x12345678u);
    for (int i = 0; i < 128; i++) assert(out.event_count[i] == 0);
    assert(out.in_wilderness == 0);
    assert(out.hero_hp == 10 && out.hero_hp_max == 10);
    assert(out.ship_thrusters == 1);
    for (int i = 0; i < DR_RES_KIND_COUNT; i++)
        assert(out.res_total[i] >= out.res[i]);
    printf("test_round_trip passed\n");
}

static void test_tamper(void) {
    dr_game_t g;
    make_game(&g);
    char code[DR_CODE_MAX_CHARS + 1];
    size_t n = dr_savecode_export(&g, code, sizeof(code));
    assert(n > 0);

    dr_code_core_t core;
    // 单字符篡改(在码尾改动会落到 CRC 之后?不——整帧都有 CRC 覆盖,任意
    // 位置翻转都应被拒;跳过前 2 字符避免把魔数改没也能测 CRC 路径)
    char bad[DR_CODE_MAX_CHARS + 1];
    memcpy(bad, code, n);
    bad[10] = (bad[10] == 'A') ? 'B' : 'A';
    assert(!dr_savecode_import(bad, n, &core));

    // 截断:少 1 字符
    assert(!dr_savecode_import(code, n - 1, &core));
    // 非法字符
    memcpy(bad, code, n);
    bad[5] = '1';                                       // 1 不在 base32 表
    assert(!dr_savecode_import(bad, n, &core));
    // 前缀 7 字符(帧头都不够)
    assert(dr_savecode_expect_len(code, 7) == 0);
    printf("test_tamper passed\n");
}

static void test_rle_edges(void) {
    // 全零档:RLE 压缩后码应显著短(零游程生效)
    dr_game_t g0, out;
    memset(&g0, 0, sizeof(g0));
    char code0[DR_CODE_MAX_CHARS + 1];
    size_t n0 = dr_savecode_export(&g0, code0, sizeof(code0));
    assert(n0 > 0 && n0 < 120);                         // 174B 全零 → ~13B 帧
    dr_code_core_t core;
    assert(dr_savecode_import(code0, n0, &core));
    dr_savecode_apply(&core, &out, 1u, 100u);
    assert(out.population == 0 && out.flags == 0 && out.res[0] == 0);

    // 全非零(0xFF):纯字面段最坏路径 174+2B → 码仍在 320 上限内
    dr_game_t gf;
    memset(&gf, 0xFF, sizeof(gf));
    char codef[DR_CODE_MAX_CHARS + 1];
    size_t nf = dr_savecode_export(&gf, codef, sizeof(codef));
    assert(nf > 0 && nf <= DR_CODE_MAX_CHARS);
    assert(dr_savecode_import(codef, nf, &core));
    assert(core.res[0] == 0xFFFFFFFFu && core.building_lv[39] == 0xFF);
    printf("test_rle_edges passed (zero=%zu ff=%zu)\n", n0, nf);
}

static void test_version_gate(void) {
    // 字节级:解出真码前 5 字节,把版本字节改成 1,重编回前 8 字符。
    // 版本门 + CRC 门都应拒绝(版本变了 CRC 也对不上,双保险)。
    dr_game_t g;
    make_game(&g);
    char code[DR_CODE_MAX_CHARS + 1];
    size_t n = dr_savecode_export(&g, code, sizeof(code));
    assert(n > 0);
    assert(dr_savecode_expect_len(code, 8) > 0);        // 真码帧头可解

    uint8_t head[5] = {0};
    uint32_t acc = 0;
    int bits = 0, o = 0;
    for (int i = 0; i < 8 && o < 5; i++) {              // mini 解码
        const char *p = strchr(DR_CODE_B32, code[i]);
        assert(p);
        acc = (acc << 5) | (uint32_t)(p - DR_CODE_B32);
        bits += 5;
        if (bits >= 8) { head[o++] = (uint8_t)(acc >> (bits - 8)); bits -= 8; }
    }
    assert(head[0] == 'D' && head[1] == 'K');
    head[2] = 1;                                        // 版本 → 1(非当前)
    char bad[DR_CODE_MAX_CHARS + 1];
    memcpy(bad, code, n);
    acc = 0; bits = 0; o = 0;                           // mini 编码回 8 字符
    for (int i = 0; i < 5; i++) {
        acc = (acc << 8) | head[i];
        bits += 8;
        while (bits >= 5) { bad[o++] = DR_CODE_B32[(acc >> (bits - 5)) & 0x1Fu]; bits -= 5; }
    }
    assert(o == 8);
    assert(dr_savecode_expect_len(bad, 8) == 0);        // 版本门
    dr_code_core_t core;
    assert(!dr_savecode_import(bad, n, &core));
    printf("test_version_gate passed\n");
}

int main(void) {
    test_round_trip();
    test_tamper();
    test_rle_edges();
    test_version_gate();
    printf("test_dr_savecode passed\n");
    return 0;
}
