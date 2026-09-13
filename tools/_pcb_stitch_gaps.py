#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""縫合同網路走線端點之間的微小縫隙。在 KiCad 的 python 裡跑。

重繞之後常留下 0.1–0.3 mm 的縫：兩段走線的端點差一點點沒對上。
柵格繞線器看不到（0.15 mm 網格把它抹平了），但 KiCad 的連通性引擎會，
於是 DRC 報「未連接」，而畫面上看起來明明是連著的。

只補「同一條網路、同一層、距離小於門檻」的端點對 —— 不會把不該接的東西接起來。
"""
import math, os, sys
import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _pcb_route as RT                                    # noqa: E402
from _pcb_restructure import WIDTH                         # noqa: E402

MAX_GAP = 0.2       # mm

# 只接「懸空」的端點：該座標在這條網路這一層上只出現一次，而且不在焊盤裡。
# 早期版本接了所有距離小於門檻的端點對，一次補了 207 段 ——
# 把只是彎折處剛好靠近的走線也串起來了。


def main():
    path = sys.argv[1]
    b = pcbnew.LoadBoard(path)
    import collections
    count = collections.Counter()
    for t in b.GetTracks():
        if t.Type() == pcbnew.PCB_VIA_T:
            count[(t.GetNetname(), t.GetPosition().x, t.GetPosition().y)] += 2
        else:
            for q in (t.GetStart(), t.GetEnd()):
                count[(t.GetNetname(), q.x, q.y)] += 1
    inpad = set()
    for f in b.GetFootprints():
        for pd in f.Pads():
            sh = pd.GetEffectiveShape(pcbnew.F_Cu)
            for (net, x, y), c in count.items():
                if net == pd.GetNetname() and sh.Collide(pcbnew.VECTOR2I(x, y), 0):
                    inpad.add((net, x, y))
    ends = {}
    for t in b.GetTracks():
        if t.Type() == pcbnew.PCB_VIA_T:
            continue
        for q in (t.GetStart(), t.GetEnd()):
            k = (t.GetNetname(), q.x, q.y)
            if count[k] == 1 and k not in inpad:
                ends.setdefault((t.GetNetname(), t.GetLayer()), []).append(q)
    added = 0
    for (net, layer), pts in sorted(ends.items()):
        if not net:
            continue
        obs = RT.obstacles(b, net)
        seen = set()
        for i in range(len(pts)):
            for j in range(i + 1, len(pts)):
                a, c = pts[i], pts[j]
                d = math.hypot(pcbnew.ToMM(a.x - c.x), pcbnew.ToMM(a.y - c.y))
                if d < 1e-6 or d > MAX_GAP:
                    continue
                key = ((a.x, a.y), (c.x, c.y))
                if key in seen:
                    continue
                seen.add(key)
                w = WIDTH.get(net, 0.2)
                if not RT.seg_ok(b, net, layer, a, c, w, obs):
                    continue
                RT.add_route(b, net, layer, [a, c], w)
                added += 1
    print("縫合微小縫隙 %d 段" % added)
    b.BuildConnectivity()
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    b.BuildConnectivity()
    pcbnew.SaveBoard(path, b)


if __name__ == "__main__":
    main()
