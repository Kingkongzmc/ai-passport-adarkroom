// main/game_darkroom/dr_events_data.c —— 事件表(M1 种子版,M6 全量灌装)。
// 纯数据,无逻辑;文案引用 dr_text.h 的 id。
#include "dr_events.h"

#define C(op, a1, a2) { (uint8_t)(op), (a1), (a2) }
#define E(op, a1, a2) { (uint8_t)(op), (a1), (int16_t)(a2) }
#define COND_NONE C(DR_COND_NONE, 0, 0)
#define EFF_NONE  E(DR_EFF_NONE, 0, 0)

static const dr_event_t k_events[] = {
    // id 0:开场剧情——陌生人到来(一次性,主页,木材>=10 时触发)。
    // 留下:人口+1、置标记0;赶走:置标记1、获得木材(她留下的柴)。
    {
        .text_id = DR_T_EV_STRANGER_1,
        .scene_mask = DR_SCENE_HOME,
        .priority = 10,
        .max_seen = 1,
        .cooldown_s = 0,
        .conds = { C(DR_COND_RES_GE, DR_RES_WOOD, 10), COND_NONE, COND_NONE, COND_NONE },
        .choices = {
            { DR_T_EV_STRANGER_1_C0, { E(DR_EFF_POP_ADD, 0, 1),
                                       E(DR_EFF_FLAG_SET, 0, 0) } },
            { DR_T_EV_STRANGER_1_C1, { E(DR_EFF_RES_ADD, DR_RES_WOOD, 5),
                                       E(DR_EFF_FLAG_SET, 1, 0) } },
        },
        .choice_count = 2,
    },
};

const dr_event_t *dr_events_table(uint16_t *count) {
    if (count) *count = sizeof(k_events) / sizeof(k_events[0]);
    return k_events;
}
