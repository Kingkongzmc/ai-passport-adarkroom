#!/usr/bin/env bash
# 构建主机模拟器:LVGL 全源码 + 游戏代码 + 垫片,帧缓冲导出 BMP。
set -e
cd "$(dirname "$0")/../.."
SRC=$(find managed_components/lvgl__lvgl/src -name '*.c')
gcc -g -O0 -DLV_CONF_INCLUDE_SIMPLE -Itools/sim -Itools/sim/shim -Imanaged_components/lvgl__lvgl -Imain \
    \
    $SRC \
    main/roulette_app.c main/roulette_model.c main/roulette_face.c \
    assets/fonts/app_font_16.c assets/fonts/app_font_24.c \
    tools/sim/sim_main.c -o tools/sim/sim.exe -lm
echo "sim built: tools/sim/sim.exe"

SRC2=$(find managed_components/lvgl__lvgl/src -name '*.c')
gcc -g -O0 -DLV_CONF_INCLUDE_SIMPLE -Itools/sim -Itools/sim/shim -Imanaged_components/lvgl__lvgl -Imain \
    $SRC2 main/roulette_face.c tools/sim/face_preview.c -o tools/sim/face_preview.exe -lm
echo "face preview built: tools/sim/face_preview.exe"
