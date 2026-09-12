#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""網格迷宮繞線器（A*）。在 KiCad 的 python 裡跑。

為什麼需要：把底層 649mm 的走線搬到頂層，L 形／Z 形那種候選路徑完全不夠用 ——
頂層已經有 62 個 SMD 和 771mm 的走線，路徑必須真的會轉彎閃避。

作法是把銅箔柵格化成 numpy 陣列（每格存網路編號），對要繞的網路把
「其他網路的銅箔」按所需間距膨脹成障礙，再跑 A*。間距沿用 .kicad_dru：
一般 0.15mm，120V 0.5mm。

只繞頂層。底層要整片留給地平面 —— 那正是這次結構調整的目的。
"""
import heapq, math
import numpy as np
import pcbnew

GRID = 0.15         # mm，走線 0.25mm + 間距 0.15mm 需要 0.4mm 節距
# 底層每走一格的成本倍率，以及換層打過孔的成本（以格為單位）。
# 目的不是「底層完全沒有走線」—— 那對這塊板子不現實 ——
# 而是讓底層只剩短跳線，不出現切開地平面的長走線。
BOTTOM_COST = 4.0
VIA_COST = 12.0
EDGE_KEEP = 0.3     # 銅箔離板邊
HV_NETS = {"120V"}
HV_CLEAR = 0.5
DEF_CLEAR = 0.15


def clearance_for(net_a, net_b):
    if net_a in HV_NETS or net_b in HV_NETS:
        return HV_CLEAR
    return DEF_CLEAR


LAYERS = (pcbnew.F_Cu, pcbnew.B_Cu)


class Maze(object):
    def __init__(self, b):
        self.b = b
        e = b.GetBoardEdgesBoundingBox()
        self.x0 = pcbnew.ToMM(e.GetLeft())
        self.y0 = pcbnew.ToMM(e.GetTop())
        self.w = int(math.ceil(pcbnew.ToMM(e.GetWidth()) / GRID)) + 1
        self.h = int(math.ceil(pcbnew.ToMM(e.GetHeight()) / GRID)) + 1
        self.net_ids = {}
        # 兩層各一張圖：index 0 = 頂層，1 = 底層
        self.grid = np.zeros((2, self.h, self.w), dtype=np.int32)
        self._rasterize()

    # ---------------------------------------------------------- 座標
    def to_cell(self, p):
        return (int(round((pcbnew.ToMM(p.y) - self.y0) / GRID)),
                int(round((pcbnew.ToMM(p.x) - self.x0) / GRID)))

    def to_point(self, rc):
        r, c = rc
        return pcbnew.VECTOR2I_MM(self.x0 + c * GRID, self.y0 + r * GRID)

    # ---------------------------------------------------------- 柵格化
    def _code(self, net):
        if net not in self.net_ids:
            self.net_ids[net] = len(self.net_ids) + 1
        return self.net_ids[net]

    def _stamp(self, li, shape, code, grow_mm=0.0):
        bb = shape.BBox()
        g = pcbnew.FromMM(grow_mm)
        r0, c0 = self.to_cell(pcbnew.VECTOR2I(bb.GetLeft() - g, bb.GetTop() - g))
        r1, c1 = self.to_cell(pcbnew.VECTOR2I(bb.GetRight() + g, bb.GetBottom() + g))
        r0 = max(r0, 0); c0 = max(c0, 0)
        r1 = min(r1, self.h - 1); c1 = min(c1, self.w - 1)
        rad = pcbnew.FromMM(GRID / 2.0 + grow_mm)
        for r in range(r0, r1 + 1):
            for c in range(c0, c1 + 1):
                if self.grid[li, r, c]:
                    continue
                if shape.Collide(self.to_point((r, c)), rad):
                    self.grid[li, r, c] = code

    def _rasterize(self):
        for li, layer in enumerate(LAYERS):
            for f in self.b.GetFootprints():
                for p in f.Pads():
                    if p.IsOnLayer(layer):
                        self._stamp(li, p.GetEffectiveShape(layer),
                                    self._code(p.GetNetname() or chr(0)))
            for t in self.b.GetTracks():
                if t.Type() == pcbnew.PCB_VIA_T or t.GetLayer() == layer:
                    self._stamp(li, t.GetEffectiveShape(layer),
                                self._code(t.GetNetname() or chr(0)))

    # ---------------------------------------------------------- 障礙
    def blocked_for(self, net, width):
        """這條網路看到的障礙圖（兩層）：其他網路的銅箔按所需間距膨脹。"""
        return np.stack([self._blocked_layer(li, net, width) for li in (0, 1)])

    def _blocked_layer(self, li, net, width):
        g = self.grid[li]
        mine = self.net_ids.get(net, -1)
        others = (g != 0) & (g != mine)
        # 間距依網路配對而異，保守起見統一用最大值；
        # 120V 自己繞時用 HV，其他網路遇到 120V 也要 HV。
        pad = DEF_CLEAR
        if net in HV_NETS:
            pad = HV_CLEAR
        elif any(n in HV_NETS for n in self.net_ids):
            hv_mask = np.zeros_like(others)
            for n in HV_NETS:
                if n in self.net_ids:
                    hv_mask |= (g == self.net_ids[n])
            others = dilate(others, cells(DEF_CLEAR + width / 2.0)) | \
                dilate(hv_mask, cells(HV_CLEAR + width / 2.0))
            return self._edge_block(others)
        return self._edge_block(dilate(others, cells(pad + width / 2.0)))

    def _edge_block(self, mask):
        k = cells(EDGE_KEEP)
        if k:
            mask[:k, :] = True
            mask[-k:, :] = True
            mask[:, :k] = True
            mask[:, -k:] = True
        return mask


# 柵格量化餘裕：格心取樣 + 斜向步進會讓實際銅箔比格子外擴，
# 不補這一點的話 A* 算出來的路徑在 KiCad 的精確幾何下會擦邊。
QUANT = GRID * 0.75


def cells(mm):
    return int(math.ceil((mm + QUANT) / GRID))


def dilate(mask, n):
    """用連續的位移取 OR 近似圓形膨脹。numpy 沒有 scipy 的 binary_dilation。"""
    out = mask.copy()
    for _ in range(max(n, 0)):
        nxt = out.copy()
        nxt[1:, :] |= out[:-1, :]
        nxt[:-1, :] |= out[1:, :]
        nxt[:, 1:] |= out[:, :-1]
        nxt[:, :-1] |= out[:, 1:]
        out = nxt
    return out


NEI = [(-1, 0, 1.0), (1, 0, 1.0), (0, -1, 1.0), (0, 1, 1.0),
       (-1, -1, 1.414), (-1, 1, 1.414), (1, -1, 1.414), (1, 1, 1.414)]


def astar(blocked, starts, goals, bend_cost=0.6):
    """兩層 A*。節點是 (層, row, col)，層 0 = 頂層、1 = 底層。

    底層每一步乘 BOTTOM_COST、換層加 VIA_COST，所以路徑會盡量待在頂層，
    只有繞不過去時才短暫下到底層 —— 底層因此只剩短跳線，地平面保持完整。
    """
    _, h, w = blocked.shape
    goal = set(goals)
    if not goal or not starts:
        return None
    gr = np.array([g[1] for g in goal], dtype=np.float64)
    gc = np.array([g[2] for g in goal], dtype=np.float64)

    def hx(r, c):
        return float(np.min(np.hypot(gr - r, gc - c)))

    pq = []
    for s0 in starts:
        if 0 <= s0[1] < h and 0 <= s0[2] < w:
            heapq.heappush(pq, (hx(s0[1], s0[2]), 0.0, s0, None, None))
    seen = {}
    while pq:
        f, g, node, prev, pdir = heapq.heappop(pq)
        if node in seen and seen[node][0] <= g:
            continue
        seen[node] = (g, prev)
        if node in goal:
            path = [node]
            while prev is not None:
                path.append(prev)
                prev = seen[prev][1]
            path.reverse()
            return path
        li, r, c = node
        mult = 1.0 if li == 0 else BOTTOM_COST
        for dr, dc, cost in NEI:
            nr, nc = r + dr, c + dc
            if not (0 <= nr < h and 0 <= nc < w):
                continue
            if blocked[li, nr, nc] and (li, nr, nc) not in goal:
                continue
            ng = g + cost * mult + (bend_cost if pdir and pdir != (dr, dc) else 0.0)
            nxt = (li, nr, nc)
            if nxt in seen and seen[nxt][0] <= ng:
                continue
            heapq.heappush(pq, (ng + hx(nr, nc), ng, nxt, node, (dr, dc)))
        # 換層
        ol = 1 - li
        if not blocked[ol, r, c] or (ol, r, c) in goal:
            ng = g + VIA_COST
            nxt = (ol, r, c)
            if not (nxt in seen and seen[nxt][0] <= ng):
                heapq.heappush(pq, (ng + hx(r, c), ng, nxt, node, None))
    return None


def simplify(path):
    """把連續同方向、同層的格子併成一段，換層處保留成過孔點。

    回傳 [(層, [(r,c), ...]), ...]，每個元素是同一層上的一段折線。
    """
    if not path:
        return []
    runs, cur = [], [path[0]]
    for p in path[1:]:
        if p[0] != cur[-1][0]:
            runs.append((cur[0][0], cur))
            cur = [p]
        else:
            cur.append(p)
    runs.append((cur[0][0], cur))
    out = []
    for li, pts in runs:
        keep = [pts[0]]
        for i in range(1, len(pts) - 1):
            a, b, c = pts[i - 1], pts[i], pts[i + 1]
            if (b[1] - a[1], b[2] - a[2]) != (c[1] - b[1], c[2] - b[2]):
                keep.append(b)
        if len(pts) > 1:
            keep.append(pts[-1])
        out.append((li, [(p[1], p[2]) for p in keep]))
    return out
