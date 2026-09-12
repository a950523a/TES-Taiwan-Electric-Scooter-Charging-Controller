#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""比對 .kicad_pcb 的焊盤網路和 V1.3 原始網表。由 pcb_import.py 呼叫。"""
import os, sys
import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sch_gen import read_netlist                           # noqa: E402


def main():
    b = pcbnew.LoadBoard(sys.argv[1])
    got, noref = {}, 0
    for f in b.GetFootprints():
        ref = f.GetReference()
        if not ref:
            noref += 1
            continue
        for p in f.Pads():
            if p.GetNetname():
                got[(ref, p.GetNumber())] = p.GetNetname()
    want = {(d, p): n for d, p, n in read_netlist()[0]}

    missing = sorted(k for k in want if k not in got)
    extra = sorted(k for k in got if k not in want)
    wrong = sorted(k for k in want if k in got and got[k] != want[k])
    print("  原始接線 %d 條，板上 %d 條" % (len(want), len(got)))
    for label, items in (("漏接", missing), ("接錯", wrong), ("多出來", extra)):
        print("  %-4s %d" % (label, len(items)))
        for k in items[:12]:
            print("      %-10s pad %-6s 應為 %-14s 實際 %s"
                  % (k[0], k[1], want.get(k, "-"), got.get(k, "(無)")))
    if noref:
        print("  仍有 %d 個封裝沒有位號" % noref)
    ok = not (missing or wrong or extra or noref)
    print("  → %s" % ("零差異，通過" if ok else "有差異，未通過"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
