#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""照 DRC 報告清掉懸空走線／過孔與重疊鑽孔。在 KiCad 的 python 裡跑。

用 KiCad 自己的判定，不要自己寫啟發式：先前用「端點只出現一次」判斷懸空，
刪掉 21 段之後斷了 7 條連接 —— 端點不在焊盤中心但仍落在焊盤範圍內的情況
那個規則看不出來。

重疊鑽孔的來源是繞線時 via_at_start 在路徑起點放過孔，而起點正好是通孔焊盤，
等於在同一個位置鑽兩次。

用法：python tools/_pcb_tidy.py <board> <drc.json>
"""
import io, json, os, re, sys
import pcbnew

TOL = pcbnew.FromMM(0.01)


def near(a, bx, by):
    return abs(a.x - bx) <= TOL and abs(a.y - by) <= TOL


def main():
    path, report = sys.argv[1], sys.argv[2]
    d = json.load(io.open(report, encoding="utf-8"))
    # 只比對座標會誤刪：同一個端點上往往還接著一段正常的走線。
    # 報告裡有長度，一起比對才能鎖定就是那一段。
    dangling, holes = [], []
    for v in d["violations"]:
        if v["type"] in ("track_dangling", "via_dangling"):
            for it in v.get("items", []):
                p = it.get("pos")
                if not p:
                    continue
                m = re.search(r"length ([0-9.]+) mm", it.get("description", ""))
                dangling.append((pcbnew.FromMM(p["x"]), pcbnew.FromMM(p["y"]),
                                 float(m.group(1)) if m else None))
        elif v["type"] == "holes_co_located":
            for it in v.get("items", []):
                if it.get("description", "").startswith("Via"):
                    p = it["pos"]
                    holes.append((pcbnew.FromMM(p["x"]), pcbnew.FromMM(p["y"])))

    b = pcbnew.LoadBoard(path)
    doomed = []
    for t in b.GetTracks():
        if t.Type() == pcbnew.PCB_VIA_T:
            for hx, hy in holes + [(x, y) for x, y, _ in dangling]:
                if near(t.GetPosition(), hx, hy):
                    doomed.append(t)
                    break
        else:
            L = pcbnew.ToMM(t.GetLength())
            for hx, hy, dl in dangling:
                if dl is None or abs(L - dl) > 0.002:
                    continue
                if near(t.GetStart(), hx, hy) or near(t.GetEnd(), hx, hy):
                    doomed.append(t)
                    break
    seen, uniq = set(), []
    for t in doomed:
        k = id(t)
        if k not in seen:
            seen.add(k)
            uniq.append(t)
    print("清掉 %d 個（懸空 %d 處、重疊鑽孔 %d 處）"
          % (len(uniq), len(dangling), len(holes)))
    for t in uniq:
        b.Remove(t)
    b.BuildConnectivity()
    pcbnew.SaveBoard(path, b)


if __name__ == "__main__":
    main()
