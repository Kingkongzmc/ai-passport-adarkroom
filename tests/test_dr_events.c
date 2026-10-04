// tests/test_dr_events.c —— 事件运行时主机测试:条件求值、场景/优先级/一次性/
// 冷却、全局节拍门、效果(资源增减钳底、标记、人口、建筑、比例减员、
// 加权跳转)、选项代价、M6 事件表数据校验。
// 表(dr_events_data)为 M6 切片一(12 事件 36 表项);机制用本地表自测。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dr_events.h"
#include "dr_rules.h"   // DR_BLD_* 建筑id(条件/效果引用)
#include "dr_state.h"
#include "dr_text.h"
#include "dr_util.h"

static void test_table_not_empty(void) {
    uint16_t count = 0;
    const dr_event_t *ev = dr_events_table(&count);
    assert(ev != NULL);
    assert(count >= 30 && count <= 128);   // 切片一 36 条;M6 全量 50~70 主事件
}

// 条件门 + 一次性 + 效果(资源/标记/人口)与累计口径
static void test_condition_gates_and_effects(void) {
    dr_event_t tbl[2] = {0};
    // id0:木材>=10 时触发,一次性;选项0=人口+1+置标记0,选项1=+5木+置标记1
    tbl[0].scene_mask = DR_SCENE_HOME;
    tbl[0].priority = 10;
    tbl[0].max_seen = 1;
    tbl[0].choice_count = 2;
    tbl[0].conds[0] = (dr_cond_t){ (uint8_t)DR_COND_RES_GE, DR_RES_WOOD, 10 };
    tbl[0].choices[0].effects[0] = (dr_eff_t){ (uint8_t)DR_EFF_POP_ADD, 0, 1 };
    tbl[0].choices[0].effects[1] = (dr_eff_t){ (uint8_t)DR_EFF_FLAG_SET, 0, 0 };
    tbl[0].choices[1].effects[0] = (dr_eff_t){ (uint8_t)DR_EFF_RES_ADD, DR_RES_WOOD, 5 };
    tbl[0].choices[1].effects[1] = (dr_eff_t){ (uint8_t)DR_EFF_FLAG_SET, 1, 0 };
    // id1:10% 概率,无一次性限制(对照:低优先级)
    tbl[1].scene_mask = DR_SCENE_ANY;
    tbl[1].priority = 1;
    tbl[1].max_seen = 0;
    tbl[1].choice_count = 1;
    tbl[1].conds[0] = (dr_cond_t){ (uint8_t)DR_COND_CHANCE, 0, 100 };

    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    dr_event_session_t sess;
    dr_event_session_init(&sess);
    dr_rng_t rng;
    uint16_t idx;

    // 条件不满足(木 9 < 10):id0 永不触发(概率命中 id1 是允许的)
    g.res[DR_RES_WOOD] = 9;
    for (int i = 0; i < 8; i++) {
        dr_rng_seed(&rng, (uint32_t)i);
        dr_event_result_t r = dr_event_pick(tbl, 2, &g, &sess, 1000 + i * 400,
                                            DR_SCENE_HOME, &rng, &idx);
        if (r == DR_EVENT_FIRED) assert(idx != 0);
    }

    // 条件满足:id0 优先级 10 必胜
    g.res[DR_RES_WOOD] = 10;
    dr_rng_seed(&rng, 1);
    assert(dr_event_pick(tbl, 2, &g, &sess, 5000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_FIRED && idx == 0);

    // 选项0:人口+1、标记0
    assert(dr_event_choose(tbl, 2, &g, &sess, 5000, &rng, 0, 0) == -1);
    assert(g.population == 1);
    assert((g.flags & 0x1) == 1);
    assert(g.event_count[0] == 1);

    // 一次性:id0 不再触发
    uint16_t idx2;
    dr_rng_seed(&rng, 2);
    dr_event_result_t r2 =
        dr_event_pick(tbl, 2, &g, &sess, 10000, DR_SCENE_HOME, &rng, &idx2);
    if (r2 == DR_EVENT_FIRED) assert(idx2 != 0);

    // 选项1 效果(另一档):+5 木计入存量与累计
    dr_game_t g2;
    dr_game_init(&g2, 1, 1000);
    g2.res[DR_RES_WOOD] = 12;
    dr_event_session_t sess2;
    dr_event_session_init(&sess2);
    dr_rng_seed(&rng, 1);
    assert(dr_event_pick(tbl, 2, &g2, &sess2, 1000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_FIRED);
    uint32_t wood_before = g2.res[DR_RES_WOOD];
    dr_event_choose(tbl, 2, &g2, &sess2, 1000, &rng, idx, 1);
    assert(g2.res[DR_RES_WOOD] == wood_before + 5);
    assert(g2.res_total[DR_RES_WOOD] == 15 + 5);   // 累计口径(新档起始 15 木)
    assert(g2.population == 0);
    assert((g2.flags >> 1 & 0x1) == 1);
}

static void test_session_cooldown_and_gate(void) {
    dr_event_t tbl[1] = {0};
    tbl[0].scene_mask = DR_SCENE_ANY;
    tbl[0].priority = 1;
    tbl[0].max_seen = 0;
    tbl[0].cooldown_s = 30;
    tbl[0].choice_count = 1;
    tbl[0].conds[0] = (dr_cond_t){ (uint8_t)DR_COND_CHANCE, 0, 1000 };  // 必中

    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    dr_event_session_t sess;
    dr_event_session_init(&sess);
    dr_rng_t rng;
    dr_rng_seed(&rng, 1);

    uint16_t idx;
    assert(dr_event_pick(tbl, 1, &g, &sess, 1000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_FIRED && idx == 0);
    // 全局节拍门:触发后 180~360s 内任何事件不轮询(先于事件冷却判定)
    assert(dr_event_pick(tbl, 1, &g, &sess, 1100, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_NONE);
    assert(sess.next_ok_ts >= 1000 + 180 && sess.next_ok_ts <= 1000 + 360);
    assert(dr_event_choose(tbl, 1, &g, &sess, 1000, &rng, 0, 0) == -1);
    assert(g.event_count[0] == 1);
    // 门已过但事件冷却(30s)未过 → 仍不触发
    assert(dr_event_pick(tbl, 1, &g, &sess, 1020, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_NONE);
    // 冷却与门都过后可再触发(条件必中)
    assert(dr_event_pick(tbl, 1, &g, &sess, 1400, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_FIRED && idx == 0);
}

static void test_scene_mask_and_chain_effect(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.res[DR_RES_WOOD] = 10;

    dr_event_t tbl[2] = {0};
    tbl[0].scene_mask = DR_SCENE_MAP;   // 只在地图场景
    tbl[0].priority = 5;
    tbl[0].max_seen = 0;
    tbl[0].choice_count = 1;
    tbl[0].choices[0].effects[0] =
        (dr_eff_t){ (uint8_t)DR_EFF_GOTO_EVENT, 1, 0 };
    tbl[1].scene_mask = DR_SCENE_ANY;
    tbl[1].priority = 1;
    tbl[1].max_seen = 0;
    tbl[1].choice_count = 1;
    tbl[1].choices[0].effects[0] =
        (dr_eff_t){ (uint8_t)DR_EFF_RES_ADD, DR_RES_FOOD, 3 };

    dr_event_session_t sess;
    dr_event_session_init(&sess);
    dr_rng_t rng;
    dr_rng_seed(&rng, 1);

    uint16_t idx;
    // HOME 场景:tbl[0] 被场景掩码挡住
    assert(dr_event_pick(tbl, 2, &g, &sess, 1000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_FIRED && idx == 1);
    // MAP 场景:优先级高者胜(时刻已过全局门)
    dr_rng_seed(&rng, 1);
    assert(dr_event_pick(tbl, 2, &g, &sess, 2000, DR_SCENE_MAP, &rng, &idx)
           == DR_EVENT_FIRED && idx == 0);
    // 选项0 效果为链到事件1
    assert(dr_event_choose(tbl, 2, &g, &sess, 2000, &rng, 0, 0) == 1);
}

static void test_building_lt_and_pop_lt(void) {
    dr_event_t tbl[1] = {0};
    tbl[0].scene_mask = DR_SCENE_HOME;
    tbl[0].priority = 1;
    tbl[0].choice_count = 1;
    tbl[0].conds[0] = (dr_cond_t){ (uint8_t)DR_COND_BUILDING_GE, DR_BLD_HUT, 5 };
    tbl[0].conds[1] = (dr_cond_t){ (uint8_t)DR_COND_BUILDING_LT, DR_BLD_HUT, 20 };

    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    dr_event_session_t sess;
    dr_event_session_init(&sess);
    dr_rng_t rng;
    dr_rng_seed(&rng, 1);
    uint16_t idx;

    g.building_lv[DR_BLD_HUT] = 4;    // <5:不触发
    assert(dr_event_pick(tbl, 1, &g, &sess, 1000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_NONE);
    g.building_lv[DR_BLD_HUT] = 5;    // 5..19:触发(阴险工匠口径)
    assert(dr_event_pick(tbl, 1, &g, &sess, 2000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_FIRED);
    g.building_lv[DR_BLD_HUT] = 20;   // ≥20:不再触发
    assert(dr_event_pick(tbl, 1, &g, &sess, 3000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_NONE);

    // POP_LT(疫病口径:11 ≤ pop < 50)
    tbl[0].conds[0] = (dr_cond_t){ (uint8_t)DR_COND_POP_GE, 0, 11 };
    tbl[0].conds[1] = (dr_cond_t){ (uint8_t)DR_COND_POP_LT, 0, 50 };
    g.building_lv[DR_BLD_HUT] = 0;
    g.population = 10;
    assert(dr_event_pick(tbl, 1, &g, &sess, 4000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_NONE);
    g.population = 11;
    assert(dr_event_pick(tbl, 1, &g, &sess, 5000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_FIRED);
    g.population = 50;
    assert(dr_event_pick(tbl, 1, &g, &sess, 6000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_NONE);
}

// POP_ADD 减员裁岗(§3.3 硬化):在编总数不得超过人口,从职业表尾端撤
static void test_pop_add_clips_jobs(void) {
    dr_event_t tbl[1] = {0};
    tbl[0].scene_mask = DR_SCENE_ANY;
    tbl[0].choice_count = 1;
    tbl[0].choices[0].effects[0] = (dr_eff_t){ (uint8_t)DR_EFF_POP_ADD, 0, -5 };

    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.population = 10;
    g.job[0] = 3;   // 猎人 3
    g.job[1] = 4;   // 捕兽人 4(在编 7)
    dr_event_session_t sess;
    dr_event_session_init(&sess);
    dr_rng_t rng;
    dr_rng_seed(&rng, 1);
    uint16_t idx;
    assert(dr_event_pick(tbl, 1, &g, &sess, 1000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_FIRED);
    dr_event_choose(tbl, 1, &g, &sess, 1000, &rng, 0, 0);
    assert(g.population == 5);
    assert(g.job[0] == 3 && g.job[1] == 2);   // 尾端(捕兽人)撤 2,猎人保留

    // 减员穿底到 0:全岗位清空,无幽灵工人
    g.population = 2;
    g.job[0] = 1; g.job[1] = 1;
    dr_rng_seed(&rng, 2);
    assert(dr_event_pick(tbl, 1, &g, &sess, 2000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_FIRED);
    dr_event_choose(tbl, 1, &g, &sess, 2000, &rng, 0, 0);
    assert(g.population == 0);
    assert(g.job[0] == 0 && g.job[1] == 0);
}

// 比例减员与建筑增减(M6 新效果)
static void test_frac_and_bld_effects(void) {
    dr_event_t tbl[1] = {0};
    tbl[0].scene_mask = DR_SCENE_ANY;
    tbl[0].choice_count = 1;

    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    dr_event_session_t sess;
    dr_event_session_init(&sess);
    dr_rng_t rng;
    dr_rng_seed(&rng, 1);
    uint16_t idx;

    // POP_FRAC 250‰:pop 40 → 失 10;在编同步裁
    g.population = 40;
    g.job[0] = 30;
    tbl[0].choices[0].effects[0] = (dr_eff_t){ (uint8_t)DR_EFF_POP_FRAC, 0, 250 };
    assert(dr_event_pick(tbl, 1, &g, &sess, 1000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_FIRED);
    dr_event_choose(tbl, 1, &g, &sess, 1000, &rng, 0, 0);
    assert(g.population == 30);
    assert(g.job[0] == 30);            // 在编未超员,不裁
    // POP_FRAC 至少 1:pop 3 × 250‰ = 0 → 失 1
    g.population = 3;
    tbl[0].cooldown_s = 0;
    assert(dr_event_pick(tbl, 1, &g, &sess, 2000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_FIRED);
    dr_event_choose(tbl, 1, &g, &sess, 2000, &rng, 0, 0);
    assert(g.population == 2);

    // BLD_ADD:白盖屋 +1 / 失火 -1 钳 0
    g.building_lv[DR_BLD_HUT] = 5;
    tbl[0].choices[0].effects[0] = (dr_eff_t){ (uint8_t)DR_EFF_BLD_ADD, DR_BLD_HUT, 1 };
    assert(dr_event_pick(tbl, 1, &g, &sess, 3000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_FIRED);
    dr_event_choose(tbl, 1, &g, &sess, 3000, &rng, 0, 0);
    assert(g.building_lv[DR_BLD_HUT] == 6);
    tbl[0].choices[0].effects[0] = (dr_eff_t){ (uint8_t)DR_EFF_BLD_ADD, DR_BLD_HUT, -1 };
    assert(dr_event_pick(tbl, 1, &g, &sess, 4000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_FIRED);
    dr_event_choose(tbl, 1, &g, &sess, 4000, &rng, 0, 0);
    assert(g.building_lv[DR_BLD_HUT] == 5);

    // BLD_FRAC 500‰:陷阱 10 → 失 5;陷阱 1 → 至少失 1
    g.building_lv[DR_BLD_TRAP] = 10;
    tbl[0].choices[0].effects[0] = (dr_eff_t){ (uint8_t)DR_EFF_BLD_FRAC, DR_BLD_TRAP, 500 };
    assert(dr_event_pick(tbl, 1, &g, &sess, 5000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_FIRED);
    dr_event_choose(tbl, 1, &g, &sess, 5000, &rng, 0, 0);
    assert(g.building_lv[DR_BLD_TRAP] == 5);
    g.building_lv[DR_BLD_TRAP] = 1;
    assert(dr_event_pick(tbl, 1, &g, &sess, 6000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_FIRED);
    dr_event_choose(tbl, 1, &g, &sess, 6000, &rng, 0, 0);
    assert(g.building_lv[DR_BLD_TRAP] == 0);
}

// 加权跳转 GOTO_W(乞讨者 50/30/20 口径):同一种子分布确定;兜底 GOTO 生效
static void test_goto_w_branch(void) {
    // choices[0] = [GOTO_W(1,500), GOTO_W(2,600), GOTO(3)]
    dr_event_t tbl[4] = {0};
    tbl[0].scene_mask = DR_SCENE_ANY;
    tbl[0].priority = 1;
    tbl[0].choice_count = 1;
    tbl[0].choices[0].effects[0] = (dr_eff_t){ (uint8_t)DR_EFF_GOTO_W, 1, 500 };
    tbl[0].choices[0].effects[1] = (dr_eff_t){ (uint8_t)DR_EFF_GOTO_W, 2, 600 };
    tbl[0].choices[0].effects[2] = (dr_eff_t){ (uint8_t)DR_EFF_GOTO_EVENT, 3, 0 };

    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    dr_event_session_t sess;
    dr_event_session_init(&sess);
    dr_rng_t rng0;
    dr_rng_seed(&rng0, 9);
    uint16_t idx;
    assert(dr_event_pick(tbl, 4, &g, &sess, 1000, DR_SCENE_HOME, &rng0, &idx)
           == DR_EVENT_FIRED && idx == 0);

    int cnt[4] = { 0, 0, 0, 0 };
    dr_rng_t rng;
    dr_rng_seed(&rng, 42);   // 单流连续掷点(逐次重播种会吃 xorshift 首轮高位偏置)
    for (int s = 1; s <= 4000; s++) {
        int16_t chain = dr_event_choose(tbl, 4, &g, &sess, 1000, &rng, 0, 0);
        assert(chain == 1 || chain == 2 || chain == 3);
        cnt[chain]++;
        g.event_count[0] = 0;   // 重复选择同一入口(仅统计分支)
    }
    // 4000 掷:三支各占约 50%/30%/20%(xorshift 均匀性,±5% 容差)
    assert(cnt[1] > 1800 && cnt[1] < 2200);
    assert(cnt[2] > 1000 && cnt[2] < 1400);
    assert(cnt[3] > 600 && cnt[3] < 1000);
}

// 选项代价:付得起校验/扣除/不足拒绝且零副作用
static void test_choice_cost(void) {
    dr_event_t tbl[1] = {0};
    tbl[0].scene_mask = DR_SCENE_ANY;
    tbl[0].priority = 1;
    tbl[0].choice_count = 2;
    tbl[0].choices[0].text_id = DR_T_UI_CONFIRM;
    tbl[0].choices[0].cost_res[0] = DR_RES_FUR;
    tbl[0].choices[0].cost_amt[0] = 50;
    tbl[0].choices[0].effects[0] = (dr_eff_t){ (uint8_t)DR_EFF_RES_ADD, DR_RES_SCALES, 20 };
    tbl[0].choices[1].text_id = DR_T_UI_BACK;   // 无代价

    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.res[DR_RES_FUR] = 60;
    dr_event_session_t sess;
    dr_event_session_init(&sess);
    dr_rng_t rng;
    dr_rng_seed(&rng, 1);

    // 付得起:扣除并给效果
    assert(dr_event_choice_affordable(&tbl[0], 0, &g));
    assert(dr_event_choose(tbl, 1, &g, &sess, 1000, &rng, 0, 0) == -1);
    assert(g.res[DR_RES_FUR] == 10);
    assert(g.res[DR_RES_SCALES] == 20);

    // 不再付得起:拒绝(-2),无任何资源变动
    assert(!dr_event_choice_affordable(&tbl[0], 0, &g));
    uint32_t fur = g.res[DR_RES_FUR], sc = g.res[DR_RES_SCALES];
    assert(dr_event_choose(tbl, 1, &g, &sess, 1000, &rng, 0, 0)
           == DR_EVENT_CHOOSE_REJECTED);
    assert(g.res[DR_RES_FUR] == fur && g.res[DR_RES_SCALES] == sc);

    // 无代价选项恒可用
    assert(dr_event_choice_affordable(&tbl[0], 1, &g));
}

// dr_event_poll:未触发也回写概率序列(§3.3 硬化)——CHANCE 未命中
// 不能每次轮询重掷同一个值;场景不匹配的轮询同样推进序列。
static void test_poll_advances_seed(void) {
    dr_event_t tbl[1] = {0};
    tbl[0].scene_mask = DR_SCENE_HOME;
    tbl[0].choice_count = 1;
    tbl[0].conds[0] = (dr_cond_t){ (uint8_t)DR_COND_CHANCE, 0, 1 };  // 1‰

    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    dr_event_session_t sess;
    dr_event_session_init(&sess);
    uint16_t idx;

    uint32_t seed0 = g.rng_seed_state;
    (void)dr_event_poll(tbl, 1, &g, &sess, 1000, DR_SCENE_HOME, &idx);
    assert(g.rng_seed_state != seed0);   // 修前:未触发不回写,恒为 seed0

    // 场景不匹配(事件挂 HOME,按 VILLAGE 轮询):序列同样推进
    uint32_t seed1 = g.rng_seed_state;
    (void)dr_event_poll(tbl, 1, &g, &sess, 1000, DR_SCENE_VILLAGE, &idx);
    assert(g.rng_seed_state != seed1);

    // 连续轮询序列持续变化(不存在停在首个值上的死序列)
    uint32_t prev = g.rng_seed_state;
    for (int i = 0; i < 8; i++) {
        (void)dr_event_poll(tbl, 1, &g, &sess, 1000, DR_SCENE_HOME, &idx);
        assert(g.rng_seed_state != prev);
        prev = g.rng_seed_state;
    }
}

// M6 事件表数据校验:文案/操作码/代价/链目标/场景全量合法
static void test_table_data_valid(void) {
    uint16_t count = 0;
    const dr_event_t *ev = dr_events_table(&count);
    uint16_t text_max = 0;
    while (dr_text(text_max + 1)[0] != '\0') text_max++;   // 最大有效文案 id

    int polled = 0;
    for (uint16_t i = 0; i < count; i++) {
        const dr_event_t *e = &ev[i];
        assert(e->text_id != 0 && e->text_id <= text_max);
        assert(e->choice_count >= 1 && e->choice_count <= DR_EVENT_MAX_CHOICES);
        assert(e->scene_mask <= DR_SCENE_ANY);
        if (e->scene_mask != 0) polled++;
        for (int c = 0; c < DR_EVENT_MAX_CONDS; c++) {
            uint8_t op = e->conds[c].op;
            if (op == DR_COND_NONE) break;
            assert(op <= DR_COND_POP_LT);              // 已知条件码
            if (op == DR_COND_RES_GE || op == DR_COND_RES_LT)
                assert(e->conds[c].arg1 < DR_RES_KIND_COUNT);
            if (op == DR_COND_BUILDING_GE || op == DR_COND_BUILDING_LT)
                assert(e->conds[c].arg1 < DR_BLD_KIND_COUNT);
        }
        for (int c = 0; c < e->choice_count; c++) {
            const dr_event_choice_t *ch = &e->choices[c];
            assert(ch->text_id != 0 && ch->text_id <= text_max);
            for (int k = 0; k < 2; k++) {
                if (ch->cost_res[k] == 0xFF) continue;
                assert(ch->cost_res[k] < DR_RES_KIND_COUNT);
                assert(ch->cost_amt[k] > 0);           // 代价非零
            }
            for (int k = 0; k < 4; k++) {
                uint8_t op = ch->effects[k].op;
                if (op == DR_EFF_NONE) break;
                assert(op <= DR_EFF_GOTO_W);           // 已知效果码
                if (op == DR_EFF_GOTO_EVENT || op == DR_EFF_GOTO_W)
                    assert((uint16_t)ch->effects[k].arg1 < count);  // 链在表内
                if (op == DR_EFF_RES_ADD)
                    assert(ch->effects[k].arg1 < DR_RES_KIND_COUNT);
                if (op == DR_EFF_BLD_ADD || op == DR_EFF_BLD_FRAC)
                    assert(ch->effects[k].arg1 < DR_BLD_KIND_COUNT);
            }
        }
    }
    assert(polled >= 10);   // 轮询入口数(切片一 12)
}

// 端到端走一条链:乞讨者给 50 毛(种子确定分支)→ 结果事件给 20 资源
static void test_beggar_flow(void) {
    uint16_t count = 0;
    const dr_event_t *ev = dr_events_table(&count);
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.res[DR_RES_FUR] = 100;
    dr_event_session_t sess;
    dr_event_session_init(&sess);

    // 从表中定位乞讨者入口(按文案,避免硬编码下标)
    uint16_t beggar = count;
    for (uint16_t i = 0; i < count; i++)
        if (ev[i].text_id == DR_T_EV_BEGGAR) { beggar = i; break; }
    assert(beggar < count);

    dr_rng_t rng;
    dr_rng_seed(&rng, 3);
    int16_t chain = dr_event_choose(ev, count, &g, &sess, 1000, &rng, beggar, 0);
    assert(chain >= 0);                    // 链到三支之一
    assert(g.res[DR_RES_FUR] == 50);       // 代价已扣
    assert(ev[chain].text_id == DR_T_EV_BEGGAR_S ||
           ev[chain].text_id == DR_T_EV_BEGGAR_T ||
           ev[chain].text_id == DR_T_EV_BEGGAR_C);
    dr_event_choose(ev, count, &g, &sess, 1000, &rng, (uint16_t)chain, 0);
    uint32_t sums = g.res[DR_RES_SCALES] + g.res[DR_RES_TEETH] + g.res[DR_RES_CLOTH];
    assert(sums == 20);                    // 恰一支 +20
}

#define RUN(t) do { printf("%s\n", #t); fflush(stdout); t(); } while (0)
int main(void) {
    RUN(test_table_not_empty);
    RUN(test_condition_gates_and_effects);
    RUN(test_session_cooldown_and_gate);
    RUN(test_scene_mask_and_chain_effect);
    RUN(test_building_lt_and_pop_lt);
    RUN(test_pop_add_clips_jobs);
    RUN(test_frac_and_bld_effects);
    RUN(test_goto_w_branch);
    RUN(test_choice_cost);
    RUN(test_poll_advances_seed);
    RUN(test_table_data_valid);
    RUN(test_beggar_flow);
    printf("test_dr_events: all passed\n");
    return 0;
}
