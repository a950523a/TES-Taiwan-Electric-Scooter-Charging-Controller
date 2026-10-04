#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""外殼上蓋 V2：按鍵和上蓋一起印（print-in-place），BOOT／EN 改成舌片。

    "C:/Program Files/FreeCAD 1.1/bin/freecadcmd.exe" tools/make_case_lid.py

以現有上蓋 docs/PCB/TES_Controller_V1_Case_Top.stp 為底改出來，輸出到
hardware/enclosure/：
  TES_Case_Top_V2.step        上蓋 + 4 顆按鍵，使用座標、列印時的相對位置
  TES_Case_Top_V2_print.stl   同上，翻過來（上蓋頂面朝下）放在熱床上，直接切片
  preview_*.png               俯視圖與剖面圖，檢查用

座標（上蓋 STEP 的原座標）：上蓋頂面 z = 3、板底 z = 0，往下為負。換算到 PCB：
x_pcb = x + 152.07、y_pcb = 104.66 − y（CLAUDE.md 的 hw 頁一節）。

**高度從實測推回來，不是從圖面。** 舊按鍵全長 16 mm，裝好後突出上蓋 2.5 mm
（2026-10-04 實測），所以主按鍵開關（TS-1187A，總高 1.5 mm）頂端在 z = −10.5，
PCB 頂面在 −12.0；BOOT／EN 的 TS-1088 高 2.0 mm，頂端在 −10.0。

主按鍵（START／SETTING／STOP／EMERGENCY，四顆相同）：
  - 列印時按鍵帽頂面與上蓋頂面齊平，一起貼著熱床。裝上 PCB 後，開關把按鍵往上
    頂 3.2 mm，靜止時突出 2.5 mm（同舊版）。
  - 上蓋下方加 6 mm 套筒，導引從 3 mm 變成約 5.3 mm。按鍵帽 Ø7（舊版手指壓的是
    Ø4.5 的柱頂）。
  - 擋：按鍵帽卡在沉孔裡（不會掉進殼內），下方凸緣比孔大（上蓋翻過來不會掉出）。
    凸緣到套筒底在靜止時還有約 1.25 mm，上蓋裝上去不會把按鍵頂死。
  - 沉孔底與按鍵帽背面是 45° 斜面而不是平面：翻過來列印時，平面會是一圈懸空
    1.3 mm 的環，下垂就黏在按鍵上。
  - 列印間隙：徑向 0.3、斜面處約 0.5（垂直 0.7）。兩個頂緣都倒角 0.4，避免第一層的
    象腳把按鍵帽和上蓋黏在一起。
BOOT／EN：U 形槽切出的舌片，與表面齊平，下方細柱離開關 0.6 mm —— 組裝誤差
不能讓細柱一直頂著 BOOT（開機會進燒錄模式）。兩顆開關只差 3.4 mm，所以兩片
往相反方向伸，細柱在中間相對。
"""
import math, os, sys
import FreeCAD as App
import Part, Mesh, MeshPart

V = App.Vector
try:                              # 中文輸出：Windows 主控台預設是 cp950
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "docs", "PCB", "TES_Controller_V1_Case_Top.stp")
OUT = os.path.join(REPO, "hardware", "enclosure")

# ── 量出來的高度 ──────────────────────────────────────────────────────────────
Z_TOP = 3.0                      # 上蓋頂面
Z_SW_MAIN = -10.5                # TS-1187A 頂端（舊按鍵 16 mm − 突出 2.5 mm）
Z_SW_BOOT = -10.0                # TS-1088 頂端（高 2.0 mm，主按鍵那顆 1.5 mm）

# ── 主按鍵 ────────────────────────────────────────────────────────────────────
BUTTONS = {                      # 上蓋原本 Ø5.0 孔的中心（從 STEP 量）
    "START":     (12.61, 39.11),
    "SETTING":   (12.61, 30.22),
    "STOP":      (12.61, 21.39),
    "EMERGENCY": (21.58, 21.39),
}
PROTRUDE   = 2.5                 # 靜止時突出上蓋（維持舊版手感）
CAP_R      = 3.5                 # 按鍵帽 Ø7.0
CAP_FLAT   = 2.0                 # 按鍵帽直筒段高度（斜面以上）
STEM_R     = 2.2                 # 柱子 Ø4.4
BORE_R     = 2.5                 # 孔 Ø5.0。徑向間隙 0.3：舊版 Ø4.5 是分開印再裝的，
                                 # 一起印的話 0.25 有黏住的風險
CB_R       = 3.8                 # 沉孔 Ø7.6（徑向間隙 0.3）
CB_FLAT    = 2.4                 # 沉孔直筒段深度
CHAMFER    = 0.4                 # 頂緣倒角
SLEEVE_R   = 3.7                 # 套筒外徑 Ø7.4
SLEEVE_LEN = 6.0                 # 上蓋板底以下
FLANGE_R   = 3.1                 # 防脫凸緣 Ø6.2
FLANGE_H   = 0.65                # 凸緣直筒段（上面另有 45° 錐）

# ── BOOT／EN 舌片 ─────────────────────────────────────────────────────────────
TONGUES = {"BOOT": (28.64, 23.22), "EN": (28.64, 26.63)}
TONGUE_X   = (27.20, 30.44)      # 寬 3.24；左緣往右偏，讓出 EMERGENCY 沉孔旁的肉
TONGUE_LEN = 9.0
TONGUE_T   = 1.2                 # 舌片厚度（從頂面算）
SLOT       = 0.5
POST_R     = 1.2                 # 細柱 Ø2.4
POST_GAP   = 0.6                 # 細柱到開關
DIMPLE_D   = 0.4                 # 頂面小凹點：標出按壓位置

# ── 海龜電能 Logo：雙色嵌入（上蓋深色、Logo 淺色，X2D 雙噴頭一次印完）──────────
# 轉換規則（加粗細線、去掉髮絲線、清掉碎片）見 tools/logo_inlay.py
#
# **Logo 檔不在 repo 裡**（hardware/enclosure/logo/ 已 gitignore）：名稱與 Logo 不在
# CC BY-NC-SA 授權範圍內（README「名稱與 Logo」），原始向量檔不公開。沒有這個檔時
# 照樣產生上蓋，只是沒有 Logo —— 別人拿 repo 印出來的外殼不會像原廠品。
# 也可以用環境變數 TES_LOGO_SVG 指向別的位置。
LOGO_SVG    = os.environ.get("TES_LOGO_SVG") or \
              os.path.join(REPO, "hardware", "enclosure", "logo", "turtle-power-icon.svg")
LOGO_CENTER = (0.0, 6.5)         # 上蓋正中：OLED 與按鍵下方、兩顆中間螺絲孔之間
LOGO_HEIGHT = 24.0               # 19 mm 時殼的外環只剩 0.38 mm，會被當成碎片清掉
INLAY_DEPTH = 0.8                # 4 層：太薄的淺色會透出底下的深色
LOGO_CLEAR  = 1.0                # Logo 到任何開孔、沉孔、槽的最小距離

# 估計的 OLED 模組 PCB（0.96″ 常見 27.3 mm 寬），只用來檢查間距
OLED_X_MAX = 160.19 - 152.07


def cyl(r, z0, z1, x, y):
    return Part.makeCylinder(r, z1 - z0, V(x, y, z0))


def cone(r0, z0, r1, z1, x, y):
    """z0 處半徑 r0、z1 處半徑 r1（z1 > z0）"""
    return Part.makeCone(r0, r1, z1 - z0, V(x, y, z0))


def box(x0, x1, y0, y1, z0, z1):
    return Part.makeBox(x1 - x0, y1 - y0, z1 - z0, V(x0, y0, z0))


def make_lid(lid):
    # 主按鍵：套筒 → 沉孔、45° 底、孔、頂緣倒角
    z_cb = Z_TOP - CB_FLAT                       # 沉孔直筒段底
    z_cone = z_cb - (CB_R - BORE_R)              # 45° 斜面到孔徑
    for x, y in BUTTONS.values():
        lid = lid.fuse(cyl(SLEEVE_R, -SLEEVE_LEN, 0.01, x, y))
        cut = cyl(CB_R, z_cb, Z_TOP + 1, x, y)
        cut = cut.fuse(cone(BORE_R, z_cone, CB_R, z_cb, x, y))
        cut = cut.fuse(cyl(BORE_R, -SLEEVE_LEN - 1, z_cone + 0.01, x, y))
        cut = cut.fuse(cone(CB_R, Z_TOP - CHAMFER, CB_R + CHAMFER, Z_TOP, x, y))
        cut = cut.fuse(cyl(CB_R + CHAMFER, Z_TOP - 0.001, Z_TOP + 1, x, y))
        lid = lid.cut(cut)

    # BOOT／EN 舌片
    (bx, by), (ex, ey) = TONGUES["BOOT"], TONGUES["EN"]
    y_mid = (by + ey) / 2
    boot_y = (y_mid - SLOT / 2 - TONGUE_LEN, y_mid - SLOT / 2)    # 根部在 −y
    en_y   = (y_mid + SLOT / 2, y_mid + SLOT / 2 + TONGUE_LEN)    # 根部在 +y
    x0, x1 = TONGUE_X
    y0, y1 = boot_y[0], en_y[1]
    slots = box(x0 - SLOT, x0, y0, y1, -1, Z_TOP + 1)
    slots = slots.fuse(box(x1, x1 + SLOT, y0, y1, -1, Z_TOP + 1))
    slots = slots.fuse(box(x0, x1, boot_y[1], en_y[0], -1, Z_TOP + 1))
    lid = lid.cut(slots)
    lid = lid.cut(box(x0, x1, y0, y1, -1, Z_TOP - TONGUE_T))       # 舌片從下面削薄
    for x, y in TONGUES.values():
        lid = lid.fuse(cyl(POST_R, Z_SW_BOOT + POST_GAP, Z_TOP, x, y))   # 也補上舊的 Ø2 針孔
        lid = lid.cut(cone(0.01, Z_TOP - DIMPLE_D, POST_R, Z_TOP + 0.001, x, y))
    return lid.removeSplitter()


def make_button(x, y):
    """列印位置：按鍵帽頂面與上蓋頂面齊平。使用時被開關往上頂 PROTRUDE + 0.7。"""
    z_cap0 = Z_TOP - CAP_FLAT
    z_cone = z_cap0 - (CAP_R - STEM_R)
    # 靜止時凸緣頂（錐尖）在套筒底下方：靜止位置 = 列印位置 + PROTRUDE
    rest_shift = PROTRUDE
    z_bottom = Z_SW_MAIN - rest_shift                      # 列印位置的底端
    z_fl_top = -SLEEVE_LEN - 1.0 - rest_shift - (BORE_R - STEM_R)   # 凸緣錐的上端（柱徑處）
    z_fl_cone = z_fl_top - (FLANGE_R - STEM_R)
    b = cyl(CAP_R, z_cap0, Z_TOP - CHAMFER, x, y)
    b = b.fuse(cone(CAP_R, Z_TOP - CHAMFER, CAP_R - CHAMFER, Z_TOP, x, y))
    b = b.fuse(cone(STEM_R, z_cone, CAP_R, z_cap0, x, y))
    b = b.fuse(cyl(STEM_R, z_bottom, z_cone + 0.01, x, y))
    b = b.fuse(cone(FLANGE_R, z_fl_cone, STEM_R, z_fl_top, x, y))
    b = b.fuse(cyl(FLANGE_R, z_fl_cone - FLANGE_H, z_fl_cone + 0.001, x, y))
    return b.removeSplitter()


def checks(lid, buttons):
    ok = True
    print("── 列印間隙（按鍵到上蓋的最近距離，應 ≥ 0.3）")
    for name, b in buttons.items():
        d = lid.distToShape(b)[0]
        flag = "" if d >= 0.29 else "  ✗ 太近"
        ok &= d >= 0.29
        print("  %-10s %.3f mm%s" % (name, d, flag))

    print("── 靜止位置（按鍵往上 %.1f mm 坐在開關上）" % PROTRUDE)
    for name, b in buttons.items():
        r = b.copy()
        r.translate(V(0, 0, PROTRUDE))
        top = r.BoundBox.ZMax - Z_TOP
        bot = r.BoundBox.ZMin
        hit = lid.distToShape(r)[0]
        print("  %-10s 突出 %.2f mm，底端 z=%.2f（開關頂 %.2f），與上蓋最近 %.2f mm"
              % (name, top, bot, Z_SW_MAIN, hit))

    print("── 往上的餘量（上蓋裝上時開關把按鍵頂高，凸緣不能先撞到套筒）")
    for name, b in buttons.items():
        lo, hi = 0.0, 8.0
        for _ in range(30):                     # 二分搜尋：按鍵還能往上多少才碰到上蓋
            mid = (lo + hi) / 2
            r = b.copy()
            r.translate(V(0, 0, mid))
            if lid.distToShape(r)[0] > 0.01: lo = mid
            else: hi = mid
        print("  %-10s 從列印位置可往上 %.2f mm（需要 %.1f，餘 %.2f）"
              % (name, lo, PROTRUDE, lo - PROTRUDE))
        ok &= lo - PROTRUDE >= 0.8

    sx = min(x for x, _ in [BUTTONS["START"], BUTTONS["SETTING"]]) - SLEEVE_R
    print("── START／SETTING 套筒外緣 x=%.2f，估計的 OLED 模組板邊 x=%.2f：相距 %.2f mm"
          % (sx, OLED_X_MAX, sx - OLED_X_MAX))
    ex, ey = BUTTONS["EMERGENCY"]
    web = (TONGUE_X[0] - SLOT) - (ex + CB_R + CHAMFER)
    print("── EMERGENCY 沉孔（含倒角）到 BOOT 舌片槽的肉厚：%.2f mm" % web)

    # 舌片：細柱底端到開關、壓多深才觸發
    print("── BOOT／EN 細柱底 z=%.2f，開關頂 %.2f：間隙 %.2f mm；要壓下約 %.2f mm 才觸發"
          % (Z_SW_BOOT + POST_GAP, Z_SW_BOOT, POST_GAP, POST_GAP + 0.25))
    E, w = 2000.0, TONGUE_X[1] - TONGUE_X[0]
    k = 3 * E * (w * TONGUE_T ** 3 / 12) / TONGUE_LEN ** 3
    print("   舌片剛性約 %.1f N/mm（PETG E≈2 GPa）：按到觸發約 %.1f N + 開關 1.6 N；根部應變 %.1f %%"
          % (k, k * (POST_GAP + 0.25),
             100 * 3 * TONGUE_T * (POST_GAP + 0.25) / (2 * TONGUE_LEN ** 2)))
    return ok


def make_logo(lid):
    """回傳 (挖好嵌入槽的上蓋, 嵌入件, 檢查是否通過)"""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import logo_inlay
    faces, w, h = logo_inlay.logo_faces(LOGO_SVG, LOGO_CENTER, LOGO_HEIGHT)
    faces.translate(V(0, 0, Z_TOP))
    inlay = Part.makeCompound([f.extrude(V(0, 0, -INLAY_DEPTH)) for f in faces.Faces])

    # 嵌入槽不能碰到頂面上任何開孔：沉孔倒角、螺絲孔、OLED 窗、舌片槽、端子開口
    top = [f for f in lid.Faces if f.Surface.__class__.__name__ == "Plane"
           and abs(f.BoundBox.ZMin - Z_TOP) < 1e-6 and f.normalAt(0, 0).z > 0.9][0]
    edges = Part.makeCompound([w for w in top.Wires])
    d = faces.distToShape(edges)[0]
    ok = d >= LOGO_CLEAR
    print("── Logo %.1f × %.1f mm、%d 塊，嵌入 %.1f mm；離最近的開孔 %.2f mm%s"
          % (w, h, len(faces.Faces), INLAY_DEPTH, d, "" if ok else "  ✗ 太近"))
    return lid.cut(inlay).removeSplitter(), inlay, ok


def to_print(shape):
    """繞 X 軸轉 180°（不是鏡像），上蓋頂面朝下貼熱床。z_print = Z_TOP − z。"""
    s = shape.copy()
    s.rotate(V(0, 0, 0), V(1, 0, 0), 180)
    s.translate(V(0, 0, Z_TOP))
    return s


def write_stl(shape, name):
    mesh = MeshPart.meshFromShape(Shape=shape, LinearDeflection=0.01, AngularDeflection=0.15,
                                  Relative=False)
    mesh.write(os.path.join(OUT, name))
    return mesh.CountFacets


def main():
    os.makedirs(OUT, exist_ok=True)
    src = Part.Shape()
    src.read(SRC)
    lid = make_lid(src)
    buttons = {n: make_button(x, y) for n, (x, y) in BUTTONS.items()}
    ok = checks(lid, buttons)
    logo_path = os.path.join(OUT, "TES_Case_Top_V2_print_logo.stl")
    if os.path.exists(LOGO_SVG):
        lid, inlay, ok_logo = make_logo(lid)
        ok &= ok_logo
    else:
        inlay = Part.makeCompound([])
        print("── 沒有 Logo 檔（%s）：產生不含 Logo 的上蓋" % os.path.relpath(LOGO_SVG, REPO))
        if os.path.exists(logo_path):
            os.remove(logo_path)               # 別留下上一次的嵌入件，跟這次的上蓋對不上

    comp = Part.makeCompound([lid] + list(buttons.values()) + inlay.Solids)
    comp.exportStep(os.path.join(OUT, "TES_Case_Top_V2.step"))

    # 列印用：兩個 STL 同一個座標系。Bambu Studio 一起匯入、選「作為單一物件的
    # 多個零件」，嵌入件指定淺色，其餘（上蓋 + 按鍵）深色。
    body = to_print(Part.makeCompound([lid] + list(buttons.values())))
    n1 = write_stl(body, "TES_Case_Top_V2_print_body.stl")
    n2 = write_stl(to_print(inlay), "TES_Case_Top_V2_print_logo.stl") if inlay.Solids else 0
    bb = body.BoundBox
    print("輸出：%s（%.1f × %.1f × %.1f mm；本體 %d 個實體 %d 三角形，Logo %d 塊 %d 三角形）"
          % (os.path.relpath(OUT, REPO), bb.XLength, bb.YLength, bb.ZLength,
             1 + len(buttons), n1, len(inlay.Solids), n2))

    # 試片：只切出按鍵和舌片那一角，印整個上蓋之前先試間隙與手感（十幾分鐘）
    cut = box(3.0, 34.0, 12.0, 47.0, -20, 10)
    coupon = Part.makeCompound([lid.common(cut)] + list(buttons.values()))
    write_stl(to_print(coupon), "TES_Case_Top_V2_test_coupon.stl")
    print("試片：按鍵與舌片那一角（x 3–34、y 12–47），單色印即可")
    print("檢查：%s" % ("全部通過" if ok else "有項目不符，見上"))
    return lid, buttons


# freecadcmd 執行腳本時 __name__ 不是 "__main__"，所以不加 guard
main()
