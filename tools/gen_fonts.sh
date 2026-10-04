#!/usr/bin/env bash
# 重新生成 assets/fonts 下的三个点阵字库。
# f12/f16 字形集 = tools/gen_font_symbols.py 扫描 main/ 源码所得(闭集,
# 新增/修改文案后必须重跑本脚本,否则新字形显示为方块)。依赖 npx lv_font_conv。
# f24 只渲染两个标题(s_t_big/s_dtitle),不扫全仓:固定 10 字纯中文闭集,
# 且不带 ASCII range(§6 flash 账拍板 ~2KB);若标题文案改版或出现
# 字母/数字,须同步扩此处与 font_audit.py 的 f24 口径。
set -e
cd "$(dirname "$0")/.."

FONT=managed_components/lvgl__lvgl/tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf
SYMS=$(python tools/gen_font_symbols.py)
# f24 标题字闭集:"小黑屋"+"确认操作"+备用"新的一局"
SYMS_24="小黑屋确认操作新的一局"

for size in 12 16 24; do
    SYMSCUR="$SYMS"
    RANGECUR="--range 0x20-0x7E"
    if [ "$size" = 24 ]; then
        SYMSCUR="$SYMS_24"
        RANGECUR=""
    fi
    npx --yes lv_font_conv \
        --font "$FONT" \
        $RANGECUR \
        --symbols "$SYMSCUR" \
        --size "$size" --bpp 4 --format lvgl --no-compress \
        --lv-font-name "dr_font_$size" --lv-include lvgl.h \
        --output "assets/fonts/dr_font_$size.c"
    echo "dr_font_$size regenerated"
done
