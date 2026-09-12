#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 changes_v13 宣告的變更套到 .kicad_pcb 上。在 KiCad 的 python 裡跑。

擺放不是隨便找個座標寫死：候選點依序試，每個點都檢查
  1) 外框有沒有和既有元件的 courtyard 重疊
  2) 有沒有超出板框（留 1mm）
  3) 有沒有壓到模組的高度禁置區（元件會被壓在降壓模組底下）
第一個通過的就用。這樣改了旁邊的東西之後重跑，還是會找到合法位置。
"""
import math, os, sys
import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import changes_v13                                         # noqa: E402
import _pcb_route as R                                      # noqa: E402

LIB = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "hardware", "kicad", "lib", "TES.pretty")
# 降壓模組的高度禁置區（PCB 座標，板框左上為 (120,60)）
KEEPOUT = (127.86, 100.22, 176.46, 124.22)
MARGIN = 1.0


def board_pos(b, ref, num=None):
    for f in b.GetFootprints():
        if f.GetReference() == ref:
            if num is None:
                return f.GetPosition()
            for p in f.Pads():
                if p.GetNumber() == num:
                    return p.GetPosition()
    return None


def overlaps(b, fp, skip=()):
    bb = fp.GetCourtyard(pcbnew.F_Cu).BBox()
    if bb.GetWidth() == 0:
        bb = fp.GetBoundingBox(False, False)
    for f in b.GetFootprints():
        # 不能用 `f is fp`：SWIG 每次迭代都包一個新的 Python 物件出來，
        # 同一個 C++ footprint 也會比對失敗，結果變成自己撞自己。
        if f.GetReference() == fp.GetReference() or f.GetReference() in skip:
            continue
        ob = f.GetCourtyard(pcbnew.F_Cu).BBox()
        if ob.GetWidth() == 0:
            ob = f.GetBoundingBox(False, False)
        if bb.Intersects(ob):
            return f.GetReference()
    return None


def pads_too_close(b, fp):
    """這顆元件的焊盤有沒有違反間距（含 .kicad_dru 的高壓規則）。"""
    for p in fp.Pads():
        net = p.GetNetname()
        obs = R.obstacles(b, net)
        for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
            if not p.IsOnLayer(layer):
                continue
            sh = p.GetEffectiveShape(layer)
            for other_net, osh in obs[layer]:
                need = pcbnew.FromMM(R.required_clearance(net, other_net))
                if sh.Collide(osh, need):
                    return True
    return False


def inside_board(b, fp):
    e = b.GetBoardEdgesBoundingBox()
    bb = fp.GetBoundingBox(False, False)
    m = pcbnew.FromMM(MARGIN)
    return (bb.GetLeft() > e.GetLeft() + m and bb.GetRight() < e.GetRight() - m
            and bb.GetTop() > e.GetTop() + m and bb.GetBottom() < e.GetBottom() - m)


def in_keepout(fp):
    bb = fp.GetBoundingBox(False, False)
    x0, y0, x1, y1 = KEEPOUT
    return not (pcbnew.ToMM(bb.GetRight()) < x0 or pcbnew.ToMM(bb.GetLeft()) > x1
                or pcbnew.ToMM(bb.GetBottom()) < y0 or pcbnew.ToMM(bb.GetTop()) > y1)


def candidates(anchor, step=0.635, reach=20.0):
    """定位基準周圍的候選點，由近而遠。

    早期版本只沿 8 個方向、以 1.27mm 為級距找，結果 D10 被推到離 W2
    19.76mm 才找到位置 —— TVS 離入口那麼遠等於沒放。改成密網格全面搜尋。
    """
    ax, ay = pcbnew.ToMM(anchor.x), pcbnew.ToMM(anchor.y)
    n = int(reach / step)
    out = []
    for i in range(-n, n + 1):
        for j in range(-n, n + 1):
            dx, dy = i * step, j * step
            d = math.hypot(dx, dy)
            if d < 2.0 or d > reach:
                continue
            for rot in (900, 0):
                out.append((d, ax + dx, ay + dy, rot))
    out.sort(key=lambda t: t[0])
    return [(x, y, r) for _d, x, y, r in out]


def add_part(b, spec, anchor_ref, anchor_pad, existing=None):
    """新增（或重新定位已存在的）元件。

    不走「先刪再加」：b.Remove() 之後再迭代 b.GetFootprints()，
    SWIG 會吐出已失效的物件，下一次 GetReference() 直接爆掉。
    """
    if existing is not None:
        fp = existing
    else:
        lib_fp = spec["footprint"].split(":", 1)[1]
        fp = pcbnew.FootprintLoad(LIB, lib_fp)
        if fp is None:
            raise SystemExit("載入封裝失敗: %s" % lib_fp)
        b.Add(fp)
    fp.SetReference(spec["ref"])
    fp.SetValue(spec["value"])
    for p in fp.Pads():
        net = spec["pins"].get(p.GetNumber())
        if net:
            ni = b.FindNet(net)
            if ni is None:
                raise SystemExit("找不到網路 %s" % net)
            p.SetNet(ni)

    anchor = board_pos(b, anchor_ref, anchor_pad)
    if anchor is None:
        raise SystemExit("找不到定位基準 %s.%s" % (anchor_ref, anchor_pad))
    width = spec.get("track_width", 0.4)
    for x, y, rot in candidates(anchor):
        fp.SetPosition(pcbnew.VECTOR2I_MM(x, y))
        fp.SetOrientation(pcbnew.EDA_ANGLE(rot, pcbnew.TENTHS_OF_A_DEGREE_T))
        if not inside_board(b, fp) or in_keepout(fp):
            continue
        if overlaps(b, fp) is not None:
            continue
        # 外框不重疊還不夠：焊盤本身也要和其他網路保持電氣間距。
        # D9 第一版就是外框合法、但 120V 焊盤離 12V 走線只有 0.047 mm。
        if pads_too_close(b, fp):
            continue
        # 位置合法還不夠 —— 每個腳位都要真的繞得出來。
        # D9 第一次就是擺在 12V 幹線的另一側，位置沒問題但怎麼繞都要跨過去。
        plan, ok = [], True
        for p in fp.Pads():
            net = p.GetNetname()
            # 有完整平面的網路（GND）就近打孔，不要在平面上拉長線
            if R.plane_of(b, net) is not None:
                plan.append((p.GetNumber(), net, "plane"))
                continue
            tgt = R.nearest_same_net(b, net, p.GetPosition(), exclude_pad=p)
            if tgt is None:
                ok = False
                break
            r = R.find_route(b, net, p.GetPosition(), tgt, width)
            if r is None:
                ok = False
                break
            plan.append((p.GetNumber(), net, r))
        if not ok:
            continue
        d = math.hypot(x - pcbnew.ToMM(anchor.x), y - pcbnew.ToMM(anchor.y))
        print("   %s %-9s → (%.2f, %.2f) rot %d，距 %s.%s %.2f mm"
              % (spec["ref"], spec["value"], x, y, rot // 10,
                 anchor_ref, anchor_pad, d))
        for num, net, r in plan:
            if r == "plane":
                pad = [q for q in fp.Pads() if q.GetNumber() == num][0]
                d = R.via_to_plane(b, net, pad.GetPosition(), width)
                print("      腳 %-2s %-10s 過孔接地平面（%.2f mm）"
                      % (num, net, d if d else 0))
                continue
            lay, pts, length = r
            R.add_route(b, net, lay, pts, width, via_at_start=True)
            print("      腳 %-2s %-10s %s 繞線 %.2f mm"
                  % (num, net, b.GetLayerName(lay), length))
        return fp
    raise SystemExit("%s 找不到既合法又繞得通的位置" % spec["ref"])


ANCHORS = {"D9": ("U10", "1"), "D10": ("W2", "1")}


def main():
    path = sys.argv[1]
    b = pcbnew.LoadBoard(path)
    redo = "--redo" in sys.argv
    have = {}
    for f in b.GetFootprints():
        have[f.GetReference()] = f
    for spec in changes_v13.CHANGES:
        if spec["op"] != "add_part":
            continue
        cur = have.get(spec["ref"])
        if cur is not None and not redo:
            continue
        add_part(b, spec, *ANCHORS[spec["ref"]], existing=cur)
    b.BuildConnectivity()
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    b.BuildConnectivity()
    pcbnew.SaveBoard(path, b)
    print("已存")


if __name__ == "__main__":
    main()
