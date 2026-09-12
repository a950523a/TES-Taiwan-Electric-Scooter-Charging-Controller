#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把每個 GND 焊盤用過孔縫到底層地平面。在 KiCad 的 python 裡跑。

結構調整之後，頂層有 1300mm 走線，頂層的補地被切成二十幾塊孤島，
靠它連接 GND 焊盤只會每次重新鋪銅就斷掉一批。底層現在是一整片
5343 mm^2 的地平面，讓每個 GND 焊盤就近打孔下去才是穩定的做法。

順便也是好的設計：回流路徑最短，而不是在頂層繞過一堆走線去找地。
"""
import os, sys, math
import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _pcb_route as RT                                    # noqa: E402

NET = "GND"
VIA_DRILL = 0.3
VIA_DIA = 0.6


def has_via_near(b, pos, r_mm=1.6):
    r = pcbnew.FromMM(r_mm)
    for t in b.GetTracks():
        if t.Type() != pcbnew.PCB_VIA_T or t.GetNetname() != NET:
            continue
        d = t.GetPosition() - pos
        if d.x * d.x + d.y * d.y <= r * r:
            return True
    return False


def main():
    path = sys.argv[1]
    b = pcbnew.LoadBoard(path)
    plane = RT.plane_of(b, NET, pcbnew.B_Cu)
    if plane is None:
        raise SystemExit("底層找不到 GND 地平面")
    poly = plane.GetFilledPolysList(pcbnew.B_Cu)

    targets = []
    for f in b.GetFootprints():
        for p in f.Pads():
            if p.GetNetname() != NET or not p.IsOnLayer(pcbnew.F_Cu):
                continue
            if p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH:
                continue          # 通孔焊盤本來就穿到底層
            targets.append((f.GetReference(), p))

    obs = RT.obstacles(b, NET)
    added, skipped, failed = 0, 0, []
    for ref, p in targets:
        pos = p.GetPosition()
        if has_via_near(b, pos):
            skipped += 1
            continue
        done = False
        for r in (0.85, 1.1, 1.4, 1.7, 2.1, 2.6):
            for ang in range(0, 360, 20):
                vp = pcbnew.VECTOR2I(
                    pos.x + int(pcbnew.FromMM(r * math.cos(math.radians(ang)))),
                    pos.y + int(pcbnew.FromMM(r * math.sin(math.radians(ang)))))
                if not poly.Collide(vp, pcbnew.FromMM(VIA_DIA / 2 + 0.2)):
                    continue
                if not RT.seg_ok(b, NET, pcbnew.F_Cu, pos, vp, 0.35, obs):
                    continue
                if not RT.via_ok(b, NET, vp, VIA_DIA, VIA_DRILL, obs):
                    continue
                v = pcbnew.PCB_VIA(b)
                v.SetPosition(vp)
                v.SetDrill(pcbnew.FromMM(VIA_DRILL))
                v.SetWidth(pcbnew.FromMM(VIA_DIA))
                v.SetNet(b.FindNet(NET))
                b.Add(v)
                RT.add_route(b, NET, pcbnew.F_Cu, [pos, vp], 0.35)
                added += 1
                done = True
                break
            if done:
                break
        if not done:
            failed.append(ref)
    print("GND 焊盤 %d 個：新增過孔 %d、已有過孔 %d、無法縫 %d"
          % (len(targets), added, skipped, len(failed)))
    if failed:
        print("   無法縫：%s" % ", ".join(sorted(set(failed))[:12]))
    b.BuildConnectivity()
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    b.BuildConnectivity()
    pcbnew.SaveBoard(path, b)


if __name__ == "__main__":
    main()
