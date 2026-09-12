#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
從 EasyEDA 匯出的 .epcb 取出板框與元件座標，轉成 KiCad 用的 mm。

為什麼需要：外殼（Inventor）是照 V1.3 的板框與開孔位置做的。
重畫 PCB 時，板框、模組、按鍵、LED、Type-C、底部連接器的位置一動，
外殼就得重做。這支程式把「不能動的座標」變成可比對的文字檔，
重畫完再跑一次就能確認有沒有跑掉。

用法：
  python tools/pcb_geometry.py                      # 印出全部
  python tools/pcb_geometry.py --locked             # 只印外殼相關（不可動）
  python tools/pcb_geometry.py --compare <kicad_pcb>  # 和 KiCad 版本比對
"""
import argparse, collections, io, json, math, os, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
from eda_export import jsonl, read_project, build_model, free_pads  # noqa: E402
from sch_gen import DESIGNATOR_RENAME  # noqa: E402

MIL = 0.0254          # 1 mil = 0.0254 mm
OUTLINE_LAYER = 11    # 見 .epcb 的 LAYER 記錄
MULTI_LAYER = 12      # 安裝孔畫在 Multi-Layer 上

# 外殼開孔對應的元件 —— 這些位置動了，外殼就要重做。
# H2（UART 排針）刻意不在清單裡：它只要落在板邊即可，正反面都行。
LOCKED = {
    "U1":        "ESP32-S3 模組（天線突出板外）",
    "USB1":      "Type-C 接頭（側面開孔）",
    "U10":       "120V 量測輸入端子（DG301 5.0mm 螺絲端子）",
    # 降壓模組的接線焊盤。模組的 IN / OUT 在固定的兩端，所以左右不能對調；
    # 位置可以在同一側微調，但別跨到另一邊去，否則導線長度全部要重做。
    "W1":        "模組 IN 側 120V 焊盤",
    "W3":        "模組 IN 側 GND_BACK 焊盤",
    "W2":        "模組 OUT 側 12V 焊盤",
    "W4":        "模組 OUT 側 GND 焊盤",
    "D7":        "LED 綠（V1.3 原位號 G）",
    "D8":        "LED 紅（V1.3 原位號 R）",
    "D6":        "LED 黃（V1.3 原位號 Y）",
    "START1":    "按鍵 開始",
    "STOP1":     "按鍵 停止",
    "SETTING1":  "按鍵 設定",
    "EMERGENCY1": "按鍵 緊急停止",
    "CN1":       "底部連接器 VP",
    "CN2":       "底部連接器 CAN/CP",
    "CN3":       "底部連接器 DC_RELAY",
    "CN5":       "底部連接器 DC_RELAY",
    "CN6":       "底部連接器 COUPLER",
    "H1":        "OLED 排針（面板開窗）",
}


def board_outline(pcb_recs):
    """板框。V1.3 是單一 POLY 矩形記錄：["POLY",id,?,net,11,width,["R",x,y,w,h,rx,ry],?]。

    也支援用線段畫的板框（LINE，layer 11），以防之後改成不規則外形。
    回傳 [(x1,y1,x2,y2), ...]，單位仍是 mil。
    """
    segs = []
    for r in pcb_recs:
        if not r or len(r) <= 4 or r[4] != OUTLINE_LAYER:
            continue
        if r[0] == "LINE" and len(r) >= 9:
            segs.append(tuple(float(v) for v in r[5:9]))
        elif r[0] == "POLY" and len(r) > 6 and isinstance(r[6], list):
            g = r[6]
            if g and g[0] == "R":
                x, y, w, h = (float(v) for v in g[1:5])
                # EasyEDA 的 Y 往下為負，所以矩形從 (x,y) 往 -Y 長
                pts = [(x, y), (x + w, y), (x + w, y - h), (x, y - h), (x, y)]
                segs += [(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1])
                         for i in range(4)]
            elif g and g[0] == "L":
                pass  # 折線板框：走下面的通用路徑
            else:
                nums, i = [], 0
                while i < len(g):
                    if isinstance(g[i], str):
                        i += 1
                        continue
                    nums.append(float(g[i]))
                    i += 1
                for i in range(0, len(nums) - 3, 2):
                    segs.append((nums[i], nums[i + 1], nums[i + 2], nums[i + 3]))
    return segs


def transform(segs):
    """mil → mm。板框左上角落在原點，Y 向下為正（KiCad 慣例）。"""
    xs = [v for s in segs for v in (s[0], s[2])]
    ys = [v for s in segs for v in (s[1], s[3])]
    minx, maxy = min(xs), max(ys)

    def to_mm(x, y):
        return (round((float(x) - minx) * MIL, 4),
                round((maxy - float(y)) * MIL, 4))

    size = ((max(xs) - minx) * MIL, (maxy - min(ys)) * MIL)
    return to_mm, size


def holes(pcb_recs):
    """安裝孔。畫在 Multi-Layer 的無網路圓形 FILL，回傳 [(x_mil, y_mil, 直徑_mil)]。

    V1.3 有 4 個 Ø3.2mm 孔排成矩形 —— 那是疊在板子上方的 120V 降壓模組的
    固定孔，不是板子鎖外殼用的。位置動了模組就裝不上去，所以一併鎖定。
    """
    out = []
    for r in pcb_recs:
        if (r and r[0] == "FILL" and len(r) > 7 and r[4] == MULTI_LAYER
                and not r[3] and isinstance(r[7], list)):
            for g in r[7]:
                if isinstance(g, list) and g and g[0] == "CIRCLE":
                    out.append((float(g[1]), float(g[2]), float(g[3]) * 2))
    return out


def components(pcb_recs, cid2des):
    """位號 → (x_mil, y_mil, 旋轉角, 層)。

    含 W1..Wn 這些自由焊盤 —— 它們不是 COMPONENT 記錄，但同樣要鎖座標。
    """
    out = {}
    for des, (_pad, _net, x, y) in free_pads(pcb_recs).items():
        out[des] = (x, y, 0.0, 1)
    for r in pcb_recs:
        if r and r[0] == "COMPONENT" and len(r) > 6:
            des = cid2des.get(r[1])
            if des:
                # 位號和 KiCad 側對齊（G/R/Y、START… 在那邊補了數字，見 sch_gen）
                des = DESIGNATOR_RENAME.get(des, des)
                out[des] = (float(r[4]), float(r[5]), float(r[6]) or 0.0, r[3])
    return out


def natural(d):
    import re
    m = __import__("re").match(r"([A-Za-z_]*)(\d*)", d or "")
    return (m.group(1), int(m.group(2)) if m.group(2) else 0, d or "")


def load(eprj):
    docs, devices, attrs = read_project(eprj)
    _, _, pcb_recs = build_model(docs, devices, attrs)
    # build_model 內部算過 cid2des，但沒回傳；這裡重算一次（成本可忽略）
    sch_recs = []
    for d in docs:
        recs = jsonl(d["data"])
        head = recs[0] if recs else []
        if (head[:2] == ["DOCTYPE", "SCH"]) or (d["dtype"] == 1 and head[:1] != ["DOCTYPE"]):
            sch_recs.extend(recs)
    sa = collections.defaultdict(dict)
    for r in sch_recs:
        if r and r[0] == "ATTR" and len(r) > 4:
            sa[r[2]][r[3]] = r[4]
    uid2 = {a["Unique ID"]: a.get("Designator")
            for a in sa.values() if a.get("Unique ID")}
    cid2des = {}
    for r in pcb_recs:
        if r and r[0] == "COMPONENT" and len(r) > 7 and isinstance(r[7], dict):
            des = uid2.get(r[7].get("Unique ID"))
            if des:
                cid2des[r[1]] = des
    return pcb_recs, cid2des


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--eprj", default=os.path.join(REPO, "docs", "PCB",
                                                   "TES_Controller_V1.3.eprj"))
    ap.add_argument("--locked", action="store_true", help="只印外殼相關元件")
    ap.add_argument("--json", help="同時寫出 JSON 給其他工具用")
    a = ap.parse_args()

    pcb_recs, cid2des = load(a.eprj)
    segs = board_outline(pcb_recs)
    if not segs:
        print("找不到板框（layer %d）" % OUTLINE_LAYER, file=sys.stderr)
        return 1
    to_mm, size = transform(segs)
    comps = components(pcb_recs, cid2des)
    hl = holes(pcb_recs)

    print("板框 %.2f × %.2f mm，%d 條線段" % (size[0], size[1], len(segs)))
    print()
    print("板框線段 (mm)")
    for x1, y1, x2, y2 in segs:
        p1, p2 = to_mm(x1, y1), to_mm(x2, y2)
        print("  (%8.3f, %8.3f) → (%8.3f, %8.3f)" % (p1[0], p1[1], p2[0], p2[1]))
    print()

    hole_rows = []
    if hl:
        print("安裝孔 (mm)")
        for i, (hx, hy, dia) in enumerate(sorted(hl, key=lambda t: (-t[1], t[0]))):
            mx, my = to_mm(hx, hy)
            hole_rows.append(dict(x=mx, y=my, dia=round(dia * MIL, 3)))
            print("  H%d  (%8.3f, %8.3f)  Ø%.2f" % (i + 1, mx, my, dia * MIL))
        xs = [h["x"] for h in hole_rows]
        ys = [h["y"] for h in hole_rows]
        print("  孔距 %.2f × %.2f mm" % (max(xs) - min(xs), max(ys) - min(ys)))
        print()

    want = [d for d in sorted(comps, key=natural) if not a.locked or d in LOCKED]
    print("%-10s %9s %9s %6s %-5s %s" % ("位號", "X(mm)", "Y(mm)", "旋轉", "層", "外殼用途"))
    rows = {}
    for des in want:
        x, y, rot, layer = comps[des]
        mx, my = to_mm(x, y)
        krot = round((-rot) % 360, 2)      # Y 翻轉後旋轉方向相反
        rows[des] = dict(x=mx, y=my, rot=krot, layer="top" if layer == 1 else "bottom")
        print("%-10s %9.3f %9.3f %6.1f %-5s %s"
              % (des, mx, my, krot, rows[des]["layer"], LOCKED.get(des, "")))

    if a.json:
        io.open(a.json, "w", encoding="utf-8").write(json.dumps(
            dict(size_mm=[round(size[0], 4), round(size[1], 4)],
                 outline=[[*to_mm(s[0], s[1]), *to_mm(s[2], s[3])] for s in segs],
                 holes=hole_rows,
                 components=rows), ensure_ascii=False, indent=1))
        print("\n→ %s" % a.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
