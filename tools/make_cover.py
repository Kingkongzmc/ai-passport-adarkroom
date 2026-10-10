#!/usr/bin/env python3
"""生成《小黑屋》AI Passport 封面图:真实游戏截图 + 设备化相框 + 品牌文字。
用法: python tools/make_cover.py  → docs/cover.png (900×1300)"""
import os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHOT = os.path.join(ROOT, "docs", "screenshots")
OUT = os.path.join(ROOT, "docs", "cover.png")
W, H = 900, 1300

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

# ---- 背景:垂直暗渐变 + 底部余烬光晕(呼应标题页火光带) ----
bg = Image.new("RGB", (W, H))
top, bot = (16, 16, 20), (30, 26, 23)
for y in range(H):
    t = y / H
    row = tuple(int(a + (b - a) * t) for a, b in zip(top, bot))
    for x in range(W):
        bg.putpixel((x, y), row)
glow = Image.new("L", (W, H), 0)
gd = ImageDraw.Draw(glow)
gd.ellipse((W//2 - 420, H - 460, W//2 + 420, H + 140), fill=70)
glow = glow.filter(ImageFilter.GaussianBlur(120))
ember = Image.new("RGB", (W, H), AMBER)
bg = Image.composite(ember, bg, glow)

d = ImageDraw.Draw(bg)

# ---- 主画面:标题页截装入圆角"设备屏"相框 ----
hero = Image.open(os.path.join(SHOT, "01_title.png")).convert("RGB")
HW, HH = 396, 528                      # 240×320 ×1.65
hero = hero.resize((HW, HH), Image.LANCZOS)
fx, fy = (W - HW) // 2, 190
BEZ = 18
frame = Image.new("RGBA", (HW + BEZ * 2, HH + BEZ * 2), (0, 0, 0, 0))
fd = ImageDraw.Draw(frame)
fd.rounded_rectangle((0, 0, frame.width - 1, frame.height - 1), radius=34,
                     fill=(26, 26, 30, 255), outline=(70, 68, 64, 255), width=2)
fd.rounded_rectangle((BEZ - 4, BEZ - 4, BEZ + HW + 3, BEZ + HH + 3), radius=22,
                     outline=(10, 10, 10, 255), width=4)
mask = Image.new("L", frame.size, 0)
ImageDraw.Draw(mask).rounded_rectangle((0, 0, frame.width - 1, frame.height - 1),
                                       radius=34, fill=255)
bg.paste(frame, (fx - BEZ, fy - BEZ), mask)
rmask = Image.new("L", (HW, HH), 0)
ImageDraw.Draw(rmask).rounded_rectangle((0, 0, HW - 1, HH - 1), radius=24, fill=255)
bg.paste(hero, (fx, fy), rmask)
d = ImageDraw.Draw(bg)

# ---- 品牌区:游戏名 + 英文副题 ----
title = "小黑屋"
f_title = cjk(96, bold=True)
tw = d.textlength(title, font=f_title)
tx, ty = (W - tw) / 2, 44
for dx, dy in ((-2, 0), (2, 0), (0, -2), (0, 2)):   # 暗描边
    d.text((tx + dx, ty + dy), title, font=f_title, fill=(90, 70, 8))
d.text((tx, ty), title, font=f_title, fill=GOLD)
sub = "A DARK ROOM · ESP32 完整重制"
f_sub = cjk(30)
d.text(((W - d.textlength(sub, font=f_sub)) / 2, 158), sub, font=f_sub, fill=DIM)

# ---- 底部:AI PASSPORT 品牌条 + 三小截图 ----
brand_y = fy + HH + BEZ + 52
d.text((W / 2 - 168, brand_y), "AI", font=lat(58, True), fill=PAPER)
d.text((W / 2 - 84, brand_y), "PASSPORT", font=lat(58, True), fill=GOLD)
tag = "FoloToy AI Passport · ESP32-C3 · 240×320 · 离线文字放置冒险"
f_tag = cjk(24)
d.text(((W - d.textlength(tag, font=f_tag)) / 2, brand_y + 78), tag,
       font=f_tag, fill=DIM)

strip_y = brand_y + 128
names = ["02_home.png", "07_combat.png", "05_map.png"]
sw_, sh_ = 168, 224
total = len(names) * sw_ + (len(names) - 1) * 24
sx = (W - total) // 2
for i, n in enumerate(names):
    im = Image.open(os.path.join(SHOT, n)).convert("RGB").resize((sw_, sh_),
                                                                 Image.LANCZOS)
    m = Image.new("L", (sw_, sh_), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, sw_ - 1, sh_ - 1), radius=12,
                                        fill=255)
    bg.paste(im, (sx + i * (sw_ + 24), strip_y), m)
    d.rounded_rectangle((sx + i * (sw_ + 24), strip_y,
                         sx + i * (sw_ + 24) + sw_, strip_y + sh_),
                        radius=12, outline=(70, 68, 64), width=2)

bg.save(OUT)
print("cover ->", OUT, bg.size)
