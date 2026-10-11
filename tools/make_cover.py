#!/usr/bin/env python3
"""生成《小黑屋》AI Passport 封面(发布规范:3:4 竖版 1152×1536)。
玩法优先:官方设备参考图 + 亮屏真实游戏画面(标题页全揭示态)+
金色标题与品牌条,底部余烬光晕呼应火光带。
用法: python tools/make_cover.py  → docs/cover.png"""
import os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHOT = os.path.join(ROOT, "docs", "screenshots")
REF = os.path.expanduser(
    r"~\.zcode\skills\folotoy-ai-passport-publisher"
    r"\references\ai-passport-device-reference.png")
OUT = os.path.join(ROOT, "docs", "cover.png")
W, H = 1152, 1536

GOLD = (255, 217, 40)
AMBER = (224, 138, 60)
PAPER = (231, 229, 228)
DIM = (168, 162, 158)

def cjk(size, bold=False):
    for n in (["msyhbd.ttc", "msyh.ttc"] if bold else ["msyh.ttc"]):
        try:
            return ImageFont.truetype(os.path.join(r"C:\Windows\Fonts", n), size)
        except OSError:
            pass
    return ImageFont.truetype(os.path.join(r"C:\Windows\Fonts", "simhei.ttf"), size)

def lat(size, bold=False):
    name = "arialbd.ttf" if bold else "arial.ttf"
    return ImageFont.truetype(os.path.join(r"C:\Windows\Fonts", name), size)

# ---- 背景:垂直暗渐变 + 底部余烬光晕(呼应游戏火光带) ----
bg = Image.new("RGB", (W, H))
top, bot = (14, 14, 18), (32, 27, 22)
for y in range(H):
    t = y / H
    row = tuple(int(a + (b - a) * t) for a, b in zip(top, bot))
    for x in range(W):
        bg.putpixel((x, y), row)
glow = Image.new("L", (W, H), 0)
gd = ImageDraw.Draw(glow)
gd.ellipse((W//2 - 520, H - 560, W//2 + 520, H + 160), fill=72)
glow = glow.filter(ImageFilter.GaussianBlur(140))
bg = Image.composite(Image.new("RGB", (W, H), AMBER), bg, glow)

# ---- 设备参考图(官方透明壳)×1.38,底部对齐充满下半 ----
dev = Image.open(REF).convert("RGBA")
SCALE = 1.38
DW, DH = int(dev.width * SCALE), int(dev.height * SCALE)
dev = dev.resize((DW, DH), Image.LANCZOS)
dx, dy = (W - DW) // 2, H - DH - 2
bg.paste(dev, (dx, dy), dev)
d = ImageDraw.Draw(bg)

# ---- 屏幕区换装:标题页全揭示态(参考图屏幕 (115,122)-(413,518)) ----
sx0, sy0 = dx + int(114 * SCALE), dy + int(120 * SCALE)
sx1, sy1 = dx + int(415 * SCALE), dy + int(533 * SCALE)
hero = Image.open(os.path.join(SHOT, "01_title.png")).convert("RGB")
hero = hero.resize((sx1 - sx0, sy1 - sy0), Image.LANCZOS)
rmask = Image.new("L", hero.size, 0)
ImageDraw.Draw(rmask).rounded_rectangle((0, 0, hero.width - 1, hero.height - 1),
                                        radius=16, fill=255)
bg.paste(hero, (sx0, sy0), rmask)
d = ImageDraw.Draw(bg)

# ---- 顶部:游戏标题(金)+ 副题 ----
title = "小黑屋"
f_title = cjk(140, bold=True)
tw = d.textlength(title, font=f_title)
tx, ty = (W - tw) / 2, 36
for ddx, ddy in ((-3, 0), (3, 0), (0, -3), (0, 3)):
    d.text((tx + ddx, ty + ddy), title, font=f_title, fill=(92, 72, 8))
d.text((tx, ty), title, font=f_title, fill=GOLD)
sub = "A DARK ROOM · ESP32-C3 完整重制"
f_sub = cjk(38)
d.text(((W - d.textlength(sub, font=f_sub)) / 2, 196), sub, font=f_sub, fill=DIM)

# ---- 底部:品牌条(叠在设备底部暗区,与参考图品牌行同位) ----
brand = "AI PASSPORT · 小黑屋 · 离线文字放置冒险"
f_brand = cjk(44)
d.text(((W - d.textlength(brand, font=f_brand)) / 2, 1442), brand,
       font=f_brand, fill=PAPER)

bg.save(OUT)
print("cover ->", OUT, bg.size)
