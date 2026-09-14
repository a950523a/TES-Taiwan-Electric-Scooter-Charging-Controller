# KiCad 專案（已由 EasyEDA Pro 遷移完成，待出圖）

V1.3 起的硬體設計在 KiCad 進行。EasyEDA Pro 的 V1.3 仍保留在
`docs/PCB/`，並由 `tools/eda_export.py` 匯出成可 diff 的文字檔放在
`hardware/TES_Controller_V1.3/`，作為對照基準。

## 目錄

| 路徑 | 內容 | 進版控 |
|---|---|---|
| `lib/TES.kicad_sym` | 45 個符號：34 個由 BOM 的 LCSC 料號產生、4 個沒有料號的自行產生、6 個電源符號加 PWR_FLAG | ✅ |
| `lib/TES.pretty/` | 28 個封裝 | ✅ |
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

這幾個在 EasyEDA 用的是內建通用元件，沒有料號可轉。改成由
`tools/make_symbols.py` / `tools/make_footprints.py` 直接產生，留在同一個
`TES` 庫裡，不必混用 KiCad 內建庫：

| 位號 | EasyEDA 元件 | 現在的符號 |
|---|---|---|
| `D6` `D7` `D8` | led_th-r_5mm | `TES:LED-5MM` |
| `H1` | hdr-f_2.54_1x4p | `TES:HDR-1X4` |
| `H2` | hdr-f_2.54_1x3p | `TES:HDR-1X3` |
| `W1`–`W4` | 焊盤 | `TES:SOLDERPAD-1P` |

## 遷移狀態

- [x] 元件庫（符號 / 封裝 / 3D）
- [x] 電路圖 —— Altium 中轉匯入不了；先改成由網表機器排版，再改成
      **照原稿座標重畫**（`tools/sch_import_easyeda.py`），版面和 EasyEDA 一致
- [x] 網表驗證：230 條接線對 V1.3 原始網表**零差異**
- [x] ERC：0 條違規
- [x] PCB layout —— 雙層、單面貼片；底層整片接地銅箔，機構座標未動
- [x] DRC：0 錯誤、0 未連接（尚有 167 條絲印／外框重疊類警告）
- [x] 電氣修正 —— R10 耐壓、ADS1115 I²C 準位、120V 間距、降壓板進出各一顆 TVS
- [ ] Gerber 出圖 —— 還沒做，也還沒下過單

> START 的去彈跳接點不必改：TS-1187A 的 1、2 腳在封裝內部就是短路的，
> 網表裡 START1.3 和 START1.4 也都接 GND，原本以為的問題不存在。

### 全部重新產生

```bash
sh tools/regen_hw.sh            # 含 3D 模型
sh tools/regen_hw.sh --no-3d    # 不抓 3D，快很多
```

八個步驟都是冪等的 —— 但**冪等指的是結果，不是檔案位元組**：符號和導線的
UUID 每次重新產生都是新的，所以重跑一次 `TES_Controller.kicad_sch` 就會出現
兩千多行的 diff，內容其實只有 UUID 不同。要確認有沒有實質變化，跑
`git diff -- hardware/kicad/TES_Controller.kicad_sch | grep -v uuid`，
或乾脆 `git checkout` 掉。

第 4 步會拿 `kicad-cli sch export netlist` 的結果對
`hardware/TES_Controller_V1.3/netlist.txt` 逐條比對，**有任何一條對不上就失敗** ——
這是整條流程的意義所在：轉檔有沒有漏東西是被驗證的，不是用看的。

第 4 步跑的是 `tools/sch_import_easyeda.py`，不是 `tools/sch_gen.py`。後者是早期
「由網表機器排版」的版本，直接跑會把照原稿重畫的版面蓋掉；它現在的角色是
函式庫（載入元件庫、讀網表、驗證），由前者匯入使用。

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

### V1.3 新增與變更的元件

`tools/changes_v13.py` 是唯一的來源，每一項都附了理由，電路圖和 PCB 都照它套用：
R10 換值並改接 R33、新增 R33/R34（115 k ×3 串聯的上臂）、R11 換 9.09 k、
U5 改回 3.3 V、D9/D10 兩顆 TVS。這些元件在電路圖上另外擺在圖面下方
「V1.3 新增」區，用標籤接線。

### 網路改名

EasyEDA 自動產生的 `$1N24345` 這類網名沒有意義，已改成 `VOUT_SENSE`、`CP_SENSE`、
`VP_PGATE` 等。對照關係在 `tools/sch_gen.py` 的 `NET_RENAME`，每一條都附了推導依據。

匯入完成後跑設計檢查：

```bash
kicad-cli pcb drc --severity-error --exit-code-violations hardware/kicad/TES_Controller.kicad_pcb
```

DRC **不進 CI** —— 設計檢查是設計當下的工作，不是每次 push 都要跑的東西。
