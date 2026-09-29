#!/bin/sh
# V1.3 生產檔：Gerber、鑽孔、嘉立創 SMT 用的 BOM 與座標檔，全部重新產生。
# 設定照嘉立創的 KiCad 出圖建議：Protel 副檔名、不用 X2、絲印扣掉防焊開窗、
# Excellon 公釐十進位、長圓孔用 alternate、PTH 與 NPTH 分開。
set -e
KICAD="${KICAD:-/c/Program Files/KiCad/10.0/bin/kicad-cli.exe}"
KPY="${KPY:-C:/Program Files/KiCad/10.0/bin/python.exe}"
B=hardware/kicad/TES_Controller.kicad_pcb
OUT=hardware/fab/V1.3
export PYTHONIOENCODING=utf-8

echo "1/5 DRC（重灌鋪銅）"
"$KICAD" pcb drc --output /dev/null --severity-error --refill-zones "$B" 2>&1 | grep -oE 'Found [0-9]+ (violations|unconnected items)'
echo "2/5 機構（拿 KiCad 的板子比對鎖定座標與安裝孔）"
"$KPY" tools/check_mech_kicad.py 2>&1 | grep -v 'memory leak\|image handler' | tail -1

echo "3/5 Gerber 與鑽孔"
rm -rf "$OUT/gerber"; mkdir -p "$OUT/gerber"
"$KICAD" pcb export gerbers --layers F.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts \
    --no-x2 --subtract-soldermask --check-zones --output "$OUT/gerber/" "$B" >/dev/null
"$KICAD" pcb export drill --format excellon --excellon-units mm --excellon-zeros-format decimal \
    --excellon-oval-format alternate --excellon-separate-th --generate-map --map-format gerberx2 \
    --output "$OUT/gerber/" "$B" >/dev/null

echo "4/5 SMT：BOM 與座標檔"
"$KPY" tools/make_jlc_assembly.py 2>&1 | grep -v 'memory leak\|image handler'

echo "5/5 打包上傳用的 zip"
rm -f "$OUT/TES_Controller_V1.3_gerber.zip"
(cd "$OUT/gerber" && "$KPY" -c "import zipfile,glob; z=zipfile.ZipFile('../TES_Controller_V1.3_gerber.zip','w',zipfile.ZIP_DEFLATED); [z.write(f) for f in sorted(glob.glob('*'))]; z.close()")
ls -la "$OUT"
