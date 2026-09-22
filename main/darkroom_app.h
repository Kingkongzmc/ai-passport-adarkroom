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
void dr_debug_dump(void);  // 临时:模拟器诊断
void dr_sweep_next(void);  // 临时:模拟器页面轮播

#ifdef __cplusplus
}
#endif
