#!/usr/bin/env python
"""Headless auto-route of lumigate.kicad_pcb via Freerouting.

Pipeline (no KiCad GUI needed):
  load board -> ExportSpecctraDSN -> Freerouting (autoroute) -> ImportSpecctraSES
  -> refill zones -> save.

Requires:
  - KiCad 10 bundled python (has pcbnew): run THIS script with it.
  - Java 11+ and the Freerouting 1.9.0 jar (newer jars need Java 25).
    Download: https://github.com/freerouting/freerouting/releases/tag/v1.9.0
    Set FREEROUTING_JAR below or via env var.

Usage:
  "C:\\Program Files\\KiCad\\10.0\\bin\\python.exe" route.py
"""
import os, sys, subprocess, tempfile
import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.path.join(HERE, 'lumigate.kicad_pcb')
JAR = os.environ.get('FREEROUTING_JAR', r'C:\tmp\freerouting19.jar')
PASSES = 20

dsn = os.path.join(tempfile.gettempdir(), 'lumigate_route.dsn')
ses = os.path.join(tempfile.gettempdir(), 'lumigate_route.ses')

print('loading', BOARD, flush=True)
board = pcbnew.LoadBoard(BOARD)

print('exporting DSN...', flush=True)
pcbnew.ExportSpecctraDSN(board, dsn)

print('running Freerouting (headless)...', flush=True)
r = subprocess.run(['java', '-jar', JAR, '-de', dsn, '-do', ses, '-mp', str(PASSES)],
                   capture_output=True, text=True)
print(r.stdout[-800:])
if not os.path.exists(ses):
    sys.exit('Freerouting produced no SES:\n' + r.stderr[-800:])

print('importing SES...', flush=True)
pcbnew.ImportSpecctraSES(board, ses)

print('refilling zones...', flush=True)
pcbnew.ZONE_FILLER(board).Fill(board.Zones())

pcbnew.SaveBoard(BOARD, board)
ntracks = len(list(board.GetTracks()))
print(f'DONE: {ntracks} track segments, board saved -> {BOARD}', flush=True)
print('Run DRC in KiCad (or kicad-cli) to see any leftover unrouted (USB-C fine pitch may need hand-routing).')
