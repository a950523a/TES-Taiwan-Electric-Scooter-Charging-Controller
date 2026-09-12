import pcbnew, sys, os
sys.path.insert(0, os.path.abspath("tools"))
import _pcb_route as R
P="hardware/kicad/TES_Controller.kicad_pcb"
b=pcbnew.LoadBoard(P)
# 1) 刪掉 EasyEDA 匯入帶來的微型銅皮，只留兩片大地平面
small=[z for z in b.Zones()
       if sum(z.GetFilledPolysList(l).Area()/1e12 for l in z.GetLayerSet().Seq()
              if z.GetFilledPolysList(l)) < 100]
print("刪除微型銅皮 %d 個，保留大銅面 %d 個"%(len(small), b.GetAreaCount()-len(small)))
for z in small: b.Remove(z)
b.BuildConnectivity()
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.BuildConnectivity()
pcbnew.SaveBoard(P,b)
