#!/bin/sh
# 從 V1.3 的 EasyEDA 匯出資料重建整個 KiCad 設計，並驗證。
# 每一步都是冪等的，隨時可以重跑。
set -e
PY="${PY:-$HOME/.venvs/eda/Scripts/python.exe}"
KICAD="${KICAD:-/c/Program Files/KiCad/10.0/bin/kicad-cli.exe}"
export PYTHONIOENCODING=utf-8

echo "1/8 由 .eprj 匯出可 diff 的文字檔"
"$PY" tools/eda_export.py --all

# 元件庫已進版控，平常不必重抓（每次都要連 LCSC，慢又容易失敗）。
# 要重抓：sh tools/regen_hw.sh --relib [--no-3d]
if [ "$1" = "--relib" ]; then
    shift
    echo "2/8 由 LCSC 料號重新產生元件庫"
    "$PY" tools/kicad_lib.py "$@"
    "$KICAD" sym upgrade hardware/kicad/lib/TES.kicad_sym >/dev/null
else
    echo "2/8 沿用已進版控的元件庫（--relib 可強制重抓）"
fi

echo "3/8 補上沒有 LCSC 料號的符號與封裝、修正腳位電氣型別"
"$PY" tools/make_symbols.py
"$PY" tools/make_footprints.py
"$PY" tools/fix_pin_types.py | head -1

echo "4/8 由網表生成電路圖並比對"
"$PY" tools/sch_gen.py --verify

echo "5/8 ERC"
"$KICAD" sch erc --output /dev/null --severity-error --severity-warning \
    hardware/kicad/TES_Controller.kicad_sch 2>&1 | grep -o 'Found.*'

echo "6/8 PCB：匯入 V1.3、套用電氣變更、修補連接"
"$PY" tools/pcb_import.py
KPY="C:/Program Files/KiCad/10.0/bin/python.exe"
"$KPY" tools/_pcb_strip_pours.py  hardware/kicad/TES_Controller.kicad_pcb 2>&1 | grep -v 'memory leak\|image handler' || true
"$KPY" tools/_pcb_apply_changes.py hardware/kicad/TES_Controller.kicad_pcb 2>&1 | grep -v 'memory leak\|image handler' || true
"$KPY" tools/_pcb_repair.py       hardware/kicad/TES_Controller.kicad_pcb 2>&1 | grep -v 'memory leak\|image handler' || true

echo "7/8 PCB DRC"
"$PY" tools/check_project.py
"$KICAD" pcb drc --output /dev/null --severity-error hardware/kicad/TES_Controller.kicad_pcb 2>&1 | grep -oE 'Found [0-9]+ (violations|unconnected items)'

echo "8/8 機構鎖定座標"
"$PY" tools/pcb_geometry.py --locked --json hardware/kicad/mechanical_lock.json \
    > hardware/kicad/mechanical_lock.txt
echo "完成"
