#!/usr/bin/env python3
"""闭集字库覆盖审计(分套口径),只读分析,不改任何文件。

口径(f24 子集化后三套不再同集,不能再用全仓统一比对):
  f12/f16 全量校验 —— 提取 --symbols 集,与 main/ 全部源码字符串字面量
      逐字比对(f16 并入 f12 后本脚本同步裁掉 f16 套)。
  f24 仅校验两个标题串 —— f24 只渲染 s_t_big / s_dtitle 两个标题位,
      需求字自动取自 darkroom_app.c 里对这两处的 lv_label_set_text 调用
      文案(连 ASCII 一并校验:f24 无 --range,标题出现字母/数字即缺字);
      改标题须同步扩 gen_fonts.sh 的 SYMS_24。

退出码:0 = 0 缺失;1 = 有缺失(可挂 CI 门)。
"""
import re
import glob
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else "."


def symbols(path):
    src = open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r"--symbols (\S+)", src)
    return set(m.group(1)) if m else set()


f12 = symbols(f"{ROOT}/assets/fonts/dr_font_12.c")
f16 = symbols(f"{ROOT}/assets/fonts/dr_font_16.c")
f24 = symbols(f"{ROOT}/assets/fonts/dr_font_24.c")
print(f"f12 {len(f12)} / f16 {len(f16)} / f24 {len(f24)} 符号(分套口径,f24 为标题子集)")
fonts = {"f12": f12, "f16": f16}

# f24 需求字:从 s_t_big / s_dtitle 的 set_text 调用点提取(f24 唯二消费者);
# 含 ASCII——f24 无 --range,标题里出现字母/数字也必须显式进 SYMS_24
app_src = open(f"{ROOT}/main/darkroom_app.c", encoding="utf-8").read()
title_lits = re.findall(
    r'lv_label_set_text\(s_(?:t_big|dtitle),\s*"((?:[^"\\]|\\.)*)"\)', app_src)
if not title_lits:
    print("致命:未找到 s_t_big/s_dtitle 的 set_text 调用,f24 需求集无法推导")
    sys.exit(2)
f24_demand = {ch for lit in title_lits for ch in lit}
print(f"f24 标题串: {title_lits} -> 需求 {len(f24_demand)} 字")

files = [f"{ROOT}/main/darkroom_app.c"] + glob.glob(f"{ROOT}/main/game_darkroom/*.c")
str_re = re.compile(r'"((?:[^"\\]|\\.)*)"')

# 字符 -> [(文件, 字符串, 次数)]
missing = {}
for path in files:
    src = open(path, encoding="utf-8").read()
    src = re.sub(r"//.*", "", src)
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    for lit in str_re.findall(src):
        lit_dec = lit.encode().decode("unicode_escape") if "\\x" in lit or "\\u" in lit else lit
        for ch in lit_dec:
            if ord(ch) > 126:
                if ch not in f12 or ch not in f16:
                    key = ch
                    missing.setdefault(key, {"f12": [], "f16": [], "f24": []})
                    for fname in fonts:
                        if ch not in fonts[fname]:
                            missing[key][fname].append((path.split("/")[-1], lit[:40]))

for ch in sorted(f24_demand, key=ord):
    if ch not in f24:
        missing.setdefault(ch, {"f12": [], "f16": [], "f24": []})
        missing[ch]["f24"].append(("标题串", "".join(sorted(f24_demand))))

total = len(missing)
print(f"\n缺失字符总数: {total}")
for ch in sorted(missing, key=lambda c: ord(c)):
    info = missing[ch]
    where = info["f12"] or info["f16"] or info["f24"]
    lits = sorted({f"{f}:{s}" for f, s in where})
    miss_in = [k for k in ("f12", "f16", "f24") if info[k]]
    print(f"U+{ord(ch):04X} {ch}  缺于{'+'.join(miss_in)}  出现{len(where)}处")
    for l in lits[:4]:
        print(f"      {l}")

print(f"\n审计{'通过: 0 缺失' if total == 0 else '不通过'}")
sys.exit(0 if total == 0 else 1)
