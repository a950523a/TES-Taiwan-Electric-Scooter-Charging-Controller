#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把板上還沒進元件庫的封裝匯出到 TES.pretty。在 KiCad 的 python 裡跑。

EasyEDA Pro 的專案 zip 裡其實帶了通用元件的封裝（5mm LED、2.54 排針、
焊接跳線），只是 easyeda2kicad 走 LCSC 料號那條路看不到它們，
所以我一開始自己畫了一份。兩份不一樣：LED 的 1 腳左右相反、孔徑 0.90 vs 1.00，
排針一個縱向一個橫向。

**以板上的為準** —— 那是 V1.1／V1.2 實際做出來、焊過的版本。
"""
import os, sys
import pcbnew

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRETTY = os.path.join(REPO, "hardware", "kicad", "lib", "TES.pretty")


def main():
    path = sys.argv[1]
    b = pcbnew.LoadBoard(path)
    mgr = pcbnew.PCB_IO_MGR.FindPlugin(pcbnew.PCB_IO_MGR.KICAD_SEXP)
    have = {n[:-len(".kicad_mod")] for n in os.listdir(PRETTY)
            if n.endswith(".kicad_mod")}
    done = set()
    for f in b.GetFootprints():
        name = f.GetFPIDAsString().rpartition(":")[2]
        if name in have or name in done or name.startswith("Pad_e"):
            continue
        fp = f.Duplicate(False)
        try:
            mgr.FootprintSave(PRETTY, fp)
            print("   匯出 %s  (%s)" % (name, f.GetReference()))
            done.add(name)
        except Exception as e:
            print("   %s 匯出失敗: %s" % (name, str(e)[:70]))
    print("匯出 %d 個封裝到 TES.pretty" % len(done))


if __name__ == "__main__":
    main()
