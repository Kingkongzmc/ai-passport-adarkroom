// main/game_darkroom/dr_events_data.c —— 事件表(M6 全量灌装)。
// 纯数据,无逻辑;文案引用 dr_text.h 的 id。
// v3 起"陌生人"不再走事件对话框——对齐原版:陌生人(建造者)是规则层状态机
// (晕倒→发抖→沉睡→帮忙,室温驱动,见 dr_rules.c),无玩家选择分支。
// 引擎与文案 id 保留,M6 按原版 events.js 灌装 50~70 条。
#include "dr_events.h"

#define C(op, a1, a2) { (uint8_t)(op), (a1), (a2) }
#define E(op, a1, a2) { (uint8_t)(op), (a1), (int16_t)(a2) }
#define COND_NONE C(DR_COND_NONE, 0, 0)
#define EFF_NONE  E(DR_EFF_NONE, 0, 0)

static const dr_event_t k_events[] = {
    // (空表:M6 灌装;开场剧情由规则层建造者状态机承担)
};

const dr_event_t *dr_events_table(uint16_t *count) {
    if (count) *count = sizeof(k_events) / sizeof(k_events[0]);
    return k_events;
}
