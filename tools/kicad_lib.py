#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
依 BOM 的 LCSC 料號重建 KiCad 元件庫（符號 / 封裝 / 3D 模型）。

符號與封裝會進版控（約 200 KB）；3D 模型不會（約 39 MB，且完全可重新產生）。
換到新機器或要看 3D 時跑一次就好。

前置：
  python -m venv C:\\Users\\<你>\\.venvs\\eda
  C:\\Users\\<你>\\.venvs\\eda\\Scripts\\python -m pip install easyeda2kicad

用法：
  python tools/kicad_lib.py                       # 用 V1.3 的 BOM
  python tools/kicad_lib.py --bom hardware/TES_Controller_V1.4/bom.csv
  python tools/kicad_lib.py --no-3d               # 只要符號與封裝
"""
import argparse, csv, io, os, re, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BOM = os.path.join(REPO, "hardware", "TES_Controller_V1.3", "bom.csv")
OUT_SYM = os.path.join(REPO, "hardware", "kicad", "lib", "TES.kicad_sym")
VENV_PY = os.path.expanduser(r"~\.venvs\eda\Scripts\python.exe")


def lcsc_ids(bom_path):
    ids, skipped = [], []
    with io.open(bom_path, encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            sup = (row.get("supplier") or "").strip()
            if re.fullmatch(r"C\d+", sup):
                if sup not in ids:
                    ids.append(sup)
            else:
                skipped.append((row.get("designator", "?"), row.get("device", "")))
    return ids, skipped


def relocate_3d_paths(pretty_dir):
    """easyeda2kicad 把 3D 模型寫成絕對路徑，換台機器就全部失效。
    改成 ${KIPRJMOD} 相對路徑（專案檔位於 hardware/kicad/）。"""
    marker = "/lib/TES.3dshapes/"
    n = 0
    if not os.path.isdir(pretty_dir):
        return 0
    for name in os.listdir(pretty_dir):
        if not name.endswith(".kicad_mod"):
            continue
        f = os.path.join(pretty_dir, name)
        t = io.open(f, encoding="utf-8").read()
        out, hit = [], False
        for line in t.splitlines(True):
            i = line.find(marker)
            if i >= 0 and '"' in line:
                q = line.index('"')
                line = line[:q + 1] + "${KIPRJMOD}" + line[i:]
                hit = True
            out.append(line)
        if hit:
            io.open(f, "w", encoding="utf-8").write("".join(out))
            n += 1
    return n


# 嘉立創給 C20917（AOS AO3400A）配的 SOT-23-3 封裝，焊盤 1、2 的編號和
# JEDEC TO-236 相反 —— 俯視應該是 1→2→3 逆時針，它是順時針，所以標成
# 「1」的那個焊盤實際上落在晶片的腳 2（Source）位置上。
#
# 同一個庫裡另外五種封裝（MSOP-10、SOIC-8、SOT-23-6、SOT-223，以及
# AO3401A 用的 SOT-23）全部是逆時針，功能也都對得上（TJA1051 腳1=TXD、
# AMS1117 腳1=GND、USBLC6 腳2=GND），所以錯的是這一顆封裝。
#
# 板子是照著這個編號畫的、也已經驗證過，所以封裝維持原樣，改成讓**符號**
# 跟著它 —— 原本 EasyEDA 的稿子就是這樣處理的（用 AO3400A-MS 這顆
# 1、2 腳相反的符號）。照嘉立創原本的符號畫，閘極會被接到 GND、源極吃
# 閘極驅動，MOSFET 永遠不導通。
PIN_RENUMBER = {"AO3400A": {"G": "2", "S": "1", "D": "3"}}


def renumber_pins(sym_path):
    """把 PIN_RENUMBER 列的符號腳位改號，回傳改動的符號數。"""
    if not os.path.exists(sym_path):
        return 0
    lines = io.open(sym_path, encoding="utf-8").read().splitlines(True)
    cur, name, n = None, None, 0
    touched = set()
    for i, ln in enumerate(lines):
        t = ln.strip()
        m = re.fullmatch(r'"([^"]+)"', t)
        if m and i and lines[i - 1].strip() == "(symbol":
            base = m.group(1).rsplit("_", 2)[0]
            cur = base if base in PIN_RENUMBER else None
        if cur and t.startswith('(name "'):
            name = t.split('"')[1]
        elif cur and name and t.startswith('(number "'):
            want = PIN_RENUMBER[cur].get(name)
            if want and t.split('"')[1] != want:
                lines[i] = ln.replace('(number "%s"' % t.split('"')[1],
                                      '(number "%s"' % want, 1)
                touched.add(cur)
            name = None
    if touched:
        io.open(sym_path, "w", encoding="utf-8").write("".join(lines))
    return len(touched)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bom", default=DEFAULT_BOM)
    ap.add_argument("--no-3d", action="store_true", help="跳過 3D 模型（省 39 MB 與下載時間）")
    ap.add_argument("--python", default=VENV_PY, help="裝了 easyeda2kicad 的 python")
    a = ap.parse_args()

    if not os.path.exists(a.python):
        print("找不到 %s —— 請先依檔頭說明建立 venv 並安裝 easyeda2kicad" % a.python,
              file=sys.stderr)
        return 1

    ids, skipped = lcsc_ids(a.bom)
    print("BOM: %s" % os.path.relpath(a.bom, REPO))
    print("LCSC 料號 %d 個" % len(ids))
    if skipped:
        print("無 LCSC 料號、需用 KiCad 內建庫替代的元件：")
        for des, dev in skipped:
            print("   %-6s %s" % (des, dev))

    os.makedirs(os.path.dirname(OUT_SYM), exist_ok=True)
    cmd = [a.python, "-m", "easyeda2kicad",
           "--symbol", "--footprint", "--overwrite",
           "--output", OUT_SYM, "--lcsc_id"] + ids
    if not a.no_3d:
        cmd.insert(4, "--3d")

    # easyeda2kicad 會在 cwd 建 .easyeda_cache，內含中文字在 cp950 下寫入會失敗
    # （只是警告，不影響轉換）。這裡不開快取，避免在 repo 裡留下暫存目錄。
    r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    # easyeda2kicad 把進度訊息寫到 stderr，不是 stdout
    log = (r.stdout or "") + (r.stderr or "")
    created = len(re.findall(r"Created Kicad symbol", log))
    fails = re.findall(r"\[ERROR\].*", log)
    print("產生符號 %d 個" % created)
    for f in fails:
        print("  " + f)
    print("腳位編號修正：%d 個符號（見 PIN_RENUMBER 的說明）"
          % renumber_pins(OUT_SYM))
    print("3D 路徑改為相對：%d 個封裝"
          % relocate_3d_paths(os.path.join(os.path.dirname(OUT_SYM), "TES.pretty")))
    cache = os.path.join(REPO, ".easyeda_cache")
    if os.path.isdir(cache):
        for f in os.listdir(cache):
            os.remove(os.path.join(cache, f))
        os.rmdir(cache)
    return 0 if created else 1


if __name__ == "__main__":
    sys.exit(main())
