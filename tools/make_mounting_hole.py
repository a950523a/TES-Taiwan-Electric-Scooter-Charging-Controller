#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""產生安裝孔封裝 TES.pretty/MountingHole_3.2mm_NPTH_Keepout4.6mm。在 KiCad 的 python 裡跑。

降壓模組用 **尼龍柱** 鎖在這 4 個孔上，外殼也從這 4 個孔固定。孔是非電鍍的
Ø3.2 mm，外加兩面半徑 2.3 mm（孔邊外 0.7 mm）的禁銅區，只為了讓鑽孔的位置
誤差與防焊開窗不碰到銅。

不需要更大的禁銅區：尼龍柱配塑膠螺絲，兩面都沒有金屬接觸（就算日後換金屬
螺絲，背面螺絲頭碰到的也只有底層 GND 地平面）。**如果哪天改用銅柱，要加大到半徑 3.0 mm**
（M3 六角銅柱對邊 5 mm、外接圓半徑約 2.9 mm）—— 到時左上孔旁的 GND_BACK
（孔心 2.37 mm）與 CP_SENSE（2.58 mm）要先繞開，它們會壓在銅柱底下。
"""
import math, os
import pcbnew

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB = os.path.join(REPO, "hardware", "kicad", "lib", "TES.pretty")
NAME = "MountingHole_3.2mm_NPTH_Keepout4.6mm"
DRILL = 3.2
KEEPOUT_R = 2.3


def circle(r, n=48):
    ch = pcbnew.SHAPE_LINE_CHAIN()
    for i in range(n):
        a = 2 * math.pi * i / n
        ch.Append(pcbnew.FromMM(r * math.cos(a)), pcbnew.FromMM(r * math.sin(a)))
    ch.SetClosed(True)
    return ch


def main():
    fp = pcbnew.FOOTPRINT(None)
    fp.SetFPID(pcbnew.LIB_ID("TES", NAME))
    fp.SetReference("MH**")
    fp.SetValue(NAME)
    fp.Reference().SetVisible(False)
    fp.Value().SetVisible(False)
    fp.Value().SetLayer(pcbnew.F_Fab)
    fp.SetAttributes(pcbnew.FP_BOARD_ONLY | pcbnew.FP_EXCLUDE_FROM_BOM
                     | pcbnew.FP_EXCLUDE_FROM_POS_FILES)
    fp.SetLibDescription("M3 安裝孔（尼龍柱），非電鍍 Ø3.2 mm，兩面 Ø4.6 mm 禁銅。"
                         "由 tools/make_mounting_hole.py 產生。")

    pad = pcbnew.PAD(fp)
    pad.SetAttribute(pcbnew.PAD_ATTRIB_NPTH)
    pad.SetShape(pcbnew.PAD_SHAPE_CIRCLE)
    pad.SetSize(pcbnew.VECTOR2I_MM(DRILL, DRILL))
    pad.SetDrillSize(pcbnew.VECTOR2I_MM(DRILL, DRILL))
    ls = pcbnew.LSET(); ls.AddLayer(pcbnew.F_Cu); ls.AddLayer(pcbnew.B_Cu)
    ls.AddLayer(pcbnew.F_Mask); ls.AddLayer(pcbnew.B_Mask)
    pad.SetLayerSet(ls)
    pad.SetNumber("")
    fp.Add(pad)

    z = pcbnew.ZONE(fp)
    z.SetIsRuleArea(True)
    z.SetDoNotAllowTracks(True)
    z.SetDoNotAllowVias(True)
    z.SetDoNotAllowPads(False)       # 不然孔自己的 NPTH 焊盤也違規
    z.SetDoNotAllowZoneFills(True)
    z.SetDoNotAllowFootprints(False)
    zl = pcbnew.LSET(); zl.AddLayer(pcbnew.F_Cu); zl.AddLayer(pcbnew.B_Cu)
    z.SetLayerSet(zl)
    z.Outline().AddOutline(circle(KEEPOUT_R))
    z.SetZoneName("mounting_keepout")
    fp.Add(z)

    for layer in (pcbnew.F_CrtYd, pcbnew.B_CrtYd):
        c = pcbnew.PCB_SHAPE(fp)
        c.SetShape(pcbnew.SHAPE_T_CIRCLE)
        c.SetCenter(pcbnew.VECTOR2I_MM(0, 0))
        c.SetEnd(pcbnew.VECTOR2I_MM(KEEPOUT_R + 0.25, 0))
        c.SetWidth(pcbnew.FromMM(0.05))
        c.SetLayer(layer)
        fp.Add(c)
    fab = pcbnew.PCB_SHAPE(fp)
    fab.SetShape(pcbnew.SHAPE_T_CIRCLE)
    fab.SetCenter(pcbnew.VECTOR2I_MM(0, 0))
    fab.SetEnd(pcbnew.VECTOR2I_MM(KEEPOUT_R, 0))
    fab.SetWidth(pcbnew.FromMM(0.1))
    fab.SetLayer(pcbnew.F_Fab)
    fp.Add(fab)

    pcbnew.FootprintSave(LIB, fp)
    print("→ %s" % NAME)


if __name__ == "__main__":
    main()
