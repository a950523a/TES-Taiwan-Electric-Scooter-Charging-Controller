#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把海龜電能的 logo（線稿 PNG）轉成絲印封裝。在 KiCad 的 python 裡跑。

    "C:/Program Files/KiCad/10.0/bin/python.exe" tools/make_logo.py

產生 TES.pretty/LOGO_TurtlePower_<寬>mm.kicad_mod，每個尺寸一個。封裝只有
F.SilkS 上的填滿多邊形、沒有焊盤，標成 board-only 並排除在 BOM／座標檔之外，
所以不會出現在電路圖、網表比對或貼片資料裡。放到背面時用 Flip，絲印自動換到
B.SilkS 並鏡像，從背面看是正的。

**線寬是重點。** 原圖是細線稿，線條只佔寬度的 0.7%。縮到 9 mm 時只剩
0.066 mm，嘉立創絲印最細約 0.15 mm（建議 0.2 mm 以上），會印不出來或斷線。
所以點陣化之後先量中位數線寬，不足 MIN_LINE 就向外膨脹補足。

點陣 → 多邊形：每一列的連續墨水段各是一個矩形，全部丟進 SHAPE_POLY_SET 做
聯集，再 Fracture 成沒有洞的外框（封裝多邊形不支援洞）。0.02 mm 的鋸齒遠小於
絲印的印刷解析度，看不出來。
"""
import os, sys
import numpy as np
from PIL import Image
import pcbnew

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "hardware", "kicad", "lib", "logo", "turtle_power.png")
LIB = os.path.join(REPO, "hardware", "kicad", "lib", "TES.pretty")

SIZES_MM = (9, 20)     # 正面最大空位約 9 mm；背面放 20 mm
MIN_LINE = 0.20        # mm，絲印最細線寬（含餘裕）
PX = 0.02              # mm／像素


def load_ink(path):
    im = np.asarray(Image.open(path).convert("L"), dtype=np.float32)
    ink = im < 128
    ys, xs = np.where(ink)
    return ink[ys.min():ys.max() + 1, xs.min():xs.max() + 1]


def median_run(ink):
    """中位數的水平墨水段長度，當作線寬。粗陰影線不影響中位數。"""
    runs = []
    for r in ink[::5]:
        d = np.diff(np.concatenate([[0], r.astype(int), [0]]))
        runs += list(np.where(d == -1)[0] - np.where(d == 1)[0])
    return float(np.median(runs))


def rasterise(ink, width_mm):
    h, w = ink.shape
    W = int(round(width_mm / PX))
    H = int(round(W * h / w))
    a = np.asarray(Image.fromarray((ink * 255).astype(np.uint8))
                   .resize((W, H), Image.LANCZOS)) > 100
    line = median_run(ink) * W / w * PX
    rad = max(0, int(np.ceil((MIN_LINE - line) / 2 / PX)))
    if rad:
        yy, xx = np.mgrid[-rad:rad + 1, -rad:rad + 1]
        disk = (xx * xx + yy * yy) <= rad * rad
        p = np.pad(a, rad)
        out = np.zeros_like(a)
        for dy, dx in zip(*np.where(disk)):
            out |= p[dy:dy + H, dx:dx + W]
        a = out
    print("  %2d mm：原始線寬 %.3f mm，膨脹 %d px → 約 %.2f mm"
          % (width_mm, line, rad, line + 2 * rad * PX))
    return a


def to_polys(a):
    """點陣 → 以封裝原點為中心的多邊形（mm → KiCad 內部單位）。"""
    H, W = a.shape
    ox, oy = W * PX / 2, H * PX / 2
    ps = pcbnew.SHAPE_POLY_SET()
    for r in range(H):
        d = np.diff(np.concatenate([[0], a[r].astype(int), [0]]))
        for s, e in zip(np.where(d == 1)[0], np.where(d == -1)[0]):
            x0, x1 = s * PX - ox, e * PX - ox
            y0, y1 = r * PX - oy, (r + 1) * PX - oy
            ch = pcbnew.SHAPE_LINE_CHAIN()
            for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
                ch.Append(pcbnew.FromMM(float(x)), pcbnew.FromMM(float(y)))
            ch.SetClosed(True)
            ps.AddOutline(ch)
    ps.Simplify()
    ps.Fracture()
    return ps


def make_footprint(width_mm, a):
    name = "LOGO_TurtlePower_%dmm" % width_mm
    fp = pcbnew.FOOTPRINT(None)
    fp.SetFPID(pcbnew.LIB_ID("TES", name))
    fp.SetReference("LOGO**")
    fp.SetValue(name)
    fp.Reference().SetVisible(False)
    fp.Value().SetVisible(False)
    fp.Value().SetLayer(pcbnew.F_Fab)
    fp.SetAttributes(pcbnew.FP_BOARD_ONLY | pcbnew.FP_EXCLUDE_FROM_BOM
                     | pcbnew.FP_EXCLUDE_FROM_POS_FILES)
    fp.SetLibDescription("海龜電能 Turtle Power logo，絲印線稿，寬 %d mm。"
                         "由 tools/make_logo.py 產生。" % width_mm)
    ps = to_polys(a)
    for i in range(ps.OutlineCount()):
        sh = pcbnew.PCB_SHAPE(fp)
        sh.SetShape(pcbnew.SHAPE_T_POLY)
        one = pcbnew.SHAPE_POLY_SET()
        one.AddOutline(ps.Outline(i))
        sh.SetPolyShape(one)
        sh.SetFilled(True)
        sh.SetWidth(0)
        sh.SetLayer(pcbnew.F_SilkS)
        fp.Add(sh)
    pcbnew.FootprintSave(LIB, fp)
    print("  → %s（%d 個多邊形）" % (name, ps.OutlineCount()))


def main():
    ink = load_ink(SRC)
    print("原圖內容 %d × %d px" % (ink.shape[1], ink.shape[0]))
    for w in SIZES_MM:
        make_footprint(w, rasterise(ink, w))


if __name__ == "__main__":
    main()
