import pcbnew
PCB=r"C:\dev\DMX\hardware\lumigate.kicad_pcb"; FM=pcbnew.FromMM; TM=pcbnew.ToMM
b=pcbnew.LoadBoard(PCB)
npth=0; widened=0
for fp in b.GetFootprints():
    for p in fp.Pads():
        if not p.HasHole(): continue
        d=p.GetDrillSize(); s=p.GetSize()
        ann=(min(TM(s.x),TM(s.y))-max(TM(d.x),TM(d.y)))/2
        if ann>=0.13: continue
        if p.GetNetCode()==0:                          # mounting post -> NPTH (no annular needed)
            p.SetAttribute(pcbnew.PAD_ATTRIB_NPTH); p.SetLayerSet(pcbnew.PAD.UnplatedHoleMask()); npth+=1
        else:                                          # electrical THT -> minimal 0.15mm ring
            p.SetSize(pcbnew.VECTOR2I(d.x+FM(0.3), d.y+FM(0.3))); widened+=1
print(f"NPTH posts: {npth} | widened: {widened} (silk untouched)")
pcbnew.ZONE_FILLER(b).Fill(b.Zones()); pcbnew.SaveBoard(PCB,b)
