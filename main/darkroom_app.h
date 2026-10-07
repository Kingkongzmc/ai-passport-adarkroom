// main/darkroom_app.h —— 《小黑屋》LVGL 应用壳。
#pragma once

#include <stdbool.h>

#include "bsp_button.h"
#include "dr_rules.h"
#include "dr_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void darkroom_app_enter(void);
void darkroom_app_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void darkroom_app_stop(void);
void dr_debug_dump(void);       // 模拟器诊断:打印关键状态
void dr_sweep_next(void);       // 模拟器:页面轮播(SIM_PAGE_SWEEP)
void dr_debug_set_wild(uint8_t x, uint8_t y);  // 模拟器:注入远征态(SIM_WILD)
void dr_debug_force_event(uint16_t idx);       // 模拟器:直达事件弹窗(SIM_EVENT)

#ifdef __cplusplus
}
#endif
