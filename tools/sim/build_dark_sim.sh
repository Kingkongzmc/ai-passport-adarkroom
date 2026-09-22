#!/usr/bin/env bash
# 构建小黑屋主机模拟器:LVGL 全源码 + 游戏逻辑/UI + 垫片 + sim_dark。
set -e
cd "$(dirname "$0")/../.."
gcc -g -O0 -DLV_CONF_INCLUDE_SIMPLE \
    -Itools/sim -Itools/sim/shim -Imanaged_components/lvgl__lvgl \
    -Imain -Imain/game_darkroom \
    $(find managed_components/lvgl__lvgl/src -name '*.c') \
    main/darkroom_app.c \
    main/game_darkroom/dr_rules.c main/game_darkroom/dr_state.c \
    main/game_darkroom/dr_events.c main/game_darkroom/dr_events_data.c \
    main/game_darkroom/dr_text.c main/game_darkroom/dr_util.c \
    assets/fonts/dr_font_12.c assets/fonts/dr_font_16.c assets/fonts/dr_font_24.c \
    tools/sim/sim_dark.c -o tools/sim/sim_dark.exe -lm
echo "sim_dark built"
