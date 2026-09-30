#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""從 KiCad 板子產生硬體狀態頁（/hw）的電路板圖。在 KiCad 的 python 裡跑。

    "C:/Program Files/KiCad/10.0/bin/python.exe" tools/make_hw_board_svg.py

把 SVG 寫進 firmware/components/services/web/hw.html 的
<!--BOARD--> … <!--/BOARD--> 之間，頁面其他部分不動。

畫的東西，全部是板子上的真實位置（座標直接用 KiCad 的 mm，y 向下）：
  - 板框、安裝孔、頂層絲印的文字與外框（VP、Coupler、DC Relay、降壓模組…）
  - 每個焊盤（金色）與每顆零件的 courtyard（淡框）
  - 每顆零件包成 <g id="fp-<位號>">，頁面的 JS 靠這個 id 上色、點選

**同一張圖也適用於 V1.1／V1.2。** 外殼鎖定的 20 個位置（按鈕、LED、接頭）在
各版之間沒有動過（hardware/kicad/mechanical_lock.json，tools/check_mech_kicad.py
驗證），頁面標示狀態的零件都在其中或緊鄰其旁。V1.3 才有的 R35–R37、D9、D10
在舊板上不存在，但它們只是背景，不帶狀態。

改了板子之後重跑一次即可；頁面的 JS 只依賴 fp-<位號> 這些 id。
"""
import os, re
import pcbnew

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOARD = os.path.join(REPO, "hardware", "kicad", "TES_Controller.kicad_pcb")
PAGE = os.path.join(REPO, "firmware", "components", "services", "web", "hw.html")

SKIP = re.compile(r"^LOGO")          # 絲印 logo 太大，對狀態頁沒有資訊
mm = pcbnew.ToMM


def f2(v):
    s = "%.2f" % v
    return s.rstrip("0").rstrip(".") if "." in s else s


def rect(x0, y0, x1, y1, cls=None, rx=None):
    a = ' class="%s"' % cls if cls else ""
    r = ' rx="%s"' % f2(rx) if rx else ""
    return '<rect x="%s" y="%s" width="%s" height="%s"%s%s/>' % (
        f2(x0), f2(y0), f2(x1 - x0), f2(y1 - y0), r, a)


def pad_svg(p):
    bb = p.GetBoundingBox()
    x0, y0 = mm(bb.GetX()), mm(bb.GetY())
    x1, y1 = x0 + mm(bb.GetWidth()), y0 + mm(bb.GetHeight())
    shape = p.GetShape()
    if shape == pcbnew.PAD_SHAPE_CIRCLE:
        c = p.GetPosition()
        s = '<circle cx="%s" cy="%s" r="%s"/>' % (f2(mm(c.x)), f2(mm(c.y)), f2((x1 - x0) / 2))
    else:
        rr = min(x1 - x0, y1 - y0) / 2 if shape == pcbnew.PAD_SHAPE_OVAL else None
        s = rect(x0, y0, x1, y1, rx=rr)
    if p.GetAttribute() in (pcbnew.PAD_ATTRIB_PTH, pcbnew.PAD_ATTRIB_NPTH):
        c = p.GetPosition()
        d = p.GetDrillSize()
        s += '<circle class="h" cx="%s" cy="%s" r="%s"/>' % (
            f2(mm(c.x)), f2(mm(c.y)), f2(mm(min(d.x, d.y)) / 2))
    return s


def courtyard(f):
    cy = f.GetCourtyard(pcbnew.F_CrtYd)
    bb = cy.BBox() if cy.OutlineCount() else f.GetBoundingBox(False, False)
    x0, y0 = mm(bb.GetX()), mm(bb.GetY())
    return x0, y0, x0 + mm(bb.GetWidth()), y0 + mm(bb.GetHeight())


def silk(b):
    out = []
    for d in b.GetDrawings():
        if d.GetLayer() != pcbnew.F_SilkS:
            continue
        if d.GetClass() == "PCB_TEXT":
            t = d.GetText().replace("&", "&amp;").replace("<", "&lt;")
            p = d.GetTextPos()
            h = mm(d.GetTextHeight())
            ang = d.GetTextAngleDegrees()
            tr = ' transform="rotate(%s %s %s)"' % (f2(-ang), f2(mm(p.x)), f2(mm(p.y))) if ang else ""
            # KiCad 的水平對齊：-1 靠左、0 置中、1 靠右（頁面 CSS 預設置中）
            j = {-1: ' text-anchor="start"', 1: ' text-anchor="end"'}.get(int(d.GetHorizJustify()), "")
            if d.IsMirrored():
                continue    # 鏡像字在頂層絲印上不該出現；有的話多半是背面的，略過
            out.append('<text x="%s" y="%s" font-size="%s"%s%s>%s</text>' % (
                f2(mm(p.x)), f2(mm(p.y) + h * 0.35), f2(h * 1.15), j, tr, t))
        elif d.GetShape() == pcbnew.SHAPE_T_RECT:
            s, e = d.GetStart(), d.GetEnd()
            out.append(rect(min(mm(s.x), mm(e.x)), min(mm(s.y), mm(e.y)),
                            max(mm(s.x), mm(e.x)), max(mm(s.y), mm(e.y)), cls="so"))
        elif d.GetShape() == pcbnew.SHAPE_T_SEGMENT:
            s, e = d.GetStart(), d.GetEnd()
            out.append('<line x1="%s" y1="%s" x2="%s" y2="%s"/>' % (
                f2(mm(s.x)), f2(mm(s.y)), f2(mm(e.x)), f2(mm(e.y))))
    return out


def main():
    b = pcbnew.LoadBoard(BOARD)
    e = b.GetBoardEdgesBoundingBox()
    ex, ey, ew, eh = mm(e.GetX()), mm(e.GetY()), mm(e.GetWidth()), mm(e.GetHeight())
    pad = 1.5

    parts = []
    for f in sorted(b.GetFootprints(), key=lambda f: f.GetReference()):
        ref = f.GetReference()
        if SKIP.match(ref) or f.GetLayer() != pcbnew.F_Cu:
            continue
        x0, y0, x1, y1 = courtyard(f)
        body = [rect(x0, y0, x1, y1, cls="cy", rx=0.3)]
        body += [pad_svg(p) for p in f.Pads()]
        parts.append('<g id="fp-%s">%s</g>' % (ref, "".join(body)))

    svg = ('<svg id="board" viewBox="%s %s %s %s" xmlns="http://www.w3.org/2000/svg" '
           'role="img" aria-label="TES 控制板俯視圖">'
           % (f2(ex - pad), f2(ey - pad), f2(ew + 2 * pad), f2(eh + 2 * pad)))
    svg += rect(ex + 0.13, ey + 0.13, ex + ew - 0.13, ey + eh - 0.13, cls="pcb", rx=1.2)
    svg += '<g class="silk">%s</g>' % "".join(silk(b))
    svg += '<g class="fp">%s</g>' % "".join(parts)
    svg += '<g id="tags"></g></svg>'

    page = open(PAGE, encoding="utf-8").read()
    new, n = re.subn(r"<!--BOARD-->.*?<!--/BOARD-->",
                     lambda _: "<!--BOARD-->" + svg + "<!--/BOARD-->", page, flags=re.S)
    if n != 1:
        raise SystemExit("hw.html 裡找不到唯一的 <!--BOARD--> … <!--/BOARD--> 區塊")
    with open(PAGE, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(new)
    print("板子 %.1f × %.1f mm，%d 顆零件，SVG %.1f KB → %s"
          % (ew, eh, len(parts), len(svg.encode()) / 1024, os.path.relpath(PAGE, REPO)))


if __name__ == "__main__":
    main()
