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
# 0603 電阻共用同一個符號與封裝，只有 Value 不同 —— 和 TVS 一樣不必建新零件。
R_SYMBOL = "0603WAF1002T5E"
R_FOOTPRINT = "TES:R0603"

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

    # ---------------------------------------------------------- R10 分壓
    dict(
        op="set_value", ref="R10", value="118k",
        why="""120V 分壓上臂拆成三顆串聯，這是第一顆。

        原本是單顆 348k 的 0603，在 120V 輸入下要承受 116V ——
        0603 的額定工作電壓是 75V，超了 1.5 倍。這不是「裕度不足」，
        是長期跑在規格外，電阻會漂移甚至崩潰，而它是量測電壓的上臂，
        一漂移整個電壓讀值就錯，而且錯的方向是**讀得比實際低**
        （上臂阻值變大 → 分壓變小），車端看到的電壓因此偏低。

        118k + 115k + 115k = 348k，剛好等於原值，所以分壓比不變、
        AIN0 在 120V 時仍是 4.0000 V，**韌體的換算常數完全不用動**。
        三顆都是 E96 標準值。每顆承受 38–39V，對 75V 有 1.9 倍餘裕，
        功耗 13 mW 對 0603 的 100 mW 也很寬鬆。""",
    ),
    dict(
        op="add_part", ref="R33", value="115k",
        symbol=R_SYMBOL, footprint=R_FOOTPRINT,
        pins={"1": "HV_DIV2", "2": "HV_DIV1"},
        why="R10 分壓上臂的第二顆（見 R10 的說明）。",
    ),
    dict(
        op="add_part", ref="R34", value="115k",
        symbol=R_SYMBOL, footprint=R_FOOTPRINT,
        pins={"1": "VOUT_SENSE", "2": "HV_DIV2"},
        why="R10 分壓上臂的第三顆，接到分壓中點 VOUT_SENSE（見 R10 的說明）。",
    ),
    dict(
        op="set_net", ref="R10", pad="1", net="HV_DIV1",
        why="R10 原本直接接分壓中點，現在改接到串聯的下一顆 R33。",
    ),

    # ---------------------------------------------------------- 按鍵
    # 這裡本來有一項「修正 START 去彈跳電容接點」，已撤銷。
    #
    # V1.3 是 U1.35 接 START1 的 1 腳、C13 接 2 腳，看起來和其他三顆按鍵
    # （電容與 MCU 接同一腳）不一致，我原本判斷電容是經由按鍵內部接點
    # 才到 MCU，濾在接觸電阻的後面。
    #
    # 那是錯的。TS-1187A 是 4 腳輕觸開關，1、2 腳在封裝內實體相連，
    # 3、4 腳也是，會彈跳的接點在 {1,2} 與 {3,4} 之間 ——
    # 網表可以印證：START1.3 與 START1.4 都接 GND。
    # 所以 C13 和 U1.35 本來就在同一個實體節點上，中間沒有接觸電阻，
    # 和其他按鍵電氣上完全等價。改了只會多繞 7.4 mm 的線，沒有任何好處。
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
        elif c["op"] == "disconnect":
            conns = [t for t in conns if (t[0], t[1]) != (c["ref"], c["pad"])]
            open_pins.append((c["ref"], c["pad"]))
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
        elif c["op"] == "disconnect":
            out.append("斷開 %s.%s（標為未接）" % (c["ref"], c["pad"]))
    return out


if __name__ == "__main__":
    print("V1.3 電氣變更 %d 條：" % len(CHANGES))
    for line in summary():
        print("  " + line)
