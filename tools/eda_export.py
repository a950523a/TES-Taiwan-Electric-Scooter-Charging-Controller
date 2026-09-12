#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 EasyEDA Pro 專案匯出成可進版控、可 diff 的文字檔。

為什麼需要這個：
  .eprj 是 SQLite 二進位檔。它「能」進 git，但看不出兩版之間改了什麼，
  而且每次存檔就是一顆 2MB 全新 blob（無法差異壓縮）。
  專案內部的 .esch / .epcb 其實是逐行 JSON —— 解出來就能正常 diff。

產出（每個版本一個目錄）：
  hardware/<版本>/raw/         原始 .esch / .epcb / .esym（逐行 JSON，可 diff）
  hardware/<版本>/bom.csv      位號 → 型號（改了什麼料，一眼看出）
  hardware/<版本>/netlist.txt  位號.腳位 → 網路（改了什麼接線，一眼看出）
  hardware/<版本>/rules.txt    設計規則摘要（間距、線寬、過孔）

後兩者是重點：raw JSON 的 diff 充滿內部 id 雜訊，
bom.csv 與 netlist.txt 是穩定排序的衍生檔，版本之間的差異清清楚楚。

用法：
  python tools/eda_export.py docs/PCB/TES_Controller_V1.3.eprj
  python tools/eda_export.py --all            # 匯出 docs/PCB 下所有 .eprj
"""
import argparse, base64, collections, csv, gzip, io, json, os, re, sqlite3, sys


def decode_datastr(s):
    """documents.dataStr 是 'base64' 前綴 + base64(gzip(逐行 JSON))。

    舊專案可能直接存明文，所以沒有前綴時原樣回傳。
    """
    if not s:
        return ""
    if s.startswith("base64"):
        raw = base64.b64decode(s[len("base64"):])
        try:
            return gzip.decompress(raw).decode("utf-8", "replace")
        except OSError:
            return raw.decode("utf-8", "replace")
    return s

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PCB_DIR = os.path.join(REPO, "docs", "PCB")
OUT_ROOT = os.path.join(REPO, "hardware")


def jsonl(text):
    """逐行 JSON → 記錄清單。壞行直接跳過（EasyEDA 偶爾夾雜空行）。"""
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except Exception:
            pass
    return out


def read_project(eprj_path):
    """從 .eprj (SQLite) 取出文件內容與裝置資料。

    documents 表存放 schematic/PCB 的 dataStr；devices/attributes 是料號資訊。
    備份 zip 也能用，但 .eprj 才是當前狀態，優先用它。
    """
    con = sqlite3.connect("file:%s?mode=ro" % eprj_path.replace("\\", "/"), uri=True)
    docs = []
    for uuid, title, dtype, data in con.execute(
            "SELECT uuid, COALESCE(display_title, title), docType, dataStr FROM documents"):
        text = decode_datastr(data)
        if text:
            docs.append(dict(uuid=uuid, title=title or uuid, dtype=dtype, data=text))
    devices = {u: (t or dt or "") for u, t, dt in
               con.execute("SELECT uuid, title, display_title FROM devices")}
    attrs = collections.defaultdict(dict)
    for k, v, du in con.execute("SELECT key, value, device_uuid FROM attributes"):
        attrs[du][k] = v
    con.close()
    return docs, devices, attrs


def free_pads(pcb_recs):
    """自由焊盤 —— 直接畫在銅箔上、不屬於任何元件的焊盤。

    V1.3 用它們當降壓模組的接線點（120V / GND_BACK 出去，12V / GND 回來）。
    因為不是元件，BOM 和逐元件網表都看不到它們，第一版抽取程式就漏了 ——
    結論變成「板子沒有 12V 落點」，但其實一直都在。

    給它們 W1..Wn 的位號（依座標排序，穩定），這樣網表才是完整的。
    """
    pads = []
    for r in pcb_recs:
        if r and r[0] == "PAD" and len(r) > 8 and r[3]:
            pads.append((float(r[6]), -float(r[7]), str(r[5]), r[3]))
    pads.sort(key=lambda t: (t[1], t[0]))
    return {"W%d" % (i + 1): (pad, net)
            for i, (_, _, pad, net) in enumerate(pads)}


def build_model(docs, devices, attrs):
    """把電路圖與 PCB 的記錄組成位號 → 料號 / 網路的對照。"""
    sch_recs, pcb_recs = [], []
    for d in docs:
        recs = jsonl(d["data"])
        # docType 1 = 電路圖分頁，3 = PCB。以 DOCTYPE 記錄複核，兩者不符時以內容為準。
        head = recs[0] if recs else []
        is_sch = (head[:2] == ["DOCTYPE", "SCH"]) or (d["dtype"] == 1 and head[:1] != ["DOCTYPE"])
        (sch_recs if is_sch else pcb_recs).extend(recs)

    sa = collections.defaultdict(dict)
    for r in sch_recs:
        if r and r[0] == "ATTR" and len(r) > 4:
            sa[r[2]][r[3]] = r[4]
    uid2 = {}
    for owner, a in sa.items():
        if a.get("Unique ID"):
            uid2[a["Unique ID"]] = (a.get("Designator"), a.get("Device"))

    cid2des, bom = {}, {}
    for r in pcb_recs:
        if r and r[0] == "COMPONENT" and len(r) > 7 and isinstance(r[7], dict):
            d = uid2.get(r[7].get("Unique ID"))
            if d and d[0]:
                cid2des[r[1]] = d[0]
                a = attrs.get(d[1], {})
                bom[d[0]] = dict(
                    device=devices.get(d[1], ""),
                    part=a.get("Manufacturer Part") or a.get("Value") or "",
                    value=a.get("Resistance") or a.get("Capacitance") or a.get("Value") or "",
                    supplier=a.get("Supplier Part", ""),
                )
    netlist = collections.defaultdict(list)
    for r in pcb_recs:
        if r and r[0] == "PAD_NET" and len(r) >= 4:
            des = cid2des.get(r[1])
            if des:
                netlist[des].append((str(r[2]), r[3]))
    for des, (pad, net) in free_pads(pcb_recs).items():
        netlist[des].append((pad, net))
        bom[des] = dict(device="solder wire pad", part="SOLDERPAD-1P",
                        value="", supplier="")
    return bom, netlist, pcb_recs


def natural(des):
    """R10 排在 R9 後面，不是 R1 後面。"""
    m = re.match(r"([A-Za-z_]*)(\d*)", des or "")
    return (m.group(1), int(m.group(2)) if m.group(2) else 0, des or "")


def write_bom(path, bom):
    with io.open(path, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["designator", "part", "value", "device", "supplier"])
        for des in sorted(bom, key=natural):
            b = bom[des]
            w.writerow([des, b["part"], b["value"], b["device"], b["supplier"]])


def write_netlist(path, netlist):
    lines = []
    for des in sorted(netlist, key=natural):
        for pad, net in sorted(netlist[des], key=lambda t: natural(t[0])):
            lines.append("%-10s pad %-4s %s" % (des, pad, net or "(未接)"))
    lines.append("")
    lines.append("# 依網路彙整")
    bynet = collections.defaultdict(list)
    for des, pads in netlist.items():
        for pad, net in pads:
            if net:
                bynet[net].append("%s.%s" % (des, pad))
    for net in sorted(bynet):
        lines.append("%-14s %s" % (net, "  ".join(sorted(bynet[net], key=natural))))
    io.open(path, "w", encoding="utf-8").write("\n".join(lines) + "\n")


def write_rules(path, pcb_recs):
    """間距/線寬/過孔規則。高壓間距被改動時要看得出來。"""
    lines = []
    for r in pcb_recs:
        if r and r[0] == "RULE":
            lines.append("RULE %-22s %s" % (r[2], json.dumps(r[4], ensure_ascii=False)))
    lines.append("")
    sel = collections.defaultdict(list)
    for r in pcb_recs:
        if r and r[0] == "RULE_SELECTOR" and isinstance(r[1], list) and len(r[1]) > 1:
            sel[r[2]].append(r[1][1])
    for cls in sorted(sel, key=str):
        nets = sorted(n for n in sel[cls] if n)
        lines.append("CLASS %-4s (%d nets) %s" % (cls, len(nets), ", ".join(nets)))
    lines.append("")
    for r in pcb_recs:
        if r and r[0] == "POUR":
            lines.append("POUR net=%s layer=%s clearance=%s name=%s" % (r[3], r[4], r[5], r[6]))
    io.open(path, "w", encoding="utf-8").write("\n".join(lines) + "\n")


def export(eprj_path):
    name = os.path.splitext(os.path.basename(eprj_path))[0]
    out = os.path.join(OUT_ROOT, name)
    raw = os.path.join(out, "raw")
    # 先清空：文件改名或刪除時，殘留的舊檔會被誤當成現況提交進版控
    if os.path.isdir(raw):
        for f in os.listdir(raw):
            os.remove(os.path.join(raw, f))
    os.makedirs(raw, exist_ok=True)

    docs, devices, attrs = read_project(eprj_path)
    for d in docs:
        safe = re.sub(r"[^\w.\-]+", "_", d["title"])[:60] or d["uuid"][:8]
        ext = ".esch" if d["dtype"] == 1 else ".epcb"
        io.open(os.path.join(raw, safe + ext), "w", encoding="utf-8").write(d["data"])

    bom, netlist, pcb_recs = build_model(docs, devices, attrs)
    write_bom(os.path.join(out, "bom.csv"), bom)
    write_netlist(os.path.join(out, "netlist.txt"), netlist)
    write_rules(os.path.join(out, "rules.txt"), pcb_recs)
    print("%s → %s  (%d 文件, %d 元件, %d 有網路的元件)"
          % (os.path.basename(eprj_path), os.path.relpath(out, REPO),
             len(docs), len(bom), len(netlist)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("eprj", nargs="*", help=".eprj 檔案路徑")
    ap.add_argument("--all", action="store_true", help="匯出 docs/PCB 下所有 .eprj")
    a = ap.parse_args()

    targets = list(a.eprj)
    if a.all or not targets:
        targets = sorted(os.path.join(PCB_DIR, f)
                         for f in os.listdir(PCB_DIR) if f.endswith(".eprj"))
    if not targets:
        print("找不到 .eprj", file=sys.stderr)
        return 1
    for t in targets:
        export(t)
    return 0


if __name__ == "__main__":
    sys.exit(main())
