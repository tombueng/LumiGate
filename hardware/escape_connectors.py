import pcbnew, math
PCB=r"C:\dev\DMX\hardware\lumigate.kicad_pcb"; FM=pcbnew.FromMM; TM=pcbnew.ToMM
b=pcbnew.LoadBoard(PCB)
# gather ALL pad data BEFORE any mutation
pads=[]
for fp in b.GetFootprints():
    fc=fp.GetPosition(); fcx,fcy=TM(fc.x),TM(fc.y)
    for pad in fp.Pads():
        bb=pad.GetBoundingBox(); pc=pad.GetPosition()
        pads.append(dict(net=pad.GetNetCode(),cx=TM(pc.x),cy=TM(pc.y),
                         r=(TM(bb.GetLeft()),TM(bb.GetTop()),TM(bb.GetRight()),TM(bb.GetBottom())),
                         ref=fp.GetReference(),pn=pad.GetNumber(),fcx=fcx,fcy=fcy,
                         onF=pad.IsOnLayer(pcbnew.F_Cu)))
for t in list(b.GetTracks()): b.Remove(t)        # now clear
def ptr(px,py,r): dx=max(r[0]-px,0,px-r[2]); dy=max(r[1]-py,0,py-r[3]); return math.hypot(dx,dy)
def escape(ref,pn):
    tgt=next((p for p in pads if p['ref']==ref and p['pn']==pn),None)
    if not tgt: print(f"  !! {ref}.{pn} not found"); return
    nc=tgt['net']; others=[p['r'] for p in pads if p['net']!=nc]
    x,y=tgt['cx'],tgt['cy']
    ang=math.atan2(y-tgt['fcy'],x-tgt['fcx']) if (x-tgt['fcx'] or y-tgt['fcy']) else 0
    def vok(vx,vy): return all(ptr(vx,vy,r)>=0.45 for r in others)
    def sok(x0,y0,x1,y1):
        for i in range(11):
            t=i/10.0; px=x0+(x1-x0)*t; py=y0+(y1-y0)*t
            if any(ptr(px,py,r)<0.26 for r in others): return False
        return True
    for dd in [0.7+k*0.2 for k in range(20)]:
        for da in [k*0.25 for k in range(-9,10)]:
            vx,vy=x+dd*math.cos(ang+da),y+dd*math.sin(ang+da)
            if vok(vx,vy) and sok(x,y,vx,vy):
                v=pcbnew.PCB_VIA(b); v.SetPosition(pcbnew.VECTOR2I(FM(vx),FM(vy))); v.SetViaType(pcbnew.VIATYPE_THROUGH)
                v.SetDrill(FM(0.25)); v.SetWidth(FM(0.5)); v.SetNetCode(nc); v.SetLayerPair(pcbnew.F_Cu,pcbnew.B_Cu); v.SetLocked(True); b.Add(v)
                tr=pcbnew.PCB_TRACK(b); tr.SetStart(pcbnew.VECTOR2I(FM(x),FM(y))); tr.SetEnd(pcbnew.VECTOR2I(FM(vx),FM(vy)))
                tr.SetWidth(FM(0.2)); tr.SetLayer(pcbnew.F_Cu if tgt['onF'] else pcbnew.B_Cu); tr.SetNetCode(nc); tr.SetLocked(True); b.Add(tr)
                print(f"  escaped+locked {ref}.{pn} -> via ({vx:.1f},{vy:.1f})"); return
    print(f"  !! could not escape {ref}.{pn}")
for ref,pn in [("U2","8"),("U2","9"),("J2","B5")]:
    escape(ref,pn)
pcbnew.SaveBoard(PCB,b); print("saved with locked escapes")
