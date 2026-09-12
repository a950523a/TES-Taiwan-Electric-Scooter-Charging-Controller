#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
補上 3 個 EasyEDA 沒有匯出封裝幾何的通用元件：5mm LED、1x3 與 1x4 排針。

不用 KiCad 內建庫的原因和符號一樣是焊盤編號：內建 LED_THT:LED_D5.0mm
是 pad1=K，但 V1.3 的網表是 pad1=陽極。沿用內建封裝會讓絲印把陰極標在
陽極那一腳，手焊時反過來插。自己畫就沒有這個問題。

幾何是標準值（LED 腳距 2.54mm、排針 2.54mm），不是從 .eprj 抄的 ——
EasyEDA 的專案匯出不含通用元件的封裝定義。焊盤對原點左右對稱，
所以元件原點 = LED 本體中心，和 mechanical_lock.json 的鎖定座標一致。
"""
import io, os, sys, uuid

PRETTY = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "hardware", "kicad", "lib", "TES.pretty")

HDR = '''(footprint "%(name)s"
\t(version 20241229)
\t(generator "tes_make_footprints")
\t(generator_version "10.0")
\t(layer "F.Cu")
\t(descr "%(descr)s")
\t(attr through_hole)
\t(property "Reference" "REF**"
\t\t(at 0 %(ref_y).2f 0)
\t\t(layer "F.SilkS")
\t\t(uuid "%(u1)s")
\t\t(effects (font (size 1 1) (thickness 0.15)))
\t)
\t(property "Value" "%(name)s"
\t\t(at 0 %(val_y).2f 0)
\t\t(layer "F.Fab")
\t\t(uuid "%(u2)s")
\t\t(effects (font (size 1 1) (thickness 0.15)))
\t)
'''


def uid():
    return str(uuid.uuid4())


def line(x1, y1, x2, y2, layer="F.SilkS", w=0.12):
    return ('\t(fp_line (start %.3f %.3f) (end %.3f %.3f)\n'
            '\t\t(stroke (width %.2f) (type solid)) (layer "%s") (uuid "%s")\n\t)\n'
            % (x1, y1, x2, y2, w, layer, uid()))


def circle(cx, cy, r, layer="F.SilkS", w=0.12):
    return ('\t(fp_circle (center %.3f %.3f) (end %.3f %.3f)\n'
            '\t\t(stroke (width %.2f) (type solid)) (fill none) (layer "%s") (uuid "%s")\n\t)\n'
            % (cx, cy, cx + r, cy, w, layer, uid()))


def pad(num, x, y, shape, size, drill):
    return ('\t(pad "%s" thru_hole %s\n'
            '\t\t(at %.3f %.3f) (size %.3f %.3f) (drill %.3f)\n'
            '\t\t(layers "*.Cu" "*.Mask") (remove_unused_layers no) (uuid "%s")\n\t)\n'
            % (num, shape, x, y, size, size, drill, uid()))


def led_5mm():
    """5mm 直插 LED。pad1 = 陽極（方形，標示 1 腳），pad2 = 陰極（本體切邊側）。"""
    s = HDR % dict(name="LED-TH_5MM", descr="5mm 直插 LED，pad1=A pad2=K（沿用 V1.3 編號）",
                   ref_y=-4.0, val_y=4.0, u1=uid(), u2=uid())
    s += pad("1", -1.27, 0, "rect", 1.8, 0.9)
    s += pad("2", 1.27, 0, "circle", 1.8, 0.9)
    # 本體輪廓：5mm 圓，陰極側切平（和實體 LED 的切邊一致）
    s += circle(0, 0, 2.5)
    s += line(2.25, -1.09, 2.25, 1.09)
    s += line(-3.2, -2.6, -3.2, -1.6)      # 陽極側的 "+" 記號
    s += line(-3.7, -2.1, -2.7, -2.1)
    s += circle(0, 0, 2.5, layer="F.Fab", w=0.1)
    s += line(-2.54, -3.2, -2.54, -3.2, layer="F.CrtYd", w=0.05)
    for a, b, c, d in ((-3.0, -3.0, 3.0, -3.0), (3.0, -3.0, 3.0, 3.0),
                       (3.0, 3.0, -3.0, 3.0), (-3.0, 3.0, -3.0, -3.0)):
        s += line(a, b, c, d, layer="F.CrtYd", w=0.05)
    return s + ")\n"


def header(n):
    """1xN 2.54mm 直立排針。pad1 方形，其餘圓形；腳位沿 +Y 方向排列。"""
    name = "HDR-TH_1X%d_P2.54" % n
    span = (n - 1) * 2.54
    s = HDR % dict(name=name, descr="2.54mm 單排排針 %d pin" % n,
                   ref_y=-2.5, val_y=span + 2.5, u1=uid(), u2=uid())
    for i in range(n):
        s += pad(str(i + 1), 0, i * 2.54, "rect" if i == 0 else "circle", 1.7, 1.0)
    x0, y0, x1, y1 = -1.27, -1.27, 1.27, span + 1.27
    for a, b, c, d in ((x0, y0, x1, y0), (x1, y0, x1, y1),
                       (x1, y1, x0, y1), (x0, y1, x0, y0)):
        s += line(a, b, c, d, layer="F.Fab", w=0.1)
    # 絲印讓開焊盤，只畫兩側；1 腳外側加一條短線標示
    s += line(-1.45, -1.45, -1.45, y1 + 0.18)
    s += line(1.45, -1.45, 1.45, y1 + 0.18)
    s += line(-1.45, -1.45, 1.45, -1.45)
    for a, b, c, d in ((-1.7, -1.7, 1.7, -1.7), (1.7, -1.7, 1.7, y1 + 0.43),
                       (1.7, y1 + 0.43, -1.7, y1 + 0.43), (-1.7, y1 + 0.43, -1.7, -1.7)):
        s += line(a, b, c, d, layer="F.CrtYd", w=0.05)
    return s + ")\n"


def pad_rect(num, x, y, w, h, drill):
    """長方形通孔焊盤。pad() 只做正方形，焊線焊盤是 3.05 x 1.52 的長條。"""
    return ('\t(pad "%s" thru_hole rect\n'
            '\t\t(at %.3f %.3f) (size %.3f %.3f) (drill %.3f)\n'
            '\t\t(layers \"*.Cu\" \"*.Mask\") (remove_unused_layers no) (uuid \"%s\")\n\t)\n'
            % (num, x, y, w, h, drill, uid()))


def solderpad():
    """焊線焊盤。尺寸照抄 V1.3：3.048 x 1.524 mm 矩形焊盤、0.914 mm 孔。

    導線穿孔再焊，比純表面焊盤耐拉扯 —— 這四條線接在會震動的車上設備裡。
    """
    s = HDR % dict(name="SOLDERPAD-TH_3.0X1.5",
                   descr="焊線焊盤 3.05x1.52mm / 孔 0.91mm",
                   ref_y=-2.2, val_y=2.2, u1=uid(), u2=uid())
    s += pad_rect("1", 0, 0, 3.048, 1.524, 0.914)
    for a, b, c, d in ((-1.8, -1.0, 1.8, -1.0), (1.8, -1.0, 1.8, 1.0),
                       (1.8, 1.0, -1.8, 1.0), (-1.8, 1.0, -1.8, -1.0)):
        s += line(a, b, c, d, layer="F.CrtYd", w=0.05)
    s += line(-1.524, -0.762, 1.524, -0.762, layer="F.Fab", w=0.1)
    s += line(-1.524, 0.762, 1.524, 0.762, layer="F.Fab", w=0.1)
    return s + ")" + chr(10)


def main():
    made = []
    for fp in (led_5mm(), header(3), header(4), solderpad()):
        name = fp.split('"')[1]
        io.open(os.path.join(PRETTY, name + ".kicad_mod"), "w",
                encoding="utf-8").write(fp)
        made.append(name)
    print("寫入 %s：%s" % (os.path.basename(PRETTY), ", ".join(made)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
