#!/usr/bin/env python3
"""闭集字库覆盖审计:提取三套字库 --symbols 集,与全部源码字符串字面量逐字比对。
只读分析,不改任何文件。输出:缺失字符清单(按使用处分组)。"""
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
print(f"f12 {len(f12)} / f16 {len(f16)} / f24 {len(f24)} 符号,三套一致: {f12 == f16 == f24}")
fonts = {"f12": f12, "f16": f16, "f24": f24}

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
                if ch not in f12 or ch not in f16 or ch not in f24:
                    key = ch
                    missing.setdefault(key, {"f12": [], "f16": [], "f24": []})
                    for fname in fonts:
                        if ch not in fonts[fname]:
                            missing[key][fname].append((path.split("/")[-1], lit[:40]))

print(f"\n缺失字符总数: {len(missing)}")
for ch in sorted(missing, key=lambda c: ord(c)):
    info = missing[ch]
    where = info["f12"] or info["f16"] or info["f24"]
    lits = sorted({f"{f}:{s}" for f, s in where})
    miss_in = [k for k in ("f12", "f16", "f24") if info[k]]
    print(f"U+{ord(ch):04X} {ch}  缺于{'+'.join(miss_in)}  出现{len(where)}处")
    for l in lits[:4]:
        print(f"      {l}")
