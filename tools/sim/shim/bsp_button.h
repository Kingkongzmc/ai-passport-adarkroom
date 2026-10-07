#pragma once
#include <stdbool.h>

typedef enum { BSP_BTN_UP = 0, BSP_BTN_DOWN, BSP_BTN_OK } bsp_btn_t;
typedef enum {
    BSP_BTN_PRESS = 0, BSP_BTN_CLICK, BSP_BTN_DOUBLE, BSP_BTN_LONG,
    BSP_BTN_HOLD,
} bsp_btn_ev_t;

// 主机垫片:模拟器没有 ADC,恒"读不到"——mv=-1、窗口鉴别一律放行(与设备侧
// 读取失败时的 fail-open 同口径),按键行为与旧版完全一致。
static inline int bsp_button_read_mv(void) { return -1; }
static inline bool bsp_button_mv_in_window(bsp_btn_t btn) {
    (void)btn;
    return true;
}
