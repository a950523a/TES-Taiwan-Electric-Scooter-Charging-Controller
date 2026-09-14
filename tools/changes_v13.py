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
        op="add_part", ref="D10", value="SMBJ12A",
        symbol=TVS_SYMBOL, footprint=TVS_FOOTPRINT,
        pins={"1": "12V", "2": "GND"},
        track_width=0.8,   # .kicad_dru 的「12V 幹線最小寬度」是 0.75mm，
                           # 用預設的 0.4 會直接違規
        why="""12V 軌突波箝位，放在 W2（模組 12V 進板處）。

        **這顆保護的是實際發生過的故障。** 現場的症狀是：熱插 120V 的瞬間，
        降壓模組和 MOSFET 一起燒毀，外觀完全無損傷，而且事後量測模組輸出
        是 0V。輸出 0V 排除了「模組輸入對輸出短路、把 120V 灌進 12V 軌」
        —— 12V 軌從來沒有停在 120V 過。真正的路徑是耦合過來的暫態：

        非隔離降壓模組輸入與輸出共地，熱插時輸入端 LC 過衝（線纜電感加
        模組輸入電容，可近兩倍），這個快緣經高側開關的 C_oss 耦合到切換
        節點、再經電感灌到輸出端。即使模組的控制 IC 當下就被輸入過壓打死、
        輸出從此不再有電壓，那一瞬間的尖波已經送到 12V 軌上了。板上四顆
        MOSFET 全是 V_DS 30V 的料，扛不住。

        「有限能量的快速尖波」正是 TVS 的守備範圍，所以這顆要留。
        （反過來說，如果真的是持續 120V，600W 的 TVS 擋不住，板上也沒有
        保險絲可以熔 —— 那種情況要靠 D9 在源頭擋掉。）

        用 SMBJ12A 而不是 SMBJ13A，兩個理由：

        1. **箝位更低。** SMBJ12A 的 V_C 是 19.9V，SMBJ13A 是 21.5V。
           對 30V 的 MOSFET，餘裕從 8.5V 變成 10.1V。
        2. **不增加料號。** D3 已經是 SMBJ12A，而且它跨接的 VP 就是 Q4
           從 12V 切出去的 —— 板上早就有一顆 SMBJ12A 在 12V 軌上工作。

        原本排除 SMBJ12A 的理由是「V_RWM 12.0V 等於工作電壓，漣波到 12.6V
        會漏電發熱」。這個顧慮對 D3 一模一樣成立，而 D3 已經在板上了；
        12.6V 下的漏電是次毫安級、幾個 mW，熱上不成問題。""",
    ),

    # ------------------------------------------------- Q4 閘極耐壓（設計瑕疵）
    dict(
        op="add_part", ref="R35", value="0603WAF1002T5E",
        symbol=R_SYMBOL, footprint=R_FOOTPRINT,
        pins={"1": "VP_PGATE", "2": "VP_PGATE_DRV"},
        why="""Q4 的 V_GS 原本踩在絕對最大額定上。這是設計瑕疵，不是餘裕不足。

        原電路：R26（10k）把 Q4 的閘極拉到源極（12V）保持關閉，要導通時
        Q2 把閘極**直接拉到 GND**。於是導通時

            V_GS(Q4) = 0V − 12V = −12.0V

        而 AOS 的 AO3401A 資料表寫得很清楚：V_DS = −30V、**V_GS = ±12V**。
        −12.0V 剛好等於絕對最大值，餘裕是零。降壓模組輸出含誤差與漣波實際
        會到 12.6V，那就是 −12.6V，**超出額定 5%**。

        絕對最大額定不是「可以這樣用」，是「超過就開始損傷」。閘極氧化層的
        劣化是累積性的，最後 punch-through —— 症狀就是偶發、換一顆就好、
        外觀完好，和現場看到的一致。

        修法：在 Q2 汲極與 Q4 閘極之間插這顆 10k，與 R26 形成分壓：

            V_GS(on) = −12V × 10k/(10k+10k) = −6V      餘裕變成 2 倍

        AO3401A 在 −4.5V 就完全導通（R_DS(on) 60mΩ），而 VP 只驅動車端的
        訊號線、電流很小，−6V 綽綽有餘。10k 是板上已有的料號（R17、R20、
        R26、R28 都是），不增加任何成本。

        導通變慢的代價：10k 配 C_iss 645pF 約 6.5us。VP 是繼電器等級的
        切換，無關緊要。""",
    ),
    dict(
        op="set_net", ref="Q2", pad="3", net="VP_PGATE_DRV",
        why="""Q2 的汲極原本直接接 Q4 的閘極，現在改接到 R35 的另一端，
        原本的單一節點就此分成「閘極側 VP_PGATE」與「驅動側 VP_PGATE_DRV」。
        理由見 R35。""",
    ),

    # ------------------------------------------------------------ 硬體版本識別
    dict(
        op="add_part", ref="U13", value="24C02S",
        symbol="24C02S", footprint="TES:SOT-23-5_L3.0-W1.6-P0.95-LS2.8-BR",
        at=(158.00, 86.75, 0),   # 位置是挑的，不是搜出來的 —— 見
                                 # _pcb_apply_changes.py 裡 spec["at"] 的說明。
                                 # 這裡距最近的 I2C 腳位 13.5mm，而且四周有
                                 # 6.5x6.5mm 的淨空讓三支訊號腳出得來
        track_width=0.25,   # SOT-23-5 焊盤 0.6mm 寬、間距 0.95mm，相鄰間隙只有
                            # 0.35mm，0.4mm 的預設線寬 escape 不出來。I2C 是
                            # 訊號級，0.25mm 綽綽有餘
        pins={"1": "SCL", "2": "GND", "3": "SDA", "4": "VDD33", "5": "GND"},
        why="""存硬體版本，讓一份韌體能同時服務 V1.3 和現場的舊分壓板。

        V1.3 把分壓係數從 30.000 改成 38.954，但現場機器跑的是舊分壓，
        韌體不能直接改常數（見 CLAUDE.md 的 Deployment constraints）。
        板子必須能自報版本，而 V1.1/V1.2/V1.3 上沒有任何零件做得到這件事。

        XBLW 24C02S，LCSC C18723540，SOT-23-5，1.8–5.5V。固定位址 0x50，
        和 ADS1115 的 0x48 不衝突。腳位 1=SCL 2=GND 3=SDA 4=VCC 5=WP。

        **WP 接地（可寫入）**，防寫由韌體負責 —— 這是刻意的選擇：WP 拉高
        的話韌體永遠寫不進去，版本就只能在產線寫入，而 V1.3 是小批量、
        沒有那道工序。代價是韌體要自己確保不亂寫。

        讀不到 EEPROM 時（舊板、或這顆沒焊）韌體必須退回係數 30.000 ——
        那才是現場每一台的實際值。""",
    ),
    dict(
        op="add_part", ref="C30", value="CC0603KRX7R9BB104",
        symbol="CC0603KRX7R9BB104", footprint="TES:C0603",
        track_width=0.25,
        pins={"1": "VDD33", "2": "GND"},
        why="U13 的去耦電容。100nF 0603 是板上用得最多的料號，不增加成本。",
    ),

    # ---------------------------------------------------------- R10 分壓
    dict(
        op="set_value", ref="R10", value="RT0603BRD07115KL",
        why="""120V 分壓上臂拆成三顆串聯，這是第一顆。

        原本是單顆 348k 的 0603，在 120V 輸入下要承受 116V ——
        0603 的額定工作電壓是 75V，超了 1.5 倍。這不是「裕度不足」，
        是長期跑在規格外，電阻會漂移甚至崩潰，而它是量測電壓的上臂，
        一漂移整個電壓讀值就錯，而且錯的方向是**讀得比實際低**
        （上臂阻值變大 → 分壓變小），車端看到的電壓因此偏低。

型號沿用原本 R10／R11 的 Yageo RT 薄膜系列（0.1%），
        不是一般的 1% 厚膜 —— 分壓比的精度直接就是電壓讀值的精度，
        上臂換成三顆 1% 會讓誤差從 0.1% 掉到約 0.58%（三顆的均方根）。
        ⚠ 料號要自己去 LCSC 確認有貨，RT 系列不是每個 E96 值都常備。

        三顆都用 115k，合計 345k。上臂總值不必湊成原本的 348k ——
        R11 改成 9.09k 之後韌體係數本來就要重算，沒有「維持原值」這回事。
        三顆同值只有好處：**少一個料號**，而嘉立創 SMT 是按不同料號收上件費的。
        （這是使用者提的，我原本堅持 118k+115k+115k=348k 是多餘的限制。）

        原本的寫法 118k + 115k + 115k = 348k，剛好等於原值，所以分壓比不變、
        AIN0 在 120V 時仍是 4.0000 V，**韌體的換算常數完全不用動**。
        三顆都是 E96 標準值。每顆承受 38–39V，對 75V 有 1.9 倍餘裕，
        功耗 13 mW 對 0603 的 100 mW 也很寬鬆。""",
    ),
    dict(
        op="add_part", ref="R33", value="RT0603BRD07115KL",
        symbol=R_SYMBOL, footprint=R_FOOTPRINT,
        pins={"1": "HV_DIV2", "2": "HV_DIV1"},
        why="R10 分壓上臂的第二顆（見 R10 的說明）。",
    ),
    dict(
        op="add_part", ref="R34", value="RT0603BRD07115KL",
        symbol=R_SYMBOL, footprint=R_FOOTPRINT,
        pins={"1": "VOUT_SENSE", "2": "HV_DIV2"},
        why="R10 分壓上臂的第三顆，接到分壓中點 VOUT_SENSE（見 R10 的說明）。",
    ),
    dict(
        op="set_net", ref="R10", pad="1", net="HV_DIV1",
        why="R10 原本直接接分壓中點，現在改接到串聯的下一顆 R33。",
    ),

    # ---------------------------------------------------------- ADS1115 供電
    dict(
        op="set_net", ref="U5", pad="8", net="VDD33",
        why="""ADS1115 從 5V 改回 3.3V，解決 I2C 準位不足。

        ADS1115 的 V_IH 是 0.7 x VDD（SBAS444E 表 5.5）。掛在 5V 時
        門檻是 3.50 V，但 I2C 上拉電阻 R5／R6 接的是 VDD33，
        匯流排高準位只有 3.3 V —— **低於門檻 0.2 V**。
        V1.3 還沒生產，這條 I2C 從來沒有被實際驗證過。
        改到 3.3V 之後門檻降到 2.31 V，餘裕 1.43 倍。

        當初改到 5V 是為了量測範圍（V1.2 掛 3.3V 時上限不到 120V）。
        現在改由 R11 補回來，見 R11 的說明 —— 而且上限反而更高。

        另一條路是維持 5V 再加 PCA9306 準位轉換，韌體完全不用動，
        但要在已經很擠的 U5 周邊多塞一顆 IC 和兩顆上拉電阻。""",
    ),
    dict(
        op="set_net", ref="C20", pad="1", net="VDD33",
        why="C20 是 U5 的去耦電容，跟著供電一起換到 VDD33。",
    ),
    dict(
        op="set_value", ref="R11", value="RT0603BRD079K09L",
        why="""分壓下臂 12k → 9.09k，補回 3.3V 供電損失的量測範圍。

        120V 時 AIN0 從 4.0000 V 變成 3.0806 V，在 3.3V 供電的
        VDD+0.3 = 3.6 V 限制內。量測上限反而從 122.9 V 提高到 140.2 V
        （5V 時卡在 PGA 的 ±4.096V，3.3V 時卡在接腳耐壓，後者比較寬）。
        解析度從 3.750 到 4.869 mV/LSB，在 120V 上是 0.004%，無所謂。

        ⚠ 韌體要改：分壓係數從 30.000 變成 38.954 —— (345k+9.09k)/9.09k。
        這正是之後 hw_info EEPROM 要存的東西 —— 不同硬體版本的分壓值。

        ⚠ 另一個副作用：CP 也掛在這顆 ADC 上（R9 150R / R8 51R，比例 0.2537），
        量測上限從 20.9 V 降到 14.2 V。實測充電時 CP = 8.99 V，餘裕 58%，
        但若 TES 規格允許 CP 到 12V，餘裕只剩 18%，值得實測確認。""",
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
