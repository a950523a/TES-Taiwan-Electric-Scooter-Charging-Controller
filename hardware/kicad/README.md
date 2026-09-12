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
- [ ] PCB —— 需從 EasyEDA Pro 匯出 Altium 格式，再 `kicad-cli pcb import --format altium`
- [ ] 電路圖 —— `kicad-cli sch` **沒有** import 子指令，只能用 KiCad GUI 匯入，
      或依 `hardware/TES_Controller_V1.3/netlist.txt` 重建
- [ ] 匯入後用 `netlist.txt` 比對網表，確認沒有漏接

匯入完成後跑設計檢查：

```bash
kicad-cli pcb drc --severity-error --exit-code-violations hardware/kicad/TES_Controller.kicad_pcb
```

DRC **不進 CI** —— 設計檢查是設計當下的工作，不是每次 push 都要跑的東西。
