// tests/test_dr_world.c —— M4 世界与远征主机测试(对齐原版 world.js 口径):
// 生成确定性/地形与地标/出发/移动消耗(水每步1,干肉每2步1)/警告-死亡/
// 回家提交(矿 flags+余粮入库)/哨站补水一次/吃干肉回 8 HP。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dr_world.h"
#include "dr_rules.h"
#include "dr_state.h"
#include "dr_util.h"

static uint16_t manhattan(int x, int y) {
    int dx = x - DR_WORLD_CENTER, dy = y - DR_WORLD_CENTER;
    return (uint16_t)((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy));
}

static void test_gen(void) {
    dr_world_t a, b;
    dr_world_gen(&a, 1234);
    dr_world_gen(&b, 1234);
    assert(memcmp(a.tiles, b.tiles, sizeof(a.tiles)) == 0);   // 同种子同图
    dr_world_gen(&b, 4321);
    assert(memcmp(a.tiles, b.tiles, sizeof(a.tiles)) != 0);   // 异种子异图

    // 村庄在正中,周边一圈森林
    assert(dr_world_tile(&a, 30, 30) == DR_WT_VILLAGE);
    assert(dr_world_tile(&a, 29, 30) == DR_WT_FOREST);
    assert(dr_world_tile(&a, 30, 31) == DR_WT_FOREST);

    // 三矿各 1,落位在距村半径 ±1 的环带
    int iron = 0, coal = 0, sulphur = 0;
    for (int y = 0; y < DR_WORLD_SIZE; y++)
        for (int x = 0; x < DR_WORLD_SIZE; x++) {
            uint8_t t = dr_world_tile(&a, x, y);
            uint16_t d = manhattan(x, y);
            if (t == DR_WT_IRON)     { iron++;     assert(d >= 4 && d <= 6); }
            if (t == DR_WT_COAL)     { coal++;     assert(d >= 9 && d <= 11); }
            if (t == DR_WT_SULPHUR)  { sulphur++;  assert(d >= 19 && d <= 21); }
        }
    assert(iron == 1 && coal == 1 && sulphur == 1);

    // 哨站 3 个 + 星舰 1 个
    int outposts = 0, ships = 0;
    for (int y = 0; y < DR_WORLD_SIZE; y++)
        for (int x = 0; x < DR_WORLD_SIZE; x++) {
            uint8_t t = dr_world_tile(&a, x, y);
            if (t == DR_WT_OUTPOST) outposts++;
            if (t == DR_WT_SHIP) ships++;
        }
    assert(outposts == 3 && ships == 1);
}

static void test_embark_and_supplies(void) {
    dr_world_t w;
    dr_world_gen(&w, 7);
    dr_game_t g;
    dr_game_init(&g, 7, 1000);

    assert(!dr_world_embark(&w, &g));       // 无干肉不可出发
    g.res[DR_RES_FOOD] = 25;
    assert(dr_world_embark(&w, &g));
    assert(g.in_wilderness == 1);
    assert(g.res[DR_RES_FOOD] == 15);       // 带走 10(基础负重)
    assert(g.food == 10 && g.water == 10);
    assert(g.hero_x == 30 && g.hero_y == 30);
    assert(g.hero_hp == 10 && g.hero_hp_max == 10);
    assert(dr_world_seen(&w, 30, 30));

    // 移动 2 步:水 -2(每步 1),干肉 -1(每 2 步 1)
    assert(dr_world_move(&w, &g, 1, 0) == DR_MOVE_OK);
    assert(g.water == 9);
    assert(dr_world_move(&w, &g, 0, 1) == DR_MOVE_OK);
    assert(g.water == 8 && g.food == 9);

    // 走回村庄格 = 回家:4 步共吃 2 口干肉,余 8 口入库
    assert(dr_world_move(&w, &g,  0, -1) == DR_MOVE_OK);
    dr_move_result_t r = dr_world_move(&w, &g, -1, 0);
    assert(r == DR_MOVE_HOME);
    assert(g.in_wilderness == 0);
    assert(g.res[DR_RES_FOOD] == 15 + 8);
    assert(g.food == 0 && g.water == 0);
}

static void test_thirst_death(void) {
    dr_world_t w;
    dr_world_gen(&w, 7);
    dr_game_t g;
    dr_game_init(&g, 7, 1000);
    g.res[DR_RES_FOOD] = 10;
    assert(dr_world_embark(&w, &g));
    // 村庄 3×3 恒为森林(无地标干扰):向北走测断供语义
    g.water = 1;
    assert(dr_world_move(&w, &g, 0, -1) == DR_MOVE_OK);      // 最后一口水
    assert(dr_world_move(&w, &g, 0, -1) == DR_MOVE_WARN_THIRST);  // 第一次缺水仅警告
    assert(dr_world_move(&w, &g, 0, -1) == DR_MOVE_DEATH);   // 再无水 = 死亡
    assert(g.in_wilderness == 0);
    assert(g.res[DR_RES_FOOD] == 0);        // 带走的干肉不返还
}

static void test_outpost_and_mines(void) {
    dr_world_t w;
    dr_world_gen(&w, 99);
    dr_game_t g;
    dr_game_init(&g, 99, 1000);
    g.res[DR_RES_FOOD] = 30;
    assert(dr_world_embark(&w, &g));

    // 找哨站,把主角放到它旁边,直接踏上
    int ox = -1, oy = -1;
    for (int y = 0; y < DR_WORLD_SIZE && ox < 0; y++)
        for (int x = 0; x < DR_WORLD_SIZE; x++)
            if (dr_world_tile(&w, x, y) == DR_WT_OUTPOST) { ox = x; oy = y; break; }
    assert(ox > 0);
    g.hero_x = (uint8_t)(ox - 1);
    g.hero_y = (uint8_t)oy;
    g.water = 1;
    assert(dr_world_move(&w, &g, 1, 0) == DR_MOVE_OUTPOST);
    assert(g.water == dr_world_water_cap(&g));   // 补满
    g.water = 2;
    assert(dr_world_move(&w, &g, -1, 0) == DR_MOVE_OK);
    assert(dr_world_move(&w, &g, 1, 0) == DR_MOVE_OK);   // 二访不补(仅移动耗水)
    assert(g.water == 0);

    // 铁矿:踏上标记 visited;回家后 flags 提交,矿工职业解锁
    int ix = -1, iy = -1;
    for (int y = 0; y < DR_WORLD_SIZE && ix < 0; y++)
        for (int x = 0; x < DR_WORLD_SIZE; x++)
            if (dr_world_tile(&w, x, y) == DR_WT_IRON) { ix = x; iy = y; break; }
    g.water = 5;                            // 前段耗尽,补水续测
    g.hero_x = (uint8_t)(ix - 1);
    g.hero_y = (uint8_t)iy;
    assert(dr_world_move(&w, &g, 1, 0) == DR_MOVE_IRON);
    assert(!dr_rules_job_unlocked(&g, DR_JOB_IRON_MINER));   // 回家前不解锁
    // 逐步走回村庄(测试里直接置位坐标再踏入村庄格)
    g.hero_x = 31; g.hero_y = 30;
    assert(dr_world_move(&w, &g, -1, 0) == DR_MOVE_HOME);
    assert((g.flags & ((uint64_t)1u << DR_FLAG_IRON_MINE)) != 0);
    assert(dr_rules_job_unlocked(&g, DR_JOB_IRON_MINER));
}

static void test_eat_and_caps(void) {
    dr_world_t w;
    dr_world_gen(&w, 5);
    dr_game_t g;
    dr_game_init(&g, 5, 1000);
    g.res[DR_RES_FOOD] = 5;
    assert(dr_world_embark(&w, &g));
    g.armor_lv = 2;                        // 铁甲:HP 上限 10+15=25
    g.hero_hp = 3;
    assert(dr_world_eat(&g));              // +8 → 11(未到上限)
    assert(g.hero_hp == 11);
    g.hero_hp = 20;
    assert(dr_world_eat(&g));              // 钳到上限 25
    assert(g.hero_hp == 25);
    g.food = 0;
    assert(!dr_world_eat(&g));
    assert(dr_world_health_cap(&g) == 25);
    g.armor_lv = 3;
    assert(dr_world_health_cap(&g) == 45); // 钢甲 +35
}

static void test_danger(void) {
    dr_game_t g;
    dr_game_init(&g, 5, 1000);
    g.hero_x = 30; g.hero_y = 30;
    assert(!dr_world_danger(&g));
    g.hero_x = 38; g.hero_y = 30;          // 距村 8,无铁甲
    assert(dr_world_danger(&g));
    g.armor_lv = 2;                        // 铁甲
    assert(!dr_world_danger(&g));
    g.hero_x = 48;                         // 距村 18,需钢甲
    assert(dr_world_danger(&g));
    g.armor_lv = 3;
    assert(!dr_world_danger(&g));
}

int main(void) {
    test_gen();
    test_embark_and_supplies();
    test_thirst_death();
    test_outpost_and_mines();
    test_eat_and_caps();
    test_danger();
    printf("test_dr_world: all passed\n");
    return 0;
}
