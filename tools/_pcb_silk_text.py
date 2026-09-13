#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把壓在焊盤上的位號文字挪開。在 KiCad 的 python 裡跑。

絲印壓到焊盤有兩個後果：印不上去（防焊開窗處沒有絲印），
以及本來要靠它認位號的人看不到。手焊時這比外框互疊重要得多。

只動位號文字，不動封裝的外框線 —— 外框壓到是密集板的常態，
而且外框被裁掉或疊到不影響判讀哪一顆是哪一顆。
"""
import math, os, sys
import pcbnew

STEP = 0.25
MAX_R = 3.0


def pads_hit(b, item):
    """這段文字有沒有壓到任何焊盤。"""
    box = item.GetBoundingBox()
    for f in b.GetFootprints():
        for p in f.Pads():
            if not p.IsOnLayer(pcbnew.F_Cu) and not p.IsOnLayer(pcbnew.B_Cu):
                continue
            if box.Intersects(p.GetBoundingBox()):
                return True
    return False


def main():
    path = sys.argv[1]
    b = pcbnew.LoadBoard(path)
    moved, stuck = 0, []
    for f in b.GetFootprints():
        t = f.Reference()
        if not t.IsVisible():
            continue
        if not pads_hit(b, t):
            continue
        home = t.GetPosition()
        ok = False
        for r in [STEP * k for k in range(1, int(MAX_R / STEP) + 1)]:
            for ang in range(0, 360, 15):
                t.SetPosition(pcbnew.VECTOR2I(
                    home.x + int(pcbnew.FromMM(r * math.cos(math.radians(ang)))),
                    home.y + int(pcbnew.FromMM(r * math.sin(math.radians(ang))))))
                if not pads_hit(b, t):
                    print("   %s 位號文字移開 %.2f mm" % (f.GetReference(), r))
                    moved += 1
                    ok = True
                    break
            if ok:
                break
        if not ok:
            t.SetPosition(home)
            stuck.append(f.GetReference())
    print("挪開 %d 個位號文字" % moved)
    if stuck:
        print("挪不開（周圍都是焊盤）：%s" % ", ".join(stuck))
    pcbnew.SaveBoard(path, b)


if __name__ == "__main__":
    main()
