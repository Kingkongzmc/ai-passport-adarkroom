// tests/test_dr_rules.c —— 第一幕规则主机测试:火焰/生火、建造链与造价、
// 陷阱结算与 RNG 回写、人口上限。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dr_rules.h"
#include "dr_util.h"

static void test_fire_cycle(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    assert(g.fire_lv == DR_FIRE_DEAD);

    // 点火(从熄灭):不足 5 木失败;够则 5 木直接到"旺盛"(原版 lightFire)
    assert(!dr_rules_stoke_fire(&g, 1000));
    g.res[DR_RES_WOOD] = 4;
    assert(!dr_rules_stoke_fire(&g, 1000));
    g.res[DR_RES_WOOD] = 100;
    assert(dr_rules_stoke_fire(&g, 1000));
    assert(g.fire_lv == DR_FIRE_BURNING);
    assert(g.res[DR_RES_WOOD] == 100 - DR_FIRE_LIGHT_COST);

    // 添柴:1 木 +1 档,封顶"炽烈"
    assert(dr_rules_stoke_fire(&g, 1000));
    assert(g.fire_lv == DR_FIRE_ROARING);
    assert(g.res[DR_RES_WOOD] == 100 - DR_FIRE_LIGHT_COST - DR_FIRE_STOKE_COST);
    assert(dr_rules_stoke_fire(&g, 1000));   // 已封顶,仍扣 1 木(原版行为)
    assert(g.fire_lv == DR_FIRE_ROARING);

    dr_rules_rt_t rt;
    dr_rules_rt_init(&rt, &g, 0);

    // 5 分钟降一档;逐档到熄灭(期间 10s 经济 tick 也会置 changed,只看火焰档)
    bool fire_out = false;
    dr_rules_tick(&rt, &g, (DR_FIRE_LEVEL_SECONDS - 1) * 1000, &fire_out);
    assert(g.fire_lv == DR_FIRE_ROARING);
    assert(dr_rules_tick(&rt, &g, (DR_FIRE_LEVEL_SECONDS + 1) * 1000,
                         &fire_out) && fire_out);
    assert(g.fire_lv == DR_FIRE_BURNING);
}

static void test_jobs(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.population = 3;
    assert(dr_rules_job_idle(&g) == 3);

    // 解锁:伐木工常开;猎人需猎人小屋;制革匠/铁匠 M3+
    assert(dr_rules_job_unlocked(&g, DR_JOB_LUMBER));
    assert(!dr_rules_job_unlocked(&g, DR_JOB_HUNTER));
    assert(!dr_rules_job_assign(&g, DR_JOB_HUNTER, 1));
    g.building_lv[DR_BLD_LODGE] = 1;
    assert(dr_rules_job_unlocked(&g, DR_JOB_HUNTER));

    // 分配/撤下与超员
    assert(dr_rules_job_assign(&g, DR_JOB_LUMBER, 2));
    assert(dr_rules_job_assign(&g, DR_JOB_HUNTER, 1));
    assert(dr_rules_job_idle(&g) == 0);
    assert(!dr_rules_job_assign(&g, DR_JOB_LUMBER, 1));   // 没有闲人
    assert(dr_rules_job_assign(&g, DR_JOB_HUNTER, -1));
    assert(dr_rules_job_idle(&g) == 1);
    assert(dr_rules_job_assign(&g, DR_JOB_HUNTER, -5));   // 撤到 0 钳制
    assert(g.job[DR_JOB_HUNTER] == 0);

    // 产出:每 10s 采集者(闲人)+1 木,伐木工 +2 木,猎人 +1 毛 +1 肉
    // 此刻 job:伐木2 猎人0,闲人 = 1
    dr_rules_rt_t rt;
    dr_rules_rt_init(&rt, &g, 0);
    uint32_t wood0 = g.res[DR_RES_WOOD];
    assert(dr_rules_tick(&rt, &g, 10u * 1000u, 0));
    assert(g.res[DR_RES_WOOD] == wood0 + 1 + 2 * 2);      // 闲人1 + 伐木2×2
    assert(g.res[DR_RES_FUR] == 0);                        // 猎人 0 人
    assert(dr_rules_job_assign(&g, DR_JOB_HUNTER, 1));     // 闲人0
    assert(dr_rules_tick(&rt, &g, 20u * 1000u, 0));
    assert(g.res[DR_RES_FUR] == 1 && g.res[DR_RES_MEAT] == 1);
    assert(g.res[DR_RES_WOOD] == wood0 + 5 + 4);           // +伐木2×2(闲人0)
}

static void test_build_chain(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.res[DR_RES_WOOD] = 1000;

    // 前置链:陷阱需要板车;小屋需要陷阱
    assert(!dr_rules_can_build(&g, DR_BLD_TRAP));
    assert(dr_rules_build(&g, DR_BLD_CART, 1000));
    assert(dr_rules_build(&g, DR_BLD_TRAP, 1000));
    assert(!dr_rules_can_build(&g, DR_BLD_HUT) == false);
    assert(dr_rules_build(&g, DR_BLD_HUT, 1000));

    // 小屋人口:每级 +2 上限,定居钳到上限
    assert(g.population == 2);
    assert(dr_rules_pop_cap(&g) == 2);

    // 木材被实扣(1000 - 10 - 15 - 20)
    assert(g.res[DR_RES_WOOD] == 1000 - 10 - 15 - 20);

    // 额外条件:猎人小屋(150木+10毛+5肉)、贸易站(300木+20毛)
    g.population = 2;   // 过小屋前置
    assert(!dr_rules_can_build(&g, DR_BLD_LODGE));   // 缺毛/肉
    g.res[DR_RES_FUR] = 10;
    g.res[DR_RES_MEAT] = 4;
    assert(!dr_rules_can_build(&g, DR_BLD_LODGE));   // 肉不够
    g.res[DR_RES_MEAT] = 5;
    assert(dr_rules_can_build(&g, DR_BLD_LODGE));
    uint32_t wood0 = g.res[DR_RES_WOOD];
    assert(dr_rules_build(&g, DR_BLD_LODGE, 1000));
    assert(g.res[DR_RES_WOOD] == wood0 - 150u);
    assert(g.res[DR_RES_FUR] == 0 && g.res[DR_RES_MEAT] == 0);
    assert(!dr_rules_can_build(&g, DR_BLD_TRADE_POST));   // 缺 20 毛
    g.res[DR_RES_FUR] = 19;
    assert(!dr_rules_can_build(&g, DR_BLD_TRADE_POST));
    g.res[DR_RES_FUR] = 20;
    assert(dr_rules_can_build(&g, DR_BLD_TRADE_POST));
}

static void test_trap_settlement(void) {
    dr_game_t g;
    dr_game_init(&g, 7, 1000);
    g.res[DR_RES_WOOD] = 500;   // 先备料再建造(建造是即时扣费的)
    assert(dr_rules_build(&g, DR_BLD_CART, 1000));
    assert(dr_rules_build(&g, DR_BLD_TRAP, 1000));
    assert(g.building_lv[DR_BLD_TRAP] == 1);

    dr_rules_rt_t rt;
    dr_rules_rt_init(&rt, &g, 0);

    // 冷却未到:不可收获
    assert(!dr_rules_trap_ready(&rt, &g, 10 * 1000));
    assert(dr_rules_trap_check(&rt, &g, 10 * 1000) == 0);
    // 到点:可收获;手动结算 2000 轮,统计收获与确定性
    uint32_t fur = 0, meat = 0, got_cnt = 0;
    for (uint32_t t = DR_TRAP_PERIOD_S * 1000u;
         t <= 2000u * 1000u; t += DR_TRAP_PERIOD_S * 1000u) {
        assert(dr_rules_trap_ready(&rt, &g, t));
        uint8_t got = dr_rules_trap_check(&rt, &g, t);
        if (got) got_cnt++;
        if (got & DR_TRAP_GOT_FUR) fur++;
        if (got & DR_TRAP_GOT_MEAT) meat++;
    }
    fur = g.res[DR_RES_FUR];
    meat = g.res[DR_RES_MEAT];
    // ≈66 次结算 × 25%(熄火减半)→ 期望 ≈16,阈值取 8
    assert(fur + meat >= 8);
    assert(g.rng_seed_state == rt.rng.s);  // 种子已回写档
    // 重复同样流程应得到完全相同的结果(确定性)
    dr_game_t g2;
    dr_game_init(&g2, 7, 1000);
    g2.res[DR_RES_WOOD] = 500;
    assert(dr_rules_build(&g2, DR_BLD_CART, 1000));
    assert(dr_rules_build(&g2, DR_BLD_TRAP, 1000));
    dr_rules_rt_t rt2;
    dr_rules_rt_init(&rt2, &g2, 0);
    for (uint32_t t = DR_TRAP_PERIOD_S * 1000u;
         t <= 2000u * 1000u; t += DR_TRAP_PERIOD_S * 1000u) {
        dr_rules_trap_check(&rt2, &g2, t);
    }
    assert(memcmp(&g, &g2, sizeof(g)) == 0);
    (void)got_cnt;
}

static void test_trade_sell(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.res[DR_RES_FUR] = 10;
    g.res[DR_RES_MEAT] = 4;
    uint32_t wood0 = g.res[DR_RES_WOOD];
    assert(dr_rules_trade_sell_value(&g) == 10u * 5u + 4u * 3u);
    assert(dr_rules_trade_sell_all(&g) == 62u);
    assert(g.res[DR_RES_FUR] == 0 && g.res[DR_RES_MEAT] == 0);
    assert(g.res[DR_RES_WOOD] == wood0 + 62u);
    assert(dr_rules_trade_sell_all(&g) == 0u);   // 空手再卖 = 0,不动账

    // 整数组:材料不够失败;足够则按牌价成交
    assert(!dr_rules_trade_fur10(&g));
    g.res[DR_RES_FUR] = 12;
    wood0 = g.res[DR_RES_WOOD];
    assert(dr_rules_trade_fur10(&g));
    assert(g.res[DR_RES_FUR] == 2 && g.res[DR_RES_WOOD] == wood0 + 50u);
    assert(!dr_rules_trade_meat10(&g));
    g.res[DR_RES_MEAT] = 10;
    wood0 = g.res[DR_RES_WOOD];
    assert(dr_rules_trade_meat10(&g));
    assert(g.res[DR_RES_MEAT] == 0 && g.res[DR_RES_WOOD] == wood0 + 30u);
    g.res[DR_RES_WOOD] = 14;                     // 木头不够(14 < 15)
    assert(!dr_rules_trade_bait5(&g));
    g.res[DR_RES_WOOD] += 100;
    assert(dr_rules_trade_bait5(&g));
    assert(g.res[DR_RES_BAIT] == 5);
}

static void test_offline_settle(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.saved_at_ts = 100000;
    g.fire_lv = DR_FIRE_ROARING;
    g.building_lv[DR_BLD_TRAP] = 1;              // 50% 单掷
    dr_rules_rt_t rt;
    dr_rules_rt_init(&rt, &g, 0);

    // 时钟回拨:不结算
    assert(dr_rules_offline_settle(&rt, &g, 99999, 0, 0) == 0);

    // 离线 600s:60 经济 tick;火熄;陷阱 20 个周期掷定(有随机性,只验边界)
    dr_offline_yield_t y;
    uint32_t wood0 = g.res[DR_RES_WOOD];
    uint32_t ticks = dr_rules_offline_settle(&rt, &g, 100600, 0, &y);
    assert(ticks == 60 && y.ticks == 60);
    assert(g.fire_lv == DR_FIRE_DEAD && y.fire_out);
    assert(g.saved_at_ts == 100600);
    assert(g.res[DR_RES_WOOD] >= wood0);         // 人口 0 无产出,至少不减
    assert(y.fur + y.meat <= 20);                // 20 个周期,一周期至多 1 收获
    assert(y.bait == y.fur + y.meat);            // 每次收获伴生 1 诱饵
    assert(g.res[DR_RES_FUR] + g.res[DR_RES_MEAT] == y.fur + y.meat);
    assert(g.res[DR_RES_BAIT] == y.bait);

    // 村庄产出补算:闲人+伐木
    dr_game_init(&g, 2, 1000);
    g.saved_at_ts = 100000;
    g.population = 2;
    assert(dr_rules_job_assign(&g, DR_JOB_LUMBER, 2));
    dr_rules_rt_init(&rt, &g, 0);
    wood0 = g.res[DR_RES_WOOD];
    ticks = dr_rules_offline_settle(&rt, &g, 100600, 0, &y);
    assert(ticks == 60);
    assert(g.res[DR_RES_WOOD] == wood0 + 2u * 2u * 60u);  // 伐木2 × 2木 × 60tick
}

static void test_wanderer(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.building_lv[DR_BLD_HUT] = 1;               // 容量 2
    g.fire_lv = DR_FIRE_BURNING;
    g.population = 1;                            // 已有人定居才会来流浪者
    dr_rules_rt_t rt;
    dr_rules_rt_init(&rt, &g, 0);

    // 30s 内不到达;到点且条件满足 +1(上限 180s,步进 1s 轮询)
    assert(!dr_rules_wanderer_tick(&rt, &g, 10u * 1000u));
    uint32_t t = 31u * 1000u;
    bool arrived = false;
    for (int i = 0; i < 200 && !arrived; i++, t += 1000u)
        arrived = dr_rules_wanderer_tick(&rt, &g, t);
    assert(arrived && g.population == 2);

    // 满员不再来(200s 轮询内人口不变)
    g.population = 2;
    t += 1000u;
    for (int i = 0; i < 200; i++, t += 1000u)
        (void)dr_rules_wanderer_tick(&rt, &g, t);
    assert(g.population == 2);                   // 满员钳制

    // 火熄不来
    g.population = 1;
    g.fire_lv = DR_FIRE_DEAD;
    t += 1000u;
    for (int i = 0; i < 200; i++, t += 1000u)
        (void)dr_rules_wanderer_tick(&rt, &g, t);
    assert(g.population == 1);
}

int main(void) {
    test_fire_cycle();
    test_build_chain();
    test_trap_settlement();
    test_jobs();
    test_trade_sell();
    test_offline_settle();
    test_wanderer();
    printf("test_dr_rules: all passed\n");
    return 0;
}
