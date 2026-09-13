#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 .kicad_pcb 裡的封裝匯出成 .kicad_mod，寫進 TES.pretty。

用 s-expression 直接處理，不走 pcbnew 的 FootprintSave（那支的參數型別
在這個版本對不上）。順便把實例專屬的欄位剝掉：位號改回 REF**、
拿掉座標、網路、uuid、path —— 否則存進元件庫的會是「R19 在某座標上」
而不是一個可重用的封裝。

為什麼要以板上的為準：EasyEDA Pro 的專案 zip 帶了通用元件的封裝
（5mm LED、2.54 排針、焊接跳線），而 easyeda2kicad 走 LCSC 料號那條路
看不到它們。兩份不一樣 —— LED 的 1 腳左右相反、孔徑 0.90 vs 1.00。
板上那份是 V1.1／V1.2 實際做出來、焊過的，所以它才是對的。

用法：python tools/pcb_export_fp.py <board.kicad_pcb> [封裝名 ...]
"""
import io, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sexpr
from sexpr import Sym as S

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRETTY = os.path.join(REPO, "hardware", "kicad", "lib", "TES.pretty")
DROP = {"at", "uuid", "path", "net", "sheetname", "sheetfile", "net_tie_pad_groups"}


def strip(node, top=True):
    """剝掉實例專屬欄位。封裝本體的 (at ...) 要拿掉，但子元素的座標要留。"""
    out = []
    for c in node:
        if not isinstance(c, list) or not c:
            out.append(c)
            continue
        tag = str(c[0])
        if top and tag in ("at", "path", "uuid", "sheetname", "sheetfile"):
            continue
        if tag == "net":
            continue
        if tag == "property" and len(c) > 2 and c[1] == "Reference":
            c = list(c)
            c[2] = "REF**"
        out.append(strip(c, top=False) if isinstance(c, list) else c)
    return out


def main():
    board = sys.argv[1]
    want = set(sys.argv[2:])
    root = sexpr.load(board)
    have = {n[:-len(".kicad_mod")] for n in os.listdir(PRETTY)
            if n.endswith(".kicad_mod")}
    done = set()
    for node in sexpr.findall(root, "footprint"):
        name = str(node[1]).rpartition(":")[2]
        if name in done or (want and name not in want):
            continue
        # .kicad_mod 需要 version / generator 這幾個標頭，
        # 板檔裡的 footprint 節點沒有 —— 少了就是 "Unable to load library"
        fp = ([S("footprint"), name,
               [S("version"), S("20241229")],
               [S("generator"), "tes_pcb_export_fp"],
               [S("generator_version"), "10.0"]]
              + strip(node[2:]))
        # 位置歸零：把整個封裝平移回原點才是可重用的庫元件
        io.open(os.path.join(PRETTY, name + ".kicad_mod"), "w",
                encoding="utf-8").write(sexpr.dumps(fp) + "\n")
        done.add(name)
        print("   %s %s" % ("覆寫" if name in have else "新增", name))
    print("匯出 %d 個封裝" % len(done))


if __name__ == "__main__":
    main()
