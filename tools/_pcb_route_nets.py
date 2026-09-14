#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""用迷宮繞線器把指定網路繞完。在 KiCad 的 python 裡跑。

    "C:/Program Files/KiCad/10.0/bin/python.exe" tools/_pcb_route_nets.py <board> SDA SCL

為什麼需要：_pcb_route.py 只試幾條 L 形／Z 形候選路徑，在 ADS1115 那種
密集區完全不夠 —— U13 的 SDA/SCL 就是這樣繞不出去的。_pcb_repair.py 用的
也是同一個繞線器，所以它也修不動。這支改用 _pcb_maze 的 A*，和當初把
底層走線搬到頂層時用的是同一套。

GND 這種有完整平面的網路不在這裡處理，交給就近打過孔。
"""
import os, sys
sys.path.insert(0, os.path.abspath("tools"))
import pcbnew                                              # noqa: E402
import _pcb_maze as MZ                                     # noqa: E402
import _pcb_restructure as RS                              # noqa: E402
import _pcb_route as RT                                    # noqa: E402


def main():
    path = sys.argv[1]
    nets = sys.argv[2:]
    if not nets:
        raise SystemExit("要繞哪些網路？")
    b = pcbnew.LoadBoard(path)
    maze = MZ.Maze(b)
    changed = False
    for net in nets:
        if RT.plane_of(b, net) is not None:
            print("   %-10s 有完整平面，跳過（交給過孔縫合）" % net)
            continue
        parts = RS.components(maze, net)
        if len(parts) <= 1:
            print("   %-10s 已經連通" % net)
            continue
        made, length, vias, fail = RS.route_net(b, maze, net)
        changed = changed or bool(made)
        print("   %-10s %d 段 %.2f mm 過孔 %d %s"
              % (net, made, length, vias, "** 未完成 **" if fail else "OK"))
    if changed:
        b.BuildConnectivity()
        pcbnew.ZONE_FILLER(b).Fill(b.Zones())
        b.BuildConnectivity()
        pcbnew.SaveBoard(path, b)
        print("已存")
    else:
        print("沒有變動，不存檔")


if __name__ == "__main__":
    main()
