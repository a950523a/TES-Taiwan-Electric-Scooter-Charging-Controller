#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""結構調整：把底層清空給地平面，其餘網路全部改走頂層。在 KiCad 的 python 裡跑。

V1.3 把 120V(72mm)、12V(64mm)、三路閘極驅動、CAN、USB 共 649mm 都放在底層，
底層因此沒有完整地平面，只能靠頂層補地 —— 而頂層又要擺 62 個 SMD，
補出來的地被切成一堆孤島。症狀就是每次重新鋪銅都有電容的 GND 腳脫離。

可行的關鍵是降壓模組那塊 48.5 x 24 mm 保留區：它只限「高度」不限銅箔，
上方接 ESP32、下方接驅動與連接器，等於板子正中央一條沒有元件的走線通道。
以 0.4mm 節距算容得下約 120 條並行走線，而要搬的只有 18 條。

流程：拆掉底層非 GND 的走線 → 頂層用 A* 重繞 → 底層鋪成一整片地。
"""
import os, sys, math, collections
import numpy as np
import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _pcb_maze as MZ                                     # noqa: E402
import _pcb_route as RT                                    # noqa: E402

KEEP_ON_BOTTOM = {"GND"}


def rip_bottom(b):
    """拆掉底層所有非 GND 的走線，以及因此失去作用的過孔。

    先一次掃描決定要刪什麼、再統一刪除：b.Remove() 之後 b.GetTracks()
    就不能再迭代了（SwigPyObject is not iterable），邊走邊刪一定爆。
    """
    tracks, vias, ripped = [], [], collections.Counter()
    keep_bottom_nets = set()
    all_items = []
    for t in b.GetTracks():
        is_via = t.Type() == pcbnew.PCB_VIA_T
        all_items.append((t, is_via, t.GetLayer(), t.GetNetname()))
    for t, is_via, layer, net in all_items:
        if is_via:
            continue
        if layer == pcbnew.B_Cu:
            if net in KEEP_ON_BOTTOM:
                keep_bottom_nets.add(net)
            else:
                tracks.append(t)
                ripped[net] += 1
    for t, is_via, layer, net in all_items:
        if is_via and net not in keep_bottom_nets:
            vias.append(t)
    for t in tracks + vias:
        b.Remove(t)
    return ripped, len(vias)


def components(maze, net):
    """這條網路的銅箔目前分成幾塊。

    同層 8 連通；兩層之間只有在同一格上下都有這條網路的銅箔時才相連
    —— 那就是過孔或通孔焊盤的位置。
    """
    code = maze.net_ids.get(net)
    if code is None:
        return []
    mask = maze.grid == code            # (2, h, w)
    _, h, w = mask.shape
    seen = np.zeros_like(mask, dtype=bool)
    comps = []
    for li, r0, c0 in np.argwhere(mask):
        if seen[li, r0, c0]:
            continue
        stack = [(int(li), int(r0), int(c0))]
        seen[li, r0, c0] = True
        cells = []
        while stack:
            l, r, c = stack.pop()
            cells.append((l, r, c))
            for dr in (-1, 0, 1):
                for dc in (-1, 0, 1):
                    nr, nc = r + dr, c + dc
                    if 0 <= nr < h and 0 <= nc < w and mask[l, nr, nc]                             and not seen[l, nr, nc]:
                        seen[l, nr, nc] = True
                        stack.append((l, nr, nc))
            ol = 1 - l
            if mask[ol, r, c] and not seen[ol, r, c]:
                seen[ol, r, c] = True
                stack.append((ol, r, c))
        comps.append(cells)
    return comps


def centroid(cells):
    r = sum(c[1] for c in cells) / float(len(cells))
    c = sum(x[2] for x in cells) / float(len(cells))
    return (r, c)


def stamp_path(maze, net, runs, width):
    code = maze._code(net)
    n = MZ.cells(width / 2.0)
    for li, pts in runs:
        for i in range(len(pts) - 1):
            (r1, c1), (r2, c2) = pts[i], pts[i + 1]
            steps = max(abs(r2 - r1), abs(c2 - c1)) or 1
            for k in range(steps + 1):
                r = r1 + (r2 - r1) * k // steps
                c = c1 + (c2 - c1) * k // steps
                r0, rr = max(r - n, 0), min(r + n + 1, maze.h)
                c0, cc = max(c - n, 0), min(c + n + 1, maze.w)
                sub = maze.grid[li, r0:rr, c0:cc]
                sub[sub == 0] = code


def add_path(b, net, maze, runs, width):
    """把算好的路徑畫上去，換層處補過孔。"""
    ni = b.FindNet(net)
    made, length, vias = 0, 0.0, 0
    prev_end = None
    for li, cells in runs:
        layer = MZ.LAYERS[li]
        pts = [maze.to_point(rc) for rc in cells]
        if prev_end is not None:
            v = pcbnew.PCB_VIA(b)
            v.SetPosition(pts[0])
            v.SetDrill(pcbnew.FromMM(0.3))
            v.SetWidth(pcbnew.FromMM(0.6))
            v.SetNet(ni)
            b.Add(v)
            vias += 1
        for p, q in zip(pts, pts[1:]):
            if p == q:
                continue
            t = pcbnew.PCB_TRACK(b)
            t.SetStart(p)
            t.SetEnd(q)
            t.SetWidth(pcbnew.FromMM(width))
            t.SetLayer(layer)
            t.SetNet(ni)
            b.Add(t)
            made += 1
            length += pcbnew.ToMM(t.GetLength())
        prev_end = pts[-1]
    return made, length, vias


# 各網路的走線寬度。電源與高壓照 .kicad_dru 的下限，其餘用預設。
WIDTH = {"120V": 0.3, "GND_BACK": 0.3, "12V": 0.8, "5V": 0.5, "VDD33": 0.5,
         "VP": 0.6, "DC_RELAY": 0.6, "COUPLER": 0.6}
DEFAULT_WIDTH = 0.25


def verify_runs(b, net, maze, runs, width, obs):
    # 換層點也要當成過孔來驗（見 _pcb_route.via_ok）
    """用 KiCad 的精確幾何複驗每一段。回傳擦邊的格座標清單。

    柵格是近似的，A* 說可以不代表 DRC 會過。與其事後收拾 122 條違規，
    不如當場驗、當場把出問題的位置封鎖掉再繞一次。
    """
    bad = []
    for i, (li, cells_) in enumerate(runs):
        layer = MZ.LAYERS[li]
        pts = [maze.to_point(rc) for rc in cells_]
        if i > 0 and not RT.via_ok(b, net, pts[0], obs=obs):
            bad.append((li, cells_[0], cells_[0]))
        for (p, q), (rc1, rc2) in zip(zip(pts, pts[1:]),
                                      zip(cells_, cells_[1:])):
            if p == q:
                continue
            if not RT.seg_ok(b, net, layer, p, q, width, obs):
                bad.append((li, rc1, rc2))
    return bad


def block_between(blocked_extra, li, rc1, rc2, radius=1):
    """把一段走線經過的格子標成禁用，讓下一次 A* 繞開。

    過孔失敗時（rc1 == rc2）要封大一點：只封 1 格的話，下一次 A* 會把
    過孔往旁邊挪 0.3mm 再撞一次，八次嘗試都在原地打轉。孔位受限於
    孔對孔、孔對焊盤的距離，挪一點點沒有意義。
    """
    if rc1 == rc2:
        radius = max(radius, MZ.cells(1.2))
    (r1, c1), (r2, c2) = rc1, rc2
    steps = max(abs(r2 - r1), abs(c2 - c1)) or 1
    for k in range(steps + 1):
        r = r1 + (r2 - r1) * k // steps
        c = c1 + (c2 - c1) * k // steps
        for dr in range(-radius, radius + 1):
            for dc in range(-radius, radius + 1):
                rr, cc = r + dr, c + dc
                if 0 <= rr < blocked_extra.shape[1] and 0 <= cc < blocked_extra.shape[2]:
                    blocked_extra[li, rr, cc] = True


# 訊號線繞不過去時可以退讓的寬度。電源與高壓網路不在此列 ——
# 它們的寬度是載流與間距要求，不能為了繞得過去而變細。
NARROW = (0.2, 0.15)


def route_net(b, maze, net):
    """把這條網路散落的各塊銅箔接成一塊。回傳 (段數, 總長, 過孔數, 失敗數)。"""
    width = WIDTH.get(net, DEFAULT_WIDTH)
    widths = [width] if net in WIDTH else [width] + list(NARROW)
    made, length, vias, fail = 0, 0.0, 0, 0
    extra = np.zeros((2, maze.h, maze.w), dtype=bool)
    for _ in range(24):
        comps = components(maze, net)
        if len(comps) <= 1:
            break
        comps.sort(key=len, reverse=True)
        base, rest = comps[0], comps[1:]
        bc = centroid(base)
        rest.sort(key=lambda cc: math.hypot(centroid(cc)[0] - bc[0],
                                            centroid(cc)[1] - bc[1]))
        obs = RT.obstacles(b, net)
        runs = None
        base_blocked = maze.blocked_for(net, width)
        via_blocked = maze.via_blocked_for(net)
        # 先試「只走頂層」。板上已有上百個過孔，擁擠處常常找不到合法孔位，
        # 而多數連接（像 IO9 那種縱向直線）本來就不需要換層。
        # 只有頂層真的過不去時才開放底層。
        top_only = base_blocked.copy()
        top_only[1] = True
        used_width = width
        for w in widths:
            bw = maze.blocked_for(net, w)
            tw = bw.copy()
            tw[1] = True
            vbw = maze.via_blocked_for(net)
            for attempt in range(10):
                blocked = (tw if attempt < 3 else bw) | extra
                path = MZ.astar(blocked, set(rest[0]), set(base),
                                via_blocked=vbw | extra)
                if path is None:
                    break
                cand = MZ.simplify(path)
                bad = verify_runs(b, net, maze, cand, w, obs)
                if not bad:
                    runs, used_width = cand, w
                    break
                for li, rc1, rc2 in bad:
                    block_between(extra, li, rc1, rc2)
            if runs is not None:
                break
            extra[:] = False
        if runs is None:
            fail += 1
            break
        stamp_path(maze, net, runs, used_width)
        m, L, v = add_path(b, net, maze, runs, used_width)
        made += m
        length += L
        vias += v
    return made, length, vias, fail
