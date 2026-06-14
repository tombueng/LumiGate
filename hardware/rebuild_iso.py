import pcbnew
PCB=r"C:\dev\DMX\hardware\lumigate.kicad_pcb"; FM=pcbnew.FromMM; TM=pcbnew.ToMM
b=pcbnew.LoadBoard(PCB)
ISO={"GNDISO","VISO","DMX_A","DMX_B","DMX_A_TERM"}
xs=[];ys=[]; gxs=[];gys=[]
for fp in b.GetFootprints():
    for p in fp.Pads():
        nm=p.GetNetname()
        if nm in ISO:
            r=p.GetBoundingBox(); xs+=[TM(r.GetLeft()),TM(r.GetRight())]; ys+=[TM(r.GetTop()),TM(r.GetBottom())]
        if nm=="GNDISO":
            r=p.GetBoundingBox(); gxs+=[TM(r.GetLeft()),TM(r.GetRight())]; gys+=[TM(r.GetTop()),TM(r.GetBottom())]
ix0,ix1,iy0,iy1=min(xs),max(xs),min(ys),max(ys)
gx0,gx1,gy0,gy1=min(gxs),max(gxs),min(gys),max(gys)
print(f"iso-pad bbox x {ix0:.1f}..{ix1:.1f} y {iy0:.1f}..{iy1:.1f} | GNDISO-pad bbox x {gx0:.1f}..{gx1:.1f}")
bb=b.GetBoardEdgesBoundingBox()
BX0,BY0,BX1,BY1=TM(bb.GetLeft())-1,TM(bb.GetTop())-1,TM(bb.GetRight())+1,TM(bb.GetBottom())+1
CRP=4.0
_z=list(b.Zones()); _t=list(b.GetTracks())
for z in _z: b.Remove(z)
for t in _t: b.Remove(t)
b.SetCopperLayerCount(4); b.SetLayerType(pcbnew.In1_Cu,pcbnew.LT_SIGNAL); b.SetLayerType(pcbnew.In2_Cu,pcbnew.LT_SIGNAL)
gnd=b.FindNet("GND"); gndiso=b.FindNet("GNDISO")
def poly(pts):
    v=pcbnew.VECTOR_VECTOR2I()
    for x,y in pts: v.append(pcbnew.VECTOR2I(FM(x),FM(y)))
    return v
# inner GND planes: U-notch to top edge (iso region carved; gateway primary pins at x<nx0 stay on plane)
nx0,nx1,ny=ix0-CRP, ix1+CRP, iy1+CRP
Unotch=[(BX0,BY0),(nx0,BY0),(nx0,ny),(nx1,ny),(nx1,BY0),(BX1,BY0),(BX1,BY1),(BX0,BY1)]
for ly in (pcbnew.In1_Cu,pcbnew.In2_Cu):
    z=pcbnew.ZONE(b); z.SetLayer(ly); z.SetNetCode(gnd.GetNetCode()); z.SetAssignedPriority(0)
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL); z.AddPolygon(poly(Unotch)); b.Add(z)
print(f"inner GND planes: U-notch x {nx0:.1f}..{nx1:.1f} y top..{ny:.1f}")
# GNDISO pour F+B over GNDISO-pad bbox (+margin), normal clearance, right of PS1 primary pins
gz=pcbnew.ZONE(b); lsfb=pcbnew.LSET(); lsfb.AddLayer(pcbnew.F_Cu); lsfb.AddLayer(pcbnew.B_Cu)
gz.SetLayerSet(lsfb); gz.SetNetCode(gndiso.GetNetCode()); gz.SetAssignedPriority(1)
gz.AddPolygon(poly([(gx0-0.4,gy0-0.4),(gx1+0.6,gy0-0.4),(gx1+0.6,gy1+0.6),(gx0-0.4,gy1+0.6)])); b.Add(gz)
print(f"GNDISO pour x {gx0-1.0:.1f}..{gx1+1.5:.1f}")
pcbnew.SaveBoard(PCB,b); print("rebuilt + saved")
