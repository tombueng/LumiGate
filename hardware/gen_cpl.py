"""Export a JLCPCB-format CPL (pick & place) DIRECTLY from the board via pcbnew.

Two structural corrections vs a naive kicad-cli pos export:

1. POSITION = footprint ANCHOR by default; pad bbox CENTER only for the
   footprints listed in POS_PADCENTER.
   Which reference JLCPCB expects depends on the footprint SOURCE (same as the
   rotation issue): easyeda/LCSC footprints were imported from the same source
   as JLCPCB's part, so their ANCHOR is already JLCPCB's placement origin -> use
   anchor (e.g. the XLR J1: anchor 158.07, pad-center 162.46 — pad-center would
   shift it 4.4mm off the holes). KiCad-STANDARD asymmetric footprints (the ESP
   WROOM: anchor at body center, 3.62mm off the pad centroid because the antenna
   side has no pads) need the pad bbox center, since JLCPCB places that part at
   its pad centroid. For all symmetric parts anchor == pad center (no difference).

2. ROTATION = footprint orientation + per-FOOTPRINT correction.
   KiCad's 0-deg reference differs from JLCPCB's pick-place library for KiCad-
   standard footprints. easyeda/LCSC-imported footprints already match JLCPCB
   (their names won't appear in the table -> 0). Keyed by exact footprint name
   so it scales: every part of that package is auto-corrected.

Coords are relative to the board aux origin (bottom-left), Y up -> positive,
matching the gerbers (also exported --use-drill-file-origin).
"""
import pcbnew, csv, os

HERE = os.path.dirname(os.path.abspath(__file__))
PCB = os.path.join(HERE, "lumigate.kicad_pcb")
OUT = os.path.join(HERE, "lumigate_CPL.csv")

# Per-footprint rotation correction (degrees added to CPL). Exact footprint-name
# match. Add entries as verified on the JLCPCB preview. easyeda footprints
# (e.g. "SOT-23-5_L3.0-W1.7-...", "LQFP-48_L7.0-...") deliberately NOT listed.
ROT_BY_FP = {
    "SOT-23": 180,   # KiCad Package_TO_SOT_SMD:SOT-23 (3-pin): Q1, Q2, D1 — verified
}

# Footprints whose CPL position must be the pad bbox CENTER instead of the anchor.
# ONLY KiCad-standard asymmetric footprints belong here; easyeda footprints must
# stay on the anchor (their anchor is JLCPCB's origin). Verified on JLCPCB preview.
POS_PADCENTER = {
    "ESP32-S3-WROOM-1",   # KiCad RF_Module: anchor=body center, JLCPCB ref=pad center (U1)
}

b = pcbnew.LoadBoard(PCB)
mm = pcbnew.ToMM
aux = b.GetDesignSettings().GetAuxOrigin()
ec = [s for s in b.GetDrawings() if s.GetLayer() == pcbnew.Edge_Cuts][0].GetBoundingBox()
if aux.x == 0 and aux.y == 0:                      # aux not set -> use board bottom-left
    aux = pcbnew.VECTOR2I(ec.GetLeft(), ec.GetBottom())

out = [["Designator", "Mid X", "Mid Y", "Layer", "Rotation"]]
flipped = []
for f in sorted(b.GetFootprints(), key=lambda x: x.GetReference()):
    ref = f.GetReference()
    pads = list(f.Pads())
    if not pads:
        continue
    name = f.GetFPID().GetUniStringLibId().split(":")[-1]
    if name in POS_PADCENTER:                       # KiCad-stock asymmetric -> pad bbox center
        xs = [p.GetPosition().x for p in pads]; ys = [p.GetPosition().y for p in pads]
        cx = (min(xs) + max(xs)) / 2.0; cy = (min(ys) + max(ys)) / 2.0
    else:                                           # default: footprint anchor (JLCPCB-aligned for easyeda)
        pos = f.GetPosition(); cx = pos.x; cy = pos.y
    X = mm(cx - aux.x)
    Y = mm(aux.y - cy)                              # flip Y (aux origin = bottom-left)
    rot = (f.GetOrientationDegrees() + ROT_BY_FP.get(name, 0)) % 360
    if f.IsFlipped():
        flipped.append(ref)
    side = "Bottom" if f.IsFlipped() else "Top"
    out.append([ref, f"{X:.4f}mm", f"{Y:.4f}mm", side, f"{rot:.0f}"])

with open(OUT, "w", newline="") as fh:
    csv.writer(fh).writerows(out)

try:
    import openpyxl
    wb = openpyxl.Workbook(); ws = wb.active; ws.title = "Sheet1"
    ws.append(out[0])
    for r in out[1:]:
        ws.append([r[0], r[1], r[2], r[3], int(float(r[4]))])
    wb.save(OUT.replace(".csv", ".xlsx"))
except ImportError:
    pass

print(f"JLCPCB CPL written: {len(out)-1} placements (pad-center, bottom-left origin)")
if flipped:
    print("WARNING bottom-side parts (need mirror review):", flipped)
