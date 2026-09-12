"""把 DRC 回報的未連接焊盤接回去。在 KiCad 的 python 裡跑。

一輪修不完：每次重新鋪銅都會讓地平面的形狀變動，可能又有別的焊盤脫離，
所以是「修 → 重鋪 → 再看 DRC」的迴圈，最多四輪。

有完整平面的網路（GND）就近打過孔，其餘找同網路最近的銅箔繞線。
"""
import pcbnew, sys, os, re, json, subprocess
sys.path.insert(0, os.path.abspath("tools"))
import _pcb_route as R
P = sys.argv[1] if len(sys.argv) > 1 else "hardware/kicad/TES_Controller.kicad_pcb"

def unconnected():
    subprocess.run([r"C:\Program Files\KiCad\10.0\bin\kicad-cli.exe","pcb","drc",
                    "--output","C:/Users/user/AppData/Local/Temp/claude/u.rpt",
                    "--severity-error","--format","json",P],capture_output=True)
    d=json.load(open("C:/Users/user/AppData/Local/Temp/claude/u.rpt",encoding="utf-8"))
    return d.get("unconnected_items",[]), d["violations"]

for rnd in range(4):
    u,_=unconnected()
    if not u:
        print("第 %d 輪：全部連接完成"%(rnd+1)); break
    print("第 %d 輪：未連接 %d 項"%(rnd+1,len(u)))
    b=pcbnew.LoadBoard(P)
    pads={}
    for f in b.GetFootprints():
        for p in f.Pads(): pads.setdefault(p.GetNetname(),[]).append((f.GetReference(),p))
    fixed=0
    for v in u:
        # 從描述抓出涉及的焊盤
        for it in v.get("items",[]):
            m=re.match(r"Pad (\S+) \[([^\]]+)\] of (\S+)", it.get("description",""))
            if not m: continue
            num,net,ref=m.group(1),m.group(2),m.group(3)
            pad=next((p for r_,p in pads.get(net,[]) if r_==ref and p.GetNumber()==num),None)
            if pad is None: continue
            if R.plane_of(b,net) is not None:
                d=R.via_to_plane(b,net,pad.GetPosition(),0.4)
                if d is not None:
                    print("   %s.%s %s → 過孔接平面"%(ref,num,net)); fixed+=1; break
            tgt=R.nearest_same_net(b,net,pad.GetPosition(),exclude_pad=pad)
            if tgt is None: continue
            r=R.find_route(b,net,pad.GetPosition(),tgt,0.3)
            if r:
                lay,pts,length=r
                R.add_route(b,net,lay,pts,0.3,via_at_start=True)
                print("   %s.%s %s → %s %.2f mm"%(ref,num,net,b.GetLayerName(lay),length))
                fixed+=1; break
    if not fixed:
        print("   這一輪沒能修好任何一項，停止"); 
        for v in u:
            print("      ", " ←→ ".join(it.get("description","")[:56] for it in v.get("items",[])[:2]))
        break
    b.BuildConnectivity(); pcbnew.ZONE_FILLER(b).Fill(b.Zones()); b.BuildConnectivity()
    pcbnew.SaveBoard(P,b)
u,viol=unconnected()
print(); print("最終：未連接 %d 項、違規 %d 條"%(len(u),len(viol)))
