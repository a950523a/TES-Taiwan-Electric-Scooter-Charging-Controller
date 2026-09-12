#!/bin/sh
# 從 V1.3 的 EasyEDA 匯出資料重建整個 KiCad 設計，並驗證。
# 每一步都是冪等的，隨時可以重跑。
set -e
PY="${PY:-$HOME/.venvs/eda/Scripts/python.exe}"
KICAD="${KICAD:-/c/Program Files/KiCad/10.0/bin/kicad-cli.exe}"
export PYTHONIOENCODING=utf-8

echo "1/6 由 .eprj 匯出可 diff 的文字檔"
"$PY" tools/eda_export.py --all

echo "2/6 由 LCSC 料號產生元件庫（3D 模型不進版控，加 --no-3d 可跳過）"
"$PY" tools/kicad_lib.py "$@"

echo "3/6 升級成 KiCad 10 格式"
"$KICAD" sym upgrade hardware/kicad/lib/TES.kicad_sym >/dev/null

echo "4/6 補上沒有 LCSC 料號的符號與封裝、修正腳位電氣型別"
"$PY" tools/make_symbols.py
"$PY" tools/make_footprints.py
"$PY" tools/fix_pin_types.py | head -1

echo "5/6 由網表生成電路圖並比對"
"$PY" tools/sch_gen.py --verify

echo "6/6 ERC"
"$KICAD" sch erc --output /dev/null --severity-error --severity-warning \
    hardware/kicad/TES_Controller.kicad_sch 2>&1 | grep -o 'Found.*'

echo "7/7 機構鎖定座標"
"$PY" tools/pcb_geometry.py --locked --json hardware/kicad/mechanical_lock.json \
    > hardware/kicad/mechanical_lock.txt
echo "完成"
