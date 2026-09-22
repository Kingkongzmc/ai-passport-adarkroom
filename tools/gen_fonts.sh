#!/usr/bin/env bash
# 重新生成 assets/fonts 下的三个点阵字库。
# 字形集 = tools/gen_font_symbols.py 扫描 main/ 源码所得(闭集,新增/修改
# 文案后必须重跑本脚本,否则新字形显示为方块)。依赖 npx lv_font_conv。
set -e
cd "$(dirname "$0")/.."

FONT=managed_components/lvgl__lvgl/tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf
SYMS=$(python tools/gen_font_symbols.py)

for size in 12 16 24; do
    npx --yes lv_font_conv \
        --font "$FONT" \
        --range 0x20-0x7E \
        --symbols "$SYMS" \
        --size "$size" --bpp 4 --format lvgl --no-compress \
        --lv-font-name "dr_font_$size" --lv-include lvgl.h \
        --output "assets/fonts/dr_font_$size.c"
    echo "dr_font_$size regenerated"
done
