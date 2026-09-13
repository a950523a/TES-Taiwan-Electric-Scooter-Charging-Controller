#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 changes_v13 的 set_net / set_value / disconnect 套到板子上。

在 KiCad 的 python 裡跑。改網路的焊盤上原有的走線會一併拆掉 ——
留著的話那段銅箔就掛在錯誤的網路上，DRC 會報短路而不是未連接，
比較難看出是哪裡出問題。
"""
import os, sys
import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import changes_v13                                         # noqa: E402


def main():
    path = sys.argv[1]
    b = pcbnew.LoadBoard(path)
    pads = {}
    for f in b.GetFootprints():
        for p in f.Pads():
            pads[(f.GetReference(), p.GetNumber())] = (f, p)

    # 新增元件會帶進新的網路名（例如分壓串聯中間的 HV_DIV1／HV_DIV2），
    # 板上還沒有，要先建出來才能指派給焊盤。
    wanted = set()
    for c in changes_v13.CHANGES:
        if c["op"] == "set_net":
            wanted.add(c["net"])
        elif c["op"] == "add_part":
            wanted.update(c["pins"].values())
    for name in sorted(wanted):
        if b.FindNet(name) is None:
            b.Add(pcbnew.NETINFO_ITEM(b, name))
            print("   建立網路 %s" % name)

    touched, doomed = [], []
    for c in changes_v13.CHANGES:
        op = c["op"]
        if op == "set_value":
            fp = None
            for f in b.GetFootprints():
                if f.GetReference() == c["ref"]:
                    fp = f
            if fp is not None:
                old = fp.GetValue()
                fp.SetValue(c["value"])
                print("   %s 型號 %s → %s" % (c["ref"], old, c["value"]))
        elif op in ("set_net", "disconnect"):
            hit = pads.get((c["ref"], c["pad"]))
            if hit is None:
                print("   找不到 %s.%s" % (c["ref"], c["pad"]))
                continue
            _f, p = hit
            old = p.GetNetname()
            if op == "set_net":
                ni = b.FindNet(c["net"])
                if ni is None:
                    print("   找不到網路 %s" % c["net"])
                    continue
                p.SetNet(ni)
                print("   %s.%s  %s → %s" % (c["ref"], c["pad"], old, c["net"]))
            else:
                p.SetNetCode(0)
                print("   %s.%s  %s → 未接" % (c["ref"], c["pad"], old))
            touched.append(p.GetPosition())

    for t in b.GetTracks():
        if t.Type() == pcbnew.PCB_VIA_T:
            continue
        for q in touched:
            if t.GetStart() == q or t.GetEnd() == q:
                doomed.append(t)
                break
    for t in doomed:
        b.Remove(t)
    print("拆掉改過網路的焊盤上的走線 %d 段" % len(doomed))
    b.BuildConnectivity()
    pcbnew.SaveBoard(path, b)


if __name__ == "__main__":
    main()
