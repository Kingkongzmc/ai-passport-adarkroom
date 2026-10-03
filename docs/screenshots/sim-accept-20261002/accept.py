#!/usr/bin/env python3
"""sim-accept-20261002 像素级验收分析器(仅读取 BMP,不改动任何文件)。

用法: python accept.py <base_dir>
  base_dir 布局:
    full/sweep1/sim_f*.bmp   R1 满档 12 页轮播(无按键)
    full/sweep2/sim_f*.bmp   R3 满档轮播 + 整备页出发按键(远征态)
    full/craft/cap_*.bmp     R2 满档制造页滚动(逐帧)
    new/new_*.bmp            R4 新档(标题页 + 主页 2 动作行)
    lock/lock_*.bmp          R5 森林开+全职业锁档(滚窗回环)
坐标口径: 内容区 198x278 @ 绝对(21,21);内容相对 y + 21 = 绝对 y。
墨迹判定: 亮度(和)显著高于面板底色 0x1C1917。
"""
import struct
import sys

W, H = 240, 320
PANEL = (0x1C, 0x19, 0x17)
GOLD = (0xFF, 0xD9, 0x28)
LOCK = (0x57, 0x53, 0x4E)
TRACK = (0x3A, 0x3A, 0x44)
THUMB = (0xA8, 0xA2, 0x9E)

RESULTS = []


def read_bmp(p):
    with open(p, "rb") as f:
        d = f.read()
    off = struct.unpack_from("<I", d, 10)[0]
    w = struct.unpack_from("<i", d, 18)[0]
    h = struct.unpack_from("<i", d, 22)[0]
    bpp = struct.unpack_from("<H", d, 28)[0]
    assert (w, h, bpp) == (W, H, 24), f"{p}: {w}x{h} bpp{bpp}"
    rs = w * 3
    px = []
    for y in range(h):
        base = off + (h - 1 - y) * rs
        row = [(d[base + x * 3 + 2], d[base + x * 3 + 1], d[base + x * 3])
               for x in range(w)]
        px.append(row)
    return px


def bright(c):
    return c[0] + c[1] + c[2]


def is_ink(c, thr=110):
    # 面板底 72、屏底 39;轨道 0x3A3A44=290、边框/锁灰 390、文字 480+
    return bright(c) > thr


def cdist(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])


def ink_cols(img, x0, x1, y0, y1, thr=110):
    """返回有墨的列号集合。"""
    s = set()
    for y in range(y0, y1 + 1):
        row = img[y]
        for x in range(x0, x1 + 1):
            if is_ink(row[x], thr):
                s.add(x)
    return s


def ink_rows(img, x0, x1, y0, y1, thr=110):
    s = set()
    for y in range(y0, y1 + 1):
        row = img[y]
        for x in range(x0, x1 + 1):
            if is_ink(row[x], thr):
                s.add(y)
                break
    return s


def span(vals):
    if not vals:
        return None
    return min(vals), max(vals)


def runs(vals, gap=1):
    """把有序离散值合并成连续段(间隔 > gap 断开)。"""
    if not vals:
        return []
    out = []
    a = p = vals[0]
    for v in vals[1:]:
        if v - p > gap:
            out.append((a, p))
            a = v
        p = v
    out.append((a, p))
    return out


def clusters_x(img, box, gap=6, thr=110):
    cols = sorted(ink_cols(img, *box, thr=thr))
    return runs(cols, gap)


def bands_y(img, box, gap=4, thr=110):
    rows = sorted(ink_rows(img, *box, thr=thr))
    return runs(rows, gap)


def has_color(img, box, target, tol=40):
    x0, x1, y0, y1 = box
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if cdist(img[y][x], target) <= tol:
                return True
    return False


def count_color(img, box, target, tol=60):
    x0, x1, y0, y1 = box
    n = 0
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if cdist(img[y][x], target) <= tol:
                n += 1
    return n


def diff_count(a, b, box, tol=40):
    x0, x1, y0, y1 = box
    n = 0
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if cdist(a[y][x], b[y][x]) > tol:
                n += 1
    return n


def max_ink_y(img, y0=300, thr=110):
    r = ink_rows(img, 0, W - 1, y0, H - 1, thr=thr)
    return max(r) if r else None


def rec(name, ok, detail):
    RESULTS.append((name, "PASS" if ok else "FAIL", detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}")


def cell_box(i, y0):
    """资源格 i(4 列两行布 局: x=21+(i%4)*49 起 46 宽,y0=该行绝对顶,第二行自动 +15)。"""
    x = 21 + (i % 4) * 49
    y = y0 + (i // 4) * 15
    return (x, x + 45, y, y + 14)


def ink_width(img, box):
    cols = sorted(ink_cols(img, *box))
    if not cols:
        return None, None
    return cols[-1] - cols[0] + 1, (cols[0], cols[-1])


def approx(want, got, tol):
    return got is not None and abs(got - want) <= tol


def main(base):
    # ================= R1 满档轮播 =================
    s1 = f"{base}/full/sweep1"
    title1 = read_bmp(f"{s1}/sim_f0045.bmp")
    home = read_bmp(f"{s1}/sim_f0095.bmp")
    build = read_bmp(f"{s1}/sim_f0145.bmp")
    village = read_bmp(f"{s1}/sim_f0195.bmp")
    mapout = read_bmp(f"{s1}/sim_f0245.bmp")
    ruin = read_bmp(f"{s1}/sim_f0295.bmp")
    combat = read_bmp(f"{s1}/sim_f0345.bmp")
    trade = read_bmp(f"{s1}/sim_f0395.bmp")
    sett = read_bmp(f"{s1}/sim_f0445.bmp")
    confirm = read_bmp(f"{s1}/sim_f0495.bmp")
    title2 = read_bmp(f"{s1}/sim_f0545.bmp")
    home2 = read_bmp(f"{s1}/sim_f0595.bmp")

    # 页序指纹(B4 事件页跳过验证的一部分)
    def fp_title(im):
        return has_color(im, (30, 210, 100, 140), GOLD, 50)
    def fp_confirm(im):
        return (has_color(im, (30, 210, 45, 95), GOLD, 50)
                and bright(im[3][3]) < 60)   # 遮罩 0x101010(和=48)盖全屏
    seq_ok = (fp_title(title1) and fp_title(title2)
              and fp_confirm(confirm) and not fp_title(confirm))
    rec("R1 轮播页序", seq_ok,
        "f0045/f0545=标题(大金字), f0095=主页, f0495=确认弹窗(金标题+全屏遮罩)"
        if seq_ok else "页序指纹不符,逐页检查")

    # ---- A1 主页 8 资源格 ----
    want_home = [("火旺", 24, 4), ("人 45", 28, 3), ("阱 3", 21.4, 3),
                 ("木 123k", 41.4, 3), ("毛 100k", 41.4, 3),
                 ("肉 12.3k", 44.7, 3), ("诱 9876", 41.4, 3), ("革 12.3k", 44.7, 3)]
    for i, (label, w, tol) in enumerate(want_home):
        wgt, _ = ink_width(home, cell_box(i, 69))
        rec(f"A1 主页格{i} [{label}]", approx(w, wgt, tol) and wgt <= 46,
            f"墨宽 {wgt}px (期望 {w}±{tol}, 格宽 46)")

    # ---- A5 荒野页签锁灰 ----
    tab2 = (121, 168, 45, 65)
    n_lock = count_color(village, tab2, LOCK, 30)   # village 页也有同款页签
    n_lock_home = count_color(home, tab2, LOCK, 30)
    n_gold_home = count_color(home, tab2, GOLD, 90)
    rec("A5 荒野页签锁灰", n_lock_home > 20 and n_gold_home == 0 and n_lock > 20,
        f"主页锁灰像素 {n_lock_home} / 金色像素 {n_gold_home};村庄页锁灰 {n_lock}")

    # ---- A1/A2 建造页资源带 ----
    want_build = [("木 123k", 41.4), ("毛 100k", 41.4),
                  ("肉 12.3k", 44.7), ("诱 9876", 41.4)]
    for i, (label, w) in enumerate(want_build):
        wgt, _ = ink_width(build, cell_box(i, 49))
        rec(f"A2 建造格{i} [{label}]", approx(w, wgt, 3) and wgt <= 46,
            f"墨宽 {wgt}px (期望 {w}±3)")

    # ---- A4 建造页炼钢厂 v 列 ----
    # 行7: 内容 y179..201 → 绝对 200..221;v 列内容 x72..192 → 绝对 93..213
    wgt, xr = ink_width(build, (93, 213, 200, 221))
    rec("A4 建造炼钢厂 v 列", approx(105, wgt, 4),
        f"墨宽 {wgt}px @x{xr} (期望 105±4, 全造价 1500木100铁100煤)")

    # ---- 真机补修1 建造页资源带与首行间隙 ----
    band_end = span(ink_rows(build, 21, 219, 45, 66))[1]
    band_start = span(ink_rows(build, 21, 219, 67, 90))[0]
    gap = band_start - band_end - 1
    rec("补修1 建造页带间间隙", gap >= 2,
        f"资源带墨止 y{band_end}, 首行墨起 y{band_start}, 空隙 {gap}px (要求 ≥2)")

    # ---- A2/C4 村庄页 ----
    n_thumb = count_color(village, (216, 217, 142, 258), THUMB, 60)
    tr_rows = ink_rows(village, 216, 217, 140, 258, thr=100)
    trs = span(tr_rows)
    tr_h = trs[1] - trs[0] + 1 if trs else 0
    rec("C4 村庄滚动轨道", trs and abs(tr_h - 113) <= 2 and n_thumb > 80,
        f"x216-217 竖轨墨 y{trs[0]}..{trs[1]} 高 {tr_h}px (期望 113±2), "
        f"滑块亮像素 {n_thumb}")
    vw, vr = ink_width(village, (21, 219, 283, 299))
    rec("A2 村庄概览紧凑行", vw is not None and vw <= 198,
        f"『鳞/牙/布/干』行墨宽 {vw}px @x{vr} (期望 ~180, 行宽 198)")
    vb = bands_y(village, (21, 213, 142, 258), gap=6)   # x 止于 213:不含滚动条轨道
    rec("B1 村庄职业窗 5 行", len(vb) == 5,
        f"职业窗行带 {len(vb)} 段 {vb}")

    # ---- B1 整备态地图页 ----
    cl = clusters_x(mapout, (21, 219, 45, 64), gap=6)
    rec("B1 整备顶部 4 簇", len(cl) == 4,
        f"y45-64 文字簇 {len(cl)} 个 {cl}")
    mr = bands_y(mapout, (21, 219, 68, 235), gap=6)
    pitches = [mr[i + 1][0] - mr[i][0] for i in range(len(mr) - 1)]
    rec("B1 整备 7 行@52 pitch26", len(mr) == 7 and all(abs(p - 26) <= 2 for p in pitches),
        f"{len(mr)} 行带 {mr}, pitch {pitches}")
    hint = span(ink_rows(mapout, 21, 219, 250, 275))
    rec("B1 整备提示@238", hint and abs(hint[0] - 259) <= 3,
        f"提示行墨 y{hint} (内容 y238 → 绝对 259)")
    want_map = [("水 90", 28), ("袋 20.0", 35), ("HP45", 28), ("承 110", 33)]
    for i, (label, w) in enumerate(want_map):
        wgt, _ = ink_width(mapout, cell_box(i, 45))
        rec(f"A3 整备格{i} [{label}]", approx(w, wgt, 3) and wgt <= 46,
            f"墨宽 {wgt}px (期望 {w}±3)")

    # ---- C1 地点页 ----
    grid = span(ink_rows(ruin, 79, 157, 63, 150, thr=100))
    rb = bands_y(ruin, (21, 219, 160, 210), gap=6)
    rhint = span(ink_rows(ruin, 21, 219, 215, 240))
    rec("C1 地点 3x3 房格带", grid and abs(grid[0] - 65) <= 2 and grid[1] >= 146,
        f"房格墨带 y{grid[0]}..{grid[1]} (期望 y65 起, 至 ~148)")
    rec("C1 地点行@150/174", len(rb) == 2 and abs(rb[0][0] - 171) <= 3
        and abs(rb[1][0] - 195) <= 3,
        f"行带 {rb} (绝对 171/195 = 内容 150/174)")
    rec("C1 地点提示行(轮播态)", rhint is None,
        f"轮播 dump 地点页提示行空(未进地点,location=0) y{rhint};"
        f"真实落位见 pace 副本洞穴断言")

    # ---- B3 战斗页 ----
    # 战报行 pitch15/字高12:逐行墨点计数剖面找 3 个峰(行间仅 1-2px 抗锯齿桥接)
    def humps(im, x0, x1, y0, y1, frac=0.12):
        counts = []
        for y in range(y0, y1 + 1):
            n = sum(1 for x in range(x0, x1 + 1) if is_ink(im[y][x]))
            counts.append(n)
        mx = max(counts) if counts else 0
        on = [y for y, n in zip(range(y0, y1 + 1), counts) if n > mx * frac]
        return runs(on, gap=2), counts
    lb, prof = humps(combat, 21, 213, 110, 175)
    rec("B3 战斗中段 3 行战报", len(lb) == 3,
        f"中段战报峰 {lb} (期望 绝对 ~121/136/151, 行距15)")
    chint = span(ink_rows(combat, 21, 219, 280, 299))
    chint_end_ok = chint and 293 <= chint[1] <= 299
    rec("B3 战斗提示行完整", chint_end_ok,
        f"提示墨 y{chint} (止于 294-298 区间, 不超 299)")

    # ---- A2 贸易资源带 ----
    want_trade = [("木 123k", 41.4), ("毛 100k", 41.4),
                  ("鳞 98.8k", 44.7), ("牙 98.8k", 44.7)]
    for i, (label, w) in enumerate(want_trade):
        wgt, _ = ink_width(trade, cell_box(i, 45))
        rec(f"A2 贸易格{i} [{label}]", approx(w, wgt, 3) and wgt <= 46,
            f"墨宽 {wgt}px (期望 {w}±3)")

    # ---- C2 设置页 ----
    sb = bands_y(sett, (21, 219, 65, 232), gap=8)
    last = sb[-1] if sb else None
    rec("C2 设置 6 行墨带", len(sb) == 6 and last and 209 <= last[0] <= 217
        and last[1] <= 227,
        f"{len(sb)} 行带, 末行 {last} (期望 ~213-225)")

    # ---- B4 确认弹窗(事件空表跳过) ----
    veil_dark = bright(confirm[3][3]) < 60   # 遮罩 0x101010(和=48)盖全屏
    dtitle = count_color(confirm, (30, 210, 45, 95), GOLD, 60)
    body = ink_rows(confirm, 30, 210, 95, 145)
    rec("B4 事件空表跳过", veil_dark and dtitle > 40 and bool(body),
        f"f0495=确认弹窗: 遮罩暗 {veil_dark}, 金标题像素 {dtitle}, 正文墨 "
        f"{span(body) if body else None} (无空弹窗)")

    # ---- 全页 y>299 零墨 ----
    names = ["f0045标题", "f0095主页", "f0145建造", "f0195村庄", "f0245整备",
             "f0295地点", "f0345战斗", "f0395贸易", "f0445设置", "f0495确认",
             "f0545标题2", "f0595主页2"]
    imgs = [title1, home, build, village, mapout, ruin, combat, trade, sett,
            confirm, title2, home2]
    bad = [f"{n}:{max_ink_y(im)}" for n, im in zip(names, imgs) if max_ink_y(im)]
    rec("R1 全 12 页 y≤299", not bad, "全部页面 300-319 行零墨" if not bad
        else f"越界页 {bad}")

    # ================= R3 远征态 + 真实地点(节奏化调试副本 pace) =================
    pc = f"{base}/full/pace"
    mapout2 = read_bmp(f"{pc}/pp05_map_outfit.bmp")
    mapexp = read_bmp(f"{pc}/pp06_map_exp.bmp")
    ruin2 = read_bmp(f"{pc}/pp07_ruin_house.bmp")   # 南南西进洞穴后的真实地点页
    wgt, _ = ink_width(mapexp, cell_box(2, 45))
    rec("A3 远征态 HP45/45", wgt is not None and 36 <= wgt <= 46,
        f"墨宽 {wgt}px (期望 ~39-45, 无省略号)")
    db = bands_y(mapexp, (21, 213, 185, 292), gap=6)
    rec("B1 远征方向 5 行", len(db) == 5,
        f"方向行带 {db} (内容 168 起 pitch22)")
    bad2 = [max_ink_y(mapout2), max_ink_y(mapexp), max_ink_y(ruin2)]
    rec("R3 远征页 y≤299", not any(bad2), "整备/远征/地点 300-319 零墨"
        if not any(bad2) else f"越界 {bad2}")
    # C1 补充:真实地点(洞穴)的提示行@200 与中心格地点色
    rhint2 = span(ink_rows(ruin2, 21, 219, 215, 240))
    rec("C1 地点提示@200(洞穴)", rhint2 and abs(rhint2[0] - 221) <= 3,
        f"洞穴提示墨 y{rhint2} (内容 200 → 绝对 221)")
    center_col = ruin2[106][120]   # 中心格 (内容99,85) ≈ 洞穴色 0x6B7FC9
    rec("C1 中心格地点色", cdist(center_col, (0x6B, 0x7F, 0xC9)) <= 60,
        f"中心格色 rgb{center_col} (洞穴 0x6B7FC9)")

    # ================= R2 制造滚动 =================
    cr = f"{base}/full/craft"
    top = read_bmp(f"{cr}/cap_0074.bmp")
    top2 = read_bmp(f"{cr}/cap_0075.bmp")
    scr = read_bmp(f"{cr}/cap_0104.bmp")
    scr2 = read_bmp(f"{cr}/cap_0105.bmp")
    fin = read_bmp(f"{cr}/cap_0145.bmp")
    fin2 = read_bmp(f"{cr}/cap_0146.bmp")
    rows_box = (21, 219, 73, 260)
    stable = (diff_count(top, top2, rows_box) == 0
              and diff_count(scr, scr2, rows_box) == 0
              and diff_count(fin, fin2, rows_box) == 0)
    rec("R2 取帧稳定性", stable, "top/f74-75, scrolled/f104-105, final/f145-146 均零差")
    d1 = diff_count(top, scr, rows_box)
    rec("B2 制造下滚 6 格", d1 > 1000, f"行区像素差 {d1}px (要求 >1000)")
    d2 = diff_count(top, fin, rows_box)
    # 终态内容必须与顶部一致(只有焦点行颜色/箭头可不同):逐行带比较
    fin_bands = bands_y(fin, rows_box, gap=6)
    top_bands = bands_y(top, rows_box, gap=6)
    same_rows = len(fin_bands) == len(top_bands)
    rec("B2 制造上滚回顶", d2 < 300 and same_rows,
        f"回顶差异 {d2}px (要求 <300), 行带数 {len(top_bands)}→{len(fin_bands)}")
    ctr = ink_rows(top, 216, 217, 70, 262, thr=100)
    ctrs = span(ctr)
    tr_h2 = ctrs[1] - ctrs[0] + 1 if ctrs else 0
    rec("C4 制造轨道 181px", abs(tr_h2 - 181) <= 3,
        f"x216-217 轨道 y{ctrs[0]}..{ctrs[1]} 高 {tr_h2}px (期望 181±3)")
    # 滑块位移:扫描 74..104 帧,找滑块顶首次变化
    def thumb_top(im):
        r = [y for y in range(73, 262)
             if cdist(im[y][216], THUMB) <= 90 or cdist(im[y][217], THUMB) <= 90]
        return min(r) if r else None
    tt0 = thumb_top(top)
    moved = None
    for f in range(75, 105):
        im = read_bmp(f"{cr}/cap_{f:04d}.bmp")
        tt = thumb_top(im)
        if tt is not None and tt0 is not None and tt != tt0:
            moved = (f, tt - tt0)
            break
    rec("C4 制造滑块位移", moved and 10 <= moved[1] <= 16,
        f"顶部态滑块顶 y{tt0}, 首次位移 @f{moved[0]}: {moved[1]}px (期望 13)"
        if moved else "滑块未检测到位移")

    # ================= R4 新档 =================
    nw = f"{base}/new"
    tnew = read_bmp(f"{nw}/new_0005.bmp")
    big = ink_rows(tnew, 40, 200, 100, 145)
    bs = span(big)
    rec("C5 标题大标题上移", bs and 108 <= bs[0] <= 114,
        f"大标题墨带起 y{bs[0]} (期望 ~111, 旧版 123)")
    hnew = read_bmp(f"{nw}/new_0060.bmp")
    ab = bands_y(hnew, (21, 219, 240, 299), gap=8)
    lasta = ab[-1] if ab else None
    rec("C3 新档 2 动作行贴底", len(ab) == 2 and lasta and 274 <= lasta[0] <= 280
        and 288 <= lasta[1] <= 293,
        f"动作行带 {ab}, 末行墨 {lasta} (期望 ~278-291)")
    rec("R4 新档页 y≤299", max_ink_y(hnew) is None and max_ink_y(tnew) is None,
        "标题/主页 300-319 零墨")

    # ================= R5 滚窗回环 =================
    lk = f"{base}/lock"
    vtop = read_bmp(f"{lk}/lock_0078.bmp")
    vtop2 = read_bmp(f"{lk}/lock_0079.bmp")
    vscr = read_bmp(f"{lk}/lock_0102.bmp")
    vfin = read_bmp(f"{lk}/lock_0134.bmp")
    vfin2 = read_bmp(f"{lk}/lock_0135.bmp")
    job_box = (21, 219, 142, 256)
    st5 = (diff_count(vtop, vtop2, job_box) == 0
           and diff_count(vfin, vfin2, job_box) == 0)
    rec("R5 取帧稳定性", st5, "top/f78-79, final/f134-135 职业窗区零差")
    dscr = diff_count(vtop, vscr, job_box)
    rec("R5 全锁档可下滚", dscr > 500, f"down×4 后职业窗区差 {dscr}px (>500=已滚动)")
    dback = diff_count(vtop, vfin, job_box)
    rec("补修2 滚窗回环", dback == 0,
        f"up×6 回顶后职业窗区差 {dback}px (要求 0)")
    lk_tr = span(ink_rows(vtop, 216, 217, 140, 258, thr=100))
    rec("R5 全锁档轨道可见", lk_tr is not None,
        f"职业窗轨道 y{lk_tr} (9 职业 5 行窗)")

    print()
    npass = sum(1 for _, v, _ in RESULTS if v == "PASS")
    print(f"== {npass}/{len(RESULTS)} PASS ==")


if __name__ == "__main__":
    main(sys.argv[1])
