#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
修正元件庫的腳位電氣型別。

easyeda2kicad 轉出來的腳位幾乎都是 input 或 unspecified。電阻兩腳掛 input
的後果是：ERC 認為「兩個輸入接在一起、沒有任何輸出驅動」，於是每一條網路
都報錯（原始狀態 134 條 pin_to_pin + 22 條 pin_not_driven）。
這些不是設計問題，是符號的中繼資料不對。

分類方式刻意保守：兩端被動元件、連接器、按鍵、二極體一律 passive；
IC 只認得出電源腳時才改，其餘維持原樣 —— 猜錯型別會製造假的 ERC 結果，
比沒改還糟。
"""
import io, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sexpr
from sexpr import Sym as S

LIB = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "hardware", "kicad", "lib", "TES.kicad_sym")

# 電源輸入腳（IC 吃電的那一端）
PWR_IN = re.compile(r"^(GND|VSS|VSSA?|EP|EPAD|VDD|VDDA?|VCC|VIN|VBUS|VIO|"
                    r"3V3|5V|AVDD|DVDD)(_\d+)?$", re.I)
# 電源輸出腳（穩壓器 / 電源模組吐出來的那一端）
PWR_OUT = re.compile(r"^(VOUT|VO)(_\d+)?$", re.I)
# 數位 I/O：雙向最不會製造假警告（ESP32 的 IO 腳本來就可進可出）
BIDI = re.compile(r"^(IO\d+|GPIO\d+|SDA|SCL|TXD\d*|RXD\d*|U\d?[TR]XD|"
                  r"D[+-]|DP\d*|DN\d*|CANH|CANL)$", re.I)
# 只進不出
# EN 在 ESP32 模組和 LMZM23601 上都是輸入（致能腳），不是雙向
INP = re.compile(r"^(MODE|MODE_SYNC|ADDR|S|AIN\d+|FB|EN_?\d*)$", re.I)
# 開汲極輸出（拉低有效，外部上拉）
OD = re.compile(r"^(PGOOD|ALERT|ALERT/RDY|INT\d*|RDY)$", re.I)


def is_passive_part(name, pins):
    """兩端被動元件、連接器、按鍵、二極體 —— 全部腳位都是 passive。"""
    if re.match(r"^(\d{4}[WR]|CC\d{4}|CL\d{2}|RT\d{4}|RC\d{4}|FRP|FRC)", name):
        return True                       # 電阻電容的型號
    if name.startswith(("SS14", "SMBJ", "LED-5MM", "HDR-1X", "AO34")):
        return True
    if name.startswith(("B2PS", "S4B", "DG381", "DG301", "TYPE-C", "TS-")):
        return True
    return all(re.fullmatch(r"\d+", p) for p in pins) and len(pins) <= 3


def main():
    lib = sexpr.load(LIB)
    changed = {}
    for sym in sexpr.findall(lib, "symbol"):
        name = sym[1]
        pins = []
        for sub in sexpr.findall(sym, "symbol"):
            pins += sexpr.findall(sub, "pin")
        names = [sexpr.find(p, "name")[1] for p in pins]
        passive_part = is_passive_part(name, names)
        seen_out = set()
        for p in pins:
            pname = sexpr.find(p, "name")[1]
            old = str(p[1])
            if passive_part:
                new = "passive"
            elif PWR_OUT.match(pname):
                # 模組常把同一路輸出拉到多個焊盤（例如 LMZM23601 的 VOUT x2）。
                # 兩個 power_out 接在一起 ERC 會報錯，所以只留第一個。
                new = "power_out" if pname.upper() not in seen_out else "passive"
                seen_out.add(pname.upper())
            elif PWR_IN.match(pname):
                new = "power_in"
            elif OD.match(pname):
                new = "open_collector"
            elif BIDI.match(pname):
                new = "bidirectional"
            elif INP.match(pname):
                new = "input"
            elif old == "unspecified":
                new = "passive"     # 剩下認不出來的，用最不會製造假警告的型別
            else:
                continue
            if new != old:
                p[1] = S(new)
                changed[name] = changed.get(name, 0) + 1
    io.open(LIB, "w", encoding="utf-8").write(sexpr.dumps(lib) + "\n")
    print("調整 %d 個符號、%d 個腳位" % (len(changed), sum(changed.values())))
    for n in sorted(changed):
        print("   %-26s %d" % (n, changed[n]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
