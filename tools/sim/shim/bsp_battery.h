#pragma once
// 模拟器垫片:电量计返回固定值;SIM_SOC=<百分比> 环境变量覆盖,
// 供低电降背光(≤20%)/低电深睡(≤10%)的模拟器断言用。
#include <stdlib.h>
static inline int bsp_battery_init(void) { return 0; }
static inline int bsp_battery_soc(void) {
    char *e = getenv("SIM_SOC");
    return e ? atoi(e) : 87;
}
static inline int bsp_battery_mv(void) { return 3900; }
static inline int bsp_battery_sleep(void) { return 0; }
