#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
V1.3 重新設計時的電氣變更，逐條宣告。

為什麼要這樣做：整條工具鏈的價值在於「KiCad 的網表 == V1.3 的原始網表」是
可驗證的。一旦開始改電路，零差異就不成立了 —— 如果只是放棄驗證，等於
把稽核能力整個丟掉。所以改成驗證「V1.3 網表 ＋ 這裡宣告的變更 == KiCad 網表」，
每一條變更都留下理由，diff 仍然看得出只動了該動的。

sch_gen.read_netlist() 會套用這些變更，所以電路圖、PCB 驗證、BOM
全部自動一致，不會有某一邊忘了改。
"""

# 目前選用的 TVS 都沿用元件庫裡既有的 SMBJ12A 符號與 SMB 封裝，
# 只改 Value —— 同系列同封裝同腳位，不必建新零件。
TVS_SYMBOL = "SMBJ12A_C908793"
TVS_FOOTPRINT = "TES:SMB_L4.6-W3.6-LS5.3-RD"

CHANGES = [
    dict(
        op="add_part", ref="D9", value="SMBJ130A",
        symbol=TVS_SYMBOL, footprint=TVS_FOOTPRINT,
        pins={"1": "120V", "2": "GND_BACK"},
        why="""120V 輸入突波箝位。

        V_RWM 130V 高於本板支援的最高 120V DC（TES 規格 120V 以內的機車），
        正常充電時不導通。跨接在 120V ↔ GND_BACK 之間而不是板子 GND，
        突波電流才留在高壓迴路裡，不會灌進訊號地。

        位置放 U10（120V 進板的螺絲端子）而不是 W1（模組接線焊盤）：
        兩者同一條網路，但突波從 U10 進來，中間隔著 64mm 走線約 50nH，
        放 W1 的話 U10 端會衝得比箝位值高，而 R10 正好掛在 U10 那一端。

        用 SMBJ（600W）而非 SMCJ（1500W）：實際的重複性應力是外部直流
        接觸器斷開時的線纜電感回衝，5m 線約 5uH、15A 下約 0.56 mJ，
        600W 綽綽有餘。這種能量下實際箝位落在 V_BR 附近（約 144-160V），
        遠低於資料表上滿額電流的 209V，模組那兩顆 160V 輸入電容安全。""",
    ),
    dict(
        op="add_part", ref="D10", value="SMBJ13A",
        symbol=TVS_SYMBOL, footprint=TVS_FOOTPRINT,
        pins={"1": "12V", "2": "GND"},
        why="""12V 軌突波箝位，放在 W2（模組 12V 進板處）。

        用 13V 不用 12V：SMBJ12A 的 V_RWM 正好就是 12.0V，等於工作電壓，
        降壓模組輸出含誤差與漣波可能到 12.6V，TVS 會在正常工作時漏電發熱。
        SMBJ13A 的 V_RWM 13V、V_BR 最低 14.4V 有合理餘裕，
        箝位 21.5V 也遠低於 U8 LMZM23601 的輸入耐壓。""",
    ),
]


def apply(conns, open_pins):
    """把變更套用到 (位號, 焊盤, 網路) 清單上。回傳新的清單與新增元件資料。"""
    conns = list(conns)
    added = {}
    for c in CHANGES:
        if c["op"] == "add_part":
            for pad, net in c["pins"].items():
                conns.append((c["ref"], pad, net))
            added[c["ref"]] = c
        elif c["op"] == "set_net":
            conns = [(d, p, c["net"] if (d, p) == (c["ref"], c["pad"]) else n)
                     for d, p, n in conns]
        elif c["op"] == "set_value":
            added.setdefault(c["ref"], {}).update(value=c["value"])
        else:
            raise SystemExit("不認得的變更 op: %s" % c["op"])
    return conns, added


def summary():
    out = []
    for c in CHANGES:
        if c["op"] == "add_part":
            pins = "  ".join("%s=%s" % (k, v) for k, v in sorted(c["pins"].items()))
            out.append("新增 %-4s %-10s %s" % (c["ref"], c["value"], pins))
        elif c["op"] == "set_net":
            out.append("改網路 %s.%s → %s" % (c["ref"], c["pad"], c["net"]))
        elif c["op"] == "set_value":
            out.append("改型號 %s → %s" % (c["ref"], c["value"]))
    return out


if __name__ == "__main__":
    print("V1.3 電氣變更 %d 條：" % len(CHANGES))
    for line in summary():
        print("  " + line)
