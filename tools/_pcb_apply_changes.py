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
# 疊放在上面的降壓模組佔掉的板面。用**模組本體的外框**（F.SilkS 上那個
# 矩形，也是絲印寫 "DC120V-DC12V Buck" 的地方），不是四個鎖點圍成的矩形
# —— 鎖點矩形每一邊都比本體小 2.4~3.0mm，D10 第一版就是從右邊那 2.5mm
# 的縫隙擠進去、結果卡在模組底下。
KEEPOUT = (125.5, 97.2, 179.0, 127.2)
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
                # 還沒有任何焊盤用到的網路存檔時會被丟掉，所以這裡要重建
                ni = pcbnew.NETINFO_ITEM(b, net)
                b.Add(ni)
            p.SetNet(ni)

    anchor = board_pos(b, anchor_ref, anchor_pad)
    if anchor is None:
        raise SystemExit("找不到定位基準 %s.%s" % (anchor_ref, anchor_pad))
    width = spec.get("track_width", 0.4)
    best = None
    # spec["at"] = (x, y, 角度) 表示位置是人挑的，不要自動搜。
    # 自動搜位置對細腳距的多腳元件不管用：它的繞通性測試只會試幾條 L 形
    # 路徑，分不出「這裡三支腳出得來」和「這裡出不來」，結果 U13 被推到
    # 離 I2C 25mm、SDA/SCL 各走 28mm 底層 —— 正好把整片地平面切開。
    fixed = spec.get("at")
    seq = [(fixed[0], fixed[1], int(fixed[2]) * 10)] if fixed else         candidates(anchor, reach=spec.get("reach", 20.0))
    for x, y, rot in seq:
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
        # 但「一條都繞不出來就整個放棄」也不對：U13 在 ADS1115 旁邊時，
        # 104 個幾何上合法的位置全都有 SCL 繞不通（這裡的繞線器只試幾條
        # 簡單的 L 形路徑，不是迷宮搜尋），結果是連擺都擺不上去。
        # 改成挑「繞不通的最少、其次最靠近錨點」的位置，剩下的交給
        # _pcb_repair.py 的修補迴圈，並且把欠的網路印出來。
        plan, unrouted = [], []
        for p in fp.Pads():
            net = p.GetNetname()
            # 有完整平面的網路（GND）就近打孔，不要在平面上拉長線
            if R.plane_of(b, net) is not None:
                plan.append((p.GetNumber(), net, "plane"))
                continue
            tgt = R.nearest_same_net(b, net, p.GetPosition(), exclude_pad=p)
            if tgt is None:
                # 串聯鏈的中間網路在下一顆放上去之前沒有對手（R33 的 HV_DIV2
                # 要等 R34）。這種腳位先跳過，全部擺完再統一繞。
                continue
            r = R.find_route(b, net, p.GetPosition(), tgt, width)
            if r is None:
                unrouted.append(net)
                continue
            plan.append((p.GetNumber(), net, r))
        d = math.hypot(x - pcbnew.ToMM(anchor.x), y - pcbnew.ToMM(anchor.y))
        key = (len(unrouted), d)
        if best is None or key < best[0]:
            best = (key, x, y, rot, list(plan), list(unrouted))
        if not unrouted:
            break

    if best is None:
        raise SystemExit("%s 找不到合法位置（幾何上就放不下）" % spec["ref"])
    _key, x, y, rot, plan, unrouted = best
    fp.SetPosition(pcbnew.VECTOR2I_MM(x, y))
    fp.SetOrientation(pcbnew.EDA_ANGLE(rot, pcbnew.TENTHS_OF_A_DEGREE_T))
    print("   %s %-9s → (%.2f, %.2f) rot %d，距 %s.%s %.2f mm"
          % (spec["ref"], spec["value"], x, y, rot // 10,
             anchor_ref, anchor_pad, _key[1]))
    for num, net, r in plan:
        if r == "plane":
            pad = [q for q in fp.Pads() if q.GetNumber() == num][0]
            dd = R.via_to_plane(b, net, pad.GetPosition(), width)
            print("      腳 %-2s %-10s 過孔接地平面（%.2f mm）"
                  % (num, net, dd if dd else 0))
            continue
        lay, pts, length = r
        R.add_route(b, net, lay, pts, width, via_at_start=True)
        print("      腳 %-2s %-10s %s 繞線 %.2f mm"
              % (num, net, b.GetLayerName(lay), length))
    for net in unrouted:
        print("      ** %s 這裡繞不通，留給 _pcb_repair.py **" % net)
    return fp



ANCHORS = {"D9": ("U10", "1"), "D10": ("W2", "1"),
           # R35 是 Q4 的閘極分壓，要和閘極同一處，走線拉長沒有意義
           "R35": ("Q4", "1"),
           # EEPROM 錨在 I2C 上拉電阻 R5（SCL）那一側，不是 ADS1115 的腳下。
           # U5 是 MSOP-10、0.5mm 腳距，旁邊又有 CP_SENSE 的 0.762mm 幹線，
           # SOT-23-5 擠進去之後三支訊號腳出不來（SDA 只剩 0.45mm 的縫，
           # 0.25mm 的線需要 0.55mm）。錨在 R5 會往旁邊比較空的地方找。
           "U13": ("R5", "1"), "C30": ("U13", "4"),
           # 分壓串聯的三顆要擠在一起：鏈上的節點帶 40–80V，
           # 走線拉長等於把高壓帶到板子各處，而且串聯電阻分開擺
           # 會讓雜訊耦合進分壓中點。依序貼著上一顆。
           "R33": ("R10", "1"), "R34": ("R33", "1")}


def main():
    path = sys.argv[1]
    b = pcbnew.LoadBoard(path)
    # --redo 會把已經擺好的元件全部重新定位，連帶扯掉它們既有的走線。
    # --redo=D10 只重擺指定的那幾顆，其餘保持不動。
    redo = set()
    for a in sys.argv[2:]:
        if a == "--redo":
            redo = None                      # None = 全部重擺
        elif a.startswith("--redo="):
            redo = set(a.split("=", 1)[1].split(","))
    have = {}
    for f in b.GetFootprints():
        have[f.GetReference()] = f
    for spec in changes_v13.CHANGES:
        if spec["op"] != "add_part":
            continue
        cur = have.get(spec["ref"])
        again = redo is None or spec["ref"] in (redo or ())
        if cur is not None and not again:
            continue
        add_part(b, spec, *ANCHORS[spec["ref"]], existing=cur)
        print("   擺放 %s" % spec["ref"])
    b.BuildConnectivity()
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    b.BuildConnectivity()
    pcbnew.SaveBoard(path, b)
    print("已存")


if __name__ == "__main__":
    main()
