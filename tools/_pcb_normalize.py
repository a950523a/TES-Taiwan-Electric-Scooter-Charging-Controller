#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""在 KiCad 自帶的 python 裡跑（pcbnew 只有那裡有）。由 pcb_import.py 呼叫。

argv: <輸入 .kicad_pcb> <輸出 .kicad_pcb>
"""
import os, sys
import pcbnew

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from sch_gen import DESIGNATOR_RENAME                      # noqa: E402
from eda_export import free_pads                           # noqa: E402

BACKUP_DELTA = {("C20", "1"): "5V", ("U5", "8"): "5V"}


def main():
    src, dst = sys.argv[1], sys.argv[2]
    b = pcbnew.LoadBoard(src)

    # 1) 位號改名
    renamed = 0
    for f in b.GetFootprints():
        new = DESIGNATOR_RENAME.get(f.GetReference())
        if new:
            f.SetReference(new)
            renamed += 1
    print("位號改名 %d 個" % renamed)

    # 2) 四個自由焊盤補位號。匯入後它們是沒有位號的 footprint，
    #    每個掛在唯一的網路上，所以用網路名對應就夠了。
    want = {net: des for des, (_p, net, _x, _y) in free_pads_of_current().items()}
    wired = 0
    for f in b.GetFootprints():
        if f.GetReference():
            continue
        nets = {p.GetNetname() for p in f.Pads()}
        if len(nets) == 1:
            des = want.get(nets.pop())
            if des:
                f.SetReference(des)
                f.SetValue("SOLDERPAD-1P")
                wired += 1
    print("自由焊盤補位號 %d 個" % wired)

    # 3) 備份落後於當前 .eprj 的兩個焊盤
    fixed, ripped = 0, 0
    for f in b.GetFootprints():
        for p in f.Pads():
            netname = BACKUP_DELTA.get((f.GetReference(), p.GetNumber()))
            if netname:
                ni = b.FindNet(netname)
                if ni is None:
                    raise SystemExit("找不到網路 %s" % netname)
                old = p.GetNetname()
                p.SetNet(ni)
                fixed += 1
                print("   %s.%s  %s → %s" % (f.GetReference(), p.GetNumber(),
                                             old, netname))
    # 改了網路之後，原本接到那兩個焊盤的走線會變成錯誤網路上的孤線。
    # 把碰到這兩個焊盤位置的走線拆掉，之後重繞。
    targets = []
    for f in b.GetFootprints():
        for p in f.Pads():
            if (f.GetReference(), p.GetNumber()) in BACKUP_DELTA:
                targets.append(p.GetPosition())
    for t in list(b.GetTracks()):
        if t.Type() == pcbnew.PCB_VIA_T:
            continue
        for pt in targets:
            if (t.GetStart() == pt or t.GetEnd() == pt):
                b.Remove(t)
                ripped += 1
                break
    print("修正焊盤網路 %d 個，拆掉懸空走線 %d 條" % (fixed, ripped))

    b.BuildConnectivity()
    pcbnew.SaveBoard(dst, b)
    print("已存 %s" % os.path.basename(dst))


def free_pads_of_current():
    """從當前 .eprj 取四個焊盤的 位號 → 網路 對應。"""
    import sqlite3
    from eda_export import read_project, build_model, jsonl
    eprj = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "docs", "PCB", "TES_Controller_V1.3.eprj")
    docs, devices, attrs = read_project(eprj)
    _, _, pcb_recs = build_model(docs, devices, attrs)
    return free_pads(pcb_recs)


if __name__ == "__main__":
    main()
