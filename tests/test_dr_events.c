// tests/test_dr_events.c —— 事件运行时主机测试:条件求值、场景/优先级/一次性/
// 冷却、效果(资源增减钳底、标记、人口、事件链)、累计口径。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dr_events.h"
#include "dr_state.h"
#include "dr_text.h"
#include "dr_util.h"

static const dr_event_t *ev;
static uint16_t ev_count;

static void setup(void) {
    ev = dr_events_table(&ev_count);
    assert(ev != NULL);
    assert(ev_count >= 1);
}

static void test_stranger_onchain(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.res[DR_RES_WOOD] = 10;  // 恰好达到触发门槛

    dr_event_session_t sess;
    dr_event_session_init(&sess);
    dr_rng_t rng;
    dr_rng_seed(&rng, 1);

    uint16_t idx = 0xFFFF;
    // 自测事件(id1)是 10% 概率,可能同帧命中;陌生人优先级 10 > 1,必胜
    assert(dr_event_pick(ev, ev_count, &g, &sess, 2000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_FIRED);
    assert(idx == 0);
    assert(strcmp(dr_text(ev[0].text_id), "门被推开,一个陌生的女人站在寒风里。她说她可以帮忙看火。") == 0);

    // 选"让她留下":人口+1、标记0置位
    int16_t chain = dr_event_choose(ev, ev_count, &g, &sess, 2000, 0, 0);
    assert(chain == -1);
    assert(g.population == 1);
    assert((g.flags & 0x1) == 1);
    assert(g.event_count[0] == 1);

    // 一次性:max_seen=1,陌生人(id0)不再触发(自测事件 id1 概率命中是允许的)
    uint16_t idx2;
    dr_rng_t rng2;
    dr_rng_seed(&rng2, 2);
    dr_event_result_t r2 =
        dr_event_pick(ev, ev_count, &g, &sess, 3000, DR_SCENE_HOME, &rng2, &idx2);
    if (r2 == DR_EVENT_FIRED) assert(idx2 != 0);
}

static void test_stranger_reject_branch(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.res[DR_RES_WOOD] = 12;

    dr_event_session_t sess;
    dr_event_session_init(&sess);
    dr_rng_t rng;
    dr_rng_seed(&rng, 1);

    uint16_t idx;
    assert(dr_event_pick(ev, ev_count, &g, &sess, 1000, DR_SCENE_HOME, &rng, &idx)
           == DR_EVENT_FIRED);
    uint32_t wood_before = g.res[DR_RES_WOOD];
    dr_event_choose(ev, ev_count, &g, &sess, 1000, idx, 1);  // 赶走她:+5 木
    assert(g.res[DR_RES_WOOD] == wood_before + 5);
    assert(g.res_total[DR_RES_WOOD] == 5);  // 累计口径:初始为 0,事件收入计入累计
    assert(g.population == 0);
    assert((g.flags >> 1 & 0x1) == 1);
}

static void test_condition_gates(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.res[DR_RES_WOOD] = 9;  // 低于门槛 10

    dr_event_session_t sess;
    dr_event_session_init(&sess);
    dr_rng_t rng;
    dr_rng_seed(&rng, 1);

    uint16_t idx;
    // 陌生人条件不满足;自测事件概率用确定性种子逐个试,存在全不命中的种子;
    // 用循环证明:任何结果下 idx 都不会是 0
    for (int i = 0; i < 8; i++) {
        dr_rng_seed(&rng, (uint32_t)i);
        dr_event_result_t r = dr_event_pick(ev, ev_count, &g, &sess, 1000,
                                            DR_SCENE_HOME, &rng, &idx);
        if (r == DR_EVENT_FIRED) assert(idx != 0);
    }
}

static void test_session_cooldown(void) {
    // 全局表现在只有陌生人事件(一次性),冷却语义用本地表验证
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

    // 手工构造含场景限制与事件链的表
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

static void test_resource_clamp(void) {
    dr_game_t g;
    dr_game_init(&g, 1, 1000);
    g.res[DR_RES_WOOD] = 3;

    dr_event_t tbl[1] = {0};
    tbl[0].scene_mask = DR_SCENE_ANY;
    tbl[0].priority = 1;
    tbl[0].max_seen = 0;
    tbl[0].choice_count = 1;
    tbl[0].choices[0].effects[0] =
        (dr_eff_t){ (uint8_t)DR_EFF_RES_ADD, DR_RES_WOOD, -10 };  // 扣 10,存量仅 3

    dr_event_session_t sess;
    dr_event_session_init(&sess);
    dr_event_choose(tbl, 1, &g, &sess, 1000, 0, 0);
    assert(g.res[DR_RES_WOOD] == 0);      // 钳到 0,不回绕
    assert(g.res_total[DR_RES_WOOD] == 0);  // 扣减不动累计
}

int main(void) {
    setup();
    test_stranger_onchain();
    test_stranger_reject_branch();
    test_condition_gates();
    test_session_cooldown();
    test_scene_mask_and_chain_effect();
    test_resource_clamp();
    printf("test_dr_events: all passed\n");
    return 0;
}
