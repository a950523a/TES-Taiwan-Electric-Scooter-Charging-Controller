#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 V1.3 的 EasyEDA Pro 專案匯入成 KiCad PCB，並正規化成和已驗證的電路圖一致。

為什麼走這條路：`kicad-cli pcb import --format auto` 認得 EasyEDA Pro 的
專案 zip（雖然說明裡沒列出來），匯進來是**已經繞好線的板子** ——
77 個封裝、545 條走線、板框與安裝孔都在。從這裡改進，比從空板重繞
49 條網路實際得多，也保留了已經驗證過能動的繞法。

匯入之後要做三件事才會和 tools/sch_gen.py 產生的電路圖對得起來：
  1. 位號改名（G/R/Y → D6/D7/D8 等，見 sch_gen.DESIGNATOR_RENAME）
  2. 網路改名（$1N24345 → VOUT_SENSE 等，見 sch_gen.NET_RENAME）
  3. 四個自由焊盤補上位號 W1..W4

備份 zip 是 2026-05-07 的，和當前 .eprj 只差兩個焊盤：C20.1 與 U5.8，
備份還接在 VDD33，當前已改成 5V（ADS1115 供電從 3.3V 改 5V，
修正量測上限只到 114V 的問題）。這裡會補上，並拆掉因此懸空的走線。

用法：
  python tools/pcb_import.py            # 匯入 + 正規化 + 驗證
  python tools/pcb_import.py --verify   # 只驗證現有的 .kicad_pcb
"""
import argparse, io, os, re, shutil, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sch_gen import (DESIGNATOR_RENAME, NET_RENAME, KICAD_CLI, REPO, SRC,
                     read_netlist)                         # noqa: E402

ZIP = os.path.join(REPO, "docs", "PCB", "TES_Controller_V1.3_backup",
                   "TES_Controller_V1.3_v675_2026-05-07-22-34.zip")
OUT = os.path.join(REPO, "hardware", "kicad", "TES_Controller.kicad_pcb")
KICAD_PY = r"C:\Program Files\KiCad\10.0\bin\python.exe"

# 備份 zip 落後於當前 .eprj 的部分。值是應該要有的網路。
BACKUP_DELTA = {("C20", "1"): "5V", ("U5", "8"): "5V"}


def run_kicad_py(code):
    """在 KiCad 自帶的 python 跑一段程式（pcbnew 只有那裡有）。"""
    with tempfile.NamedTemporaryFile("w", suffix=".py", delete=False,
                                     encoding="utf-8") as f:
        f.write(code)
        path = f.name
    try:
        env = dict(os.environ, PYTHONIOENCODING="utf-8")
        r = subprocess.run([KICAD_PY, path], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", env=env,
                           cwd=REPO)
        if r.returncode != 0:
            sys.stderr.write((r.stdout or "") + (r.stderr or ""))
            raise SystemExit("pcbnew 腳本失敗（exit %d）" % r.returncode)
        return r.stdout or ""
    finally:
        os.unlink(path)


def normalize(src, dst):
    """跑 _pcb_normalize.py（位號 / 自由焊盤 / 落後的兩個焊盤）。"""
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "_pcb_normalize.py")
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    r = subprocess.run([KICAD_PY, script, src, dst], capture_output=True,
                       text=True, encoding="utf-8", errors="replace",
                       env=env, cwd=REPO)
    sys.stdout.write(r.stdout or "")
    if r.returncode != 0:
        sys.stderr.write(r.stderr or "")
        raise SystemExit("正規化失敗")


def rename_nets(path):
    """網路改名。直接改檔案文字 —— EasyEDA 的 $1N... 是唯一字串，
    加上引號比對不會誤傷，比繞 pcbnew 的網路表 API 乾淨。"""
    t = io.open(path, encoding="utf-8").read()
    n = 0
    for old, new in NET_RENAME.items():
        needle = '"%s"' % old
        if needle in t:
            n += t.count(needle)
            t = t.replace(needle, '"%s"' % new)
    io.open(path, "w", encoding="utf-8").write(t)
    print("網路改名 %d 處（%d 條網路）" % (n, len(NET_RENAME)))


def verify(path):
    """板子的焊盤網路對 V1.3 原始網表逐條比對。"""
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "_pcb_verify.py")
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    r = subprocess.run([KICAD_PY, script, path], capture_output=True,
                       text=True, encoding="utf-8", errors="replace",
                       env=env, cwd=REPO)
    sys.stdout.write(r.stdout or "")
    if r.returncode != 0:
        sys.stderr.write(r.stderr or "")
    return r.returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verify", action="store_true", help="只驗證現有的 .kicad_pcb")
    a = ap.parse_args()
    if a.verify:
        return verify(OUT)

    tmp = os.path.join(tempfile.gettempdir(), "tes_import.kicad_pcb")
    print("1/4 匯入 %s" % os.path.basename(ZIP))
    r = subprocess.run([KICAD_CLI, "pcb", "import", "--format", "auto",
                        "--output", tmp, ZIP],
                       capture_output=True, text=True, encoding="utf-8",
                       errors="replace")
    if not os.path.exists(tmp):
        sys.stderr.write((r.stdout or "") + (r.stderr or ""))
        raise SystemExit("匯入失敗")
    print("2/4 正規化")
    normalize(tmp, OUT)
    print("3/4 網路改名")
    rename_nets(OUT)
    print("4/4 驗證")
    return verify(OUT)


if __name__ == "__main__":
    sys.exit(main())
