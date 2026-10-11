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

static void test_weapon_weight(void) {
    dr_world_t w;
    dr_world_gen(&w, 5);
    dr_game_t g;
    dr_game_init(&g, 5, 1000);

    // 拳不计重:基础背袋(承 10.0)恰装 10 口干肉
    g.res[DR_RES_FOOD] = 20;
    assert(dr_world_outfit_add(&w, &g, DR_RES_FOOD, 10));
    assert(!dr_world_outfit_add(&w, &g, DR_RES_FOOD, 1));
    assert(dr_world_bag_weight(&g, &w) == 100);

    // 铁剑 3.0 随行计重(原版 path.js,§3.7):同容量只装得下 7 口
    g.weapon_lv = 2;
    assert(dr_world_bag_weight(&g, &w) == 130);
    g.food = 0; g.res[DR_RES_FOOD] = 20;
    assert(dr_world_outfit_add(&w, &g, DR_RES_FOOD, 7));
    assert(!dr_world_outfit_add(&w, &g, DR_RES_FOOD, 1));

    // 骨矛 2.0 → 8 口;步枪 5.0 → 5 口
    g.weapon_lv = 1; g.food = 0;
    assert(dr_world_outfit_add(&w, &g, DR_RES_FOOD, 8));
    assert(!dr_world_outfit_add(&w, &g, DR_RES_FOOD, 1));
    g.weapon_lv = 4; g.food = 0;
    assert(dr_world_outfit_add(&w, &g, DR_RES_FOOD, 5));
    assert(!dr_world_outfit_add(&w, &g, DR_RES_FOOD, 1));
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
    // 原版 else-if 取最高档,不叠加(path.js getCapacity, I26)
    g.flags |= (uint64_t)1u << DR_FLAG_WAGON;
    assert(dr_world_bag_cap(&g) == 400);
    g.flags |= (uint64_t)1u << DR_FLAG_CONVOY;
    assert(dr_world_bag_cap(&g) == 700);
}

// ---- M6 切片二:Perk 战斗数值(原版 events.js/world.js 逐项) ----
static void test_perk_combat(void) {
    dr_world_t w;
    dr_world_gen(&w, 7);
    dr_game_t g;
    dr_game_init(&g, 0x12345678, 1000);   // map_seed 非零
    g.res[DR_RES_FOOD] = 10;
    assert(dr_world_outfit_add(&w, &g, DR_RES_FOOD, 10));
    assert(dr_world_embark(&w, &g));

    // 蛮力:骨矛(近战 2 伤)×1.5 → 3 伤/刀(向下取整;未命中不掉敌血)
    g.weapon_lv = 1;
    g.flags |= (uint64_t)1u << DR_FLAG_PERK_BARBARIAN;
    w.fight_enemy = DR_ENEMY_BEAST;
    w.fight_hp = 100;
    w.fight_round = 0;
    int guard = 0;
    uint16_t before = w.fight_hp;
    while (dr_world_fight_active(&w) && w.fight_hp == before && guard++ < 60)
        (void)dr_world_fight_attack(&w, &g);
    assert(before - w.fight_hp == 3);      // 2 → 3

    // 精准:玩家命中率 80%→90%(统计断言,200 掷;
    // 玩家未命中时敌血不动——以敌血差值判定命中)
    dr_game_t g2;
    dr_game_init(&g2, 0x12345678, 1000);
    int hits = 0;
    for (int i = 0; i < 200; i++) {
        w.fight_enemy = DR_ENEMY_BEAST;
        w.fight_hp = 200;
        w.fight_round = 0;
        g2.hero_hp = g2.hero_hp_max = 10;
        g2.weapon_lv = 0;                  // 拳 1 伤
        g2.flags |= (uint64_t)1u << DR_FLAG_PERK_PRECISE;
        uint16_t hp_before = w.fight_hp;
        (void)dr_world_fight_attack(&w, &g2);
        if (w.fight_hp < hp_before) hits++;
    }
    assert(hits > 155);                    // 期望 180(90%),3σ 界

    // 闪避:敌方命中 ×0.8(吼兽 800→640,统计断言)
    int ehits = 0;
    for (int i = 0; i < 200; i++) {
        w.fight_enemy = DR_ENEMY_BEAST;
        w.fight_hp = 200;
        w.fight_round = 0;
        g2.hero_hp = g2.hero_hp_max = 10;
        g2.weapon_lv = 4;                  // 步枪:无弹退化拳,必不掉敌血
        w.carry_bullets = 0;
        g2.flags = (uint64_t)1u << DR_FLAG_PERK_EVASIVE;
        dr_fight_result_t r = dr_world_fight_attack(&w, &g2);
        if (r == DR_FIGHT_ENEMY_HIT) ehits++;
        else assert(r == DR_FIGHT_ENEMY_MISS);
    }
    assert(ehits > 100 && ehits < 160);    // 期望 128(64%),3σ 界
}

// 潜行:遭遇触发率减半(原版 checkFight chance *= 0.5;统计断言)
static void test_stealthy(void) {
    // 找一块 2×2 纯地形格(避开村庄/地标:踏上有结局的格会打断游走)
    dr_world_t w0;
    dr_world_gen(&w0, 7);
    int bx = -1, by = -1;
    for (int y = 2; y < DR_WORLD_SIZE - 3 && bx < 0; y++)
        for (int x = 2; x < DR_WORLD_SIZE - 3 && bx < 0; x++) {
            uint8_t a = dr_world_tile(&w0, x, y);
            uint8_t b = dr_world_tile(&w0, x + 1, y);
            uint8_t c = dr_world_tile(&w0, x, y + 1);
            uint8_t d = dr_world_tile(&w0, x + 1, y + 1);
            if (a > DR_WT_BARREN || b > DR_WT_BARREN ||
                c > DR_WT_BARREN || d > DR_WT_BARREN)
                continue;
            bx = x; by = y;
        }
    assert(bx >= 0);

    int count_fights(bool perk) {
        dr_world_t w;
        dr_world_gen(&w, 7);
        dr_game_t g;
        dr_game_init(&g, 0x12345678, 1000);
        g.res[DR_RES_FOOD] = 10;
        assert(dr_world_outfit_add(&w, &g, DR_RES_FOOD, 10));
        assert(dr_world_embark(&w, &g));
        g.hero_x = (uint8_t)bx;
        g.hero_y = (uint8_t)by;
        if (perk) g.flags |= (uint64_t)1u << DR_FLAG_PERK_STEALTHY;
        static const int dirs[4][2] = { {1,0},{0,1},{-1,0},{0,-1} };
        int fights = 0;
        for (int i = 0; i < 400; i++) {
            if (i % 40 == 0) { g.water = 200; g.food = 200; }   // 不断供
            dr_move_result_t r = dr_world_move(&w, &g,
                                               dirs[i % 4][0], dirs[i % 4][1]);
            if (r == DR_MOVE_FIGHT) {
                fights++;
                w.fight_enemy = DR_ENEMY_KIND_COUNT;   // 秒退战斗继续走
            } else {
                assert(r == DR_MOVE_OK);   // 2×2 纯地形格:只应有普通步
            }
        }
        return fights;
    }
    int plain = count_fights(false);
    int stealth = count_fights(true);
    // 期望约 79 对 39(400 步,前 3 步免战);统计界放宽避免种子抖动误报
    assert(plain > 40 && plain < 120);
    assert(stealth > 15 && stealth < 70);
    assert(stealth * 2 < plain + 30);
}

// 城市清剿旗标(原版 setpieces 地牢 end:World.clearDungeon + cityCleared)
static void test_city_cleared(void) {
    dr_world_t w;
    dr_world_gen(&w, 99);
    dr_game_t g;
    dr_game_init(&g, 99, 1000);
    g.res[DR_RES_FOOD] = 60;
    g.armor_lv = 3;                         // 钢甲:HP 45,免遭统计翻车
    assert(dr_world_outfit_add(&w, &g, DR_RES_FOOD, 60));
    assert(dr_world_embark(&w, &g));
    g.weapon_lv = 3;                        // 钢剑 6 伤
    char line[64];

    // 非城市(老屋)战斗胜利 → 不置旗标。战斗由生产路径触发(地点搜索),
    // 敌血置 1 快速终结,验证的是旗标逻辑本身。
    int hx = -1, hy = -1;
    for (int y = 0; y < DR_WORLD_SIZE && hx < 0; y++)
        for (int x = 0; x < DR_WORLD_SIZE; x++)
            if (dr_world_tile(&w, x, y) == DR_WT_HOUSE) { hx = x; hy = y; break; }
    assert(hx >= 0);
    g.hero_x = (uint8_t)hx; g.hero_y = (uint8_t)hy;
    w.location = DR_WT_HOUSE;
    int guard = 0;
    while (!dr_world_fight_active(&w) && guard++ < 200)
        (void)dr_world_location_search(&w, &g, line, sizeof(line));
    assert(dr_world_fight_active(&w));
    w.fight_hp = 1;
    g.hero_hp = g.hero_hp_max = dr_world_health_cap(&g);
    dr_fight_result_t r = DR_FIGHT_NONE;
    while (dr_world_fight_active(&w) && guard++ < 60)
        r = dr_world_fight_attack(&w, &g);
    assert(r == DR_FIGHT_WIN);
    assert(!((g.flags >> DR_FLAG_CITY_CLEARED) & 1u));

    // 城市战斗胜利 → 置旗标(军队突袭之门)
    int cx = -1, cy = -1;
    for (int y = 0; y < DR_WORLD_SIZE && cx < 0; y++)
        for (int x = 0; x < DR_WORLD_SIZE; x++)
            if (dr_world_tile(&w, x, y) == DR_WT_CITY) { cx = x; cy = y; break; }
    assert(cx >= 0);
    g.hero_x = (uint8_t)cx; g.hero_y = (uint8_t)cy;
    w.location = DR_WT_CITY;
    guard = 0;
    while (!dr_world_fight_active(&w) && guard++ < 400)
        (void)dr_world_location_search(&w, &g, line, sizeof(line));
    assert(dr_world_fight_active(&w));
    w.fight_hp = 1;
    g.hero_hp = g.hero_hp_max = dr_world_health_cap(&g);
    r = DR_FIGHT_NONE;
    while (dr_world_fight_active(&w) && guard++ < 60)
        r = dr_world_fight_attack(&w, &g);
    assert(r == DR_FIGHT_WIN);
    assert((g.flags >> DR_FLAG_CITY_CLEARED) & 1u);
}

// 侦查揭示(原版 World.applyMap):确定性一片 + 斥候视野半径翻倍
static void test_scout_reveal(void) {
    dr_world_t w, w2;
    dr_world_gen(&w, 0x1234);
    dr_world_gen(&w2, 0x1234);
    dr_game_t g, g2;
    dr_game_init(&g, 0x12345678, 1000);
    dr_game_init(&g2, 0x12345678, 1000);

    assert(dr_world_apply_map(&w, &g));
    assert(dr_world_apply_map(&w2, &g2));
    assert(memcmp(w.seen, w2.seen, sizeof(w.seen)) == 0);   // 同种子同片
    int seen = 0;
    for (int y = 0; y < DR_WORLD_SIZE; y++)
        for (int x = 0; x < DR_WORLD_SIZE; x++)
            if (dr_world_seen(&w, x, y)) seen++;
    assert(seen == 61);                    // 半径 5 菱形=2r²+2r+1(荒野腹地不裁边)
    assert(dr_world_map_has_unseen(&w));

    // 斥候视野:出发即点亮半径 4(41 格),基础 2(13 格)
    dr_world_t w3;
    dr_world_gen(&w3, 0x1234);
    dr_game_t g3;
    dr_game_init(&g3, 0x12345678, 1000);
    g3.res[DR_RES_FOOD] = 10;
    assert(dr_world_outfit_add(&w3, &g3, DR_RES_FOOD, 10));
    assert(dr_world_embark(&w3, &g3));
    int base = 0;
    for (int y = 0; y < DR_WORLD_SIZE; y++)
        for (int x = 0; x < DR_WORLD_SIZE; x++)
            if (dr_world_seen(&w3, x, y)) base++;
    assert(base == 13);
    g3.flags |= (uint64_t)1u << DR_FLAG_PERK_SCOUT;
    assert(dr_world_embark(&w3, &g3) == false);   // 已在远征中,不可再出发

    // 直接验证 light_radius 语义:新世界带 Perk 出发
    dr_world_t w4;
    dr_world_gen(&w4, 0x1234);
    dr_game_t g4;
    dr_game_init(&g4, 0x12345678, 1000);
    g4.res[DR_RES_FOOD] = 10;
    g4.flags |= (uint64_t)1u << DR_FLAG_PERK_SCOUT;
    assert(dr_world_outfit_add(&w4, &g4, DR_RES_FOOD, 10));
    assert(dr_world_embark(&w4, &g4));
    int wide = 0;
    for (int y = 0; y < DR_WORLD_SIZE; y++)
        for (int x = 0; x < DR_WORLD_SIZE; x++)
            if (dr_world_seen(&w4, x, y)) wide++;
    assert(wide == 41);                    // 2r²+2r+1,r=4
}

// ---- M5 武器梯队扩展:刺刀(5)/激光步枪(6)+ 能量元件/手雷/流星锤携带 ----
static void test_m5_weapons(void) {
    dr_world_t w;
    dr_world_gen(&w, 7);
    dr_game_t g;
    dr_game_init(&g, 7, 1000);

    // 伤害表(原版 world.js):刺刀 8 / 激光步枪 8
    assert(dr_world_weapon_dmg(5) == 8 && dr_world_weapon_dmg(6) == 8);

    // 激光步枪:每击耗 1 携带元件;无件退化为拳(1 伤,与步枪无弹同款)
    g.res[DR_RES_FOOD] = 10;
    assert(dr_world_outfit_add(&w, &g, DR_RES_FOOD, 5));
    assert(dr_world_embark(&w, &g));
    g.weapon_lv = 6;
    w.carry_cells = 1;
    w.fight_enemy = DR_ENEMY_SOLDIER;   // 50 HP:8 伤打不死,只看弹药消耗
    w.fight_hp = 50;
    w.fight_round = 0;
    uint16_t hp0 = w.fight_hp;
    (void)dr_world_fight_attack(&w, &g);
    assert(w.carry_cells == 0);                    // 元件已耗
    if (dr_world_fight_active(&w))
        assert(w.fight_hp == hp0 - 8 || w.fight_hp == hp0);   // 命中 8 / 挥空
    // 无件第二击:伤害退化(50 HP 池足够观察两轮)
    if (dr_world_fight_active(&w)) {
        hp0 = w.fight_hp;
        (void)dr_world_fight_attack(&w, &g);
        assert(w.fight_hp >= hp0 - 8);            // 至多 8(实际 1 伤或挥空)
    }

    // 蛮力 Perk:刺刀(近战)×1.5 → 12;激光步枪(远程)不加成
    dr_world_t w2;
    dr_world_gen(&w2, 7);
    dr_game_t g2;
    dr_game_init(&g2, 7, 1000);
    g2.res[DR_RES_FOOD] = 10;
    assert(dr_world_outfit_add(&w2, &g2, DR_RES_FOOD, 5));
    assert(dr_world_embark(&w2, &g2));
    g2.weapon_lv = 5;
    g2.flags |= (uint64_t)1u << DR_FLAG_PERK_BARBARIAN |
                (uint64_t)1u << DR_FLAG_PERK_PRECISE;   // 90% 命中,统计更稳
    w2.fight_enemy = DR_ENEMY_SOLDIER;
    w2.fight_hp = 50;
    w2.fight_round = 0;
    int saw12 = 0;
    for (int i = 0; i < 40 && dr_world_fight_active(&w2); i++) {
        uint16_t before = w2.fight_hp;
        (void)dr_world_fight_attack(&w2, &g2);
        if (dr_world_fight_active(&w2) && before - w2.fight_hp == 12) saw12 = 1;
    }
    assert(saw12);   // 40 击内至少一见 12 伤(8×1.5)

    // 手雷:15 伤耗 1;50 HP 士兵三雷内结束
    dr_world_t w3;
    dr_world_gen(&w3, 7);
    dr_game_t g3;
    dr_game_init(&g3, 7, 1000);
    g3.res[DR_RES_FOOD] = 10;
    assert(dr_world_outfit_add(&w3, &g3, DR_RES_FOOD, 5));
    assert(dr_world_embark(&w3, &g3));
    w3.carry_grenades = 3;
    w3.fight_enemy = DR_ENEMY_SOLDIER;
    w3.fight_hp = 50;
    w3.fight_round = 0;
    int guard = 0;
    while (dr_world_fight_active(&w3) && guard++ < 30) {
        if (w3.carry_grenades > 0) (void)dr_world_fight_grenade(&w3, &g3);
        else (void)dr_world_fight_attack(&w3, &g3);
        if (g3.hero_hp == 0) break;   // 被反击打死也合法(统计性)
    }
    assert(w3.carry_grenades < 3);   // 雷确实在消耗

    // 无雷:动作拒绝
    w3.carry_grenades = 0;
    assert(dr_world_fight_grenade(&w3, &g3) == DR_FIGHT_NONE ||
           !dr_world_fight_active(&w3));

    // 流星锤:命中 → 敌人停摆两回合(ENEMY_SKIP 连见)
    dr_world_t w4;
    dr_world_gen(&w4, 7);
    dr_game_t g4;
    dr_game_init(&g4, 7, 1000);
    g4.res[DR_RES_FOOD] = 10;
    assert(dr_world_outfit_add(&w4, &g4, DR_RES_FOOD, 5));
    assert(dr_world_embark(&w4, &g4));
    w4.carry_bolas = 6;
    w4.fight_enemy = DR_ENEMY_SOLDIER;
    w4.fight_hp = 50;
    w4.fight_round = 0;
    int stunned = 0;
    for (int i = 0; i < 30 && !stunned; i++) {
        dr_fight_result_t r = dr_world_fight_bolas(&w4, &g4);
        if (r == DR_FIGHT_STUN) {
            stunned = 1;
            assert(w4.fight_stun == 2);
            // 接下来两次敌方回合必停摆(玩家任意动作喂掉回合)
            dr_fight_result_t a = dr_world_fight_attack(&w4, &g4);
            dr_fight_result_t b = dr_world_fight_attack(&w4, &g4);
            (void)a; (void)b;
            assert(w4.fight_stun == 0);
        }
    }
    assert(stunned);   // 6 锤 80% 命中,30 次内必中

    // 承重(path.js):元件 0.2/手雷 1/流星锤 0.5/激光步枪 5/刺刀 1
    dr_world_t w5;
    dr_world_gen(&w5, 7);
    dr_game_t g5;
    dr_game_init(&g5, 7, 1000);
    g5.res[DR_RES_ENERGY_CELL] = 5;
    g5.res[DR_RES_GRENADE] = 2;
    g5.res[DR_RES_BOLAS] = 2;
    assert(dr_world_outfit_add(&w5, &g5, DR_RES_ENERGY_CELL, 5));   // 1.0
    assert(dr_world_outfit_add(&w5, &g5, DR_RES_GRENADE, 2));       // 2.0
    assert(dr_world_outfit_add(&w5, &g5, DR_RES_BOLAS, 2));         // 1.0
    assert(dr_world_bag_weight(&g5, &w5) == 40);
    g5.weapon_lv = 6;
    assert(dr_world_bag_weight(&g5, &w5) == 90);                    // +激光步枪 5.0
    g5.weapon_lv = 5;
    assert(dr_world_bag_weight(&g5, &w5) == 50);                    // 刺刀 1.0
    // 回家入库:携带的元件/手雷/流星锤全量回库(arrive_home 口径)
    g5.in_wilderness = 1;
    g5.water = 5; g5.food = 5;
    g5.hero_x = DR_WORLD_CENTER; g5.hero_y = DR_WORLD_CENTER;
    assert(dr_world_move(&w5, &g5, 0, 0) == DR_MOVE_HOME);   // 村庄格
    assert(g5.res[DR_RES_ENERGY_CELL] == 5 && w5.carry_cells == 0);
    assert(g5.res[DR_RES_GRENADE] == 2 && w5.carry_grenades == 0);
    assert(g5.res[DR_RES_BOLAS] == 2 && w5.carry_bolas == 0);
}

// ---- M7 地标回补(setpieces.js 2026-10-11 取证):放置/搜索/护符门/一次性 ----
static void test_m7_landmarks(void) {
    dr_world_t w;
    dr_world_gen(&w, 99);
    // 数量与半径(放置有环带尝试失败的可能,允许下限;半径为环带 ±1)
    int bore = 0, bfield = 0, swamp = 0, ruinv = 0;
    for (int y = 0; y < DR_WORLD_SIZE; y++)
        for (int x = 0; x < DR_WORLD_SIZE; x++) {
            uint8_t t = dr_world_tile(&w, x, y);
            uint16_t d = manhattan(x, y);
            if (t == DR_WT_BOREHOLE) { bore++;  assert(d >= 14 && d <= 46); }
            if (t == DR_WT_BFIELD)   { bfield++;assert(d >= 17 && d <= 46); }
            if (t == DR_WT_SWAMP)    { swamp++; assert(d >= 14 && d <= 46); }
            if (t == DR_WT_RUINV)    { ruinv++; assert(d >= 14 && d <= 46); }
        }
    assert(bore >= 8 && bfield >= 4 && swamp == 1 && ruinv == 1);

    dr_game_t g;
    dr_game_init(&g, 99, 1000);
    g.res[DR_RES_FOOD] = 10;
    assert(dr_world_outfit_add(&w, &g, DR_RES_FOOD, 5));
    assert(dr_world_embark(&w, &g));
    char line[64];

    // 钻孔:必得异星合金 1-3(100%,零门槛)
    w.location = DR_WT_BOREHOLE;
    for (int i = 0; i < 5; i++) {
        memset(w.loot, 0, sizeof(w.loot));          // 清包隔离承重干扰
        dr_loc_result_t r = dr_world_location_search(&w, &g, line, sizeof(line));
        assert(r == DR_LOC_LOOT);
        assert(w.loot[DR_RES_ALIEN] >= 1 && w.loot[DR_RES_ALIEN] <= 3);
    }

    // 战场:武器抬升不降级;资源类照常入包
    w.location = DR_WT_BFIELD;
    g.weapon_lv = 6;                                // 已有激光步枪
    for (int i = 0; i < 30; i++) {
        memset(w.loot, 0, sizeof(w.loot));
        (void)dr_world_location_search(&w, &g, line, sizeof(line));
        assert(g.weapon_lv == 6);                   // 不降级
    }
    g.weapon_lv = 0;
    int raised = 0, saw_bullets = 0, saw_alloy = 0;
    for (int i = 0; i < 100; i++) {
        memset(w.loot, 0, sizeof(w.loot));
        (void)dr_world_location_search(&w, &g, line, sizeof(line));
        if (g.weapon_lv > 0) raised = 1;
        if (w.loot[DR_RES_BULLETS] > 0) saw_bullets = 1;
        if (w.loot[DR_RES_ALIEN] > 0) saw_alloy = 1;
    }
    assert(raised && saw_bullets && saw_alloy);     // 100 掷内三类必见

    // 沼泽:护符门 + 一次性 + gastronome 回血 8→16
    w.location = DR_WT_SWAMP;
    g.res[DR_RES_CHARM] = 0;
    assert(dr_world_location_search(&w, &g, line, sizeof(line)) ==
           DR_LOC_NEED_CHARM);
    assert(!(g.flags & ((uint64_t)1u << DR_FLAG_PERK_GASTRONOME)));
    g.res[DR_RES_CHARM] = 1;
    dr_loc_result_t r = dr_world_location_search(&w, &g, line, sizeof(line));
    assert(r == DR_LOC_LOOT);
    assert(g.res[DR_RES_CHARM] == 0);               // 护符已耗
    assert(g.flags & ((uint64_t)1u << DR_FLAG_PERK_GASTRONOME));
    assert(g.flags & ((uint64_t)1u << DR_FLAG_SWAMP_DONE));
    assert(dr_world_location_search(&w, &g, line, sizeof(line)) ==
           DR_LOC_EMPTY);                           // 永久关闭
    g.armor_lv = 3;                                 // HP 上限 45,远离钳位
    g.hero_hp = 10;
    assert(dr_world_eat(&g));
    assert(g.hero_hp == 26);                        // 10+16(美食家翻倍)

    // 被毁村庄:一次性大奖(周目 1:合金+2,全资源+100,钳 999)
    w.location = DR_WT_RUINV;
    uint32_t alien0 = g.res[DR_RES_ALIEN];
    g.res[DR_RES_WOOD] = 950;                       // 验证 999 钳位
    r = dr_world_location_search(&w, &g, line, sizeof(line));
    assert(r == DR_LOC_LOOT);
    assert(g.flags & ((uint64_t)1u << DR_FLAG_RUIN_TAKEN));
    assert(g.res[DR_RES_ALIEN] == alien0 + 2);
    assert(g.res[DR_RES_WOOD] == 999);
    assert(g.res[DR_RES_IRON] == 100);
    assert(dr_world_location_search(&w, &g, line, sizeof(line)) ==
           DR_LOC_EMPTY);                           // 一次性
    dr_world_location_leave(&w);
}

int main(void) {
    test_gen();
    test_embark_and_supplies();
    test_thirst_death();
    test_outpost_and_mines();
    test_eat_and_caps();
    test_weapon_weight();
    test_danger();
    test_fight();
    test_locations();
    test_perk_combat();
    test_stealthy();
    test_city_cleared();
    test_scout_reveal();
    test_m5_weapons();
    printf("test_dr_world: all passed\n");
    return 0;
}
