#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
由 V1.3 的網表機器生成 KiCad 電路圖。

為什麼是生成而不是手畫：73 個元件、253 條接線，手畫漏一條你不會發現，
要等板子回來才知道。生成的版本可以用 kicad-cli 匯出網表，
和 hardware/TES_Controller_V1.3/netlist.txt 逐條比對 —— 對錯是可驗證的。

連線一律用 net label，不畫線。看起來沒有手畫的漂亮，但在 GUI 裡
怎麼搬元件都不會扯斷連線，重排版面很自由。

用法：
  python tools/sch_gen.py                # 生成
  python tools/sch_gen.py --verify       # 生成後匯出網表並比對
"""
import argparse, csv, io, os, re, subprocess, sys, uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sexpr                                            # noqa: E402
from sexpr import Sym                                   # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "hardware", "TES_Controller_V1.3")
KI = os.path.join(REPO, "hardware", "kicad")
LIB = os.path.join(KI, "lib", "TES.kicad_sym")
OUT = os.path.join(KI, "TES_Controller.kicad_sch")
KICAD_CLI = r"C:\Program Files\KiCad\10.0\bin\kicad-cli.exe"

GRID = 1.27          # KiCad 預設 50 mil 格點；所有座標都要對齊，否則接點不相連
SHEET_W, SHEET_H = 594.0, 420.0      # A2

# EasyEDA 自動產生的網路名稱沒有意義，趁重畫改成看得懂的。
# 對照關係寫在註解裡，之後回頭查得到。
NET_RENAME = {
    "$1N24268": "LED_Y_K",       # Y.2 → R13 → GND
    "$1N24269": "LED_G_K",       # G.2 → R14 → GND
    "$1N24273": "LED_R_K",       # R.2 → R12 → GND
    "$1N24295": "USB_CC1",       # USB1.A5 → R2 → GND
    "$1N24443": "USB_CC2",       # USB1.B5 → R3 → GND
    "$1N24313": "CP_SENSE",      # R8/R9 分壓中點 → ADS1115 AIN2
    "$1N24345": "VOUT_SENSE",    # R10/R11 分壓中點 → ADS1115 AIN0
    "$1N24383": "IO42_DEBOUNCE",  # C13 → START.2（靠按鍵內部 1-2 腳短路才接到 IO42）
    "$1N35533": "VP_PGATE",      # Q2 汲極 + Q4 閘極 + R26 上拉
    "$1N35540": "VP_NGATE",      # Q2 閘極 ← R16 ← IO9
    "$3N445":   "RELAY_GATE",    # Q1 閘極 ← R19 ← IO11
    "$3N451":   "COUPLER_GATE",  # Q3 閘極 ← R29 ← IO10
    "$1N38322": "USB_DP_ESD",    # D5 → R31 → ESP32 D+
    "$1N38324": "USB_DN_ESD",    # D5 → R30 → ESP32 D-
}

# 功能分區。純粹為了看得懂 —— 連線靠標籤，分區怎麼擺都不影響電氣。
BLOCKS = [
    ("MCU",    ["U1", "C1", "C3", "C10", "C28", "R1", "EN1", "C2", "C4",
                "BOOT1", "C11", "R32"]),
    ("電源",   ["U8", "C17", "C16", "C20", "C29", "C9", "U6", "SB1"]),
    ("USB",    ["USB1", "D5", "R30", "R31", "R2", "R3"]),
    ("CAN",    ["U12", "R21", "CN2"]),
    ("類比量測", ["U5", "R5", "R6", "R8", "R9", "C7", "R10", "R11", "C8",
                "U10", "H1"]),
    ("繼電器",  ["Q1", "R19", "R20", "D1", "CN3", "CN5"]),
    ("電磁鎖",  ["Q3", "R28", "R29", "D4", "CN6"]),
    ("VP 供電", ["Q2", "Q4", "R16", "R17", "R26", "D3", "CN1"]),
    ("指示燈",  ["D6", "R13", "D7", "R14", "D8", "R12"]),
    ("按鍵",   ["START1", "C13", "STOP1", "C12", "SETTING1", "C15",
                "EMERGENCY1", "C14"]),
    ("模組接線", ["W1", "W3", "W2", "W4"]),
    ("其他",   ["H2", "C12", "C14", "C15"]),
]


def snap(v):
    return round(round(v / GRID) * GRID, 4)


def uid():
    return str(uuid.uuid4())


# --------------------------------------------------------------- 輸入


def read_bom():
    """位號 → (LCSC, 型號)。"""
    out = {}
    with io.open(os.path.join(SRC, "bom.csv"), encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            des = DESIGNATOR_RENAME.get(r["designator"], r["designator"])
            out[des] = (r["supplier"].strip(), r["part"].strip())
    return out


def read_netlist():
    """[(位號, pad, 網路名), ...]，只取前半段的逐腳位清單。"""
    conns, open_pins = [], []
    for line in io.open(os.path.join(SRC, "netlist.txt"), encoding="utf-8"):
        if line.startswith("# 依網路彙整"):
            break
        m = re.match(r"^(\S+)\s+pad\s+(\S+)\s+(.*)$", line.rstrip())
        if m:
            net = m.group(3).strip()
            des = DESIGNATOR_RENAME.get(m.group(1), m.group(1))
            if net and net != "(未接)":
                conns.append((des, m.group(2), NET_RENAME.get(net, net)))
            else:
                open_pins.append((des, m.group(2)))
    return conns, open_pins


def load_lib():
    """LCSC → 符號定義；順便整理出腳位座標與外框大小。"""
    lib = sexpr.load(LIB)
    by_lcsc, by_name = {}, {}
    for s in sexpr.findall(lib, "symbol"):
        name = s[1]
        lcsc = ""
        for p in sexpr.findall(s, "property"):
            if p[1] == "LCSC Part":
                lcsc = p[2]
        pins, types = {}, {}
        xs, ys = [], []
        for sub in sexpr.findall(s, "symbol"):
            for p in sexpr.findall(sub, "pin"):
                at = sexpr.find(p, "at")
                num = sexpr.find(p, "number")[1]
                x, y, a = float(at[1]), float(at[2]), float(at[3])
                pins[num] = (x, y, a)
                types[num] = str(p[1])
                xs.append(x)
                ys.append(y)
            for tag in ("rectangle", "polyline", "circle", "arc"):
                for g in sexpr.findall(sub, tag):
                    for pt in ("start", "end", "center", "mid"):
                        n = sexpr.find(g, pt)
                        if n:
                            xs.append(float(n[1]))
                            ys.append(float(n[2]))
                    ptsn = sexpr.find(g, "pts")
                    if ptsn:
                        for xy in sexpr.findall(ptsn, "xy"):
                            xs.append(float(xy[1]))
                            ys.append(float(xy[2]))
        info = dict(name=name, node=s, pins=pins, types=types,
                    w=(max(xs) - min(xs)) if xs else 10.0,
                    h=(max(ys) - min(ys)) if ys else 10.0)
        by_name[name] = info
        if lcsc:
            by_lcsc[lcsc] = info
    return lib, by_lcsc, by_name


# V1.3 的三顆 LED 位號是 G / R / Y，沒有數字。KiCad 把沒有數字的位號視為
# 「未編號」，在 GUI 裡按一次 Annotate 就會被重新命名 —— 而 "R" 會被當成
# 電阻，可能被編成 R33 之類的，和真正的電阻混在一起。趁重畫改掉。
# 比對網表時會換回原名，所以驗證仍然是對著 V1.3 的原始資料做的。
DESIGNATOR_RENAME = {
    "Y": "D6", "G": "D7", "R": "D8",          # LED：R 若補數字會和電阻 R1 撞名
    "START": "START1", "STOP": "STOP1", "SETTING": "SETTING1",
    "EMERGENCY": "EMERGENCY1", "BOOT": "BOOT1", "EN": "EN1",
}
RENAME_BACK = {v: k for k, v in DESIGNATOR_RENAME.items()}


# 沒有 LCSC 料號的 5 個元件，用 tools/make_symbols.py 自建的符號。
NO_LCSC = {"D6": "LED-5MM", "D7": "LED-5MM", "D8": "LED-5MM",
           "H1": "HDR-1X4", "H2": "HDR-1X3",
           # 降壓模組的四個接線焊盤（見 eda_export.free_pads）
           "W1": "SOLDERPAD-1P", "W2": "SOLDERPAD-1P",
           "W3": "SOLDERPAD-1P", "W4": "SOLDERPAD-1P"}


def natural(d):
    m = re.match(r"([A-Za-z_]*)(\d*)", d or "")
    return (m.group(1), int(m.group(2)) if m.group(2) else 0, d or "")


def assign(bom, conns, by_lcsc, by_name):
    """位號 → 符號資訊。"""
    used = sorted({d for d, _, _ in conns}, key=natural)
    out = {}
    for des in used:
        if des in NO_LCSC:
            out[des] = by_name[NO_LCSC[des]]
        else:
            lcsc = bom.get(des, ("", ""))[0]
            if lcsc not in by_lcsc:
                raise SystemExit("%s 找不到符號（LCSC=%s）" % (des, lcsc))
            out[des] = by_lcsc[lcsc]
    return out


def layout(syms, conns):
    """把元件排進分區欄位。回傳 位號 → (x, y)，以及圖紙大小。

    連線靠標籤，所以擺放位置純粹是給人看的，怎麼排都不影響電氣。
    """
    per_des = {}
    for d, p, n in conns:
        per_des.setdefault(d, set()).add(p)

    assigned, blocks = set(), []
    for title, members in BLOCKS:
        got = [d for d in members if d in syms and d not in assigned]
        assigned.update(got)
        if got:
            blocks.append((title, got))
    rest = [d for d in sorted(syms, key=natural) if d not in assigned]
    if rest:
        blocks.append(("未分類", rest))

    margin, gap_y, label_room = 25.4, 7.62, 30.48
    col_x, cur_y, pos, headers = margin, margin, {}, []
    col_w = 0.0
    max_y, max_x = 0.0, 0.0
    for title, members in blocks:
        need = sum(syms[d]["h"] + gap_y for d in members) + 12.7
        if cur_y > margin and cur_y + need > SHEET_H - margin:
            col_x += col_w + label_room
            cur_y, col_w = margin, 0.0
        headers.append((title, snap(col_x), snap(cur_y)))
        cur_y += 12.7
        for d in members:
            s = syms[d]
            cur_y += s["h"] / 2.0
            pos[d] = (snap(col_x + s["w"] / 2.0), snap(cur_y))
            col_w = max(col_w, s["w"])
            cur_y += s["h"] / 2.0 + gap_y
            max_x = max(max_x, col_x + s["w"] + label_room)
        max_y = max(max_y, cur_y)
    return pos, headers, (max_x + margin, max_y + margin)


def font(size=1.27, justify=None, hide=False):
    n = [Sym("effects"), [Sym("font"), [Sym("size"), size, size]]]
    if justify:
        n.append([Sym("justify")] + [Sym(j) for j in justify.split()])
    if hide:
        n.append([Sym("hide"), Sym("yes")])
    return n


def prop(name, value, x, y, hide=False):
    return [Sym("property"), name, value, [Sym("at"), x, y, 0],
            font(hide=hide)]


def emit(syms, pos, headers, conns, open_pins, size, root, by_name):
    """組出 .kicad_sch 的 s-expression。"""
    sch = [Sym("kicad_sch"),
           [Sym("version"), Sym("20251024")],
           [Sym("generator"), "tes_sch_gen"],
           [Sym("generator_version"), "10.0"],
           [Sym("uuid"), root],
           [Sym("paper"), "A2"]]

    lib_syms = [Sym("lib_symbols")]
    used = {id(s): s for s in syms.values()}
    if "PWR_FLAG" in by_name:
        used[id(by_name["PWR_FLAG"])] = by_name["PWR_FLAG"]
    for info in used.values():
        node = [c for c in info["node"]]
        node[1] = "TES:" + info["name"]
        lib_syms.append(node)
    sch.append(lib_syms)

    for title, x, y in headers:
        sch.append([Sym("text"), title, [Sym("at"), x, y, 0],
                    font(3.0, "left bottom"), [Sym("uuid"), uid()]])

    # 元件
    for des in sorted(syms, key=natural):
        info, (ox, oy) = syms[des], pos[des]
        node = [Sym("symbol"),
                [Sym("lib_id"), "TES:" + info["name"]],
                [Sym("at"), ox, oy, 0],
                [Sym("unit"), 1],
                [Sym("exclude_from_sim"), Sym("no")],
                [Sym("in_bom"), Sym("yes")],
                [Sym("on_board"), Sym("yes")],
                [Sym("dnp"), Sym("no")],
                [Sym("uuid"), uid()],
                prop("Reference", des, ox, snap(oy - info["h"] / 2.0 - 2.54)),
                prop("Value", info["name"],
                     ox, snap(oy + info["h"] / 2.0 + 2.54))]
        for p in sexpr.findall(info["node"], "property"):
            if p[1] in ("Footprint", "Datasheet", "LCSC Part", "MPN"):
                node.append(prop(p[1], p[2], ox, oy, hide=True))
        for num in sorted(info["pins"]):
            node.append([Sym("pin"), num, [Sym("uuid"), uid()]])
        node.append([Sym("instances"),
                     [Sym("project"), "TES_Controller",
                      [Sym("path"), "/" + root,
                       [Sym("reference"), des], [Sym("unit"), 1]]]])
        sch.append(node)

    # 哪些網路只有 power_in、沒有 power_out —— 那就是從板外供電的
    pwr_in, pwr_out = set(), set()
    for des, pad, net in conns:
        et = syms[des]["types"].get(pad, "")
        if et == "power_in":
            pwr_in.add(net)
        elif et == "power_out":
            pwr_out.add(net)
    flags, fx = {}, snap(size[0] - 20.32)
    for i, net in enumerate(sorted(pwr_in - pwr_out)):
        flags[net] = (fx, snap(25.4 + i * 20.32))

    # 網路標籤：一個腳位一個，貼在腳位的連接點上
    placed = set()
    for des, pad, net in conns:
        info = syms[des]
        if pad not in info["pins"]:
            raise SystemExit("%s 的符號沒有腳位 %s" % (des, pad))
        if (des, pad) in placed:
            continue          # 同編號的多個焊盤（例如模組散熱墊）只放一次
        placed.add((des, pad))
        px, py, pa = info["pins"][pad]
        ox, oy = pos[des]
        x, y = snap(ox + px), snap(oy - py)
        ang = int((pa + 180) % 360)
        just = "right" if ang == 180 else "left"
        sch.append([Sym("label"), net, [Sym("at"), x, y, ang],
                    [Sym("fields_autoplaced"), Sym("yes")],
                    font(1.27, just), [Sym("uuid"), uid()]])

    # V1.3 本來就沒接的腳位：放 no-connect 記號，
    # 讓 ERC 知道是刻意的，而不是漏畫。
    for des, pad in open_pins:
        info = syms.get(des)
        if not info or pad not in info["pins"]:
            continue
        px, py, _ = info["pins"][pad]
        ox, oy = pos[des]
        sch.append([Sym("no_connect"),
                    [Sym("at"), snap(ox + px), snap(oy - py)],
                    [Sym("uuid"), uid()]])

    # 只有 power_in 腳、沒有任何 power_out 的網路（12V、GND 從連接器進來），
    # 補一個電源旗標告訴 ERC 電是從板外來的。
    for i, (net, (fx, fy)) in enumerate(sorted(flags.items())):
        ref = "#FLG%02d" % (i + 1)   # 電源旗標也要編號，否則 KiCad 視為未編號
        sch.append([Sym("symbol"),
                    [Sym("lib_id"), "TES:PWR_FLAG"],
                    [Sym("at"), fx, fy, 0],
                    [Sym("unit"), 1],
                    [Sym("exclude_from_sim"), Sym("yes")],
                    [Sym("in_bom"), Sym("no")],
                    [Sym("on_board"), Sym("no")],
                    [Sym("dnp"), Sym("no")],
                    [Sym("uuid"), uid()],
                    prop("Reference", ref, fx, snap(fy - 3.81)),
                    prop("Value", "PWR_FLAG", fx, snap(fy + 3.81)),
                    [Sym("instances"),
                     [Sym("project"), "TES_Controller",
                      [Sym("path"), "/" + root,
                       [Sym("reference"), ref], [Sym("unit"), 1]]]]])
        sch.append([Sym("label"), net, [Sym("at"), fx, fy, 270],
                    [Sym("fields_autoplaced"), Sym("yes")],
                    font(1.27, "left"), [Sym("uuid"), uid()]])

    sch.append([Sym("sheet_instances"),
                [Sym("path"), "/", [Sym("page"), "1"]]])
    sch.append([Sym("embedded_fonts"), Sym("no")])
    return sch


def export_netlist(path):
    """跑 kicad-cli 匯出網表，回傳 {(位號, 腳位): 網路名}。"""
    r = subprocess.run([KICAD_CLI, "sch", "export", "netlist",
                        "--format", "kicadsexpr", "--output", path, OUT],
                       capture_output=True, text=True, encoding="utf-8",
                       errors="replace")
    if not os.path.exists(path):
        sys.stderr.write((r.stdout or "") + (r.stderr or ""))
        raise SystemExit("匯出網表失敗")
    node = sexpr.load(path)
    got = {}
    for net in sexpr.findall(sexpr.find(node, "nets"), "net"):
        name = sexpr.find(net, "name")[1]
        name = name.lstrip("/")          # 根圖的區域標籤會被加上 "/" 前綴
        if name.startswith("unconnected-"):
            continue        # KiCad 給未接腳位取的假網路名，對應 V1.3 的「(未接)」
        for nd in sexpr.findall(net, "node"):
            got[(sexpr.find(nd, "ref")[1], sexpr.find(nd, "pin")[1])] = name
    return got, (r.stdout or "") + (r.stderr or "")


def verify():
    """把 KiCad 匯出的網表和 V1.3 的原始網表逐條比對。"""
    want = {}
    for des, pad, net in read_netlist()[0]:
        want[(des, pad)] = net
    tmp = os.path.join(os.environ.get("TEMP", "."), "tes_verify.net")
    got, log = export_netlist(tmp)

    missing = sorted(k for k in want if k not in got)
    extra = sorted(k for k in got if k not in want)
    wrong = sorted(k for k in want if k in got and got[k] != want[k])

    print()
    print("比對 %s" % os.path.relpath(os.path.join(SRC, "netlist.txt"), REPO))
    print("  原始接線 %d 條，KiCad 匯出 %d 條" % (len(want), len(got)))
    for label, items in (("漏接", missing), ("接錯", wrong), ("多出來", extra)):
        print("  %-4s %d" % (label, len(items)))
        for k in items[:15]:
            print("      %-10s pad %-6s 應為 %-14s 實際 %s"
                  % (k[0], k[1], want.get(k, "-"), got.get(k, "(無)")))
        if len(items) > 15:
            print("      ... 另外 %d 筆" % (len(items) - 15))
    ok = not (missing or wrong or extra)
    print("  → %s" % ("零差異，通過" if ok else "有差異，未通過"))
    if "annotation" in log:
        print("  註：kicad-cli 有回報 annotation 警告")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verify", action="store_true",
                    help="生成後匯出網表並和 netlist.txt 比對")
    a = ap.parse_args()

    bom = read_bom()
    conns, open_pins = read_netlist()
    _, by_lcsc, by_name = load_lib()
    syms = assign(bom, conns, by_lcsc, by_name)
    pos, headers, size = layout(syms, conns)
    sch = emit(syms, pos, headers, conns, open_pins, size, uid(), by_name)
    io.open(OUT, "w", encoding="utf-8").write(sexpr.dumps(sch) + "\n")
    print("%s：%d 元件、%d 條接線、%d 個網路"
          % (os.path.relpath(OUT, REPO), len(syms), len(conns),
             len({n for _, _, n in conns})))
    if a.verify:
        return verify()
    return 0


if __name__ == "__main__":
    sys.exit(main())
