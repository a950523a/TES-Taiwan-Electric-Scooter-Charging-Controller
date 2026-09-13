#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""收尾：把柵格繞不通的網路用精確幾何接起來。在 KiCad 的 python 裡跑。

柵格對窄出口無能為力 —— MSOP-10 腳距 0.5mm，膨脹之後焊盤四周全是障礙，
但實體上從焊盤外端出線是可行的。這裡改用 KiCad 的精確幾何直接試路徑。

重點是**跨分塊**挑焊盤：早期版本隨便挑兩個焊盤，結果一直在同一塊裡面
重複加同一條線，分塊數永遠降不下來。
"""
import math, os, sys
import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _pcb_maze as MZ                                     # noqa: E402
import _pcb_restructure as RS                              # noqa: E402
import _pcb_route as RT                                    # noqa: E402

WIDTHS = (None, 0.2, 0.15)      # None = 該網路的標準寬度


def pad_component(maze, comps, pad):
    """這個焊盤屬於哪一塊。"""
    r, c = maze.to_cell(pad.GetPosition())
    for i, cells in enumerate(comps):
        for li, rr, cc in cells:
            if abs(rr - r) <= 2 and abs(cc - c) <= 2:
                return i
    return None


def has_plane(b, net):
    """這條網路有沒有大面積鋪銅。

    有的話就不能靠柵格判斷連通性 —— 柵格不含鋪銅，會把靠平面相連的
    焊盤誤判成一堆分塊，然後在底層拉出幾十 mm 的長線把地平面切開。
    """
    return RT.plane_of(b, net) is not None


def close_net(b, maze, net, verbose=True):
    """回傳 (接上的段數, 是否完全連通)。"""
    pads = []
    for f in b.GetFootprints():
        for p in f.Pads():
            if p.GetNetname() == net:
                pads.append((f.GetReference(), p))
    made = 0
    for _ in range(6):
        comps = RS.components(maze, net)
        if len(comps) <= 1:
            return made, True
        owner = {}
        for ref, p in pads:
            i = pad_component(maze, comps, p)
            if i is not None:
                owner.setdefault(i, []).append((ref, p))
        if len(owner) < 2:
            return made, False      # 有分塊上完全沒有焊盤，接不了
        keys = sorted(owner)
        base = keys[0]
        std = RS.WIDTH.get(net, RS.DEFAULT_WIDTH)

        # 候選配對依距離排序。早期版本挑「第一個繞得通的」，
        # 結果把 START1.2 接到 12.7 mm 外的 U1.35，
        # 而 START1.1 就在隔壁 1 mm、且早就連著 U1.35。
        cand = []
        for other in keys[1:]:
            for ra, pa in owner[base]:
                for rb, pb in owner[other]:
                    d = math.hypot(
                        pcbnew.ToMM(pa.GetPosition().x - pb.GetPosition().x),
                        pcbnew.ToMM(pa.GetPosition().y - pb.GetPosition().y))
                    cand.append((d, ra, pa, rb, pb))
        cand.sort(key=lambda t: t[0])

        hit = False
        for _d, ra, pa, rb, pb in cand:
            for w in WIDTHS:
                ww = std if w is None else w
                r = RT.find_route(b, net, pa.GetPosition(), pb.GetPosition(), ww)
                if not r:
                    continue
                lay, pts, length = r
                RT.add_route(b, net, lay, pts, ww, via_at_start=True)
                if verbose:
                    print("   %-6s %s.%s → %s.%s 寬 %.2f  %s %.2f mm"
                          % (net, ra, pa.GetNumber(), rb, pb.GetNumber(), ww,
                             b.GetLayerName(lay), length))
                made += 1
                hit = True
                break
            if hit:
                break
        if not hit:
            return made, False
        maze = MZ.Maze(b)
    return made, len(RS.components(maze, net)) <= 1


def main():
    path = sys.argv[1]
    nets = sys.argv[2:] or None
    b = pcbnew.LoadBoard(path)
    maze = MZ.Maze(b)
    if nets is None:
        nets = sorted({p.GetNetname() for f in b.GetFootprints()
                       for p in f.Pads() if p.GetNetname()})
    total, stuck = 0, []
    for n in nets:
        if has_plane(b, n):
            continue        # GND 靠地平面相連，交給 _pcb_gnd_stitch.py
        if len(RS.components(maze, n)) <= 1:
            continue
        made, ok = close_net(b, maze, n)
        total += made
        if not ok:
            stuck.append(n)
        maze = MZ.Maze(b)
    print("收尾補線 %d 段" % total)
    if stuck:
        print("仍未連通：%s" % ", ".join(stuck))
    b.BuildConnectivity()
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    b.BuildConnectivity()
    pcbnew.SaveBoard(path, b)


if __name__ == "__main__":
    main()
