#!/usr/bin/env python3
"""美化批次像素断言:采样 sim BMP 色值(内容坐标+21=绝对坐标)。
点采样: 精确坐标取色;盒采样: 区域内 ≥min 个像素匹配(标题文字用)。
用法: python assert_beautify.py <bmp> <case> [case...]   退出码 0=全过"""
import struct
import sys
from PIL import Image

def load_bmp(path):
    """sim 的 BMP 缺 'BM' 魔数(write_bmp 首两字节为 0),自带容错读取。"""
    with open(path, "rb") as f:
        d = f.read()
    if d[:2] == b"BM":
        return Image.open(path).convert("RGB")
    off = struct.unpack_from("<I", d, 10)[0]
    w = struct.unpack_from("<i", d, 18)[0]
    h = struct.unpack_from("<i", d, 22)[0]
    bpp = struct.unpack_from("<H", d, 28)[0]
    assert bpp == 24, f"unsupported bpp {bpp}"
    rs = (w * 3 + 3) & ~3
    buf = bytearray(w * h * 3)
    for y in range(h):                      # BMP 自下而上
        i = off + (h - 1 - y) * rs
        buf[y * w * 3:(y + 1) * w * 3] = d[i:i + w * 3]
    return Image.frombytes("RGB", (w, h), bytes(buf), "raw", "BGR")

def c(x, y): return (x + 21, y + 21)   # 内容区绝对原点 (21,21)

GOLD  = (0xFF, 0xD9, 0x28)
INK   = (0x0D, 0x0D, 0x0D)
PANEL = (0x1C, 0x19, 0x17)
TEXT  = (0xE7, 0xE5, 0xE4)
DIM   = (0xA8, 0xA2, 0x9E)
RULE  = (0x33, 0x30, 0x2C)
ENEMY = (0xE4, 0x3B, 0x2F)
ME    = (0x66, 0xB2, 0xFF)
AMBER = (0xE0, 0x8A, 0x3C)
A_HOME  = (0xE0, 0x8A, 0x3C)
A_FOREST= (0x7F, 0xB5, 0x5C)
A_WILD  = (0xC7, 0xA8, 0x7E)
A_BUILD = (0xC9, 0x98, 0x6B)
A_TRADE = (0x6F, 0xC9, 0xC9)
A_CRAFT = (0xA9, 0xA9, 0xB5)
A_SHIP  = (0xD4, 0x9B, 0xE8)

# 用例: ("说明", 采样方式, 参数...) —— p=x,y,色,容差 | b=x0,y0,x1,y1,色,最少像素
# 标题盒采样: 16bpp 下 f12 字形纯色核稀少,容差 15(抗锯齿边 #69BCBC 级)
CASES = {
  # ---- T1 焦点反色(焦点=首个可用行,不一定 row0)----
  "home_focus_gold":    ("主页焦点行金底", "p", 170, 241, GOLD, 6),
  "home_unfocus_panel": ("主页非焦点行透明", "p", 170, 265, PANEL, 6),
  "build_ret_gold":     ("建造返回行反色金底", "p", 195, 247, GOLD, 6),
  "event_choice_gold":  ("事件选项反色金底", "p", 160, 175, GOLD, 6),
  "combat_act_gold":    ("战斗攻击行反色金底", "p", 170, 161, GOLD, 6),
  "ruin_row_gold":      ("地点搜索行反色金底", "p", 170, 161, GOLD, 6),
  "liftoff_act_gold":   ("升空继续行反色金底", "p", 170, 247, GOLD, 6),
  "ending_act_gold":    ("结局继续行反色金底", "p", 170, 223, GOLD, 6),
  "settings_row_gold":  ("设置行0反色金底", "p", 195, 63, GOLD, 6),
  # ---- T2 场景身份 ----
  "home_title":    ("主页标题暖橙", "b", 0, 0, 40, 20, A_HOME, 3),
  "build_title":   ("建造标题木色", "b", 0, 0, 40, 20, A_BUILD, 3),
  "build_rule":    ("建造规则线", "p", 150, 44, RULE, 8),
  "build_tick":    ("建造accent短条", "p", 12, 44, A_BUILD, 8),
  "village_title": ("村庄标题橄榄绿", "b", 0, 0, 40, 20, A_FOREST, 3),
  "wild_title":    ("荒野标题荒原色", "b", 0, 0, 40, 20, A_WILD, 3),
  "prep_rule":     ("整备规则线", "p", 150, 44, RULE, 8),
  "prep_tick":     ("整备accent短条", "p", 12, 44, A_WILD, 8),
  "trade_title":   ("贸易标题青色", "b", 0, 0, 40, 20, A_TRADE, 3),
  "craft_title":   ("制造标题铁灰", "b", 0, 0, 40, 20, A_CRAFT, 3),
  "settings_title":("设置标题中性灰", "b", 0, 0, 40, 20, DIM, 3),
  "ship_title":    ("星舰标题紫", "b", 0, 0, 40, 20, A_SHIP, 3),
  "fab_title":     ("制造台标题铁灰", "b", 0, 0, 40, 20, A_CRAFT, 3),
  # ---- T3 标题页 ----
  "title_rule":    ("顶饰线细线", "n", 33, 74, 165, 82, 100),
  "fire_top":      ("火光带顶部透明", "p", 100, 256, PANEL, 6),
  "fire_bottom":   ("火光带底部余烬", "p", 100, 276, (0x60, 0x40, 0x24), 16),
  "title_big":     ("大标题金", "b", 60, 88, 138, 120, GOLD, 6),
  # ---- T5 结构面板 ----
  "logpanel_bg":    ("日志面板深底", "p", 100, 150, (0x0D, 0x0D, 0x0D), 4),
  "logpanel_edge":  ("日志面板左边框", "p", 0, 150, RULE, 8),
  "jobpanel_bg":    ("职业窗面板深底", "p", 60, 130, (0x0D, 0x0D, 0x0D), 4),
  "jobpanel_edge":  ("职业窗面板上边框", "p", 60, 117, RULE, 8),
  "settings_sep":   ("设置危险分隔线", "p", 100, 75, RULE, 8),
  "event_border":   ("事件非焦点选项描边", "n2", 12, 164, 160, 235, (0x57, 0x53, 0x4E), 150),
  "confirm_border": ("确认非焦点选项描边", "n2", 12, 175, 160, 235, (0x57, 0x53, 0x4E), 150),
  "combat_title":  ("战斗标题敌红", "b", 0, 0, 40, 20, ENEMY, 3),
  "liftoff_title": ("升空标题金豁免", "b", 0, 0, 40, 20, GOLD, 2),
  "ending_title":  ("结局标题金豁免", "b", 0, 0, 40, 20, GOLD, 4),
  "export_title":  ("导出标题中性灰", "b", 0, 0, 60, 20, DIM, 3),
  "import_title":  ("导入标题中性灰", "b", 0, 0, 60, 20, DIM, 3),
}

def close(got, exp, tol):
    return all(abs(a - b) <= tol for a, b in zip(got, exp))

def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print("usage: assert_beautify.py <bmp> <case> [case...]"); return 2
    img = load_bmp(args[0])
    px = img.load()
    fails = 0
    for name in args[1:]:
        case = CASES.get(name)
        if case is None:
            print(f"[FAIL] {name}: 未知用例"); fails += 1; continue
        desc, mode = case[0], case[1]
        if mode == "n":   # 盒内非 PANEL 像素计数(细线/低对比装饰)
            _, _, x0, y0, x1, y1, least = case
            n = 0
            for yy in range(c(x0, y0)[1], c(x1, y1)[1]):
                for xx in range(c(x0, y0)[0], c(x1, y1)[0]):
                    g = px[xx, yy]
                    if abs(g[0]-28) + abs(g[1]-25) + abs(g[2]-23) > 24: n += 1
            ok = n >= least
            detail = f"box({x0},{y0})-({x1},{y1}) 非背景 {n} px (需≥{least})"
            print(f"[{'PASS' if ok else 'FAIL'}] {name} ({desc}): {detail}")
            if not ok: fails += 1
            continue
        if mode == "n2":  # 盒内指定色像素计数(1px 描边框)
            _, _, x0, y0, x1, y1, exp, least = case
            n = 0
            for yy in range(c(x0, y0)[1], c(x1, y1)[1]):
                for xx in range(c(x0, y0)[0], c(x1, y1)[0]):
                    if close(px[xx, yy], exp, 12): n += 1
            ok = n >= least
            detail = f"box({x0},{y0})-({x1},{y1}) 命中 {n} px (需≥{least})"
            print(f"[{'PASS' if ok else 'FAIL'}] {name} ({desc}): {detail}")
            if not ok: fails += 1
            continue
        if mode == "p":
            _, _, cx, cy, exp, tol = case
            x, y = c(cx, cy)
            got = px[x, y]
            ok = close(got, exp, tol)
            detail = f"abs({x},{y}) got #{got[0]:02X}{got[1]:02X}{got[2]:02X}"
        else:
            _, _, x0, y0, x1, y1, exp, least = case
            n = 0
            for yy in range(c(x0, y0)[1], c(x1, y1)[1]):
                for xx in range(c(x0, y0)[0], c(x1, y1)[0]):
                    if close(px[xx, yy], exp, 15): n += 1
            ok = n >= least
            detail = f"box({x0},{y0})-({x1},{y1}) 命中 {n} px (需≥{least})"
        print(f"[{'PASS' if ok else 'FAIL'}] {name} ({desc}): {detail} "
              f"exp #{exp[0]:02X}{exp[1]:02X}{exp[2]:02X}")
        if not ok: fails += 1
    print(f"{'ALL PASS' if fails == 0 else str(fails) + ' FAIL'} "
          f"({len(args)-1} checks on {args[0]})")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
