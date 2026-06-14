"""Generate JLCPCB BOM (Comment, Designator, Footprint, LCSC) from the v3 board.
Maps each designator -> part + LCSC#. 9 parts (5 LEDs + 12.4k/45.3k/180R) have no firm
LCSC yet — pick them in the JLCPCB BOM tool at upload (value+package given in Comment)."""
import pcbnew, csv, re
from collections import defaultdict

PCB = r"C:\dev\DMX\hardware\lumigate.kicad_pcb"
OUT = r"C:\dev\DMX\hardware\lumigate_BOM_jlcpcb.csv"

# ref -> (comment, LCSC#)   ("" = choose in JLCPCB picker)
INFO = {
    "U1": ("ESP32-S3-WROOM-1-N8 (8MB)", "C2913198"),
    "U2": ("W5500 SPI Ethernet", "C32843"),
    "U3": ("CH340C USB-UART", "C84681"),
    "U4": ("SY8089 buck 5->3.3V", "C78988"),
    "U5": ("ISO3086DWR isolated RS-485", "C183095"),
    "PS1": ("B0505S-1W iso DC-DC (EVISUN)", "C7465127"),
    "J1": ("XLR-3 out (XLR-328P)", "C309326"),
    "J2": ("USB-C data (TYPE-C-31-M-12)", "C165948"),
    "J3": ("RJ45 MagJack HR961160C SMD 10/100", "C55683"),
    "J4": ("JST SH 1.0mm 9-pin SMD (optional display; pre-crimped cables)", ""),
    "Y1": ("25MHz crystal 2520", "C2981622"),
    "L1": ("2.2uH power inductor (CKCS4030)", "C354584"),
    "D1": ("SM712 TVS SOT-23", "C404012"),
    "Q1": ("MMBT3904 SOT-23", "C20526"), "Q2": ("MMBT3904 SOT-23", "C20526"),
    "SW1": ("tact sw B3U-1000P", "C231329"), "SW2": ("tact sw B3U-1000P", "C231329"),
    # resistors
    "R1": ("10k 0402", "C25744"), "R2": ("10k 0402", "C25744"), "R6": ("10k 0402", "C25744"),
    "R7": ("10k 0402", "C25744"), "R11": ("10k 0402", "C25744"),
    "R3": ("12k 0402 W5500 EXRES1 (12.4k spec OOS at LCSC; 12k within 100BASE-TX +-5%)", "C25752"),
    "R10": ("45.3k 0402 1% (buck FB top -> 3.32V)", "C137977"),
    "R13": ("1k 0402", "C11702"), "R15": ("1k 0402", "C11702"),
    "R14": ("150R 0402", "C25082"), "R16": ("150R 0402", "C25082"),
    "R17": ("150R 0402", "C25082"),
    "R18": ("49.9R 1% 0402 W5500 TX center-tap bias (YAGEO RC0402FR-0749R9L)", "C87044"),
    "R4": ("330R 0402", "C25104"), "R5": ("330R 0402", "C25104"),
    "R8": ("5k1 0402", "C25905"), "R9": ("5k1 0402", "C25905"),
    "R12": ("120R 0805", "C17437"),
    # caps
    "C1": ("100nF 0402", "C1525"), "C3": ("1uF 0402 (EN power-on RC)", "C52923"), "C5": ("100nF 0402", "C1525"),
    "C8": ("100nF 0402", "C1525"), "C9": ("100nF 0402", "C1525"), "C10": ("100nF 0402", "C1525"),
    "C11": ("100nF 0402", "C1525"), "C14": ("100nF 0402", "C1525"), "C15": ("100nF 0402", "C1525"),
    "C18": ("100nF 0402", "C1525"), "C19": ("100nF 0402", "C1525"),
    "C12": ("22pF 0402", "C1555"), "C13": ("22pF 0402", "C1555"),
    "C4": ("1uF 0402", "C52923"), "C6": ("4.7uF 0402", "C23733"),
    "C2": ("10uF 0805", "C15850"), "C16": ("22uF 0805", "C45783"), "C17": ("22uF 0805", "C45783"),
    "C20": ("10uF 1206", "C13585"), "C21": ("10uF 1206", "C13585"), "C22": ("100nF 0402", "C1525"),
    # LEDs 0603 — all JLCPCB Basic, in stock
    "D2": ("LED red 0603", "C2286"), "D3": ("LED green 0603 KT-0603G", "C12624"),
    "D4": ("LED yellow 0603 KT-0603Y", "C2287"), "D5": ("LED blue 0603 KT-0603B", "C2288"),
    "D6": ("LED white 0603", "C2290"),
}

b = pcbnew.LoadBoard(PCB)
groups = defaultdict(list)
for fp in b.GetFootprints():
    ref = fp.GetReference()
    if ref not in INFO:
        continue
    comment, lcsc = INFO[ref]
    groups[(comment, str(fp.GetFPID().GetLibItemName()), lcsc)].append(ref)

def k(r):
    m = re.match(r"([A-Za-z]+)(\d+)", r)
    return (m.group(1), int(m.group(2))) if m else (r, 0)

rows = [["Comment", "Designator", "Footprint", "LCSC Part #"]]
for (comment, fpn, lcsc), refs in sorted(groups.items(), key=lambda x: k(x[1][0])):
    rows.append([comment, ",".join(sorted(refs, key=k)), fpn, lcsc])

with open(OUT, "w", newline="") as f:
    csv.writer(f).writerows(rows)

placed = sum(len(v) for v in groups.values())
missing = sorted([r for r, (c, l) in INFO.items() if not l], key=k)
print(f"BOM written: {len(rows)-1} lines, {placed} parts")
print(f"LCSC to pick in JLCPCB ({len(missing)}): {missing}")
