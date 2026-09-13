#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把指定元件挪到附近不重疊的位置，並重繞它的連線。在 KiCad 的 python 裡跑。

用法：python tools/_pcb_relocate.py <board> <位號> [<位號> ...]

候選位置由近而遠，每個都要同時滿足：
  * 外框不與任何元件重疊（不只是外接矩形，是實際多邊形）
  * 焊盤對其他網路的電氣間距合格（含 .kicad_dru 的高壓規則）
  * 不落在降壓模組的高度禁置區、不超出板框
  * 每一隻腳都還繞得回去
最後一項最重要：位置合法但接不出來等於沒用。
"""
import io, math, os, sys
import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _pcb_apply_changes as AC                            # noqa: E402
import _pcb_maze as MZ                                     # noqa: E402
import _pcb_restructure as RS                              # noqa: E402
import _pcb_route as RT                                    # noqa: E402


def courtyard_clash(b, fp):
    """實際多邊形相交，不是外接矩形 —— 外接矩形會誤判斜放的元件。"""
    ca = fp.GetCourtyard(pcbnew.F_Cu)
    if ca.OutlineCount() == 0:
        return None
    for f in b.GetFootprints():
        if f.GetReference() == fp.GetReference():
            continue
        cb = f.GetCourtyard(pcbnew.F_Cu)
        if cb.OutlineCount() and ca.BBox().Intersects(cb.BBox()) and ca.Collide(cb, 0):
            return f.GetReference()
    return None


def rip_net_tracks(b, refs):
    """拆掉這些元件焊盤上直接連著的走線，讓它們可以自由移動。"""
    pts = set()
    for f in b.GetFootprints():
        if f.GetReference() in refs:
            for p in f.Pads():
                pts.add((p.GetPosition().x, p.GetPosition().y))
    doomed = [t for t in b.GetTracks()
              if t.Type() != pcbnew.PCB_VIA_T
              and ((t.GetStart().x, t.GetStart().y) in pts
                   or (t.GetEnd().x, t.GetEnd().y) in pts)]
    for t in doomed:
        b.Remove(t)
    return len(doomed)


def relocate(b, ref, reach=6.0):
    fp = None
    for f in b.GetFootprints():
        if f.GetReference() == ref:
            fp = f
    if fp is None:
        print("   找不到 %s" % ref)
        return False
    home = fp.GetPosition()
    rot0 = fp.GetOrientation()
    for x, y, rot in AC.candidates(home, step=0.3175, reach=reach):
        fp.SetPosition(pcbnew.VECTOR2I_MM(x, y))
        fp.SetOrientation(rot0 if rot == 0 else
                          pcbnew.EDA_ANGLE(rot, pcbnew.TENTHS_OF_A_DEGREE_T))
        if not AC.inside_board(b, fp) or AC.in_keepout(fp):
            continue
        if courtyard_clash(b, fp) is not None:
            continue
        if AC.pads_too_close(b, fp):
            continue
        d = math.hypot(x - pcbnew.ToMM(home.x), y - pcbnew.ToMM(home.y))
        print("   %s 移動 %.2f mm → (%.2f, %.2f)" % (ref, d, x, y))
        return True
    fp.SetPosition(home)
    fp.SetOrientation(rot0)
    print("   %s 找不到不重疊的位置" % ref)
    return False


def main():
    """分階段執行。

    pcbnew.LoadBoard() 在同一個行程裡只能成功呼叫一次 —— 第二次拿到的是
    未包裝的 SwigPyObject，任何方法都叫不動。移除元素之後又必須重載才能
    繼續迭代，所以每個階段各自一個行程。
    """
    import subprocess
    path, refs = sys.argv[1], sys.argv[2:]
    if refs and refs[0].startswith("--stage="):
        stage, refs = refs[0].split("=", 1)[1], refs[1:]
        b = pcbnew.LoadBoard(path)
        if stage == "rip":
            print("拆掉相關走線 %d 段" % rip_net_tracks(b, set(refs)))
        elif stage == "move":
            ok = [r for r in refs if relocate(b, r)]
            io.open(path + ".moved", "w", encoding="utf-8").write(
                chr(10).join(ok))
        elif stage == "route":
            moved = [l.strip() for l in
                     io.open(path + ".moved", encoding="utf-8") if l.strip()]
            nets = set()
            for f in b.GetFootprints():
                if f.GetReference() in moved:
                    for p in f.Pads():
                        if p.GetNetname():
                            nets.add(p.GetNetname())
            maze = MZ.Maze(b)
            for net in sorted(nets):
                if RT.plane_of(b, net) is not None:
                    continue          # GND 交給過孔縫合
                if len(RS.components(maze, net)) <= 1:
                    continue
                made, length, vias, fail = RS.route_net(b, maze, net)
                print("   重繞 %-8s %d 段 %.1f mm 過孔 %d %s"
                      % (net, made, length, vias, "未完成" if fail else "OK"))
            pcbnew.ZONE_FILLER(b).Fill(b.Zones())
        b.BuildConnectivity()
        pcbnew.SaveBoard(path, b)
        return
    for stage in ("rip", "move", "route"):
        r = subprocess.run([sys.executable, os.path.abspath(__file__), path,
                            "--stage=" + stage] + refs,
                           capture_output=True, text=True, encoding="utf-8",
                           errors="replace",
                           env=dict(os.environ, PYTHONIOENCODING="utf-8"))
        for line in (r.stdout or "").splitlines():
            if "memory leak" in line or "image handler" in line                     or "destructor found" in line:
                continue
            print(line)
        if r.returncode:
            sys.stderr.write(r.stderr or "")
            raise SystemExit("階段 %s 失敗" % stage)
    m = path + ".moved"
    if os.path.exists(m):
        os.unlink(m)


if __name__ == "__main__":
    main()
