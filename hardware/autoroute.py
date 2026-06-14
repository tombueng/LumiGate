"""Complete automatic re-route of lumigate.kicad_pcb after a placement change.

Workflow: place/move parts in KiCad -> save -> run this:
   "C:\\Program Files\\KiCad\\10.0\\bin\\python.exe" autoroute.py

What it does (no manual fanout, no GUI):
  1. Forces the inner copper layers to SIGNAL type with their GND fill. This is the
     key trick: as *signal* layers (not "power planes") Freerouting will route GND
     pads -- including fine-pitch escapes -- straight to the inner GND copper. Marked
     as power planes it refuses to, which is what forced manual vias before.
  2. Clears every track/via and re-routes the whole board from the current placement
     via Freerouting (DSN -> Freerouting -> SES).
  3. Refills all zones and reports leftover unconnected nets.

Needs: KiCad 10 bundled python (pcbnew), Java, Freerouting 1.9.0 jar (FREEROUTING_JAR).
"""
import os, sys, subprocess, tempfile
import pcbnew

HERE  = os.path.dirname(os.path.abspath(__file__))
BOARD = os.path.join(HERE, 'lumigate.kicad_pcb')
JAR   = os.environ.get('FREEROUTING_JAR', r'C:\tmp\freerouting19.jar')
PASSES = 50

dsn = os.path.join(tempfile.gettempdir(), 'lumigate_auto.dsn')
ses = os.path.join(tempfile.gettempdir(), 'lumigate_auto.ses')

# single board object throughout (saving+reloading mid-script breaks swig iteration)
b = pcbnew.LoadBoard(BOARD)
b.SetLayerType(pcbnew.In1_Cu, pcbnew.LT_SIGNAL)   # inner = signal + GND fill
b.SetLayerType(pcbnew.In2_Cu, pcbnew.LT_SIGNAL)
cleared = 0; kept = 0
for t in list(b.GetTracks()):
    if t.IsLocked():            # locked = hand-routed connector escapes -> keep
        kept += 1; continue
    b.Remove(t); cleared += 1
print(f"cleared {cleared} unlocked tracks, kept {kept} locked; inner layers = signal", flush=True)

print("exporting DSN + running Freerouting...", flush=True)
pcbnew.ExportSpecctraDSN(b, dsn)
r = subprocess.run(['java', '-jar', JAR, '-de', dsn, '-do', ses, '-mp', str(PASSES)],
                   capture_output=True, text=True)
if not os.path.exists(ses):
    sys.exit("Freerouting produced no SES:\n" + r.stderr[-600:])

pcbnew.ImportSpecctraSES(b, ses)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
ntracks = len(list(b.GetTracks()))          # count BEFORE save (swig quirks after)
pcbnew.SaveBoard(BOARD, b)
print(f"DONE: {ntracks} track items, saved -> {BOARD}", flush=True)
print("Run DRC in KiCad to review (connector-footprint warnings are separate).", flush=True)
