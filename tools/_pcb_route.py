#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把未連接的焊盤接上同網路的既有銅箔。在 KiCad 的 python 裡跑。

不是隨便畫一條線就算：每條候選路徑的每一段都會對「所有其他網路的銅箔」
做間距檢查，過不了就換下一條。檢查用的是 KiCad 自己的幾何引擎
（EffectiveShape / GetClearance），和 DRC 看到的是同一套幾何。

候選路徑由簡到繁：直線 → 兩種 L 形 → 帶中繼點的 Z 形。
全部走頂層，因為底層要留給完整的地平面。
"""
import math, os, sys
import pcbnew

HV_NETS = {"120V"}
HV_CLEAR = 0.5          # 見 TES_Controller.kicad_dru 的 IPC-2221B 依據
DEF_CLEAR = 0.15        # 一般網路，沿用 V1.3 的 6 mil
EDGE_CLEAR = 0.3


def required_clearance(net_a, net_b):
    if net_a in HV_NETS or net_b in HV_NETS:
        return HV_CLEAR
    return DEF_CLEAR


def obstacles(b, net, include_zones=False):
    """所有「不是這條網路」的銅箔，依層分組。

    預設「不」把鋪銅算成障礙：鋪銅遇到走線會自動退讓，
    把它當障礙的話，凡是被大地平面覆蓋的區域就一條線都繞不出來。
    繞完重新鋪銅，再讓 DRC 檢查退讓後是否真的夠。
    """
    out = {pcbnew.F_Cu: [], pcbnew.B_Cu: []}
    for t in b.GetTracks():
        if t.GetNetname() == net:
            continue
        if t.Type() == pcbnew.PCB_VIA_T:
            for l in (pcbnew.F_Cu, pcbnew.B_Cu):
                out[l].append((t.GetNetname(), t.GetEffectiveShape(l)))
        elif t.GetLayer() in out:
            out[t.GetLayer()].append((t.GetNetname(),
                                      t.GetEffectiveShape(t.GetLayer())))
    for f in b.GetFootprints():
        for p in f.Pads():
            if p.GetNetname() == net:
                continue
            for l in (pcbnew.F_Cu, pcbnew.B_Cu):
                if p.IsOnLayer(l):
                    out[l].append((p.GetNetname(), p.GetEffectiveShape(l)))
    for z in b.Zones() if include_zones else ():
        if z.GetNetname() == net:
            continue
        for l in z.GetLayerSet().Seq():
            if l in out:
                poly = z.GetFilledPolysList(l)
                if poly and poly.OutlineCount():
                    out[l].append((z.GetNetname(), poly))
    return out


def seg_ok(b, net, layer, a, c, width, obs):
    """一段走線和所有其他網路的銅箔是否都保持足夠間距。"""
    t = pcbnew.PCB_TRACK(b)
    t.SetStart(a)
    t.SetEnd(c)
    t.SetWidth(pcbnew.FromMM(width))
    t.SetLayer(layer)
    sh = t.GetEffectiveShape(layer)
    for other_net, osh in obs[layer]:
        need = pcbnew.FromMM(required_clearance(net, other_net))
        if sh.Collide(osh, need):
            return False
    # 離板邊
    e = b.GetBoardEdgesBoundingBox()
    m = pcbnew.FromMM(EDGE_CLEAR) + t.GetWidth() // 2
    for p in (a, c):
        if not (e.GetLeft() + m < p.x < e.GetRight() - m
                and e.GetTop() + m < p.y < e.GetBottom() - m):
            return False
    return True


HOLE_CLEAR = 0.25       # 孔到孔、孔到銅箔的最小距離


def via_ok(b, net, pos, dia=0.6, drill=0.3, obs=None):
    """這個位置放過孔會不會違規。

    只檢查走線而不檢查過孔，是 GND 縫合一次加了 55 個過孔卻多出
    36 條間距違規的原因 —— 過孔的銅環和鑽孔都要算。
    """
    if obs is None:
        obs = obstacles(b, net)
    v = pcbnew.PCB_VIA(b)
    v.SetPosition(pos)
    v.SetDrill(pcbnew.FromMM(drill))
    v.SetWidth(pcbnew.FromMM(dia))
    for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
        sh = v.GetEffectiveShape(layer)
        for other_net, osh in obs[layer]:
            need = pcbnew.FromMM(required_clearance(net, other_net))
            if sh.Collide(osh, need):
                return False
    # 孔到孔
    r = pcbnew.FromMM(drill / 2.0 + HOLE_CLEAR)
    for t in b.GetTracks():
        if t.Type() != pcbnew.PCB_VIA_T:
            continue
        d = t.GetPosition() - pos
        need = r + t.GetDrill() // 2
        if d.x * d.x + d.y * d.y < need * need:
            return False
    for f in b.GetFootprints():
        for p in f.Pads():
            if p.GetDrillSize().x <= 0:
                continue
            d = p.GetPosition() - pos
            need = r + p.GetDrillSize().x // 2
            if d.x * d.x + d.y * d.y < need * need:
                return False
    # 離板邊
    e = b.GetBoardEdgesBoundingBox()
    m = pcbnew.FromMM(EDGE_CLEAR + dia / 2.0)
    if not (e.GetLeft() + m < pos.x < e.GetRight() - m
            and e.GetTop() + m < pos.y < e.GetBottom() - m):
        return False
    return True


def paths(a, c):
    """候選路徑（點串）。由簡到繁。"""
    yield [a, c]
    yield [a, pcbnew.VECTOR2I(c.x, a.y), c]
    yield [a, pcbnew.VECTOR2I(a.x, c.y), c]
    for off in (1.27, 2.54, -1.27, -2.54, 3.81, -3.81, 5.08, -5.08):
        d = pcbnew.FromMM(off)
        yield [a, pcbnew.VECTOR2I(a.x + d, a.y),
               pcbnew.VECTOR2I(a.x + d, c.y), c]
        yield [a, pcbnew.VECTOR2I(a.x, a.y + d),
               pcbnew.VECTOR2I(c.x, a.y + d), c]


def find_route(b, net, start, target, width, layers=(pcbnew.F_Cu, pcbnew.B_Cu),
               obs=None):
    """算出可行路徑但**不**畫上去。回傳 (layer, 點串, 長度) 或 None。

    先算再畫的原因：擺放時要反覆試繞，畫了再收回去很容易誤刪既有走線
    —— SWIG 的物件識別不可靠，沒辦法穩當地分辨「剛才加的」和「本來就有的」。
    """
    if obs is None:
        obs = obstacles(b, net)
    for layer in layers:
        for pts in paths(start, target):
            segs = [(p, q) for p, q in zip(pts, pts[1:]) if p != q]
            if segs and all(seg_ok(b, net, layer, p, q, width, obs)
                            for p, q in segs):
                length = sum(math.hypot(pcbnew.ToMM(q.x - p.x),
                                        pcbnew.ToMM(q.y - p.y))
                             for p, q in segs)
                return layer, pts, length
    return None


def add_route(b, net, layer, pts, width, via_at_start=False):
    """把算好的路徑畫上去。

    走底層時**兩端**都要打過孔 —— 起點是頂層的焊盤，終點是頂層的既有走線。
    只打起點那一個的話，終點會留下「頂層走線和底層走線互不相連」，
    DRC 會回報未連接，但看圖很像已經接好了。
    """
    ni = b.FindNet(net)
    made = 0
    if via_at_start and layer != pcbnew.F_Cu:
        for at in (pts[0], pts[-1]):
            v = pcbnew.PCB_VIA(b)
            v.SetPosition(at)
            v.SetDrill(pcbnew.FromMM(0.3))
            v.SetWidth(pcbnew.FromMM(0.6))
            v.SetNet(ni)
            b.Add(v)
            made += 1
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
    return made


def route(b, net, start, target, width, layer=pcbnew.F_Cu):
    """相容用法：算好就直接畫。"""
    r = find_route(b, net, start, target, width, layers=(layer,))
    if r is None:
        return None, 0
    lay, pts, length = r
    return length, add_route(b, net, lay, pts, width)


def plane_of(b, net, layer=pcbnew.B_Cu, min_area_mm2=100.0):
    """這條網路在該層有沒有大面積鋪銅。有的話就不必拉長線去找走線端點。"""
    for z in b.Zones():
        if z.GetNetname() != net or not z.IsOnLayer(layer):
            continue
        poly = z.GetFilledPolysList(layer)
        if poly and poly.Area() / 1e12 >= min_area_mm2:
            return z
    return None


def via_to_plane(b, net, pad_pos, width=0.4, layer=pcbnew.B_Cu, obs=None):
    """在焊盤旁打一個過孔接到地平面。

    接 GND 這種有完整平面的網路時，正確做法是就近打孔下去，
    不是在平面上方拉一條十幾 mm 的走線去找某個端點 ——
    那條線本身還會把平面切開。
    """
    z = plane_of(b, net, layer)
    if z is None:
        return None
    if obs is None:
        obs = obstacles(b, net)
    poly = z.GetFilledPolysList(layer)
    for r in (0.9, 1.27, 1.6, 2.0, 2.54, 3.2):
        for ang in range(0, 360, 30):
            dx = pcbnew.FromMM(r * math.cos(math.radians(ang)))
            dy = pcbnew.FromMM(r * math.sin(math.radians(ang)))
            vp = pcbnew.VECTOR2I(pad_pos.x + int(dx), pad_pos.y + int(dy))
            if not poly.Collide(vp, pcbnew.FromMM(0.35)):
                continue          # 過孔要真的落在鋪銅裡
            if not seg_ok(b, net, pcbnew.F_Cu, pad_pos, vp, width, obs):
                continue
            v = pcbnew.PCB_VIA(b)
            v.SetPosition(vp)
            v.SetDrill(pcbnew.FromMM(0.3))
            v.SetWidth(pcbnew.FromMM(0.6))
            v.SetNet(b.FindNet(net))
            b.Add(v)
            add_route(b, net, pcbnew.F_Cu, [pad_pos, vp], width)
            return math.hypot(pcbnew.ToMM(dx), pcbnew.ToMM(dy))
    return None


def nearest_same_net(b, net, pos, exclude_pad=None):
    """同網路最近的銅箔落點（走線端點或焊盤中心）。"""
    best = None
    for t in b.GetTracks():
        if t.GetNetname() != net or t.Type() == pcbnew.PCB_VIA_T:
            continue
        for q in (t.GetStart(), t.GetEnd()):
            d = math.hypot(q.x - pos.x, q.y - pos.y)
            if d > pcbnew.FromMM(0.05) and (best is None or d < best[0]):
                best = (d, q)
    for f in b.GetFootprints():
        for p in f.Pads():
            if p.GetNetname() != net or p is exclude_pad:
                continue
            q = p.GetPosition()
            if q == pos:
                continue
            d = math.hypot(q.x - pos.x, q.y - pos.y)
            if d > pcbnew.FromMM(0.05) and (best is None or d < best[0]):
                best = (d, q)
    return best[1] if best else None
