#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""產生嘉立創（JLCPCB）SMT 用的 BOM 與座標檔（CPL）。在 KiCad 的 python 裡跑。

    "C:/Program Files/KiCad/10.0/bin/python.exe" tools/make_jlc_assembly.py

輸出到 hardware/fab/V1.3/assembly/：
  TES_Controller_V1.3_BOM_JLC.csv   Comment, Designator, Footprint, LCSC Part #
  TES_Controller_V1.3_CPL_JLC.csv   Designator, Mid X, Mid Y, Layer, Rotation
  hand_solder.txt                   不給嘉立創貼的零件（插件、沒有料號的）

**料號的來源有兩個，這支把它們合在一起**（CLAUDE.md 的 ordering note）：
bom.csv 是 EasyEDA 原稿的匯出、不能改（它是網表驗證的基準），而 V1.3 的變更
記在 changes_v13.py。所以：原稿料號 → 套用 changes_v13 的新增／改型號 →
依 MPN_LCSC 查新型號的 LCSC 料號。位號則照 sch_gen.DESIGNATOR_RENAME 換成
KiCad 側的名字（START → START1、Y/G/R → D6/D7/D8）。

**零件清單以 KiCad 的板子為準**，不是以 bom.csv 為準：原稿有、板上沒有的不會
出現；board-only 的封裝（logo、安裝孔）不會出現。

**旋轉角直接用 KiCad 的。** 2026-09-29 逐顆比對過：77 個原稿元件的角度，KiCad
與 EasyEDA 完全一致，而嘉立創的零件方向就是 EasyEDA 那一套。座標同 Gerber
（Y 軸向上為正，所以是 −y）。下單時仍要在嘉立創的預覽裡逐顆看一次方向。
"""
import collections, csv, os, sys
import pcbnew

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import changes_v13                                         # noqa: E402
from sch_gen import DESIGNATOR_RENAME                      # noqa: E402

BOARD = os.path.join(REPO, "hardware", "kicad", "TES_Controller.kicad_pcb")
SRC_BOM = os.path.join(REPO, "hardware", "TES_Controller_V1.3", "bom.csv")
OUT = os.path.join(REPO, "hardware", "fab", "V1.3", "assembly")

# V1.3 新型號 → (LCSC 料號, 實際下單的型號, 備註)。庫存是 2026-09-29 查的，會變。
MPN_LCSC = {
    "0603WAF1002T5E":   ("C25804",  "0603WAF1002T5E",   ""),
    "RT0603BRD079K09L": ("C861611", "RT0603BRD079K09L", ""),
    # 原本指定 RT0603BRD07115KL（C861084），2026-09-29 只剩 2 顆、每片要 3 顆，
    # 改用同系列的 BRE（Yageo RT 薄膜、±0.1%，溫度係數代碼 E 而非 D），
    # changes_v13.py 的型號也一起改了。
    "RT0603BRE07115KL": ("C861635", "RT0603BRE07115KL", ""),
    "SMBJ130A":         ("C135040", "SMBJ130A-13-F",    ""),
    "SMBJ12A":          ("C908793", "SMBJ12A",          "與 D3 同料號"),
}

# 有通孔腳但仍交給 SMT 的：Type-C 的外殼固定腳是通孔，焊點在 SMT 回焊時一起上錫。
SMT_DESPITE_TH = {"USB1"}


def load_source_bom():
    out = {}
    with open(SRC_BOM, encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            des = DESIGNATOR_RENAME.get(row["designator"], row["designator"])
            out[des] = (row["part"].strip(), row["supplier"].strip(), "")
    return out


def apply_changes(bom):
    for c in changes_v13.CHANGES:
        if c["op"] in ("add_part", "set_value"):
            mpn = c["value"]
            if mpn not in MPN_LCSC:
                raise SystemExit("changes_v13 的 %s 用了 %s，MPN_LCSC 裡查不到料號"
                                 % (c["ref"], mpn))
            lcsc, ordered, note = MPN_LCSC[mpn]
            bom[c["ref"]] = (ordered, lcsc, note)
    return bom


def main():
    b = pcbnew.LoadBoard(BOARD)
    bom = apply_changes(load_source_bom())
    mm = pcbnew.ToMM

    smt, hand = [], []
    for f in sorted(b.GetFootprints(), key=lambda f: f.GetReference()):
        ref = f.GetReference()
        if f.GetAttributes() & pcbnew.FP_EXCLUDE_FROM_BOM:
            continue
        has_th = any(p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH for p in f.Pads())
        mpn, lcsc, note = bom.get(ref, ("", "", ""))
        fpname = str(f.GetFPID().GetLibItemName())
        if not lcsc:
            hand.append((ref, mpn or f.GetValue(), fpname, "沒有 LCSC 料號"))
        elif has_th and ref not in SMT_DESPITE_TH:
            hand.append((ref, mpn, fpname, "插件（%s）" % lcsc))
        else:
            if f.GetLayer() != pcbnew.F_Cu:
                raise SystemExit("%s 在背面 —— 這塊板是單面貼片" % ref)
            p = f.GetPosition()
            smt.append(dict(ref=ref, mpn=mpn, lcsc=lcsc, note=note, fp=fpname,
                            x=mm(p.x), y=-mm(p.y),
                            rot=f.GetOrientationDegrees() % 360))

    os.makedirs(OUT, exist_ok=True)
    groups = collections.OrderedDict()
    for s in smt:
        groups.setdefault((s["mpn"], s["fp"], s["lcsc"]), []).append(s)
    with open(os.path.join(OUT, "TES_Controller_V1.3_BOM_JLC.csv"), "w",
              encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #"])
        for (mpn, fp, lcsc), items in groups.items():
            w.writerow([mpn, ",".join(i["ref"] for i in items), fp, lcsc])
    with open(os.path.join(OUT, "TES_Controller_V1.3_CPL_JLC.csv"), "w",
              encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for s in smt:
            w.writerow([s["ref"], "%.4fmm" % s["x"], "%.4fmm" % s["y"], "Top",
                        "%g" % s["rot"]])
    with open(os.path.join(OUT, "hand_solder.txt"), "w", encoding="utf-8") as f:
        f.write("不交給嘉立創 SMT 的零件（手焊或另外下插件加工）\n\n")
        for ref, mpn, fp, why in hand:
            f.write("%-11s %-26s %-40s %s\n" % (ref, mpn, fp, why))

    print("SMT %d 顆、%d 種料號；手焊 %d 顆" % (len(smt), len(groups), len(hand)))
    for s in smt:
        if s["note"]:
            print("  注意 %s：%s" % (s["ref"], s["note"]))


if __name__ == "__main__":
    main()
