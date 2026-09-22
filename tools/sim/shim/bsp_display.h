#pragma once
#include <stdbool.h>
// 真互斥:LVGL 在模拟器里同样非线程安全(按键线程 vs 主循环 timer handler)
bool sim_lvgl_lock(int timeout_ms);
void sim_lvgl_unlock(void);
static inline bool bsp_lvgl_lock(int timeout_ms) { return sim_lvgl_lock(timeout_ms); }
static inline void bsp_lvgl_unlock(void) { sim_lvgl_unlock(); }
