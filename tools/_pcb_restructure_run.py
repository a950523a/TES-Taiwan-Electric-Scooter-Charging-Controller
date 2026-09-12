#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""結構調整的完整流程。在 KiCad 的 python 裡跑，一次到底。

  1. 拆掉底層所有非 GND 的走線
  2. 用兩層 A* 重繞（底層成本 x4、過孔成本 12，所以會盡量待在頂層）
  3. 每個 GND 焊盤打過孔縫到底層地平面
  4. 收尾：柵格繞不過去的短缺口改用精確幾何直接連

第 4 步是必要的：柵格對高壓網路要膨脹 0.9mm，會把 U10 周邊那條
0.255mm 寬的可行通道整個抹掉 —— 實際幾何過得去，柵格過不去。

用法：python tools/_pcb_restructure_run.py <board.kicad_pcb>
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
KPY = sys.executable


def stage(name, code, board):
    with tempfile.NamedTemporaryFile("w", suffix=".py", delete=False,
                                     encoding="utf-8") as f:
        f.write("import sys, os\n"
                "sys.path.insert(0, %r)\n" % HERE + code)
        path = f.name
    try:
        env = dict(os.environ, PYTHONIOENCODING="utf-8")
        r = subprocess.run([KPY, path, board], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", env=env,
                           cwd=os.path.dirname(HERE))
        out = "\n".join(l for l in (r.stdout or "").splitlines()
                        if "memory leak" not in l and "image handler" not in l
                        and "destructor found" not in l)
        print("[%s]" % name)
        if out.strip():
            print(out)
        if r.returncode:
            sys.stderr.write(r.stderr or "")
            raise SystemExit("%s 失敗" % name)
    finally:
        os.unlink(path)


RIP = '''
import pcbnew, _pcb_restructure as RS
P = sys.argv[1]
b = pcbnew.LoadBoard(P)
ripped, vias = RS.rip_bottom(b)
print("拆除底層 %d 段（%d 條網路）+ %d 個過孔"
      % (sum(ripped.values()), len(ripped), vias))
b.BuildConnectivity()
pcbnew.SaveBoard(P, b)
open(P + ".nets", "w", encoding="utf-8").write(
    "\n".join(n for n, _ in ripped.most_common()))
'''

ROUTE = '''
import pcbnew, _pcb_restructure as RS, _pcb_maze as MZ
P = sys.argv[1]
b = pcbnew.LoadBoard(P)
maze = MZ.Maze(b)
tot = fails = 0
for n in [l.strip() for l in open(P + ".nets", encoding="utf-8") if l.strip()]:
    made, length, vias, fail = RS.route_net(b, maze, n)
    tot += length
    fails += fail
    if fail:
        print("   %-12s 未完成（剩餘分塊 %d）" % (n, len(RS.components(maze, n))))
print("重繞 %.0f mm，未完成 %d 條" % (tot, fails))
b.BuildConnectivity()
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.BuildConnectivity()
pcbnew.SaveBoard(P, b)
'''

CLOSE = '''
import pcbnew, _pcb_restructure as RS, _pcb_maze as MZ, _pcb_route as RT
P = sys.argv[1]
b = pcbnew.LoadBoard(P)
maze = MZ.Maze(b)
pads = {}
for f in b.GetFootprints():
    for p in f.Pads():
        pads.setdefault(p.GetNetname(), []).append(p)
closed = 0
for net in sorted(set(pads)):
    if not net:
        continue
    for _ in range(3):
        if len(RS.components(maze, net)) <= 1:
            break
        ps = pads[net]
        done = False
        for i in range(len(ps)):
            for j in range(i + 1, len(ps)):
                r = RT.find_route(b, net, ps[i].GetPosition(),
                                  ps[j].GetPosition(),
                                  RS.WIDTH.get(net, RS.DEFAULT_WIDTH))
                if r:
                    lay, pts, length = r
                    RT.add_route(b, net, lay, pts,
                                 RS.WIDTH.get(net, RS.DEFAULT_WIDTH),
                                 via_at_start=True)
                    print("   %-12s 補 %.2f mm (%s)"
                          % (net, length, b.GetLayerName(lay)))
                    closed += 1
                    done = True
                    break
            if done:
                break
        if not done:
            break
        maze = MZ.Maze(b)
print("收尾補線 %d 段" % closed)
b.BuildConnectivity()
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.BuildConnectivity()
pcbnew.SaveBoard(P, b)
'''


def main():
    board = sys.argv[1]
    stage("1/4 拆除底層", RIP, board)
    stage("2/4 兩層重繞", ROUTE, board)
    subprocess.run([KPY, os.path.join(HERE, "_pcb_gnd_stitch.py"), board],
                   check=False)
    stage("4/4 收尾補線", CLOSE, board)
    n = board + ".nets"
    if os.path.exists(n):
        os.unlink(n)


if __name__ == "__main__":
    main()
