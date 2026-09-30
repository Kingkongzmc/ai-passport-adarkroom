// tests/test_dr_rules.c —— 规则层主机测试(对齐原版口径):
// 建筑造价/火焰/温度/建造者剧情/采集/陷阱六档/职业收入/流浪者/贸易/离线。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dr_rules.h"
#include "dr_state.h"
#include "dr_util.h"

// ---- 建筑造价(原版 Craftables) ----
static void test_building_costs(void) {
    dr_bld_cost_t c;
    c = dr_building_cost(DR_BLD_TRAP, 0);        assert(c.wood == 10 && !c.fur && !c.meat);
    c = dr_building_cost(DR_BLD_TRAP, 3);        assert(c.wood == 40);
    c = dr_building_cost(DR_BLD_TRAP, 10);       assert(c.wood == 0xFFFFFFFFu);  // max10
    c = dr_building_cost(DR_BLD_CART, 0);        assert(c.wood == 30);
    c = dr_building_cost(DR_BLD_CART, 1);        assert(c.wood == 0xFFFFFFFFu);
    c = dr_building_cost(DR_BLD_HUT, 0);         assert(c.wood == 100);
    c = dr_building_cost(DR_BLD_HUT, 2);         assert(c.wood == 200);
    c = dr_building_cost(DR_BLD_HUT, 20);        assert(c.wood == 0xFFFFFFFFu);
    c = dr_building_cost(DR_BLD_LODGE, 0);       assert(c.wood == 200 && c.fur == 10 && c.meat == 5);
    c = dr_building_cost(DR_BLD_TRADE_POST, 0);  assert(c.wood == 400 && c.fur == 100);
    c = dr_building_cost(DR_BLD_TANNERY, 0);     assert(c.wood == 500 && c.fur == 50);
    c = dr_building_cost(DR_BLD_SMOKEHOUSE, 0);  assert(c.wood == 600 && c.meat == 50);
    c = dr_building_cost(DR_BLD_STEELWORKS, 0);
    assert(c.wood == 1500 && c.iron == 100 && c.coal == 100);
    c = dr_building_cost(DR_BLD_STEELWORKS, 1);  assert(c.wood == 0xFFFFFFFFu);
    c = dr_building_cost(DR_BLD_ARMOURY, 0);
    assert(c.wood == 3000 && c.steel == 100 && c.sulphur == 50);
}

// 建造门槛:建造者已帮忙 + 木材过半 + 材料见过;施工需室温>冷
static void test_build_gates(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.res[DR_RES_WOOD] = 1000;
    assert(!dr_rules_can_build(&g, DR_BLD_TRAP));   // 建造者未恢复
    g.builder_lv = DR_BUILDER_HELP;
    assert(dr_rules_can_build(&g, DR_BLD_TRAP));

    // 木材过半门槛:陷阱首件 10 木,5 木即可见
    g.res[DR_RES_WOOD] = 5;
    assert(dr_rules_can_build(&g, DR_BLD_TRAP));
    g.res[DR_RES_WOOD] = 4;
    assert(!dr_rules_can_build(&g, DR_BLD_TRAP));

    // 材料见过:猎人小屋要毛 10>0 且肉 5>0 才可见
    g.res[DR_RES_WOOD] = 1000;
    g.res[DR_RES_FUR] = 0; g.res[DR_RES_MEAT] = 5;
    assert(!dr_rules_can_build(&g, DR_BLD_LODGE));
    g.res[DR_RES_FUR] = 1;
    assert(dr_rules_can_build(&g, DR_BLD_LODGE));

    // 施工需室温>冷(原版:她正打寒战没法帮忙)
    g.res[DR_RES_WOOD] = 1000; g.res[DR_RES_FUR] = 10; g.res[DR_RES_MEAT] = 5;
    g.temp_lv = DR_TEMP_COLD;
    assert(!dr_rules_build(&g, DR_BLD_LODGE, 1000));
    g.temp_lv = DR_TEMP_MILD;
    assert(dr_rules_build(&g, DR_BLD_LODGE, 1000));
    assert(g.building_lv[DR_BLD_LODGE] == 1);
    assert(g.res[DR_RES_WOOD] == 800 && g.res[DR_RES_FUR] == 0 && g.res[DR_RES_MEAT] == 0);
    assert(g.population == 0);                      // 建房不加人口(原版)
}

// ---- 火焰:点火 5 木直达旺盛;添柴 1 木 +1 档;降档时建造者代添 ----
static void test_fire(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.res[DR_RES_WOOD] = 10;
    assert(dr_rules_stoke_fire(&g, 1000));
    assert(g.fire_lv == DR_FIRE_BURNING && g.res[DR_RES_WOOD] == 5);
    assert(dr_rules_stoke_fire(&g, 1000));
    assert(g.fire_lv == DR_FIRE_ROARING && g.res[DR_RES_WOOD] == 4);
    assert(dr_rules_stoke_fire(&g, 1000));           // 封顶炽烈仍耗 1 木
    assert(g.fire_lv == DR_FIRE_ROARING && g.res[DR_RES_WOOD] == 3);

    // 降档:无建造者 → 常规 -1;火焰首次 ≥跳动同时触发陌生人入场(叙事优先)
    dr_rules_rt_t rt;
    dr_rules_rt_init(&rt, &g, 0);
    dr_rules_event_t ev; uint16_t arg;
    assert(dr_rules_tick(&rt, &g, 300u * 1000u, &ev, &arg));
    assert(g.fire_lv == DR_FIRE_BURNING && ev == DR_RT_EV_BUILDER_IN);

    // 建造者帮忙 + 火将熄 + 有木 → 先代添 1 木再降(净持平)
    g.builder_lv = DR_BUILDER_HELP;
    rt.forest_unlock_ms = 0;   // 剧情线已过(手动置 HELP),关掉待解锁
    g.res[DR_RES_WOOD] = 5;
    g.fire_lv = DR_FIRE_FLICKERING;
    rt.fire_deadline_ms = 600u * 1000u;
    assert(dr_rules_tick(&rt, &g, 600u * 1000u, &ev, &arg));
    assert(ev == DR_RT_EV_BUILDER_STOKE);
    assert(g.fire_lv == DR_FIRE_FLICKERING);          // +1(代添) 后 -1
    assert(g.res[DR_RES_WOOD] == 6);                  // 5-1(代添) +2(建造者当 tick 收入)
}

// ---- 温度:每 30s 向火焰档移动一步 ----
static void test_temperature(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.fire_lv = DR_FIRE_ROARING;
    dr_rules_rt_t rt;
    dr_rules_rt_init(&rt, &g, 0);
    dr_rules_event_t ev; uint16_t arg;
    for (int i = 1; i <= 4; i++) {
        (void)dr_rules_tick(&rt, &g, (uint32_t)(i * 31) * 1000u, &ev, &arg);
        assert(g.temp_lv == (uint8_t)i);              // 冻结→热,逐步
    }
    g.fire_lv = DR_FIRE_DEAD;
    (void)dr_rules_tick(&rt, &g, 130u * 1000u, &ev, &arg);
    assert(g.temp_lv == DR_TEMP_HOT);                 // 周期未到(下次 154s)不动
    (void)dr_rules_tick(&rt, &g, 160u * 1000u, &ev, &arg);
    assert(g.temp_lv == DR_TEMP_WARM);                // 开始回落
}

// ---- 建造者剧情:火≥跳动入场 → 15s 森林解锁(木=4) → 室温暖后三态恢复 ----
static void test_builder_arc(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.res[DR_RES_WOOD] = 100;
    dr_rules_rt_t rt;
    dr_rules_rt_init(&rt, &g, 0);
    dr_rules_event_t ev; uint16_t arg;

    // 火到"跳动" → 陌生人晕倒
    g.fire_lv = DR_FIRE_FLICKERING;
    assert(dr_rules_tick(&rt, &g, 1000, &ev, &arg));
    assert(g.builder_lv == DR_BUILDER_DOWN && ev == DR_RT_EV_BUILDER_IN);

    // 15s 后:森林解锁,木材被置为 4(原版 unlockForest)
    (void)dr_rules_tick(&rt, &g, 16000, &ev, &arg);
    assert(ev == DR_RT_EV_FOREST);
    assert((g.flags & ((uint64_t)1u << DR_FLAG_FOREST)) != 0);
    assert(g.res[DR_RES_WOOD] == 4);

    // 室温不够:状态不推进
    g.temp_lv = DR_TEMP_MILD;
    (void)dr_rules_tick(&rt, &g, 60000, &ev, &arg);
    assert(g.builder_lv == DR_BUILDER_DOWN);

    // 室温暖(火保持旺盛,否则调温会把室温拉回去):每 30s 一态 → 发抖 → 沉睡
    // (沉睡→帮忙不走定时器:原版在玩家回到房间时触发,见 dr_rules_builder_visit)
    g.fire_lv = DR_FIRE_BURNING;
    g.temp_lv = DR_TEMP_WARM;
    (void)dr_rules_tick(&rt, &g, 91000, &ev, &arg);
    assert(g.builder_lv == DR_BUILDER_SHIVER && ev == DR_RT_EV_BUILDER_SHIVER);
    (void)dr_rules_tick(&rt, &g, 121000, &ev, &arg);
    assert(g.builder_lv == DR_BUILDER_SLEEP && ev == DR_RT_EV_BUILDER_SLEEP);
    (void)dr_rules_tick(&rt, &g, 151000, &ev, &arg);   // 再等一态:仍是沉睡
    assert(g.builder_lv == DR_BUILDER_SLEEP && ev == DR_RT_EV_NONE);
    // 拜访房间(回小屋页):沉睡 → 帮忙;其他状态拜访无效
    assert(dr_rules_builder_visit(&g));
    assert(g.builder_lv == DR_BUILDER_HELP);
    assert(!dr_rules_builder_visit(&g));
}

// ---- 采集:+10 木;板车后 +50(原版 gatherWood) ----
static void test_gather(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    dr_rules_gather(&g, 1000);
    assert(g.res[DR_RES_WOOD] == 25 && g.res_total[DR_RES_WOOD] == 25);
    g.building_lv[DR_BLD_CART] = 1;
    dr_rules_gather(&g, 1000);
    assert(g.res[DR_RES_WOOD] == 75 && g.res_total[DR_RES_WOOD] == 75);
}

// ---- 陷阱:90s 冷却;每陷阱必得 1 件六档表;饵自动 min(饵,陷阱数) ----
static void test_trap(void) {
    dr_game_t g;
    dr_game_init(&g, 7, 1000);
    g.building_lv[DR_BLD_TRAP] = 2;   // 2 陷阱
    dr_rules_rt_t rt;
    dr_rules_rt_init(&rt, &g, 0);
    dr_trap_yield_t y;

    // 冷却未到
    assert(!dr_rules_trap_ready(&rt, &g, 89u * 1000u));
    assert(!dr_rules_trap_check(&rt, &g, 89u * 1000u, &y));

    // 首查:2 陷阱 + 3 饵 → 耗 2 饵,共 4 件
    g.res[DR_RES_BAIT] = 3;
    assert(dr_rules_trap_check(&rt, &g, 90u * 1000u, &y));
    assert(y.bait == 2 && g.res[DR_RES_BAIT] == 1);
    assert(y.fur + y.meat + y.scales + y.teeth + y.cloth + y.charm == 4);

    // 统计:400 次结算(饵已清,每次恰好 2 件),各档占比贴近原版表
    g.res[DR_RES_BAIT] = 0;
    uint32_t cnt[6] = {0};
    for (uint32_t k = 1; k <= 400; k++) {
        assert(dr_rules_trap_check(&rt, &g, 90u * 1000u + k * 90u * 1000u, &y));
        assert(y.fur + y.meat + y.scales + y.teeth + y.cloth + y.charm == 2);
        cnt[0] += y.fur; cnt[1] += y.meat; cnt[2] += y.scales;
        cnt[3] += y.teeth; cnt[4] += y.cloth; cnt[5] += y.charm;
    }
    // 800 件:毛期望 400±3σ≈±25;肉期望 200±3σ≈±22;杂项期望 200
    assert(cnt[0] >= 360 && cnt[0] <= 440);
    assert(cnt[1] >= 155 && cnt[1] <= 245);
    assert(cnt[2] + cnt[3] + cnt[4] + cnt[5] >= 140 &&
           cnt[2] + cnt[3] + cnt[4] + cnt[5] <= 260);
    assert(g.rng_seed_state == rt.rng.s);     // 序列回写档

    // 确定性:同种子同起点,逐次结果完全一致
    dr_game_t g2;
    dr_game_init(&g2, 7, 1000);
    g2.building_lv[DR_BLD_TRAP] = 2;
    dr_rules_rt_t rt2;
    dr_rules_rt_init(&rt2, &g2, 0);
    dr_game_t g3;
    dr_game_init(&g3, 7, 1000);
    g3.building_lv[DR_BLD_TRAP] = 2;
    dr_rules_rt_t rt3;
    dr_rules_rt_init(&rt3, &g3, 0);
    for (uint32_t k = 0; k < 10; k++) {
        dr_trap_yield_t ya, yb;
        dr_rules_trap_check(&rt2, &g2, (90u + k * 90u) * 1000u, &ya);
        dr_rules_trap_check(&rt3, &g3, (90u + k * 90u) * 1000u, &yb);
        assert(memcmp(&ya, &yb, sizeof(ya)) == 0);
    }
    assert(memcmp(&g2, &g3, sizeof(g2)) == 0);
}

// ---- 职业:解锁门/分配/闲人 ----
static void test_jobs(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.population = 4;
    assert(dr_rules_job_idle(&g) == 4);
    assert(!dr_rules_job_unlocked(&g, DR_JOB_HUNTER));     // 需猎人小屋
    assert(!dr_rules_job_assign(&g, DR_JOB_HUNTER, 1));
    g.building_lv[DR_BLD_LODGE] = 1;
    assert(dr_rules_job_unlocked(&g, DR_JOB_HUNTER));
    assert(dr_rules_job_unlocked(&g, DR_JOB_TRAPPER));     // 捕兽人同挂猎屋(原版)
    assert(!dr_rules_job_unlocked(&g, DR_JOB_TANNER));     // 需制革坊
    assert(!dr_rules_job_unlocked(&g, DR_JOB_CHARCUTIER)); // 需熏肉房
    g.building_lv[DR_BLD_TANNERY] = 1;
    g.building_lv[DR_BLD_SMOKEHOUSE] = 1;
    assert(dr_rules_job_unlocked(&g, DR_JOB_TANNER));
    assert(dr_rules_job_unlocked(&g, DR_JOB_CHARCUTIER));

    assert(dr_rules_job_assign(&g, DR_JOB_HUNTER, 2));
    assert(!dr_rules_job_assign(&g, DR_JOB_HUNTER, 3));    // 超员
    assert(dr_rules_job_idle(&g) == 2);
    assert(dr_rules_job_assign(&g, DR_JOB_HUNTER, -5));    // 负数钳 0
    assert(g.job[DR_JOB_HUNTER] == 0 && dr_rules_job_idle(&g) == 4);
}

// ---- 收入(原版 _INCOME 每 10s):采集者+建造者+猎人半率 ----
static void test_income_production(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.population = 3;
    g.builder_lv = DR_BUILDER_HELP;
    g.building_lv[DR_BLD_LODGE] = 1;
    assert(dr_rules_job_assign(&g, DR_JOB_HUNTER, 2));   // 闲 1 采集者
    g.res[DR_RES_WOOD] = 100;

    dr_rules_rt_t rt;
    dr_rules_rt_init(&rt, &g, 0);
    dr_rules_event_t ev; uint16_t arg;
    for (uint32_t k = 1; k <= 20; k++)
        (void)dr_rules_tick(&rt, &g, k * 10u * 1000u, &ev, &arg);

    // 木:100 + 采集者 20×1 + 建造者 20×2 = 160
    assert(g.res[DR_RES_WOOD] == 160 && g.res_total[DR_RES_WOOD] == 15 + 60);
    // 猎人隔 tick 半率:20 tick 命中 10 次 × 2 人 = +20 毛 +20 肉
    assert(g.res[DR_RES_FUR] == 20 && g.res[DR_RES_MEAT] == 20);
    assert(g.res_total[DR_RES_FUR] == 20 && g.res_total[DR_RES_MEAT] == 20);
}

static void test_income_conversions(void) {
    // 捕兽人:1 肉 → 1 饵,肉尽空转
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.population = 1;
    g.building_lv[DR_BLD_LODGE] = 1;
    assert(dr_rules_job_assign(&g, DR_JOB_TRAPPER, 1));
    g.res[DR_RES_MEAT] = 5;
    dr_rules_rt_t rt;
    dr_rules_rt_init(&rt, &g, 0);
    dr_rules_event_t ev; uint16_t arg;
    for (uint32_t k = 1; k <= 8; k++)
        (void)dr_rules_tick(&rt, &g, k * 10u * 1000u, &ev, &arg);
    assert(g.res[DR_RES_BAIT] == 5 && g.res_total[DR_RES_BAIT] == 5);
    assert(g.res[DR_RES_MEAT] == 0);

    // 制革匠:5 毛 → 1 革,毛不足空转
    dr_game_t g2;
    dr_game_init(&g2, 1, 1000);
    g2.population = 1;
    g2.building_lv[DR_BLD_TANNERY] = 1;
    assert(dr_rules_job_assign(&g2, DR_JOB_TANNER, 1));
    g2.res[DR_RES_FUR] = 12;
    dr_rules_rt_t rt2;
    dr_rules_rt_init(&rt2, &g2, 0);
    for (uint32_t k = 1; k <= 5; k++)
        (void)dr_rules_tick(&rt2, &g2, k * 10u * 1000u, &ev, &arg);
    assert(g2.res[DR_RES_FUR] == 2);
    assert(g2.res[DR_RES_LEATHER] == 2 && g2.res_total[DR_RES_LEATHER] == 2);

    // 熏肉匠:5 肉 + 5 木 → 1 干肉,任一不足空转
    dr_game_t g3;
    dr_game_init(&g3, 1, 1000);
    g3.population = 1;
    g3.building_lv[DR_BLD_SMOKEHOUSE] = 1;
    assert(dr_rules_job_assign(&g3, DR_JOB_CHARCUTIER, 1));
    g3.res[DR_RES_MEAT] = 20;
    g3.res[DR_RES_WOOD] = 20;
    dr_rules_rt_t rt3;
    dr_rules_rt_init(&rt3, &g3, 0);
    for (uint32_t k = 1; k <= 6; k++)
        (void)dr_rules_tick(&rt3, &g3, k * 10u * 1000u, &ev, &arg);
    assert(g3.res[DR_RES_MEAT] == 0 && g3.res[DR_RES_WOOD] == 0);
    assert(g3.res[DR_RES_FOOD] == 4 && g3.res_total[DR_RES_FOOD] == 4);
}

// ---- 流浪者:0.5~3 分钟成批到达;满员不来;建房不加人口 ----
static void test_wanderers(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.building_lv[DR_BLD_HUT] = 1;    // 上限 4(原版每屋 4 人)
    assert(dr_rules_pop_cap(&g) == 4);
    dr_rules_rt_t rt;
    dr_rules_rt_init(&rt, &g, 0);
    dr_rules_event_t ev; uint16_t arg;

    bool arrived = false;
    for (uint32_t t = 1000; t <= 200u * 1000u && !arrived; t += 1000u) {
        (void)dr_rules_tick(&rt, &g, t, &ev, &arg);
        if (ev == DR_RT_EV_WANDERER) {
            arrived = true;
            assert(arg >= 1 && arg <= 4 && g.population == arg);
        }
    }
    assert(arrived);

    // 填满后不再来
    g.population = 4;
    for (uint32_t t = 200u * 1000u; t <= 500u * 1000u; t += 1000u) {
        (void)dr_rules_tick(&rt, &g, t, &ev, &arg);
        assert(g.population == 4);
    }
}

// ---- 贸易(原版 TradeGoods:只买不卖,毛/鳞/牙支付) ----
static void test_trade(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.res[DR_RES_FUR] = 1000;
    assert(!dr_rules_trade_buy(&g, DR_TRADE_SCALES));    // 未建贸易站
    g.building_lv[DR_BLD_TRADE_POST] = 1;

    g.res[DR_RES_FUR] = 149;
    assert(!dr_rules_trade_buy(&g, DR_TRADE_SCALES));    // 毛 150 不够
    g.res[DR_RES_FUR] = 150;
    assert(dr_rules_trade_buy(&g, DR_TRADE_SCALES));     // 150 毛 → 1 鳞
    assert(g.res[DR_RES_FUR] == 0 && g.res[DR_RES_SCALES] == 1);

    g.res[DR_RES_FUR] = 1000; g.res[DR_RES_SCALES] = 50; g.res[DR_RES_TEETH] = 50;
    assert(dr_rules_trade_buy(&g, DR_TRADE_STEEL));      // 300毛+50鳞+50牙 → 1 钢
    assert(g.res[DR_RES_STEEL] == 1 && g.res[DR_RES_TEETH] == 0);
    assert(!dr_rules_trade_buy(&g, DR_TRADE_COAL));      // 牙不够

    g.res[DR_RES_SCALES] = 10;
    assert(dr_rules_trade_buy(&g, DR_TRADE_BULLETS));    // 10 鳞 → 1 子弹
    assert(g.res[DR_RES_BULLETS] == 1 && g.res[DR_RES_SCALES] == 0);

    // 罗盘:限一件,标记入档
    g.res[DR_RES_FUR] = 400; g.res[DR_RES_SCALES] = 20; g.res[DR_RES_TEETH] = 10;
    assert(dr_rules_trade_buy(&g, DR_TRADE_COMPASS));
    assert((g.flags & ((uint64_t)1u << DR_FLAG_COMPASS)) != 0);
    g.res[DR_RES_FUR] = 400; g.res[DR_RES_SCALES] = 20; g.res[DR_RES_TEETH] = 10;
    assert(!dr_rules_trade_buy(&g, DR_TRADE_COMPASS));   // 已购
}

// ---- 离线补算(设备适配):熄火 + 收入逐 tick + 陷阱按周期 ----
static void test_offline(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 100000);
    g.population = 1;                       // 1 采集者
    g.builder_lv = DR_BUILDER_HELP;
    g.building_lv[DR_BLD_TRAP] = 1;
    g.fire_lv = DR_FIRE_ROARING;
    g.saved_at_ts = 100000;
    dr_rules_rt_t rt;
    dr_rules_rt_init(&rt, &g, 0);

    dr_offline_yield_t y;
    uint32_t ticks = dr_rules_offline_settle(&rt, &g, 101800, 0, &y);
    assert(ticks == 180 && y.ticks == 180);      // 1800s → 180 tick
    assert(y.fire_out && g.fire_lv == DR_FIRE_DEAD);
    assert(y.wood == 180 * 3);                    // 采集者 1×180 + 建造者 2×180
    // 陷阱:1800s / 90s = 20 周期 × 1 陷阱 = 20 件,无饵
    assert(g.res[DR_RES_FUR] + g.res[DR_RES_MEAT] + g.res[DR_RES_SCALES] +
           g.res[DR_RES_TEETH] + g.res[DR_RES_CLOTH] + g.res[DR_RES_CHARM] == 20);
    assert(g.saved_at_ts == 101800);
    assert(g.rng_seed_state == rt.rng.s);         // 离线序列写回
}

// ---- 矿工链(M4):矿工吃干肉产矿;炼钢/军械转化 ----
static void test_miner_jobs(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.population = 5;
    // 解锁:到访矿并回家(flags),炼钢/军械挂建筑
    assert(!dr_rules_job_unlocked(&g, DR_JOB_IRON_MINER));
    g.flags |= (uint64_t)1u << DR_FLAG_IRON_MINE;
    assert(dr_rules_job_unlocked(&g, DR_JOB_IRON_MINER));
    assert(!dr_rules_job_unlocked(&g, DR_JOB_STEELWORKER));
    g.building_lv[DR_BLD_STEELWORKS] = 1;
    g.building_lv[DR_BLD_ARMOURY] = 1;
    assert(dr_rules_job_unlocked(&g, DR_JOB_STEELWORKER));
    assert(dr_rules_job_unlocked(&g, DR_JOB_ARMOURER));

    // 收入:1 铁矿工 + 1 煤矿工 + 1 炼钢工 + 1 军械工,干肉 5
    g.flags |= (uint64_t)1u << DR_FLAG_COAL_MINE;
    assert(dr_rules_job_assign(&g, DR_JOB_IRON_MINER, 1));
    assert(dr_rules_job_assign(&g, DR_JOB_COAL_MINER, 1));
    assert(dr_rules_job_assign(&g, DR_JOB_STEELWORKER, 1));
    assert(dr_rules_job_assign(&g, DR_JOB_ARMOURER, 1));
    g.res[DR_RES_FOOD] = 5;
    g.res[DR_RES_STEEL] = 1;
    g.res[DR_RES_SULPHUR] = 1;
    dr_rules_rt_t rt;
    dr_rules_rt_init(&rt, &g, 0);
    dr_rules_event_t ev; uint16_t arg;
    (void)dr_rules_tick(&rt, &g, 10u * 1000u, &ev, &arg);   // 1 个收入 tick

    // 矿工各吃 1 干肉产 1 矿;炼钢工吃 1 铁 1 煤产 1 钢;
    // 军械工吃 1 钢 1 硫(钢被矿工链后置:铁1→钢,耗尽)产 1 子弹
    assert(g.res[DR_RES_FOOD] == 3);
    assert(g.res[DR_RES_IRON] == 0 && g.res[DR_RES_COAL] == 0);
    assert(g.res[DR_RES_STEEL] == 1);   // 原 1 + 炼 1 − 军械 1
    assert(g.res[DR_RES_BULLETS] == 1);
    assert(g.res[DR_RES_SULPHUR] == 0);
}

// ---- 制造(切片三:工坊/武器/护甲/水具/背具) ----
static void test_craft(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    // 火把无需工坊:1木+1布
    g.res[DR_RES_WOOD] = 1;
    g.res[DR_RES_CLOTH] = 1;
    assert(!dr_rules_craft(&g, DR_CRAFT_BONE_SPEAR));   // 材料不够
    assert(dr_rules_craft(&g, DR_CRAFT_TORCH));
    assert(g.flags & ((uint64_t)1u << DR_FLAG_TORCH));

    // 骨矛需工坊(可见性:无工坊时不可见 → craft false)
    g.res[DR_RES_WOOD] = 200;
    g.res[DR_RES_TEETH] = 5;
    assert(!dr_rules_craft_visible(&g, DR_CRAFT_BONE_SPEAR));
    assert(!dr_rules_craft(&g, DR_CRAFT_BONE_SPEAR));
    g.building_lv[DR_BLD_WORKSHOP] = 1;
    assert(dr_rules_craft_visible(&g, DR_CRAFT_BONE_SPEAR));
    assert(dr_rules_craft(&g, DR_CRAFT_BONE_SPEAR));
    assert(g.weapon_lv == 1);
    assert(g.res[DR_RES_WOOD] == 100 && g.res[DR_RES_TEETH] == 0);

    // 武器取最优:铁剑直接覆盖骨矛
    g.res[DR_RES_WOOD] = 200;
    g.res[DR_RES_LEATHER] = 50;
    g.res[DR_RES_IRON] = 20;
    assert(dr_rules_craft(&g, DR_CRAFT_IRON_SWORD));
    assert(g.weapon_lv == 2);

    // 皮甲:护甲1
    g.res[DR_RES_LEATHER] = 200;
    g.res[DR_RES_SCALES] = 20;
    assert(dr_rules_craft(&g, DR_CRAFT_L_ARMOUR));
    assert(g.armor_lv == 1);

    // 水袋:标记位;重复造被拒
    g.res[DR_RES_LEATHER] = 50;
    assert(dr_rules_craft(&g, DR_CRAFT_WATERSKIN));
    assert(g.flags & ((uint64_t)1u << DR_FLAG_WATERSKIN));
    g.res[DR_RES_LEATHER] = 50;
    assert(!dr_rules_craft(&g, DR_CRAFT_WATERSKIN));

    // 工坊造价
    dr_bld_cost_t c = dr_building_cost(DR_BLD_WORKSHOP, 0);
    assert(c.wood == 800 && c.leather == 100 && c.scales == 10);
}

int main(void) {
    test_building_costs();
    test_build_gates();
    test_fire();
    test_temperature();
    test_builder_arc();
    test_gather();
    test_trap();
    test_jobs();
    test_income_production();
    test_income_conversions();
    test_wanderers();
    test_trade();
    test_offline();
    test_miner_jobs();
    test_craft();
    printf("test_dr_rules: all passed\n");
    return 0;
}
