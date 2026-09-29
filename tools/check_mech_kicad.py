#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""拿 KiCad 的 PCB 比對 mechanical_lock.json。在 KiCad 的 python 裡跑。

    "C:/Program Files/KiCad/10.0/bin/python.exe" tools/check_mech_kicad.py

為什麼要有這支：pcb_geometry.py 產生的鎖定座標讀的是 **EasyEDA 原稿的匯出**，
所以「機構鎖定沒變」只證明原稿沒變，不證明 KiCad 的板子對。2026-09-29 出 Gerber
時才發現：原稿的 4 個 Ø3.2 安裝孔（降壓模組的銅柱、也是外殼的固定點）在轉 KiCad
時整個掉了，而鎖定檢查一直是綠的。

檢查三件事，任何一項不符就以非零結束：
  1. 每個鎖定的安裝孔，KiCad 上同一位置有同直徑的非電鍍孔
  2. 每個鎖定的元件，KiCad 上的座標與旋轉角一致
  3. 板框尺寸一致
"""
import json, math, os, sys
import pcbnew

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOARD = os.path.join(REPO, "hardware", "kicad", "TES_Controller.kicad_pcb")
LOCK = os.path.join(REPO, "hardware", "kicad", "mechanical_lock.json")
ORIGIN = (120.0, 60.0)       # KiCad 板框左上角；鎖定座標以此為原點
TOL = 0.02                   # mm
ROT_TOL = 0.5                # 度


def main():
    b = pcbnew.LoadBoard(sys.argv[1] if len(sys.argv) > 1 else BOARD)
    lock = json.load(open(LOCK, encoding="utf-8"))
    mm = pcbnew.ToMM
    bad = 0

    holes = []
    for f in b.GetFootprints():
        for p in f.Pads():
            if p.GetAttribute() in (pcbnew.PAD_ATTRIB_NPTH, pcbnew.PAD_ATTRIB_PTH):
                q = p.GetPosition()
                holes.append((mm(q.x) - ORIGIN[0], mm(q.y) - ORIGIN[1],
                              mm(p.GetDrillSize().x), f.GetReference(),
                              p.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH))
    for h in lock["holes"]:
        hit = [x for x in holes if math.hypot(x[0] - h["x"], x[1] - h["y"]) < TOL
               and abs(x[2] - h["dia"]) < TOL]
        if not hit:
            print("  ✗ 安裝孔 (%.3f, %.3f) Ø%.1f：KiCad 上沒有" % (h["x"], h["y"], h["dia"]))
            bad += 1
        elif not hit[0][4]:
            print("  ✗ 安裝孔 (%.3f, %.3f)：是電鍍孔（%s），應為非電鍍" % (h["x"], h["y"], hit[0][3]))
            bad += 1
    print("  安裝孔 %d 個，缺 / 錯 %d" % (len(lock["holes"]), bad))

    fps = {f.GetReference(): f for f in b.GetFootprints()}
    cbad = 0
    for ref, c in lock["components"].items():
        f = fps.get(ref)
        if f is None:
            print("  ✗ %s：KiCad 上沒有這個元件" % ref)
            cbad += 1
            continue
        x = mm(f.GetPosition().x) - ORIGIN[0]
        y = mm(f.GetPosition().y) - ORIGIN[1]
        d = math.hypot(x - c["x"], y - c["y"])
        rot_k = f.GetOrientationDegrees() % 360
        # mechanical_lock.json 的角度是翻過 EasyEDA 的 Y 軸之後算的，90° 與 270°
        # 因此互換；KiCad 保留 EasyEDA 的原始角度。比對前先翻回來。
        rot_l = (-c.get("rot", -rot_k)) % 360
        drot = min(abs(rot_k - rot_l), 360 - abs(rot_k - rot_l))
        if d > TOL or drot > ROT_TOL:
            print("  ✗ %-10s 位移 %.3f mm、旋轉差 %.1f°（KiCad %.3f, %.3f, %.1f°）"
                  % (ref, d, drot, x, y, rot_k))
            cbad += 1
    print("  鎖定元件 %d 個，不符 %d" % (len(lock["components"]), cbad))
    bad += cbad

    e = b.GetBoardEdgesBoundingBox()
    w, h = mm(e.GetWidth()), mm(e.GetHeight())
    lw, lh = lock.get("size_mm", (w, h))
    # 外框框線有寬度，邊界框會多出線寬
    if abs((w - lw)) > 0.3 or abs((h - lh)) > 0.3:
        print("  ✗ 板框 %.3f × %.3f，鎖定 %.3f × %.3f" % (w, h, lw, lh))
        bad += 1
    print("→ %s" % ("通過" if not bad else "有 %d 項不符" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
