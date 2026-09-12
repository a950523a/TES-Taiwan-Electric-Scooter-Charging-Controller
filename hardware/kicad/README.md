# KiCad 專案（由 EasyEDA Pro 遷移中）

V1.4 之後的硬體設計在 KiCad 進行。EasyEDA Pro 的 V1.3 仍保留在
`docs/PCB/`，並由 `tools/eda_export.py` 匯出成可 diff 的文字檔放在
`hardware/TES_Controller_V1.3/`，作為對照基準。

## 目錄

| 路徑 | 內容 | 進版控 |
|---|---|---|
| `lib/TES.kicad_sym` | 34 個符號，由 BOM 的 LCSC 料號產生 | ✅ 98 KB |
| `lib/TES.pretty/` | 23 個封裝 | ✅ 59 KB |
| `lib/TES.3dshapes/` | 42 個 3D 模型（.step + .wrl） | ❌ **39 MB，已 gitignore** |
| `sym-lib-table` / `fp-lib-table` | 專案層級元件庫設定，開啟專案即生效 | ✅ |

3D 模型是純衍生檔，任何時候都能重新抓。不進版控是因為 `.git` 已經 785 MB，
再塞 39 MB 的二進位進去（而且每次更新元件就是一份全新 blob）划不來。

### 重新產生元件庫

```bash
python tools/kicad_lib.py            # 符號 + 封裝 + 3D
python tools/kicad_lib.py --no-3d    # 只要符號 + 封裝（不需要看 3D 時）
```

需要先建好 venv 並安裝 `easyeda2kicad`，指令寫在 `tools/kicad_lib.py` 檔頭。
腳本會把封裝裡的 3D 路徑改成 `${KIPRJMOD}/lib/TES.3dshapes/...`；
easyeda2kicad 產生的是**絕對路徑**，直接提交會在別台機器上全部失效。

## 沒有 LCSC 料號的元件

這 5 個在 EasyEDA 用的是內建通用元件，沒有料號可轉，要從 KiCad 內建庫挑：

| 位號 | EasyEDA 元件 | KiCad 替代 |
|---|---|---|
| `G` `R` `Y` | led_th-r_5mm | `LED:LED_D5.0mm` + `Device:LED` |
| `H1` | hdr-f_2.54_1x4p | `Connector_Generic:Conn_01x04` |
| `H2` | hdr-f_2.54_1x3p | `Connector_Generic:Conn_01x03` |

## 遷移狀態

- [x] 元件庫（符號 / 封裝 / 3D）
- [x] 電路圖 —— Altium 中轉匯入不了，改成**由網表機器生成**
- [x] 網表驗證：218 條接線對 V1.3 原始網表**零差異**
- [x] ERC：0 條違規
- [ ] PCB layout —— 待進行，機構座標已鎖定
- [ ] 電氣修正（R10、I2C 準位、120V 間距、START 去彈跳接點）

### 全部重新產生

```bash
sh tools/regen_hw.sh            # 含 3D 模型
sh tools/regen_hw.sh --no-3d    # 不抓 3D，快很多
```

七個步驟都是冪等的。第 5 步會拿 `kicad-cli sch export netlist` 的結果對
`hardware/TES_Controller_V1.3/netlist.txt` 逐條比對，**有任何一條對不上就失敗** ——
這是整條流程的意義所在：轉檔有沒有漏東西是被驗證的，不是用看的。

### 位號變更

KiCad 把沒有結尾數字的位號視為「未編號」，在 GUI 按一次 Annotate 就會被重新命名。
V1.3 有 9 個這種位號，趁重畫改掉：

| V1.3 | 現在 | 說明 |
|---|---|---|
| `Y` `G` `R` | `D6` `D7` `D8` | LED。`R` 若只補數字會變成 `R1`，和既有電阻撞名 |
| `START` `STOP` `SETTING` `EMERGENCY` | 各加 `1` | 絲印可讀性不變 |
| `BOOT` `EN` | `BOOT1` `EN1` | 同上 |

對照表寫在 `tools/sch_gen.py` 的 `DESIGNATOR_RENAME`，比對網表時會換回原名，
所以驗證仍然是對著 V1.3 的原始資料做的。

### 網路改名

EasyEDA 自動產生的 `$1N24345` 這類網名沒有意義，已改成 `VOUT_SENSE`、`CP_SENSE`、
`VP_PGATE` 等。對照關係在 `tools/sch_gen.py` 的 `NET_RENAME`，每一條都附了推導依據。

匯入完成後跑設計檢查：

```bash
kicad-cli pcb drc --severity-error --exit-code-violations hardware/kicad/TES_Controller.kicad_pcb
```

DRC **不進 CI** —— 設計檢查是設計當下的工作，不是每次 push 都要跑的東西。
