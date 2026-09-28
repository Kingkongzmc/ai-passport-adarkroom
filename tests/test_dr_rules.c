// tests/test_dr_rules.c —— 第一/二幕规则主机测试:火焰/生火、建造链与造价、
// 陷阱结算与 RNG 回写、人口上限、口粮与罢工、制革匠、诱饵、贸易与皮甲。
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

    // 解锁:伐木工常开;猎人需猎人小屋;制革匠需制革坊;铁匠 M4
    assert(dr_rules_job_unlocked(&g, DR_JOB_LUMBER));
    assert(!dr_rules_job_unlocked(&g, DR_JOB_HUNTER));
    assert(!dr_rules_job_unlocked(&g, DR_JOB_TANNER));
    assert(!dr_rules_job_assign(&g, DR_JOB_HUNTER, 1));
    g.building_lv[DR_BLD_LODGE] = 1;
    assert(dr_rules_job_unlocked(&g, DR_JOB_HUNTER));
    assert(!dr_rules_job_unlocked(&g, DR_JOB_TANNER));
    g.building_lv[DR_BLD_TANNERY] = 1;
    assert(dr_rules_job_unlocked(&g, DR_JOB_TANNER));
    assert(!dr_rules_job_unlocked(&g, DR_JOB_SMITH));   // 铁匠 M4

    // 分配/撤下与超员
    assert(dr_rules_job_assign(&g, DR_JOB_LUMBER, 2));
    assert(dr_rules_job_assign(&g, DR_JOB_HUNTER, 1));
    assert(dr_rules_job_idle(&g) == 0);
    assert(!dr_rules_job_assign(&g, DR_JOB_LUMBER, 1));   // 没有闲人
    assert(dr_rules_job_assign(&g, DR_JOB_HUNTER, -1));
    assert(dr_rules_job_idle(&g) == 1);
    assert(dr_rules_job_assign(&g, DR_JOB_HUNTER, -5));   // 撤到 0 钳制
    assert(g.job[DR_JOB_HUNTER] == 0);

    // 产出(每 10s 经济 tick,先吃后产):备足口粮。此刻 job:伐木2 猎人0,闲人1。
    // 木 = 闲人1 + 伐木2×2;食 = -3(吃) + 1(采集者);猎人 0 人无毛肉。
    g.res[DR_RES_FOOD] = 100;
    dr_rules_rt_t rt;
    dr_rules_rt_init(&rt, &g, 0);
    uint32_t wood0 = g.res[DR_RES_WOOD];
    uint32_t food0 = g.res[DR_RES_FOOD];
    assert(dr_rules_tick(&rt, &g, 10u * 1000u, 0));
    assert(g.res[DR_RES_WOOD] == wood0 + 1 + 2 * 2);
    assert(g.res[DR_RES_FOOD] == food0 - 3 + 1);
    assert(g.res[DR_RES_FUR] == 0 && g.res[DR_RES_MEAT] == 0);
    assert(dr_rules_job_assign(&g, DR_JOB_HUNTER, 1));     // 闲人0
    assert(dr_rules_tick(&rt, &g, 20u * 1000u, 0));
    assert(g.res[DR_RES_FUR] == 1 && g.res[DR_RES_MEAT] == 2);   // +1毛 +2肉
    assert(g.res[DR_RES_WOOD] == wood0 + 5 + 4);           // 第二 tick 无闲人
}

static void test_food_economy(void) {
    dr_game_t g;
    dr_game_init(&g, 3, 1000);
    dr_rules_rt_t rt;
    dr_rules_rt_init(&rt, &g, 0);

    // 采集者自给:pop1 闲人,吃 1 食,产 1 食 1 木 → 食持平
    g.population = 1;
    g.res[DR_RES_FOOD] = 2;
    assert(dr_rules_tick(&rt, &g, 10u * 1000u, 0));
    assert(g.res[DR_RES_FOOD] == 2);
    assert(g.res[DR_RES_WOOD] == 1);
    assert(!dr_rules_starving(&g));

    // 食物不足先扣食再扣肉:pop2 闲人,1 食 2 肉 → 吃 1食+1肉,产 2 食 2 木
    g.population = 2;
    g.res[DR_RES_FOOD] = 1;
    g.res[DR_RES_MEAT] = 2;
    uint32_t wood0 = g.res[DR_RES_WOOD];
    assert(dr_rules_tick(&rt, &g, 20u * 1000u, 0));
    assert(g.res[DR_RES_FOOD] == 2 && g.res[DR_RES_MEAT] == 1);
    assert(g.res[DR_RES_WOOD] == wood0 + 2);

    // 断粮罢工:pop2 全是伐木工,食肉皆空 → 岗位停工,无闲人拾荒 → 持续断粮
    g.population = 2;
    g.job[DR_JOB_LUMBER] = 2;
    g.res[DR_RES_FOOD] = 0;
    g.res[DR_RES_MEAT] = 0;
    wood0 = g.res[DR_RES_WOOD];
    assert(dr_rules_starving(&g));
    assert(dr_rules_tick(&rt, &g, 30u * 1000u, 0));
    assert(g.res[DR_RES_WOOD] == wood0);      // 罢工:伐木工不出木
    assert(g.res[DR_RES_FOOD] == 0);          // 无闲人:拾荒也无食
    assert(dr_rules_starving(&g));

    // 恢复:手动收陷阱喂 2 肉(玩家操作),下一 tick 吃掉并复工
    g.res[DR_RES_MEAT] = 2;
    assert(!dr_rules_starving(&g));
    assert(dr_rules_tick(&rt, &g, 40u * 1000u, 0));
    assert(g.res[DR_RES_MEAT] == 0 && g.res[DR_RES_FOOD] == 0);
    assert(g.res[DR_RES_WOOD] == wood0 + 4);  // 伐木2×2 复工
    assert(dr_rules_starving(&g));            // 又吃空了:压力仍在(设计内)

    // 罢工时闲人拾荒求生:pop2 = 伐木1 + 闲人1 → 每 tick +1 食(无木)
    g.job[DR_JOB_LUMBER] = 1;
    g.res[DR_RES_FOOD] = 0;
    g.res[DR_RES_MEAT] = 0;
    wood0 = g.res[DR_RES_WOOD];
    assert(dr_rules_tick(&rt, &g, 50u * 1000u, 0));
    assert(g.res[DR_RES_FOOD] == 1);          // 拾荒 +1
    assert(g.res[DR_RES_WOOD] == wood0);      // 不出木
}

static void test_tanner(void) {
    dr_game_t g;
    dr_game_init(&g, 5, 1000);
    g.population = 2;
    g.building_lv[DR_BLD_TANNERY] = 1;
    g.res[DR_RES_FOOD] = 100;
    assert(dr_rules_job_assign(&g, DR_JOB_TANNER, 1));
    assert(dr_rules_job_assign(&g, DR_JOB_LUMBER, 1));   // 闲人 0

    dr_rules_rt_t rt;
    dr_rules_rt_init(&rt, &g, 0);
    g.res[DR_RES_FUR] = 5;
    assert(dr_rules_tick(&rt, &g, 10u * 1000u, 0));
    assert(g.res[DR_RES_LEATHER] == 1);        // 2 毛 → 1 革
    assert(g.res[DR_RES_FUR] == 3);
    assert(g.res[DR_RES_LEATHER] == g.res_total[DR_RES_LEATHER]);

    // 毛不够当 tick 空转:1 毛不转化
    g.res[DR_RES_FUR] = 1;
    assert(dr_rules_tick(&rt, &g, 20u * 1000u, 0));
    assert(g.res[DR_RES_LEATHER] == 1 && g.res[DR_RES_FUR] == 1);
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

    // 制革坊:前置猎人小屋;120木+10毛
    assert(!dr_rules_can_build(&g, DR_BLD_TANNERY));   // 缺 10 毛
    g.res[DR_RES_FUR] = 9;
    assert(!dr_rules_can_build(&g, DR_BLD_TANNERY));
    g.res[DR_RES_FUR] = 10;
    assert(dr_rules_can_build(&g, DR_BLD_TANNERY));
    wood0 = g.res[DR_RES_WOOD];
    assert(dr_rules_build(&g, DR_BLD_TANNERY, 1000));
    assert(g.res[DR_RES_WOOD] == wood0 - 120u);
    assert(g.res[DR_RES_FUR] == 0);

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
    dr_trap_yield_t y;

    // 冷却未到:不可收获
    assert(!dr_rules_trap_ready(&rt, &g, 10 * 1000));
    assert(!dr_rules_trap_check(&rt, &g, 10 * 1000, &y));
    // 到点:可收获;手动结算 2000s = 66 次,原版语义每次必得 1 件(无落空)
    uint32_t got_cnt = 0;
    for (uint32_t t = DR_TRAP_PERIOD_S * 1000u;
         t <= 2000u * 1000u; t += DR_TRAP_PERIOD_S * 1000u) {
        assert(dr_rules_trap_ready(&rt, &g, t));
        assert(dr_rules_trap_check(&rt, &g, t, &y));
        assert(y.fur + y.meat == 1 && y.bait == 0);   // Lv1 无饵:恰好 1 件
        got_cnt += y.fur + y.meat;
    }
    assert(got_cnt == 66);                           // 总件数 = 结算次数
    uint32_t fur = g.res[DR_RES_FUR];
    uint32_t meat = g.res[DR_RES_MEAT];
    assert(fur + meat == 66);
    // 每件 50% 毛皮:期望 33,二项分布 ±3σ ≈ ±12
    assert(fur >= 21 && fur <= 45);
    // 陷阱不再掉诱饵(诱饵改为贸易购入、查看时消耗,对齐原版)
    assert(g.res[DR_RES_BAIT] == 0);
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
        dr_rules_trap_check(&rt2, &g2, t, &y);
    }
    assert(memcmp(&g, &g2, sizeof(g)) == 0);
}

static void test_trap_bait(void) {
    dr_game_t g;
    dr_game_init(&g, 11, 1000);
    g.res[DR_RES_WOOD] = 500;
    assert(dr_rules_build(&g, DR_BLD_CART, 1000));
    assert(dr_rules_build(&g, DR_BLD_TRAP, 1000));
    dr_rules_rt_t rt;
    dr_rules_rt_init(&rt, &g, 0);
    dr_trap_yield_t y;

    // 开关默认关:有饵也不消耗
    g.res[DR_RES_BAIT] = 7;
    for (uint32_t t = DR_TRAP_PERIOD_S * 1000u;
         t <= 30u * DR_TRAP_PERIOD_S * 1000u; t += DR_TRAP_PERIOD_S * 1000u) {
        assert(dr_rules_trap_check(&rt, &g, t, &y));
        assert(y.bait == 0 && y.fur + y.meat == 1);
    }
    assert(g.res[DR_RES_BAIT] == 7);

    // 开关开:Lv1 每次耗 1 饵多掷 1 件(必得 2 件),饵尽自动回落单掷
    g.trap_bait_on = 1;
    for (int k = 0; k < 10; k++) {
        uint32_t t = (31u + k) * DR_TRAP_PERIOD_S * 1000u;
        assert(dr_rules_trap_check(&rt, &g, t, &y));
        assert(y.bait == (k < 7 ? 1 : 0));           // 前 7 次有饵,后 3 次无
        assert(y.fur + y.meat == 1 + y.bait);
    }
    assert(g.res[DR_RES_BAIT] == 0);       // 7 饵耗完
    uint32_t t = 41u * DR_TRAP_PERIOD_S * 1000u;
    assert(dr_rules_trap_check(&rt, &g, t, &y));
    assert(y.bait == 0);                   // 无饵可用:不耗
    assert(g.res[DR_RES_BAIT] == 0);

    // 同种子同设置:耗饵路径也是确定性的
    dr_game_t a, b;
    dr_game_init(&a, 11, 1000);
    dr_game_init(&b, 11, 1000);
    for (dr_game_t *pg = &a; ; pg = &b) {
        pg->res[DR_RES_WOOD] = 500;
        assert(dr_rules_build(pg, DR_BLD_CART, 1000));
        assert(dr_rules_build(pg, DR_BLD_TRAP, 1000));
        pg->res[DR_RES_BAIT] = 3;
        pg->trap_bait_on = 1;
        dr_rules_rt_t r;
        dr_rules_rt_init(&r, pg, 0);
        for (uint32_t k = 1; k <= 20; k++)
            dr_rules_trap_check(&r, pg, k * DR_TRAP_PERIOD_S * 1000u, &y);
        if (pg == &b) break;
    }
    assert(memcmp(&a, &b, sizeof(a)) == 0);
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

    // 皮甲:50木+10革,限一件
    g.res[DR_RES_WOOD] = DR_TRADE_ARMOR_WOOD - 1;
    g.res[DR_RES_LEATHER] = DR_TRADE_ARMOR_LEATHER;
    assert(!dr_rules_trade_armor(&g));           // 木不够
    g.res[DR_RES_WOOD] = DR_TRADE_ARMOR_WOOD;
    assert(dr_rules_trade_armor(&g));
    assert(g.armor_lv == 1);
    assert(g.res[DR_RES_WOOD] == 0 && g.res[DR_RES_LEATHER] == 0);
    g.res[DR_RES_WOOD] = 1000;
    g.res[DR_RES_LEATHER] = 1000;
    assert(!dr_rules_trade_armor(&g));           // 已穿着,不可再买
}

static void test_offline_settle(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.saved_at_ts = 100000;
    g.fire_lv = DR_FIRE_ROARING;
    g.building_lv[DR_BLD_TRAP] = 1;              // 每周期必得 1 件
    dr_rules_rt_t rt;
    dr_rules_rt_init(&rt, &g, 0);

    // 时钟回拨:不结算,且锚点拉平到当下(防止回拨期反复判负)
    assert(dr_rules_offline_settle(&rt, &g, 99999, 0, 0) == 0);
    assert(g.saved_at_ts == 99999);

    // 离线 600s:60 经济 tick;火熄;陷阱 20 个周期 × Lv1 必得 = 恰好 20 件
    dr_offline_yield_t y;
    uint32_t wood0 = g.res[DR_RES_WOOD];
    uint32_t ticks = dr_rules_offline_settle(&rt, &g, 100600, 0, &y);
    assert(ticks == 60 && y.ticks == 60);
    assert(g.fire_lv == DR_FIRE_DEAD && y.fire_out);
    assert(g.saved_at_ts == 100600);
    assert(g.res[DR_RES_WOOD] >= wood0);         // 人口 0 无产出,至少不减
    assert(y.fur + y.meat == 20);                // 20 周期 × 1 件(火熄不减半)
    assert(y.food_eaten == 0);                   // 没人:不吃
    assert(!y.starving);
    assert(g.res[DR_RES_FUR] + g.res[DR_RES_MEAT] == y.fur + y.meat);
    assert(g.res[DR_RES_BAIT] == 0);             // 离线陷阱不掉饵

    // 村庄产出补算:闲人+伐木,口粮同步补扣
    dr_game_init(&g, 2, 1000);
    g.saved_at_ts = 100000;
    g.population = 2;
    g.res[DR_RES_FOOD] = 1000;
    assert(dr_rules_job_assign(&g, DR_JOB_LUMBER, 2));
    dr_rules_rt_init(&rt, &g, 0);
    wood0 = g.res[DR_RES_WOOD];
    ticks = dr_rules_offline_settle(&rt, &g, 100600, 0, &y);
    assert(ticks == 60);
    assert(g.res[DR_RES_WOOD] == wood0 + 2u * 2u * 60u);  // 伐木2 × 2木 × 60tick
    assert(y.food_eaten == 2u * 60u);                     // 2 人 × 60tick
    assert(g.res[DR_RES_FOOD] == 1000 - 2u * 60u);
    assert(!y.starving);

    // 断粮离线:全岗罢工,无产出,回报 starving
    dr_game_init(&g, 3, 1000);
    g.saved_at_ts = 100000;
    g.population = 2;
    assert(dr_rules_job_assign(&g, DR_JOB_LUMBER, 2));    // 无闲人
    dr_rules_rt_init(&rt, &g, 0);
    wood0 = g.res[DR_RES_WOOD];
    ticks = dr_rules_offline_settle(&rt, &g, 100600, 0, &y);
    assert(ticks == 60);
    assert(g.res[DR_RES_WOOD] == wood0);         // 罢工:无产出
    assert(y.starving);
    assert(dr_rules_starving(&g));
}

static void test_wanderer(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.building_lv[DR_BLD_HUT] = 1;               // 容量 2
    g.fire_lv = DR_FIRE_BURNING;
    g.population = 1;                            // 已有人定居才会来流浪者
    g.res[DR_RES_FOOD] = 10;                     // 村里不断粮
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

    // 断粮不来(荒年不收人)
    g.fire_lv = DR_FIRE_BURNING;
    g.res[DR_RES_FOOD] = 0;
    g.res[DR_RES_MEAT] = 0;
    t += 1000u;
    for (int i = 0; i < 200; i++, t += 1000u)
        (void)dr_rules_wanderer_tick(&rt, &g, t);
    assert(g.population == 1);
}

int main(void) {
    test_fire_cycle();
    test_build_chain();
    test_trap_settlement();
    test_trap_bait();
    test_jobs();
    test_food_economy();
    test_tanner();
    test_trade_sell();
    test_offline_settle();
    test_wanderer();
    printf("test_dr_rules: all passed\n");
    return 0;
}
