"""Build the v3 board fresh from the netlist. Keeps the existing Edge.Cuts outline and
aux origin; clears all old footprints/tracks/zones; loads every part (easyeda + KiCad
stock libs), assigns nets, drops them in a grid for the user to place."""
import pcbnew, re, os

HERE = os.path.dirname(os.path.abspath(__file__))
PCB = os.path.join(HERE, "lumigate.kicad_pcb")
NET = os.path.join(HERE, "lumigate.net")
STOCK = r"C:\Program Files\KiCad\10.0\share\kicad\footprints"
EZ = os.path.join(HERE, "easyeda")
fm = pcbnew.FromMM; mm = pcbnew.ToMM

# ---- parse netlist ----
txt = open(NET, encoding="utf-8").read()
comps = []
for m in re.finditer(r'\(comp\s+\(ref "([^"]+)"\)(.*?)(?=\(comp\s+\(ref|\Z)', txt, re.S):
    ref, body = m.group(1), m.group(2)
    fpm = re.search(r'\(footprint "([^"]+)"\)', body)
    val = re.search(r'\(value "([^"]*)"\)', body)
    comps.append((ref, fpm.group(1) if fpm else None, val.group(1) if val else ""))
padnet = {}
for blk in re.split(r'\(net\s+\(code\s+\d+\)', txt)[1:]:
    nm = re.search(r'\(name "([^"]*)"\)', blk)
    if not nm:
        continue
    for ref, pin in re.findall(r'\(node\s*\(ref "([^"]+)"\)\s*\(pin "([^"]+)"', blk):
        padnet[(ref, pin)] = nm.group(1)

def libdir(lib):
    d = os.path.join(EZ, lib + ".pretty")
    return d if os.path.isdir(d) else os.path.join(STOCK, lib + ".pretty")

import sys
b = pcbnew.LoadBoard(PCB)
print("loaded", flush=True)
# collect ALL old items first (removing zones corrupts later iteration), then remove
tracks = [t for t in b.GetTracks()]
zones = [z for z in b.Zones()]
fps = [fp for fp in b.GetFootprints()]
print(f"to remove: {len(tracks)} tracks, {len(zones)} zones, {len(fps)} fps", flush=True)
for fp in fps:
    b.Remove(fp)
for z in zones:
    b.Remove(z)
for t in tracks:
    b.Remove(t)
print("cleared", flush=True)

# nets — reuse existing (old nets persist after footprint removal), create only missing
import sys
print("cleared old items; creating nets", flush=True)
netobj = {}
for nm in sorted(set(padnet.values())):
    if not nm:
        continue
    n = b.FindNet(nm)
    if n is None:
        n = pcbnew.NETINFO_ITEM(b, nm); b.Add(n)
    netobj[nm] = n
print(f"nets ready ({len(netobj)})", flush=True)

bb = [s for s in b.GetDrawings() if s.GetLayer() == pcbnew.Edge_Cuts][0].GetBoundingBox()
x0, y0 = mm(bb.GetLeft()) + 6, mm(bb.GetTop()) + 6

import sys
missing = []
i = 0
for ref, fpid, val in comps:
    print(f"  load {ref} <- {fpid}", flush=True)
    if not fpid or ":" not in fpid:
        missing.append((ref, fpid)); continue
    lib, name = fpid.split(":", 1)
    d = libdir(lib)
    if not os.path.isdir(d):
        print(f"    !! lib dir missing: {d}", flush=True); missing.append((ref, fpid)); continue
    fp = pcbnew.FootprintLoad(d, name)
    if fp is None:
        missing.append((ref, fpid)); continue
    fp.SetReference(ref); fp.SetValue(val)
    x = x0 + (i % 9) * 8.0
    y = y0 + (i // 9) * 8.0
    fp.SetPosition(pcbnew.VECTOR2I(fm(x), fm(y)))
    b.Add(fp)
    for pad in fp.Pads():
        nm = padnet.get((ref, pad.GetNumber()))
        if nm:
            pad.SetNet(netobj[nm])
    i += 1

pcbnew.SaveBoard(PCB, b)
print(f"placed {i} footprints; missing: {missing}")
