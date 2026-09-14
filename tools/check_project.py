#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""檢查 KiCad 專案設定有沒有被改掉。

為什麼需要：KiCad 開著的時候如果我們在外面重新產生檔案，
KiCad 關閉時會把記憶體裡的舊狀態寫回 .kicad_pro 和 .kicad_sch，
把設定整個退回去。發生過兩次，而且**不會有任何錯誤訊息** ——
只會在下一次 DRC 時冒出幾百條莫名其妙的違規（舊的網路類別又活過來了）。

這支只比對幾個關鍵值，跑起來很快，適合每次 DRC 前先跑一次。
"""
import io, json, os, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRO = os.path.join(REPO, "hardware", "kicad", "TES_Controller.kicad_pro")

EXPECT = {
    "網路類別": (lambda d: [c["name"] for c in d["net_settings"]["classes"]],
                 ["Default"]),
    "一般間距": (lambda d: d["board"]["design_settings"]["rules"]["min_clearance"],
                 0.15),
    "板邊間距": (lambda d: d["board"]["design_settings"]["rules"]
                 .get("min_copper_edge_clearance"), 0.3),
    "外框重疊嚴重度": (lambda d: d["board"]["design_settings"]
                       .get("rule_severities", {}).get("courtyards_overlap"),
                       "warning"),
}


def main():
    d = json.load(io.open(PRO, encoding="utf-8"))
    bad = []
    for name, (get, want) in EXPECT.items():
        try:
            got = get(d)
        except Exception:
            got = "(讀不到)"
        if got != want:
            bad.append((name, want, got))
    if not bad:
        print("專案設定正常")
        return 0
    print("⚠ 專案設定被改掉了（多半是 KiCad 開著時我們動了檔案）：")
    for name, want, got in bad:
        print("   %-14s 應為 %-12s 實際 %s" % (name, want, got))
    print("   還原：git checkout hardware/kicad/TES_Controller.kicad_pro")
    return 1


if __name__ == "__main__":
    sys.exit(main())
