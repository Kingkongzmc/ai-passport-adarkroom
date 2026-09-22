// main/game_darkroom/dr_text.h —— 文案索引(id → UTF-8 字符串)。
// M1 只放引擎自测与教学用文案;正式文案 M6 灌装,同时统计封闭字集生成字库。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 文案 id 空间(事件标题/选项复用同一表;分段按需扩)。
enum {
    DR_T_UI_TITLE = 1,     // 《小黑屋》
    DR_T_UI_BACK,          // 返回
    DR_T_UI_CONFIRM,       // 确定
    DR_T_ACT_FIRE,         // 生火
    DR_T_ACT_GATHER,       // 收集木材
    DR_T_EV_STRANGER_1,    // 开场剧情:陌生人到来(测试用)
    DR_T_EV_STRANGER_1_C0, //   选项:让她留下
    DR_T_EV_STRANGER_1_C1, //   选项:赶走她
    DR_T_EV_DEBUG,         // 引擎自测事件
};

const char *dr_text(uint16_t id);

#ifdef __cplusplus
}
#endif
