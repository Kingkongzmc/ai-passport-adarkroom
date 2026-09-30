#!/usr/bin/env bash
# tests/run_tests.sh —— 主机端测试入口:纯逻辑模块 + 测试用 gcc/cc 编译运行。
# 只依赖 C99;Windows 下用 Git Bash/MSYS 的 gcc,或任何 cc。
set -euo pipefail
cd "$(dirname "$0")"

CC="${CC:-cc}"
command -v "$CC" >/dev/null 2>&1 || CC=gcc
command -v "$CC" >/dev/null 2>&1 || { echo "no C compiler found (cc/gcc)"; exit 1; }

SRC="../main/game_darkroom"
CFLAGS="-std=c99 -Wall -Wextra -Werror -I$SRC"

echo "== build =="
"$CC" $CFLAGS -o test_dr_state test_dr_state.c "$SRC/dr_state.c" "$SRC/dr_util.c"
"$CC" $CFLAGS -o test_dr_events test_dr_events.c "$SRC/dr_events.c" \
    "$SRC/dr_events_data.c" "$SRC/dr_text.c" "$SRC/dr_state.c" "$SRC/dr_util.c"
"$CC" $CFLAGS -o test_dr_rules test_dr_rules.c "$SRC/dr_rules.c" \
    "$SRC/dr_state.c" "$SRC/dr_util.c"
"$CC" $CFLAGS -o test_dr_world test_dr_world.c "$SRC/dr_world.c" \
    "$SRC/dr_rules.c" "$SRC/dr_state.c" "$SRC/dr_util.c"
"$CC" $CFLAGS -o test_dr_util test_dr_util.c "$SRC/dr_util.c"

echo "== run =="
./test_dr_state
./test_dr_events
./test_dr_rules
./test_dr_world
./test_dr_util
echo "all host tests passed"
