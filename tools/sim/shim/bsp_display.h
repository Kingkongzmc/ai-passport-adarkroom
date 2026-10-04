#pragma once
#include <stdbool.h>
#include <stdint.h>
// 真互斥:LVGL 在模拟器里同样非线程安全(按键线程 vs 主循环 timer handler)
bool sim_lvgl_lock(int timeout_ms);
void sim_lvgl_unlock(void);
static inline bool bsp_lvgl_lock(int timeout_ms) { return sim_lvgl_lock(timeout_ms); }
static inline void bsp_lvgl_unlock(void) { sim_lvgl_unlock(); }

// 背光桩:记录最近档位(熄屏功能模拟器断言用;定义在各驱动 main 文件)
extern int g_sim_backlight;
static inline void bsp_display_backlight(uint8_t percent) { g_sim_backlight = percent; }
