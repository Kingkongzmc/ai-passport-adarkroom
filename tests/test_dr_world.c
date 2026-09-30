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

// 测试辅助:移动并自动清掉途中的遭遇战(位移已发生,战斗只影响断言口径)
static dr_move_result_t move_ok_or_fight(dr_world_t *w, dr_game_t *g,
                                         int dx, int dy) {
    dr_move_result_t r = dr_world_move(w, g, dx, dy);
    if (r == DR_MOVE_FIGHT) {
        while (dr_world_fight_active(w))
            (void)dr_world_fight_attack(w, g);
        return DR_MOVE_OK;
    }
    return r;
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
    assert(dr_world_outfit_add(&w, &g, DR_RES_FOOD, 10));   // 整备:带 10 口
    assert(dr_world_embark(&w, &g));
    assert(g.in_wilderness == 1);
    assert(g.res[DR_RES_FOOD] == 15);       // 带走 10(基础负重)
    assert(g.food == 10 && g.water == 10);
    assert(g.hero_x == 30 && g.hero_y == 30);
    assert(g.hero_hp == 10 && g.hero_hp_max == 10);
    assert(dr_world_seen(&w, 30, 30));

    // 移动 2 步:水 -2(每步 1),干肉 -1(每 2 步 1)
    assert(move_ok_or_fight(&w, &g, 1, 0) == DR_MOVE_OK);
    assert(g.water == 9);
    assert(move_ok_or_fight(&w, &g, 0, 1) == DR_MOVE_OK);
    assert(g.water == 8 && g.food == 9);

    // 走回村庄格 = 回家:4 步共吃 2 口干肉,余 8 口入库
    assert(move_ok_or_fight(&w, &g,  0, -1) == DR_MOVE_OK);
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
    assert(dr_world_outfit_add(&w, &g, DR_RES_FOOD, 10));
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
    assert(dr_world_outfit_add(&w, &g, DR_RES_FOOD, 10));
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
    assert(move_ok_or_fight(&w, &g, -1, 0) == DR_MOVE_OK);
    assert(move_ok_or_fight(&w, &g, 1, 0) == DR_MOVE_OK);   // 二访不补(仅移动耗水)
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
    assert(dr_world_outfit_add(&w, &g, DR_RES_FOOD, 5));
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

static void test_fight(void) {
    dr_world_t w;
    dr_world_gen(&w, 7);
    dr_game_t g;
    dr_game_init(&g, 7, 1000);
    g.res[DR_RES_FOOD] = 10;
    assert(dr_world_outfit_add(&w, &g, DR_RES_FOOD, 10));
    assert(dr_world_embark(&w, &g));
    g.weapon_lv = 2;                       // 铁剑 4 伤

    // 手工开局一场吼兽战斗(5 HP):铁剑两刀内结束,战利品入包
    w.fight_enemy = DR_ENEMY_BEAST;
    w.fight_hp = 5;
    w.fight_round = 0;
    int guard = 0;
    dr_fight_result_t r = DR_FIGHT_NONE;
    while (dr_world_fight_active(&w) && guard++ < 50)
        r = dr_world_fight_attack(&w, &g);
    assert(!dr_world_fight_active(&w));
    assert(r == DR_FIGHT_WIN);
    assert(dr_world_bag_weight(&g, &w) > 0);   // 吼兽必掉毛/肉
    assert(g.in_wilderness);                   // 行程未断

    // 用药:+20 但钳到上限(无甲 10)
    w.fight_enemy = DR_ENEMY_BEAST;
    w.fight_hp = 5;
    w.fight_round = 0;
    g.hero_hp = 2;
    w.carry_medicine = 1;
    (void)dr_world_fight_medicine(&w, &g);
    assert(w.carry_medicine == 0);              // 药已用
    assert(g.hero_hp == 10 || g.hero_hp == 9);  // 满血(钳上限),或被反击 1 点

    // 逃跑:结束战斗或被追击(确定性种子下的任意合法结果)
    w.fight_enemy = DR_ENEMY_BEAST;
    w.fight_hp = 5;
    w.fight_round = 0;
    r = dr_world_fight_flee(&w, &g);
    assert(r == DR_FIGHT_FLED || r == DR_FIGHT_ENEMY_HIT ||
           r == DR_FIGHT_LOSE || r == DR_FIGHT_ENEMY_MISS);
}

// ---- 地标生成与地点搜索(切片三) ----
static void test_locations(void) {
    dr_world_t w;
    dr_world_gen(&w, 99);
    int houses = 0, caves = 0, towns = 0, cities = 0;
    for (int y = 0; y < DR_WORLD_SIZE; y++)
        for (int x = 0; x < DR_WORLD_SIZE; x++) {
            uint8_t t = dr_world_tile(&w, x, y);
            if (t == DR_WT_HOUSE) houses++;
            if (t == DR_WT_CAVE)  caves++;
            if (t == DR_WT_TOWN)  towns++;
            if (t == DR_WT_CITY)  cities++;
        }
    // 期望 10/5/10/20;放置有环带尝试失败的可能,允许下限
    assert(houses >= 8 && caves >= 4 && towns >= 8 && cities >= 16);

    // 远征中踏上老屋格 → 地点态;搜索:药/补给/遭遇三选一
    dr_game_t g;
    dr_game_init(&g, 99, 1000);
    g.res[DR_RES_FOOD] = 10;
    assert(dr_world_outfit_add(&w, &g, DR_RES_FOOD, 10));
    assert(dr_world_embark(&w, &g));
    int hx = -1, hy = -1;
    for (int y = 0; y < DR_WORLD_SIZE && hx < 0; y++)
        for (int x = 0; x < DR_WORLD_SIZE; x++)
            if (dr_world_tile(&w, x, y) == DR_WT_HOUSE) { hx = x; hy = y; break; }
    g.hero_x = (uint8_t)hx;
    g.hero_y = (uint8_t)hy;
    w.tiles[hy * DR_WORLD_SIZE + hx] = DR_WT_HOUSE;   // 确保不被先前测试改写
    // 直接设地点态测搜索
    w.location = DR_WT_HOUSE;
    char line[64];
    dr_loc_result_t r = dr_world_location_search(&w, &g, line, sizeof(line));
    assert(r == DR_LOC_LOOT || r == DR_LOC_WATER || r == DR_LOC_FIGHT);
    if (r == DR_LOC_FIGHT)
        assert(dr_world_fight_active(&w));
    else
        assert(line[0] != '\0');   // 战利品摘要非空
    dr_world_location_leave(&w);
    assert(dr_world_location(&w) == 0);

    // 洞穴:无火把 → NEED_TORCH;有火把 → 消耗并出结果
    w.location = DR_WT_CAVE;
    g.flags &= ~(uint64_t)1u << DR_FLAG_TORCH;
    assert(dr_world_location_search(&w, &g, line, sizeof(line)) ==
           DR_LOC_NEED_TORCH);
    g.flags |= (uint64_t)1u << DR_FLAG_TORCH;
    r = dr_world_location_search(&w, &g, line, sizeof(line));
    assert(!(g.flags & ((uint64_t)1u << DR_FLAG_TORCH)));   // 火把已耗
    assert(r == DR_LOC_FIGHT || r == DR_LOC_LOOT);

    // 水袋/背囊:容量生效
    g.flags |= (uint64_t)1u << DR_FLAG_WATERSKIN;
    assert(dr_world_water_cap(&g) == 20);
    g.flags |= (uint64_t)1u << DR_FLAG_RUCKSACK;
    assert(dr_world_bag_cap(&g) == 200);
}

int main(void) {
    test_gen();
    test_embark_and_supplies();
    test_thirst_death();
    test_outpost_and_mines();
    test_eat_and_caps();
    test_danger();
    test_fight();
    test_locations();
    printf("test_dr_world: all passed\n");
    return 0;
}
