#define LV_CONF_H
// 主机模拟用 LVGL 配置:其余选项走 lv_conf_internal 默认值。
#pragma once
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0
#define LV_USE_OS LV_OS_NONE
#define LV_USE_THEME_DEFAULT 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_FONT_MONTSERRAT_14 1
// 与设备 sdkconfig 对齐:浅色主题(设备 CONFIG_LV_THEME_DEFAULT_DARK 未设置)
#define LV_THEME_DEFAULT_DARK 0
// 主机内存充裕:用 libc 分配器,绕开内置 64KB 池(200+ 对象的完整游戏 UI 会爆池)
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB
