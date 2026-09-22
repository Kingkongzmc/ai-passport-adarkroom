#!/usr/bin/env python3
"""Collect the closed glyph set from game sources and emit it for lv_font_conv.

Scans every .c/.h under main/ for CJK/full-width characters inside **string
literals** (comments are stripped first — otherwise doc-comment text bloats
the glyph set and the firmware overflows the app partition), dedupes, and
writes one line of unique symbols to stdout. ASCII 0x20-0x7E is requested
separately via --range.

After editing any user-visible copy, re-run tools/gen_fonts.sh.
"""
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
files = list((root / "main").rglob("*.c")) + list((root / "main").rglob("*.h"))

chars = set()
for f in files:
    text = f.read_text(encoding="utf-8", errors="ignore")
    # 剥注释:块注释 /* */ 与行注释 //(字符串里的 // 极少,均无中文,可接受)
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    # 只收字符串字面量里的内容
    for lit in re.finditer(r'"((?:[^"\\]|\\.)*)"', text):
        for ch in lit.group(1):
            cp = ord(ch)
            if cp < 0x80:
                continue  # covered by ASCII range
            if cp in (0x2019,):  # typographic apostrophe kept
                chars.add(ch)
            if 0x4E00 <= cp <= 0x9FFF or 0x3000 <= cp <= 0x303F \
               or 0xFF00 <= cp <= 0xFFEF or cp >= 0x2010:
                chars.add(ch)

# 布局对齐用的填充符号与常用标点(源码里未必出现)
chars.update("…、·〜─│×≡!?/→")

out = "".join(sorted(chars))
sys.stdout.write(out)
