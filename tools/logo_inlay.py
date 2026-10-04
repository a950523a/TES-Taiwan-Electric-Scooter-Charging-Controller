# -*- coding: utf-8 -*-
"""把海龜電能的 icon SVG 轉成雙色嵌入用的 2D 區域（FreeCAD Part 面）。

make_case_lid.py 用它：回傳「淺色」的區域，上蓋在那裡往下挖、另一個實體填進去，
X2D 雙噴頭一次印完（上蓋深色、嵌入淺色）。

原圖是三色：深藍描邊與陰影、米色本體、琥珀色閃電。上蓋只有兩色，所以：
  淺色 = 米色 ∪ 琥珀色（依 SVG 的繪製順序，後畫的深藍會把它們蓋掉）
  深色 = 其餘全部 —— 就是上蓋本身的顏色，不需要另外印

縮到 2 公分左右，原圖的細節比噴嘴還細，所以照實體列印的極限調整（跟
make_logo.py 處理電路板絲印是同一回事）：
  - 深色描邊加粗到至少 MIN_LINE_MM —— 原本 9 單位的線縮完只剩約 0.3 mm
  - 寬度小於 HAIRLINE 單位的描邊直接拿掉：殼外緣那條 3 單位的線縮完 0.1 mm，
    而殼的邊緣本來就貼著深色的上蓋，拿掉也看得出輪廓
  - 填色的小橢圓（眼睛）放大到至少 MIN_DOT_MM
  - 最後對淺色區做一次「開運算」，窄於 MIN_LIGHT_MM 的碎片清掉

只支援這個 icon 用到的 SVG 子集：<g transform="translate/scale">、path 的 M/L/Q/Z
（絕對座標）、circle、ellipse（含 rotate）。換一張用到別的語法的圖會直接報錯，
不會默默畫錯。
"""
import math, re
import xml.etree.ElementTree as ET
import FreeCAD as App
import Part

V = App.Vector
NS = "{http://www.w3.org/2000/svg}"

MIN_LINE_MM  = 0.45   # 噴嘴 0.4 mm 能穩定印出的最細深色線
MIN_DOT_MM   = 0.5
MIN_LIGHT_MM = 0.4
HAIRLINE     = 5.0      # SVG 單位
LIGHT = {"#F6E9BF", "#F2B233"}
DARK  = {"#1F2A44"}


def _matrix(t):
    """transform 屬性 → (a,b,c,d,e,f)，只支援 translate / scale / rotate"""
    m = (1, 0, 0, 1, 0, 0)
    for name, args in re.findall(r"(\w+)\s*\(([^)]*)\)", t or ""):
        v = [float(x) for x in re.split(r"[ ,]+", args.strip()) if x]
        if name == "translate":
            n = (1, 0, 0, 1, v[0], v[1] if len(v) > 1 else 0)
        elif name == "scale":
            n = (v[0], 0, 0, v[1] if len(v) > 1 else v[0], 0, 0)
        elif name == "rotate":
            a = math.radians(v[0]); cx, cy = (v[1], v[2]) if len(v) == 3 else (0, 0)
            c, s = math.cos(a), math.sin(a)
            n = (c, s, -s, c, cx - c * cx + s * cy, cy - s * cx - c * cy)
        else:
            raise ValueError("不支援的 transform：" + name)
        m = _mul(m, n)
    return m


def _mul(m, n):
    a, b, c, d, e, f = m
    A, B, C, D, E, F = n
    return (a * A + c * B, b * A + d * B, a * C + c * D, b * C + d * D, a * E + c * F + e, b * E + d * F + f)


def _apply(m, x, y):
    a, b, c, d, e, f = m
    return a * x + c * y + e, b * x + d * y + f


def _path_points(d):
    toks = re.findall(r"[MLQZmlqz]|-?\d*\.?\d+", d)
    pts, i, cmd, cur = [], 0, None, (0.0, 0.0)
    while i < len(toks):
        t = toks[i]
        if t.isalpha():
            cmd = t; i += 1
            if cmd in "Zz":
                continue
            if cmd.islower():
                raise ValueError("只支援絕對座標的 path")
            continue
        if cmd == "M" or cmd == "L":
            cur = (float(toks[i]), float(toks[i + 1])); i += 2
            pts.append(cur)
        elif cmd == "Q":
            c = (float(toks[i]), float(toks[i + 1])); e = (float(toks[i + 2]), float(toks[i + 3])); i += 4
            for k in range(1, 13):
                u = k / 12
                pts.append(((1 - u) ** 2 * cur[0] + 2 * (1 - u) * u * c[0] + u * u * e[0],
                            (1 - u) ** 2 * cur[1] + 2 * (1 - u) * u * c[1] + u * u * e[1]))
            cur = e
        else:
            raise ValueError("不支援的 path 指令：" + str(cmd))
    return pts


def _ellipse_points(cx, cy, rx, ry, n=96):
    return [(cx + rx * math.cos(2 * math.pi * k / n), cy + ry * math.sin(2 * math.pi * k / n)) for k in range(n)]


def _elements(root):
    """依繪製順序列出 (點列, fill, stroke, stroke-width, 變換矩陣, 種類)"""
    out = []

    def walk(el, m, style):
        m = _mul(m, _matrix(el.get("transform")))
        st = dict(style)
        for k in ("fill", "stroke", "stroke-width"):
            if el.get(k) is not None:
                st[k] = el.get(k)
        tag = el.tag.replace(NS, "")
        if tag in ("svg", "g"):
            for ch in el:
                walk(ch, m, st)
            return
        if tag == "path":
            pts, kind = _path_points(el.get("d")), "path"
        elif tag == "circle":
            r = float(el.get("r"))
            pts, kind = _ellipse_points(float(el.get("cx")), float(el.get("cy")), r, r), "circle"
        elif tag == "ellipse":
            pts, kind = _ellipse_points(float(el.get("cx")), float(el.get("cy")),
                                        float(el.get("rx")), float(el.get("ry"))), "ellipse"
        elif tag in ("metadata", "defs", "title", "desc"):
            return
        else:
            raise ValueError("不支援的 SVG 元素：" + tag)
        out.append((pts, st.get("fill", "#000000"), st.get("stroke", "none"),
                    float(st.get("stroke-width", 1)), m, kind))

    walk(root, (1, 0, 0, 1, 0, 0), {})
    return out


def logo_faces(svg_path, center, height_mm, max_width_mm=None):
    """淺色區域，已縮放擺到上蓋座標（z = 0 平面，y 軸朝上）。回傳 (shape, 寬, 高)。"""
    els = _elements(ET.parse(svg_path).getroot())

    # 以淺色填色的範圍決定比例與中心
    xs, ys = [], []
    for pts, fill, _, _, m, _ in els:
        if fill.upper() in LIGHT:
            for x, y in pts:
                X, Y = _apply(m, x, y); xs.append(X); ys.append(Y)
    w_u, h_u = max(xs) - min(xs), max(ys) - min(ys)
    s = height_mm / h_u
    if max_width_mm and w_u * s > max_width_mm:
        s = max_width_mm / w_u
    cx_u, cy_u = (max(xs) + min(xs)) / 2, (max(ys) + min(ys)) / 2

    def face(pts, m, grow_to_mm=None):
        P = [_apply(m, x, y) for x, y in pts]
        if grow_to_mm:                                 # 小點放大（以自身中心）
            mx = sum(p[0] for p in P) / len(P); my = sum(p[1] for p in P) / len(P)
            span = min(max(p[0] for p in P) - min(p[0] for p in P),
                       max(p[1] for p in P) - min(p[1] for p in P)) * s
            k = max(1.0, grow_to_mm / span)
            P = [(mx + (x - mx) * k, my + (y - my) * k) for x, y in P]
        vs = [V(center[0] + (x - cx_u) * s, center[1] - (y - cy_u) * s, 0) for x, y in P]
        vs.append(vs[0])
        return Part.Face(Part.Wire(Part.makePolygon(vs)))

    light = None
    for pts, fill, stroke, sw, m, kind in els:
        f = None
        if fill.upper() in LIGHT | DARK:
            f = face(pts, m, MIN_DOT_MM if (kind == "ellipse" and fill.upper() in DARK) else None)
            if fill.upper() in LIGHT:
                light = f if light is None else light.fuse(f)
            elif light is not None:
                light = light.cut(f)
        if stroke.upper() in DARK and sw >= HAIRLINE and light is not None:
            base = f if f is not None else face(pts, m)
            half = max(sw * s, MIN_LINE_MM) / 2
            band = base.makeOffset2D(half, join=0)
            try:
                inner = base.makeOffset2D(-half, join=0)
                band = band.cut(inner)
            except Exception:
                pass                                   # 太細的形狀內縮後消失 —— 整塊都是線
            light = light.cut(band)

    # 開運算：先內縮再外擴，窄於 MIN_LIGHT_MM 的碎片消失
    # （布林運算的結果是 Shell，makeOffset2D 只吃面或面的集合，所以逐面處理）
    r = MIN_LIGHT_MM / 2
    light = Part.makeCompound(light.removeSplitter().Faces)
    kept = []
    for f in light.Faces:
        try:
            o = f.makeOffset2D(-r, join=0)
        except Exception:
            continue                                   # 整塊都比 MIN_LIGHT_MM 窄
        if not o.Faces:
            continue
        o = Part.makeCompound(o.Faces).makeOffset2D(r, join=0).common(f)
        kept += o.Faces
    return Part.makeCompound(kept), w_u * s, h_u * s
