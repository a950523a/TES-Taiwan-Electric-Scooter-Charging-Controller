#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""照 EasyEDA 原稿的版面重畫 KiCad 電路圖。

為什麼不是自己排版：使用者自己畫的那張圖他看得懂，我程式產生的他看不懂。
而原稿一直都在 —— hardware/TES_Controller_V1.3/raw/P1.esch、P2.esch 是
tools/eda_export.py 解出來的逐行 JSON，裡面有元件座標、旋轉、鏡像、
每一條導線、區塊標題文字、還有把電路分組的框線。照搬就好。

座標：EasyEDA 一個單位 = 10 mil = 0.254 mm，格點 5 單位 = 1.27 mm，
剛好就是 KiCad 的預設格點，所以座標是整數對整數，不會跑掉。

腳位對不上的情況：easyeda2kicad 轉出來的符號，有幾顆（U8、USB1、U10、
BOOT1）腳位排列和 EasyEDA 原本的不同，導線會落在空處。那些不硬套幾何，
改用網路標籤把腳位和線群綁起來 —— 少數幾顆看起來稍差，但不會接錯。
"""
import argparse, collections, io, json, math, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sexpr                                               # noqa: E402
from sexpr import Sym                                      # noqa: E402
import sch_gen as G                                        # noqa: E402
import easyeda_symbols as ES                               # noqa: E402
import changes_v13 as CH                                   # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAW = os.path.join(REPO, "hardware", "TES_Controller_V1.3", "raw")
OUT = os.path.join(REPO, "hardware", "kicad", "TES_Controller.kicad_sch")
U = 0.254                    # 1 EasyEDA 單位 = 10 mil
SHEET2_DX = 0.0              # 第二頁往下擺，見 load()
TOL = 0.7                    # 判斷腳位是否落在導線上的容差（EasyEDA 單位）


def load():
    """讀兩頁原稿。第二頁整個往下移，兩頁併成一張大圖。"""
    out = dict(comp={}, power=[], wires=[], texts=[], polys=[], sym={},
               text_at={})
    attr_all = {}
    ymax = 0
    for page, fn in enumerate(("P1.esch", "P2.esch")):
        recs = []
        for ln in io.open(os.path.join(RAW, fn), encoding="utf-8"):
            ln = ln.strip()
            if ln:
                try:
                    recs.append(json.loads(ln))
                except Exception:
                    pass
        attr = collections.defaultdict(dict)
        full = collections.defaultdict(dict)
        for r in recs:
            if r[0] == "ATTR" and len(r) > 4:
                attr[r[2]][r[3]] = r[4]
                full[r[2]][r[3]] = r
        # 第二頁往下疊。位移量要扣掉第二頁自己的起始高度 —— 不扣的話
        # 兩頁之間會空出一大片（原稿第二頁是從 y=615 開始畫的）。
        if page == 0:
            dy = 0
        else:
            ys2 = [r[4] for r in recs
                   if r[0] == "COMPONENT" and len(r) > 6 and r[2]]
            for r in recs:
                if r[0] == "WIRE":
                    for seg in r[2]:
                        ys2 += [seg[i + 1] for i in range(0, len(seg), 2)]
            dy = ymax + 80 - (min(ys2) if ys2 else 0)
        for r in recs:
            if r[0] == "COMPONENT" and len(r) > 6:
                a = attr.get(r[1], {})
                des = a.get("Designator")
                x, y, rot, mir = r[3], r[4] + dy, r[5], r[6]
                if des:
                    out["comp"][des] = (x, y, rot, mir)
                    # 原稿用的符號名。**不一定等於 BOM 給的料號符號** ——
                    # Q1~Q3 畫的是 AO3400A-MS，它的 1、2 腳和 AO3400A 對調。
                    out["sym"][des] = str(r[2]).rsplit(".", 1)[0]
                    # 位號和數值的文字位置照搬原稿，否則全部疊在元件正中間
                    f = full.get(r[1], {})
                    def _at(k):
                        v = f.get(k)
                        if not v or len(v) < 9 or v[7] is None:
                            return None
                        return (v[7], v[8] + dy, bool(v[6]))
                    out["text_at"][des] = {"ref": _at("Designator"),
                                           "val": _at("Name"),
                                           "dev": a.get("Device")}
                elif a.get("Name"):
                    out["power"].append((a["Name"], x, y, rot))
            elif r[0] == "WIRE":
                for seg in r[2]:
                    pts = [(seg[i], seg[i + 1] + dy)
                           for i in range(0, len(seg), 2)]
                    out["wires"].append(pts)
            elif r[0] == "TEXT" and len(r) > 5:
                out["texts"].append((r[2], r[3] + dy, r[5]))
            elif r[0] == "POLY" and len(r) > 2 and isinstance(r[2], list):
                p = r[2]
                out["polys"].append([(p[i], p[i + 1] + dy)
                                     for i in range(0, len(p) - 1, 2)])
        if page == 0:
            ys = [c[1] for c in out["comp"].values()]
            ys += [w[2] for w in out["power"]]   # power 是 (名稱, x, y, rot)
            ymax = max(ys) if ys else 800
        attr_all.update(attr)
    return out


def mm(v):
    return round(v * U, 4)


# V1.3 改接線之後，有腳位和舊節點的鄰居**疊在同一點**，光把線剪掉沒用 ——
# KiCad 看到兩個標籤同座標就當成一個節點。R10 腳 1 原本和 C8 腳 2 共點
# （都在分壓中點），改接 HV_DIV1 之後必須挪開。往下移 30 單位剛好讓腳 2
# 落在原本那條 120V 線的端點上，線不會變成孤兒。
MOVED = {"R10": (0, 30)}

EDA_SYM = None
DEV_ATTRS = {}


def eda_pin_offset(title, pad, rot, mirror):
    """照**原稿實際用的符號**算腳位位移（EasyEDA 單位）。找不到就回 None。

    導線要對的是這個，不是 KiCad 符號庫的幾何 —— 兩者多半一樣，但只要
    原稿挑的是同料號的另一種符號（AO3400A-MS 之於 AO3400A），腳位編號就
    會對調，照 KiCad 的幾何去比對會把閘極接到源極的線上。
    """
    global EDA_SYM
    if EDA_SYM is None:
        EDA_SYM = ES.load() if os.path.exists(ES.EPRJ) else {}
    pins = EDA_SYM.get(ES.key(title or ""))
    if not pins or pad not in pins:
        return None
    px, py = pins[pad]
    if mirror:
        px = -px
    a = math.radians(rot)
    c, s = math.cos(a), math.sin(a)
    return (px * c - py * s, px * s + py * c)


def pin_offset(info, pad, rot, mirror, kicad=False):
    """腳位相對元件原點的位移（EasyEDA 單位）。`kicad` 參數只為相容留著。

    easyeda2kicad 轉符號庫時**沒有把 Y 取負**（tools/easyeda_symbols.py 逐腳
    比對過原稿：25 個對得上的符號全部 py == EasyEDA 的 y，一個取負的都沒有）。
    KiCad 符號庫的 Y 是向上的，EasyEDA 圖紙的 Y 向下，所以每顆符號在 KiCad
    裡都是上下顛倒的 —— 這也是為什麼這裡的式子直接用 lib 座標、不翻 Y 就能
    對上原稿的導線。

    顛倒要在**輸出**時補回來，見 sym_node()。
    """
    px, py, _ = info["pins"][pad]
    if mirror:
        px = -px
    a = math.radians(rot)
    c, s = math.cos(a), math.sin(a)
    return ((px * c - py * s) / U, (px * s + py * c) / U)


def wire_groups(wires):
    """把相連的導線併成群（共用端點或端點落在另一段上）。"""
    segs = []
    for pts in wires:
        for a, b in zip(pts, pts[1:]):
            if a != b:
                segs.append((a, b))
    parent = list(range(len(segs)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    def union(i, j):
        a, b = find(i), find(j)
        if a != b:
            parent[a] = b

    at = collections.defaultdict(list)
    for i, (a, b) in enumerate(segs):
        at[a].append(i)
        at[b].append(i)
    for pts in at.values():
        for i in pts[1:]:
            union(pts[0], i)
    # 端點落在另一段中間也算相連（EasyEDA 的 T 接）
    for i, (a, b) in enumerate(segs):
        for p in (a, b):
            for j, s in enumerate(segs):
                if i != j and on_segment(p, s):
                    union(i, j)
    groups = collections.defaultdict(list)
    for i, s in enumerate(segs):
        groups[find(i)].append(s)
    return list(groups.values())


def on_segment(p, seg):
    (x1, y1), (x2, y2) = seg
    x, y = p
    if not (min(x1, x2) - TOL <= x <= max(x1, x2) + TOL
            and min(y1, y2) - TOL <= y <= max(y1, y2) + TOL):
        return False
    cross = (x2 - x1) * (y - y1) - (y2 - y1) * (x - x1)
    return abs(cross) <= TOL * max(1.0, math.hypot(x2 - x1, y2 - y1))


def sym_node(lib_id, ref, value, x, y, rot, mirror, root, props=(),
             in_bom=True, pins=(), ref_at=None, val_at=None):
    node = [Sym("symbol"),
            [Sym("lib_id"), lib_id],
            # 符號庫沒翻 Y（見 pin_offset），所以每顆都要上下翻回來：
            # 鏡像 + 角度 θ+180 合起來就是一次上下翻轉，腳位才會落在
            # 原稿導線的位置上，符號看起來也和 EasyEDA 一樣。
            # （KiCad 是先轉再鏡像，不是先鏡像再轉 —— 反了的話兩腳
            #   元件轉 90° 就會 1、2 腳對調。）
            [Sym("at"), mm(x), mm(y), (int(rot) + 180) % 360],
            [Sym("unit"), 1],
            [Sym("exclude_from_sim"), Sym("no")],
            [Sym("in_bom"), Sym("yes" if in_bom else "no")],
            [Sym("on_board"), Sym("yes")],
            [Sym("dnp"), Sym("no")],
            [Sym("uuid"), G.uid()]]
    if not mirror:
        node.insert(3, [Sym("mirror"), Sym("y")])
    rx, ry, rvis = ref_at or (x, y - 20, True)
    vx, vy, vvis = val_at or (x, y + 20, True)
    node.append(G.prop("Reference", ref, mm(rx), mm(ry), hide=not rvis))
    node.append(G.prop("Value", value, mm(vx), mm(vy), hide=not vvis))
    for k, v in props:
        node.append(G.prop(k, v, mm(x), mm(y), hide=True))
    for num in pins:
        node.append([Sym("pin"), num, [Sym("uuid"), G.uid()]])
    node.append([Sym("instances"),
                 [Sym("project"), "TES_Controller",
                  [Sym("path"), "/" + root,
                   [Sym("reference"), ref], [Sym("unit"), 1]]]])
    return node


def build(root):
    global DEV_ATTRS
    src = load()
    if not DEV_ATTRS and os.path.exists(ES.EPRJ):
        DEV_ATTRS = ES.device_attrs()
    bom = G.read_bom()
    conns, open_pins = G.read_netlist()
    _lib, by_lcsc, by_name = G.load_lib()
    syms = G.assign(bom, conns, by_lcsc, by_name)
    net_of = {(d, p): n for d, p, n in conns}

    # 位號在 EasyEDA 是舊名（G/R/Y、START…），換成現在用的
    place = {}
    for des, v in src["comp"].items():
        ref = G.DESIGNATOR_RENAME.get(des, des)
        dx, dy = MOVED.get(ref, (0, 0))
        place[ref] = (v[0] + dx, v[1] + dy, v[2], v[3])

    # 每支腳在圖上的位置（EasyEDA 單位）
    pin_at = {}
    for ref, (x, y, rot, mir) in place.items():
        if ref not in syms:
            continue
        title = src["sym"].get(ref) or src["sym"].get(
            {v: k for k, v in G.DESIGNATOR_RENAME.items()}.get(ref, ref))
        for pad in syms[ref]["pins"]:
            d = eda_pin_offset(title, pad, rot, mir)
            if d is None:
                d = pin_offset(syms[ref], pad, rot, mir)
            pin_at[(ref, pad)] = (x + d[0], y + d[1])

    # KiCad 符號庫的腳位不一定和原稿的符號一樣。差在兩件事：
    #   * 多腳 IC（U1、U5、U8、USB1）easyeda2kicad 是重新排版，不是照抄；
    #   * 同料號有多種符號 —— Q1~Q3 原稿畫的是 AO3400A-MS，1、2 腳和
    #     BOM 給的 AO3400A 剛好相反。
    # 這些元件的腳位上不能留導線，否則線會接到編號不同的另一支腳
    # （閘極接到源極、U8.5 接到 GND）。改成在 KiCad 的腳位上放標籤 ——
    # IC 用標籤接線本來就是正常電路圖的畫法，不算退步。
    open_set = {(G.DESIGNATOR_RENAME.get(d, d), p) for d, p in open_pins}
    kicad_at, mismatch = {}, set()
    for ref, (x, y, rot, mir) in place.items():
        if ref not in syms:
            continue
        title = src["sym"].get(ref)
        for pad in syms[ref]["pins"]:
            dx, dy = pin_offset(syms[ref], pad, rot, mir, kicad=True)
            kicad_at[(ref, pad)] = (x + dx, y + dy)
            e = eda_pin_offset(title, pad, rot, mir)
            if e is None or abs(e[0] - dx) > 0.5 or abs(e[1] - dy) > 0.5:
                mismatch.add(ref)
    LABEL_ONLY = tuple(sorted(mismatch))
    # 未接的腳位、以及對不上的元件的腳位，上面都不能壓到線
    bad = [kicad_at[k] for k in open_set if k in kicad_at]
    bad += [p for k, p in kicad_at.items() if k[0] in mismatch]
    # V1.3 改接線的腳位，原稿那條線要斷掉 —— 例如 R10 腳 1 本來直接接
    # 分壓中點，現在改接串聯的下一顆 R33，舊線留著就還是接到 VOUT_SENSE
    for c in CH.CHANGES:
        if c["op"] in ("set_net", "disconnect") and c.get("pad"):
            k = (G.DESIGNATOR_RENAME.get(c["ref"], c["ref"]), c["pad"])
            for tbl in (kicad_at, pin_at):
                if k in tbl:
                    bad.append(tbl[k])

    kept = []
    for pts in src["wires"]:
        run = [pts[0]]
        for a_, b_ in zip(pts, pts[1:]):
            if any(on_segment(q, (a_, b_)) for q in bad):
                if len(run) > 1:
                    kept.append(run)
                run = [b_]
            else:
                run.append(b_)
        if len(run) > 1:
            kept.append(run)
    dropped = sum(len(p) - 1 for p in src["wires"]) -         sum(len(p) - 1 for p in kept)
    src["wires"] = kept
    if dropped:
        print("   去掉壓在 KiCad 腳位對不上處的線段 %d 段" % dropped)
    if mismatch:
        print("   腳位和原稿對不上、改用標籤接線：%s" % ", ".join(LABEL_ONLY))

    groups = wire_groups(src["wires"])

    # 線群 → 網路名：看它碰到哪些已知腳位，再看有沒有接到 GND 符號
    gnet, touch = {}, collections.defaultdict(set)
    for gi, segs in enumerate(groups):
        for (ref, pad), p in pin_at.items():
            if any(on_segment(p, s) for s in segs):
                touch[gi].add((ref, pad))
        votes = collections.Counter(net_of[k] for k in touch[gi]
                                    if k in net_of)
        for nm, px, py, _r in src["power"]:
            if any(on_segment((px, py), s) for s in segs) or \
               any(abs(px - q[0]) < 3 and abs(py - q[1]) < 3
                   for s in segs for q in s):
                votes[nm] += 1
        # 多數決，不是字母序。一條線群偶爾會沾到隔壁網路的一支腳
        # （R10 腳 1 改接之後就沾在舊的分壓中點上），字母序會讓那一票
        # 蓋掉真正的三票，整個節點被改名成 HV_DIV1。
        gnet[gi] = (min(votes.items(), key=lambda kv: (-kv[1], kv[0]))[0]
                    if votes else None)

    # 哪些腳位已經靠幾何接上了
    landed_pre = set()
    for gi, segs in enumerate(groups):
        if gnet.get(gi):
            for k in touch[gi]:
                if net_of.get(k) == gnet[gi]:
                    landed_pre.add(k)

    # 認不出網路的線群：靠附近沒接上的腳位推定。
    # 那些線本來就是畫到 EasyEDA 的腳位上的，只是轉檔後 KiCad 的腳位挪了位置。
    for gi, segs in enumerate(groups):
        if gnet.get(gi):
            continue
        near = set()
        for (ref, pad), p in pin_at.items():
            if (ref, pad) in landed_pre or (ref, pad) not in net_of:
                continue
            for (a, b) in segs:
                if min(math.hypot(p[0] - q[0], p[1] - q[1])
                       for q in (a, b)) <= 15:
                    near.add(net_of[(ref, pad)])
                    break
        if len(near) == 1:
            gnet[gi] = near.pop()

    landed = set()
    for gi, segs in enumerate(groups):
        if gnet.get(gi):
            for k in touch[gi]:
                if net_of.get(k) == gnet[gi] and k[0] not in LABEL_ONLY:
                    landed.add(k)
    return src, syms, conns, open_pins, by_name, place, groups, gnet, \
        pin_at, landed, net_of, open_set, kicad_at


def junctions(groups, pin_at):
    """T 接處要放連接點 —— KiCad 沒有連接點就不算相連，只當成交叉。"""
    segs = [s for g in groups for s in g]
    ends = collections.Counter()
    for a, b in segs:
        ends[a] += 1
        ends[b] += 1
    out = {p for p, n in ends.items() if n >= 3}
    for a, b in segs:
        for p in (a, b):
            for s in segs:
                if s != (a, b) and on_segment(p, s) and p not in (s[0], s[1]):
                    out.add(p)
    return out


def safe_spot(pt, net, pin_at, net_of, open_set):
    """在這個位置放標籤或電源符號，會不會誤接到別條網路的腳位。

    原稿的接地符號畫在 EasyEDA 的腳位上，轉檔後那個位置可能變成另一支腳
    —— 照放就會把 U8.5、USB1.A8 這些本該未接的腳接到 GND。
    """
    for k, p in pin_at.items():
        if abs(p[0] - pt[0]) > 1.5 or abs(p[1] - pt[1]) > 1.5:
            continue
        if k in open_set or net_of.get(k) != net:
            return False
    return True


def emit():
    root = G.uid()
    (src, syms, conns, open_pins, by_name, place, groups, gnet,
     pin_at, landed, net_of, open_set, kicad_at) = build(root)

    xs = [c[0] for c in src["comp"].values()] + [p[1] for p in src["power"]]
    ys = [c[1] for c in src["comp"].values()] + [p[2] for p in src["power"]]
    for g in groups:
        for a, b in g:
            xs += [a[0], b[0]]
            ys += [a[1], b[1]]
    # 下面還要放 V1.3 新增區（base_y = 元件最低點 + 70）和一排電源旗標
    # （再 +60），紙張要留得下，否則那一排會被裁在圖框外面。
    w = (max(xs) + 60) * U
    h = (max(ys) + 70 + 380 + 100) * U

    sch = [Sym("kicad_sch"),
           [Sym("version"), Sym("20251024")],
           [Sym("generator"), "tes_sch_import_easyeda"],
           [Sym("generator_version"), "10.0"],
           [Sym("uuid"), root],
           [Sym("paper"), "User", round(w, 1), round(h, 1)]]

    lib_syms = [Sym("lib_symbols")]
    used = {id(s): s for s in syms.values()}
    for extra in ["PWR_FLAG"] + sorted(set(G.POWER_SYMBOL.values())):
        if extra in by_name:
            used[id(by_name[extra])] = by_name[extra]
    for info in used.values():
        node = [c for c in info["node"]]
        node[1] = "TES:" + info["name"]
        lib_syms.append(node)
    sch.append(lib_syms)

    # 分區框線與標題 —— 這是原稿最有價值的部分
    for poly in src["polys"]:
        for a, b in zip(poly, poly[1:]):
            sch.append([Sym("polyline"),
                        [Sym("pts"), [Sym("xy"), mm(a[0]), mm(a[1])],
                         [Sym("xy"), mm(b[0]), mm(b[1])]],
                        [Sym("stroke"), [Sym("width"), 0.15],
                         [Sym("type"), Sym("dash")]],
                        [Sym("uuid"), G.uid()]])
    for x, y, txt in src["texts"]:
        sch.append([Sym("text"), txt, [Sym("at"), mm(x), mm(y), 0],
                    G.font(2.5, "left bottom"), [Sym("uuid"), G.uid()]])

    # 導線
    for g in groups:
        for a, b in g:
            sch.append(G.wire(mm(a[0]), mm(a[1]), mm(b[0]), mm(b[1])))
    for p in junctions(groups, pin_at):
        sch.append([Sym("junction"), [Sym("at"), mm(p[0]), mm(p[1])],
                    [Sym("diameter"), 0], [Sym("color"), 0, 0, 0, 0],
                    [Sym("uuid"), G.uid()]])

    # 元件
    for ref in sorted(place, key=G.natural):
        if ref not in syms:
            continue
        x, y, rot, mir = place[ref]
        info = syms[ref]
        props = [(p[1], p[2]) for p in sexpr.findall(info["node"], "property")
                 if p[1] in ("Footprint", "Datasheet", "LCSC Part", "MPN")]
        # 顯示的數值用原稿的（"22Ω"、"1uF"），不是料號 —— 料號長到會蓋掉
        # 隔壁的元件，而且看圖的時候要的是阻值不是型號。
        rev = {v: k for k, v in G.DESIGNATOR_RENAME.items()}
        ta = src["text_at"].get(ref) or src["text_at"].get(rev.get(ref, ref))             or {}
        dev = DEV_ATTRS.get(ta.get("dev") or "", {})
        value = G.ADDED.get(ref, {}).get("value") or             ES.resolve(dev.get("Name"), dev) or info["name"]
        sch.append(sym_node("TES:" + info["name"], ref, value,
                            x, y, rot, mir, root, props,
                            pins=sorted(info["pins"]),
                            ref_at=ta.get("ref"), val_at=ta.get("val")))

    # EasyEDA 的接地符號
    for i, (nm, x, y, rot) in enumerate(src["power"]):
        sym = G.POWER_SYMBOL.get(nm)
        if not sym or sym not in by_name:
            continue
        # 線被剪掉之後會有接地符號孤零零地掛在那裡，ERC 會報 pin_not_connected
        touching = any(on_segment((x, y), sg) for g in groups for sg in g) or             any(abs(q[0] - x) < 0.01 and abs(q[1] - y) < 0.01
                for q in pin_at.values())
        if not touching:
            continue
        sch.append(sym_node("TES:" + sym, "#PWR%03d" % (i + 1), sym,
                            x, y, rot, 0, root, in_bom=False,
                            ref_at=(x, y - 20, False)))
    return sch, src, syms, conns, open_pins, place, groups, gnet, \
        pin_at, landed, net_of, root, open_set, kicad_at


# EasyEDA 原稿裡沒有的元件（V1.3 新增的），擺在圖面下方一塊獨立區域
EXTRA_AT = {"R33": (0, 0), "R34": (14, 0), "D9": (28, 0), "D10": (42, 0),
            "W1": (0, 12), "W3": (14, 12), "W2": (28, 12), "W4": (42, 12),
            "R35": (0, 24), "U13": (16, 24), "C30": (34, 24)}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verify", action="store_true")
    a = ap.parse_args()

    (sch, src, syms, conns, open_pins, place, groups, gnet,
     pin_at, landed, net_of, root, open_set, kicad_at) = emit()

    # 線群各放一個標籤把網路名釘住；沒靠幾何接上的腳位也放一個。
    # 同名即相連，所以腳位排列和原稿不同的那幾顆（USB1、U8）也接得起來。
    for gi, segs in enumerate(groups):
        nm = gnet.get(gi)
        if not nm:
            continue
        pts = [q for s in segs for q in s]
        # 線群的**自由端**優先貼標籤：剪掉接錯的那一段之後端點就懸空了，
        # ERC 會報 unconnected_wire_endpoint，而且看圖的人也不知道那條線
        # 是往哪去。在斷點標網路名本來就是電路圖的標準畫法。
        ends = [q for q in set(pts)
                if sum(1 for t in pts if t == q) == 1
                and not any(abs(k[0] - q[0]) < 0.01 and abs(k[1] - q[1]) < 0.01
                            for k in kicad_at.values())]
        spots = [q for q in ends
                 if safe_spot(q, nm, kicad_at, net_of, open_set)]
        if not spots:
            spots = [q for q in pts
                     if safe_spot(q, nm, kicad_at, net_of, open_set)][:1]
        for p in spots:
            sch.append([Sym("label"), nm, [Sym("at"), mm(p[0]), mm(p[1]), 0],
                        [Sym("fields_autoplaced"), Sym("yes")],
                        G.font(1.27, "left"), [Sym("uuid"), G.uid()]])

    base_y = max(c[1] for c in src["comp"].values()) + 70
    base_x = 60
    extra_n = 0
    for ref, (dx, dy) in sorted(EXTRA_AT.items()):
        if ref not in syms:
            continue
        info = syms[ref]
        x, y = base_x + dx * 10, base_y + dy * 10
        props = [(p[1], p[2]) for p in sexpr.findall(info["node"], "property")
                 if p[1] in ("Footprint", "Datasheet", "LCSC Part", "MPN")]
        sch.append(sym_node("TES:" + info["name"], ref,
                            G.ADDED.get(ref, {}).get("value", info["name"]),
                            x, y, 0, 0, root, props, pins=sorted(info["pins"])))
        at = {(ref, pad): (x + pin_offset(info, pad, 0, 0)[0],
                           y + pin_offset(info, pad, 0, 0)[1])
              for pad in info["pins"]}
        pin_at.update(at)
        kicad_at.update(at)      # 沒有這行這幾顆就一個標籤都拿不到
        extra_n += 1
    # 只有 power_in 腳、沒有任何 power_out 的網路 —— 12V、GND、120V、
    # GND_BACK 全是從板外的降壓板經焊盤進來的，補電源旗標告訴 ERC 這件事。
    for i, nm in enumerate(("120V", "GND_BACK", "12V", "GND")):
        fx, fy = base_x + i * 140, base_y + 380
        ref = "#FLG%02d" % (i + 1)
        sch.append(sym_node("TES:PWR_FLAG", ref, "PWR_FLAG",
                            fx, fy, 0, 0, root, in_bom=False,
                            ref_at=(fx, fy - 20, False),
                            val_at=(fx, fy + 20, False)))
        sch.append([Sym("label"), nm, [Sym("at"), mm(fx), mm(fy), 270],
                    [Sym("fields_autoplaced"), Sym("yes")],
                    G.font(1.27, "left"), [Sym("uuid"), G.uid()]])

    if extra_n:
        sch.append([Sym("text"), "V1.3 新增（原 EasyEDA 稿沒有）",
                    [Sym("at"), mm(base_x - 20), mm(base_y - 25), 0],
                    G.font(2.5, "left bottom"), [Sym("uuid"), G.uid()]])

    # 沒接上的腳位補標籤
    pwr_n = 0
    for (ref, pad), nm in sorted(net_of.items()):
        if (ref, pad) in landed or (ref, pad) not in kicad_at:
            continue
        # 標籤要放在 KiCad 真正的腳位上，不是原稿導線的位置。
        # 放錯的後果不是「沒接到」而是「接到同一顆 IC 的另一支腳」——
        # U8.5、USB1.A8 這些本該未接的腳就是這樣被接上 GND 和 VBUS 的。
        x, y = kicad_at[(ref, pad)]
        sym = G.POWER_SYMBOL.get(nm)
        if sym and sym in {v: v for v in G.POWER_SYMBOL.values()}:
            pwr_n += 1
            sch.append(sym_node("TES:" + sym, "#PWRX%03d" % pwr_n, sym,
                                x, y, 0, 0, root, in_bom=False,
                                ref_at=(x, y - 20, False)))
        else:
            sch.append([Sym("label"), nm, [Sym("at"), mm(x), mm(y), 0],
                        [Sym("fields_autoplaced"), Sym("yes")],
                        G.font(1.27, "left"), [Sym("uuid"), G.uid()]])

    for ref, pad in open_pins:
        ref = G.DESIGNATOR_RENAME.get(ref, ref)
        if (ref, pad) in kicad_at:
            x, y = kicad_at[(ref, pad)]
            sch.append([Sym("no_connect"), [Sym("at"), mm(x), mm(y)],
                        [Sym("uuid"), G.uid()]])

    sch.append([Sym("sheet_instances"), [Sym("path"), "/", [Sym("page"), "1"]]])
    sch.append([Sym("embedded_fonts"), Sym("no")])
    io.open(OUT, "w", encoding="utf-8").write(sexpr.dumps(sch) + "\n")
    print("%s：元件 %d、導線群 %d、接地符號 %d、分區框 %d、標題 %d"
          % (os.path.relpath(OUT, REPO), len(place) + extra_n, len(groups),
             len(src["power"]), len(src["polys"]), len(src["texts"])))
    if a.verify:
        return G.verify()
    return 0


if __name__ == "__main__":
    sys.exit(main())
