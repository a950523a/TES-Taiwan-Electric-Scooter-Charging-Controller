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
  - OLED 螢幕（<g id="oled">）：不在 KiCad 檔裡，位置取自外殼上蓋的顯示窗，見 OLED_GLASS

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

# ── OLED（0.96 吋 SSD1306 模組，經 H1 排針接 I²C，疊在 ESP32-S3 模組上方）──────
# 它不是板上的零件，KiCad 檔裡沒有，所以位置取自外殼上蓋的顯示窗：
# docs/PCB/TES_Controller_V1_Case_Top.stp 頂面的 26.50 × 19.59 mm 開孔，
# x −18.78…7.73、y 25.06…44.65（上蓋座標，y 向上）。
# 上蓋 → PCB 的換算用四個安裝孔對出來（MH1–MH4 與上蓋的四個 6.1 mm 方孔），
# 再用按鈕、LED、BOOT/EN 的開孔驗證，全部在 0.1 mm 以內：
#     x_pcb = x_lid + 152.07      y_pcb = 104.66 − y_lid
OLED_GLASS = (133.29, 60.01, 159.80, 79.60)          # 可見的玻璃（= 上蓋開孔）
# 模組 PCB 外框只是概略：常見 4 腳 I²C 模組 27.3 × 27.8 mm，排針在下緣、
# 腳位中心距板邊約 1.3 mm。以 H1 的中心（146.54, 82.35）對齊。
OLED_MODULE = (146.54 - 13.65, 82.35 + 1.3 - 27.8, 146.54 + 13.65, 82.35 + 1.3)


def oled():
    g = OLED_GLASS
    m = OLED_MODULE
    return ('<g id="oled">%s%s<text x="%s" y="%s">OLED 0.96″</text></g>' % (
        rect(*m, cls="om", rx=0.8), rect(*g, cls="og", rx=0.4),
        f2((g[0] + g[2]) / 2), f2(g[1] + 2.6)))


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
    cy = f.GetCourtyard(pcbnew.F_CrtYd if f.GetLayer() == pcbnew.F_Cu else pcbnew.B_CrtYd)
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

    parts, back = [], []
    for f in sorted(b.GetFootprints(), key=lambda f: f.GetReference()):
        ref = f.GetReference()
        if SKIP.match(ref):
            continue
        top_side = f.GetLayer() == pcbnew.F_Cu
        x0, y0, x1, y1 = courtyard(f)
        # 裝在背面的零件（例如 H2 PSU UART 排針）：從正面看得到的只有通孔焊盤，
        # 外框畫成虛線表示「在背面」。背面的貼片焊盤從正面看不到，不畫。
        pads = [p for p in f.Pads() if top_side or
                p.GetAttribute() in (pcbnew.PAD_ATTRIB_PTH, pcbnew.PAD_ATTRIB_NPTH)]
        if not top_side and not pads:
            continue
        body = [rect(x0, y0, x1, y1, cls="cy" if top_side else "cy cyb", rx=0.3)]
        body += [pad_svg(p) for p in pads]
        parts.append('<g id="fp-%s">%s</g>' % (ref, "".join(body)))
        if not top_side:
            back.append(ref)
    if back:
        print("背面零件（只畫通孔焊盤）：%s" % ", ".join(back))

    # OLED 模組會超出板子上緣，視框要把它包進來
    top = min(ey, OLED_MODULE[1]) - pad
    svg = ('<svg id="board" viewBox="%s %s %s %s" xmlns="http://www.w3.org/2000/svg" '
           'role="img" aria-label="TES 控制板俯視圖">'
           % (f2(ex - pad), f2(top), f2(ew + 2 * pad), f2(ey + eh + pad - top)))
    svg += rect(ex + 0.13, ey + 0.13, ex + ew - 0.13, ey + eh - 0.13, cls="pcb", rx=1.2)
    svg += '<g class="silk">%s</g>' % "".join(silk(b))
    svg += '<g class="fp">%s</g>' % "".join(parts)
    svg += oled()     # 疊在零件上面：實物就是蓋在 ESP32 上
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
