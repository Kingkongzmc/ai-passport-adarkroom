#pragma once
// 模拟器垫片:电量计返回固定值
static inline int bsp_battery_init(void) { return 0; }
static inline int bsp_battery_soc(void) { return 87; }
static inline int bsp_battery_mv(void) { return 3900; }
static inline int bsp_battery_sleep(void) { return 0; }
