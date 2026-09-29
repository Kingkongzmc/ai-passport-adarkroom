// tests/test_dr_events.c —— 事件运行时主机测试:条件求值、场景/优先级/一次性/
// 冷却、效果(资源增减钳底、标记、人口、事件链)、累计口径。
// 事件表(dr_events_data)当前为空表——内容 M6 灌装;引擎机制用本地表自测。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dr_events.h"
#include "dr_state.h"
#include "dr_text.h"
#include "dr_util.h"

static void test_table_empty_but_valid(void) {
    uint16_t count = 99;
    const dr_event_t *ev = dr_events_table(&count);
    assert(ev != NULL);
    assert(count == 0);   // 空表:M6 灌装前
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
        dr_event_result_t r = dr_event_pick(tbl, 2, &g, &sess, 1000,
                                            DR_SCENE_HOME, &rng, &idx);
        if (r == DR_EVENT_FIRED) assert(idx != 0);
    }

    // 条件满足:id0 优先级 10 必胜
    g.res[DR_RES_WOOD] = 10;
    dr_rng_seed(&rng, 1);
    assert(dr_event_pick(tbl, 2, &g, &sess, 2000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_FIRED && idx == 0);

    // 选项0:人口+1、标记0
    assert(dr_event_choose(tbl, 2, &g, &sess, 2000, 0, 0) == -1);
    assert(g.population == 1);
    assert((g.flags & 0x1) == 1);
    assert(g.event_count[0] == 1);

    // 一次性:id0 不再触发
    uint16_t idx2;
    dr_rng_seed(&rng, 2);
    dr_event_result_t r2 =
        dr_event_pick(tbl, 2, &g, &sess, 3000, DR_SCENE_HOME, &rng, &idx2);
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
    dr_event_choose(tbl, 2, &g2, &sess2, 1000, idx, 1);
    assert(g2.res[DR_RES_WOOD] == wood_before + 5);
    assert(g2.res_total[DR_RES_WOOD] == 15 + 5);   // 累计口径(新档起始 15 木)
    assert(g2.population == 0);
    assert((g2.flags >> 1 & 0x1) == 1);
}

static void test_session_cooldown(void) {
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
    assert(dr_event_choose(tbl, 1, &g, &sess, 1000, 0, 0) == -1);
    assert(g.event_count[0] == 1);

    // 冷却 30s 内不触发
    dr_rng_seed(&rng, 1);
    assert(dr_event_pick(tbl, 1, &g, &sess, 1000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_NONE);
    // 冷却过后可再触发(条件必中)
    assert(dr_event_pick(tbl, 1, &g, &sess, 1031, DR_SCENE_HOME, &rng, &idx)
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
    // MAP 场景:优先级高者胜
    dr_rng_seed(&rng, 1);
    assert(dr_event_pick(tbl, 2, &g, &sess, 1000, DR_SCENE_MAP, &rng, &idx)
           == DR_EVENT_FIRED && idx == 0);
    // 选项0 效果为链到事件1
    assert(dr_event_choose(tbl, 2, &g, &sess, 1000, 0, 0) == 1);
}

int main(void) {
    test_table_empty_but_valid();
    test_condition_gates_and_effects();
    test_session_cooldown();
    test_scene_mask_and_chain_effect();
    printf("test_dr_events: all passed\n");
    return 0;
}
