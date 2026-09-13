#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把板上封裝的庫參照改指向 TES 元件庫。在 KiCad 的 python 裡跑。

匯入進來的封裝掛在 `TES_Contro-easyedapro`，那個庫並不存在；電路圖指定的
卻是 `TES:`。兩邊不一致的後果不是警告而已 —— 使用者哪天在 GUI 按下
「從電路圖更新 PCB」，73 個封裝會被整批換掉，走線全部重來。

改的只是參照：板上每個封裝都內嵌自己的幾何，lib_id 不影響現有形狀。
實測 R0603 兩邊焊盤在 ±0.750 與 ±0.753 mm（差 3 µm，取整造成），尺寸相同。

用法：python tools/_pcb_relink_libs.py <board>
"""
import os, sys
import pcbnew

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRETTY = os.path.join(REPO, "hardware", "kicad", "lib", "TES.pretty")
# 四個接線焊盤匯入後叫 Pad_eNNN，對應我們自建的焊線焊盤
WIRE_PAD = "SOLDERPAD-TH_3.0X1.5"


def main():
    path = sys.argv[1]
    have = {n[:-len(".kicad_mod")] for n in os.listdir(PRETTY)
            if n.endswith(".kicad_mod")}
    b = pcbnew.LoadBoard(path)
    changed, missing = 0, {}
    for f in b.GetFootprints():
        fid = f.GetFPIDAsString()
        lib, _, name = fid.rpartition(":")
        if lib == "TES":
            continue
        target = WIRE_PAD if name.startswith("Pad_e") else name
        if target not in have:
            missing.setdefault(target, []).append(f.GetReference())
            continue
        f.SetFPIDAsString("TES:" + target)
        changed += 1
    print("重新連結 %d 個封裝 → TES:" % changed)
    if missing:
        print("TES.pretty 裡沒有對應封裝，維持原樣：")
        for k, v in sorted(missing.items()):
            print("   %-40s %s" % (k, ", ".join(sorted(v))))
    b.BuildConnectivity()
    pcbnew.SaveBoard(path, b)


if __name__ == "__main__":
    main()
