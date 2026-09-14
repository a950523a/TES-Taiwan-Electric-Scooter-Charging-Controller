#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
補上 5 個沒有 LCSC 料號的符號（3 顆 5mm LED + 2 個排針）。

不用 KiCad 內建庫的原因是腳位編號對不上：
內建 Device:LED 是 1=K、2=A，但 V1.3 的網表是 GPIO → pin1、pin2 → 電阻 → GND，
也就是 pin1 = 陽極。直接套內建符號會讓 LED 反接，而且網表比對會出現假差異。
自己畫一個和原設計同編號的符號，兩個問題一起沒有。
"""
import io, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sexpr
from sexpr import Sym as S

LIB = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "hardware", "kicad", "lib", "TES.kicad_sym")
FONT = [S("effects"), [S("font"), [S("size"), 1.27, 1.27]]]
STROKE = [S("stroke"), [S("width"), 0.2032], [S("type"), S("default")]]
NOFILL = [S("fill"), [S("type"), S("none")]]


def prop(name, value, x, y, hide=False):
    eff = [S("effects"), [S("font"), [S("size"), 1.27, 1.27]]]
    if hide:
        eff.append([S("hide"), S("yes")])
    return [S("property"), name, value, [S("at"), x, y, 0], eff]


def pin(num, name, x, y, angle, etype="passive"):
    return [S("pin"), S(etype), S("line"),
            [S("at"), x, y, angle], [S("length"), 2.54],
            [S("name"), name, FONT], [S("number"), num, FONT]]


def poly(pts, fill=None):
    n = [S("polyline"), [S("pts")] + [[S("xy"), a, b] for a, b in pts], STROKE]
    n.append([S("fill"), [S("type"), S(fill or "none")]])
    return n


def rect(x1, y1, x2, y2):
    return [S("rectangle"), [S("start"), x1, y1], [S("end"), x2, y2],
            STROKE, NOFILL]


def base(name, ref, footprint, desc):
    return [S("symbol"), name,
            [S("pin_names"), [S("offset"), 1.016]],
            [S("exclude_from_sim"), S("no")],
            [S("in_bom"), S("yes")],
            [S("on_board"), S("yes")],
            prop("Reference", ref, 0, 5.08),
            prop("Value", name, 0, -5.08, ),
            prop("Footprint", footprint, 0, -7.62, hide=True),
            prop("Datasheet", "", 0, -10.16, hide=True),
            prop("Description", desc, 0, -12.7, hide=True)]


def led_symbol():
    """5mm 直插 LED。pin1 = 陽極，和 V1.3 網表一致（非 KiCad 內建的 1=K）。"""
    s = base("LED-5MM", "D", "TES:LED-TH_BD5.8-P2.54-FD",
             "5mm 直插 LED；pin1=A pin2=K（沿用 V1.3 編號）")
    g = [S("symbol"), "LED-5MM_0_1",
         poly([(-1.27, 2.54), (-1.27, -2.54)]),          # 陽極側豎線
         poly([(-1.27, 0), (1.27, 0)]),                  # 三角形指向陰極
         poly([(1.27, 2.54), (1.27, -2.54), (-1.27, 0), (1.27, 2.54)], fill="none"),
         poly([(1.27, 2.54), (1.27, -2.54)]),            # 陰極橫棒
         poly([(2.0, 3.3), (3.3, 4.6)]), poly([(3.3, 4.6), (2.8, 4.4)]),
         poly([(3.3, 4.6), (3.1, 4.0)]),                 # 出光箭頭
         poly([(0.5, 3.3), (1.8, 4.6)]), poly([(1.8, 4.6), (1.3, 4.4)]),
         poly([(1.8, 4.6), (1.6, 4.0)])]
    u = [S("symbol"), "LED-5MM_1_1",
         pin("1", "A", -5.08, 0, 0),
         pin("2", "K", 5.08, 0, 180)]
    return s + [g, u]


def header_symbol(n):
    """1xN 2.54mm 排針，腳位由上往下 1..N。"""
    name = "HDR-1X%d" % n
    s = base(name, "H", "TES:HDR-TH_%dP-P2.54-V-F" % n,
             "2.54mm 單排排針 %d pin" % n)
    top = (n - 1) * 1.27
    g = [S("symbol"), name + "_0_1", rect(-1.27, top + 1.27, 1.27, top - n * 2.54 + 1.27)]
    u = [S("symbol"), name + "_1_1"]
    for i in range(n):
        u.append(pin(str(i + 1), "P%d" % (i + 1), -5.08, top - i * 2.54, 0))
    return s + [g, u]


def solderpad_symbol():
    """單點焊線焊盤。V1.3 用 4 個這種焊盤接 120V→模組、模組→12V。

    它們在 EasyEDA 裡是畫在銅箔上的自由焊盤、不是元件，所以原本不在網表裡。
    做成元件之後，DRC 才知道這些點屬於哪條網路，間距規則也才管得到。
    """
    s = base("SOLDERPAD-1P", "W", "TES:SOLDERPAD-TH_3.0X1.5",
             "焊線焊盤，3.05 x 1.52 mm / 孔 0.91 mm")
    g = [S("symbol"), "SOLDERPAD-1P_0_1",
         rect(-1.27, 1.27, 1.27, -1.27),
         poly([(-1.27, 1.27), (1.27, -1.27)]),
         poly([(-1.27, -1.27), (1.27, 1.27)])]
    u = [S("symbol"), "SOLDERPAD-1P_1_1", pin("1", "W", -5.08, 0, 0)]
    return s + [g, u]


def power_symbol(name, kind):
    """電源／接地符號。

    電路圖上最花眼力的就是滿版的 GND 標籤 —— 這塊板子 238 個標籤裡有 114 個
    是電源或接地（光 GND 就 73 個）。換成符號之後，「這裡接地」變成一眼可見
    的圖形，不必逐個讀字。

    GND 與 GND_BACK 用不同圖形：前者是訊號地，後者是 120V 的回流端，
    在板子上是分開的兩個節點（只透過降壓模組的線束共通），畫成一樣會誤導。
    """
    s = [S("symbol"), name,
         [S("power")],
         [S("pin_numbers"), [S("hide"), S("yes")]],
         [S("pin_names"), [S("offset"), 0.0], [S("hide"), S("yes")]],
         [S("exclude_from_sim"), S("no")],
         [S("in_bom"), S("no")],
         [S("on_board"), S("yes")],
         prop("Reference", "#PWR", 0, -3.81, hide=True),
         prop("Value", name, 0, 3.81 if kind == "gnd" else -3.81),
         prop("Footprint", "", 0, -6.35, hide=True),
         prop("Datasheet", "", 0, -7.62, hide=True),
         prop("Description", "電源符號", 0, -8.89, hide=True)]
    g = [S("symbol"), name + "_0_1"]
    if kind == "gnd":
        # 三條遞減橫線（訊號地）
        g += [poly([(0, 0), (0, -1.27)]),
              poly([(-1.905, -1.27), (1.905, -1.27)]),
              poly([(-1.27, -1.905), (1.27, -1.905)]),
              poly([(-0.635, -2.54), (0.635, -2.54)])]
    elif kind == "earth":
        # 斜線接地（機殼／回流端），刻意和訊號地不同
        g += [poly([(0, 0), (0, -1.27)]),
              poly([(-1.905, -1.27), (1.905, -1.27)])]
        for dx in (-1.27, 0, 1.27):
            g += [poly([(dx, -1.27), (dx - 0.635, -2.54)])]
    else:
        # 電源軌：一條橫線加一個小三角
        g += [poly([(0, 0), (0, 1.27)]),
              poly([(-1.27, 1.27), (1.27, 1.27)]),
              poly([(-0.762, 1.27), (0, 2.032), (0.762, 1.27)], fill="outline")]
    u = [S("symbol"), name + "_1_1",
         pin("1", name, 0.0, 0.0, 270 if kind in ("gnd", "earth") else 90,
             etype="power_in")]
    return s + [g, u]


# 符號名稱**必須**和板子上的網路名一模一樣。KiCad 的電源符號會用自己的名字
# 當網路名，取成 +12V 就會把整條 12V 網路改名，連 PCB 的網表都跟著變。
POWER_SYMBOLS = [("GND", "gnd"), ("GND_BACK", "earth"),
                 ("5V", "rail"), ("VDD33", "rail"),
                 ("12V", "rail"), ("120V", "rail")]


def pwr_flag_symbol():
    """電源旗標。

    12V 和 GND 從連接器進來，電路圖上沒有任何 power_out 腳位驅動它們，
    ERC 會判定「電源輸入腳沒有被驅動」。這個符號的作用就是告訴 ERC
    「這條網路的電從板外來，不用找驅動源」。

    in_bom / on_board 都是 no，所以不會出現在網表裡 —— 網表比對仍然乾淨。
    """
    s = [S("symbol"), "PWR_FLAG",
         [S("power")],
         [S("pin_names"), [S("offset"), 0.0]],
         [S("exclude_from_sim"), S("yes")],
         [S("in_bom"), S("no")],
         [S("on_board"), S("no")],
         prop("Reference", "#FLG", 0, 3.81, hide=True),
         prop("Value", "PWR_FLAG", 0, 5.08),
         prop("Footprint", "", 0, -1.27, hide=True),
         prop("Datasheet", "", 0, -2.54, hide=True),
         prop("Description", "板外供電的電源旗標，不進 BOM", 0, -3.81, hide=True)]
    g = [S("symbol"), "PWR_FLAG_0_0",
         pin("1", "pwr", 0.0, 0.0, 90, etype="power_out")]
    d = [S("symbol"), "PWR_FLAG_0_1",
         poly([(0, 0), (0, 1.27), (-1.016, 1.905), (0, 2.54),
               (1.016, 1.905), (0, 1.27)])]
    return s + [g, d]


def main():
    lib = sexpr.load(LIB)
    have = {s[1] for s in sexpr.findall(lib, "symbol")}
    added = []
    syms = [led_symbol(), header_symbol(3), header_symbol(4),
            solderpad_symbol(), pwr_flag_symbol()]
    syms += [power_symbol(n, k) for n, k in POWER_SYMBOLS]
    for sym in syms:
        if sym[1] in have:
            lib[:] = [c for c in lib
                      if not (isinstance(c, list) and c[:2] == [S("symbol"), sym[1]])]
        lib.append(sym)
        added.append(sym[1])
    io.open(LIB, "w", encoding="utf-8").write(sexpr.dumps(lib) + "\n")
    print("寫入 %s：%s" % (os.path.basename(LIB), ", ".join(added)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
