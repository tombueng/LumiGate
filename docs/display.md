# Design — OLED / TFT status display (issue #5)

Add an **optional on-device display** that shows the gateway's live status without a
browser — IP, universe, frame rate, source count, link quality, and DMX activity —
and **auto-rotates to an alert banner** when something needs attention (two consoles
fighting over the universe, identify, manual override).

> Status: **design / Phase 1 (config plumbing) built**. Decisions locked here for review.
> Two reference panels are in hand for bring-up:
> - **Retoo 0.96″ 128×64 SH1106**, blue monochrome, **I²C** (`dispType 3`)
> - **DollaTek 1.5″ 128×128 SSD1351**, full-color RGB, **SPI** (`dispType 4`)

---

## 1. Displays we support & the graphics library

Two display classes, **one drawing API**:

| Class | Controllers | Bus | Pins | Buffer | Boards |
|---|---|---|---|---|---|
| **Mono OLED** | SSD1306 (128×64, 128×32), SH1106 (128×64) | I²C | 2 | 1 KB | all four |
| **Color OLED** | SSD1351 (128×128 RGB) | SPI | ~5 | direct-draw | esp32 / esp32-s3 only |
| Color TFT *(§7, later)* | ST7735 / ST7789 / ILI9341 | SPI | ~5 | direct-draw | esp32 / esp32-s3 only |

**Library: the [Adafruit_GFX](https://github.com/adafruit/Adafruit-GFX-Library) family**
(not U8g2). Every driver below derives from the common `Adafruit_GFX` base, so the renderer
draws once through a single `Adafruit_GFX*` pointer with `uint16_t` RGB565 colors — monochrome
panels map any non-black color to "pixel on," the SSD1351 shows real color:

```
olikraus/U8g2          ✗  (drives SSD1351 only in monochrome — defeats the color panel)
adafruit/Adafruit GFX Library          ← base class + fonts
adafruit/Adafruit SSD1306              ← SSD1306 mono I²C   (dispType 1,2)
adafruit/Adafruit SH110X               ← SH1106  mono I²C   (dispType 3)  ← Retoo panel
adafruit/Adafruit SSD1351 OLED Library ← SSD1351 color SPI  (dispType 4)  ← DollaTek panel
```

Why this over U8g2: it's the only option that drives **both** the mono I²C OLED *and* a
**full-color** SSD1351 with **one** rendering routine, it opens the §7 color-TFT path for free,
and it matches the Adafruit ecosystem the firmware already uses (`Adafruit_NeoPixel`). Trade-off
vs U8g2: no page-buffer low-RAM mode (mono keeps a 1 KB buffer — fine; we already gate WS push
under 40 KB heap) and a smaller built-in font set (ample for a status screen).

```cpp
Adafruit_GFX* gfx = nullptr;   // nullptr until initDisplay() if dispType>0
uint8_t       dispFlush = 0;   // 0=none, 1=mono display(), 2=color direct (no-op)
// factory (initDisplay):
//   case 1: gfx = new Adafruit_SSD1306(128,64,&Wire,-1); ...->begin(SSD1306_SWITCHCAPVCC,addr);
//   case 3: gfx = new Adafruit_SH1106G(128,64,&Wire,-1); ...->begin(addr,true);
//   case 4: SPI.begin(sck,-1,mosi,cs);
//           gfx = new Adafruit_SSD1351(128,128,&SPI,cs,dc,rst); ...->begin();
```

The per-type `begin()` / flush differences live in the factory; the renderer is type-agnostic.

---

## 2. Pin availability

Mono I²C needs **2 bidirectional GPIOs** (input-only 34–39 can't drive SDA/SCL). Color OLED /
TFT over SPI needs **~5** (SCK, MOSI, CS, DC, RST).

| Board | DMX / LED / EN | Eaten by Ethernet | Free output-capable | Mono I²C | Color SPI |
|---|---|---|---|:--:|:--:|
| `esp32dev` (WROOM) | 16/17, LED 2 | — | 21, 22 (+ many) | ✓ | ✓ (HW SPI 18/23 + CS/DC/RST) |
| `esp32s3dev` | 16/17, LED 48 | — | abundant | ✓ | ✓ |
| `wt32eth01` | 4/5, LED 2 | 0,16,18,19,21,22,23,25,26,27 | **14, 15, 17** only | ✓ | ✗ |
| carrier / `esp32-poe-iso` | 4/36, EN 32, RGB 33 | 0,12,18,19,21,22,23,25,26,27 | 5, 13, 14, 15, 16 | ✓ | ✗ (needs all 5 spares) |

**The color SSD1351 runs on the WiFi dev boards (where bring-up happens), not the Ethernet
boards.** The mono Retoo panel runs everywhere.

**Strapping cautions** (baked into defaults + form help): avoid **GPIO12** (boot-straps flash
voltage; an OLED pull-up can brick boot); **GPIO15** is fine pulled high; on **WT32-ETH01
GPIO16 is the PHY-power pin** — off-limits there.

### Recommended default pins (compile-time `DEF_DISP_*`, overridable in `/config`)

| Board | I²C SDA | I²C SCL |
|---|---|---|
| `esp32dev` | 21 | 22 |
| `esp32s3dev` | 8 | 9 |
| `wt32eth01` | 14 | 15 |
| carrier / `esp32-poe-iso` | 13 | 16 |

Color-SPI pins have no universal default (they're board-specific and only used for `dispType 4`)
— the user sets CS/DC/RST/SCK/MOSI in `/config`. Default `dispType = 0` (**off**) everywhere, so
existing units are unaffected until a user opts in.

---

## 3. Config keys

Mirrors how the status LED is wired through the stack (`ledType`/`ledPin`).

| `Config` field | NVS key | `/config` field | Range | Default | Used by |
|---|---|---|---|---|---|
| `dispType` | `disptype` | select | 0=off · 1=SSD1306 128×64 · 2=SSD1306 128×32 · 3=SH1106 128×64 · **4=SSD1351 128×128 color** | `0` | all |
| `dispSda` | `dispsda` | number | −1..48 | `DEF_DISP_SDA` | I²C (1–3) |
| `dispScl` | `dispscl` | number | −1..48 | `DEF_DISP_SCL` | I²C (1–3) |
| `dispRot` | `disprot` | select | 0=normal · 1=flipped 180° | `0` | all |
| `dispCs` | `dispcs` | number | −1..48 | −1 | SPI (4) |
| `dispDc` | `dispdc` | number | −1..48 | −1 | SPI (4) |
| `dispRst` | `disprst` | number | −1..48 | −1 | SPI (4) |
| `dispSck` | `dispsck` | number | −1..48 | −1 | SPI (4) |
| `dispMosi` | `dispmosi` | number | −1..48 | −1 | SPI (4) |

- **I²C address** isn't a field — `initDisplay()` probes `0x3C` then `0x3D` and logs which
  answered. (SSD1351 ignores it.)
- The `/config` "Display" card shows the **I²C pin rows for types 1–3** and the **SPI pin rows
  for type 4**, toggled in JS by the selected type (like `updLedPin()`).
- Touchpoints (all following the LED pattern): `loadConfig` / `saveConfig`, `handleConfigPost`,
  `handleInfoJson`, `src/pages/config.html`.
- **Phase 1 status:** `dispType` (0–3), `dispSda`, `dispScl`, `dispRot` are built and compiling.
  The SPI keys + `dispType 4` are added when color support lands (Phase 2).

---

## 4. Architecture — a `displayTask`, like `ledTask`

The display renders on its **own low-priority FreeRTOS task** (≈4 Hz), exactly like
[`ledTask`](../src/main.cpp). It only **reads** existing state snapshots (`fps`, `senders`,
`netLocalIP()`, `cfg.*`, `hasConflict()`, `manualMode`, `identifyCh`, `dmxBuf`) — never touches
the DMX UART, never blocks `loop()`. A mono I²C flush is ~10–30 ms at 400 kHz; an SSD1351
partial-region redraw over SPI is a few ms — both safely off the DMX path.

```
setup():  loadConfig() → initDisplay() [splash]  →  xTaskCreate(displayTask, "disp", 4096, …, 1)
displayTask():  pick screen (priority, §5) → render via Adafruit_GFX* → flush → delay(250 ms)
```

- `initDisplay()` is a no-op when `dispType == 0`; on probe/begin failure it logs and leaves
  `gfx == nullptr` (no hang — I²C `begin()` NAK-times out; SPI begin always returns).
- Boot sequence on the panel: **"LumiGate vX.Y.Z"** splash → **"Connecting…"** → status once
  `netConnected()`.
- Color path is **direct-draw** (Adafruit_SSD1351 keeps no full canvas → low RAM). To avoid
  flicker the status renderer erases+redraws only the text rect that changed, not the whole
  screen.

---

## 5. Screens — status + auto-rotate

One **status screen** is the resting state. The task switches to a full-screen **alert banner**
while a condition is active, then falls back. No button required.

**Priority (first active wins):** `hasConflict()` → `IDENTIFY` → `manualMode` → status.
Banners dwell ≥1.5 s so a brief blip stays readable.

### 128×64 mono / 128×128 color (8 rows)

```
┌────────────────────────────┐
│ LumiGate           1.0.42  │  title + FIRMWARE_VERSION
│ 192.168.1.50               │  netLocalIP()  — the #1 walk-up info
│ Uni 0    sACN              │  cfg.universe + active protocol name
│ FPS 44.0     Src 1         │  fps + activeSenderCount()
│ WiFi ▂▄▆_ -58dBm    ● LIVE │  RSSI bars (or "ETH ↑") + DMX live/idle
└────────────────────────────┘
```

- Link row: WiFi shows RSSI bars + dBm (`netRSSI()`); Ethernet shows `ETH ↑/↓`.
- DMX state: `● LIVE` when `millis() - lastDmxMs < 1500` (the LED's threshold), else `idle`.
- **Color panel (SSD1351)** reuses the device's existing status palette: title accent + LIVE dot
  **green** when active, **amber** when idle, **red** on no-link; the conflict banner fills
  **red**. Same color language as the WS2812 status LED, so the panel and LED always agree.

> **Mono panel color is irrelevant.** White / blue / yellow / yellow-blue panels are all 1 bpp —
> the color is the physical emitter, not addressable. The yellow/blue split panel has a *fixed*
> divide (top 16 px yellow, rest blue) with a small dead gap at y=16; the 64-px layout puts the
> title in y=0..15 (the yellow band) and keeps the first data row at y≥18 so nothing straddles
> the gap. Split panels are 128×64 only.

### 128×32 mono (4 rows)

Title + RSSI bars dropped; live dot moves up beside the IP:

```
┌────────────────────────────┐
│ 192.168.1.50        ● LIVE │
│ Uni 0   sACN               │
│ FPS 44.0  Src 1   -58dBm   │
└────────────────────────────┘
```

The renderer branches on `gfx->height()`, so the same data helpers feed every layout; banners
reuse one centred-text routine with a font chosen by height. **Why size/type is configured, not
detected:** over I²C both heights answer at 0x3C with no resolution read-back, and SPI panels
can't be enumerated at all — so the panel is part of `dispType`, picked in `/config`.

### Future extra screens (not in v1)

Button-cycled diagnostics — DMX mini-bargraph, RDM device count (`rdmCount`), uptime/heap. The
screen-selector enum is built to extend.

---

## 6. platformio.ini changes

```ini
[env]                       # add to shared lib_deps
lib_deps =
    …
    adafruit/Adafruit GFX Library @ ^1.11.11
    adafruit/Adafruit SSD1306     @ ^2.5.13
    adafruit/Adafruit SH110X      @ ^2.1.12
    adafruit/Adafruit SSD1351 OLED Library @ ^1.3.2

[env:esp32dev]    build_flags = … -DDEF_DISP_SDA=21 -DDEF_DISP_SCL=22
[env:esp32s3dev]  build_flags = … -DDEF_DISP_SDA=8  -DDEF_DISP_SCL=9
[env:wt32eth01]   build_flags = … -DDEF_DISP_SDA=14 -DDEF_DISP_SCL=15
[env:wokwi]       build_flags = … -DDEF_DISP_SDA=8  -DDEF_DISP_SCL=9   # + diagram.json SSD1306
```

`dispType` stays `0` by default in all envs; the user enables the panel from `/config`.

---

## 7. Color TFT — later (esp32 / esp32-s3 only)

Same `Adafruit_GFX*` renderer, additional `dispType` values for `ST7735 160×128` /
`ST7789 240×240` / `ILI9341 320×240` behind their Adafruit drivers, reusing the same SPI pin
config keys from §3. The color SSD1351 work lands most of this — TFT is then just more driver
cases + a larger-font layout. Out of scope for issue #5's first cut.

---

## 8. Verification plan

- **Wokwi**: add a `board-ssd1306` part to [`diagram.json`](../diagram.json) on the `wokwi`
  env's SDA/SCL → status screen + auto-rotate render in simulation (alongside `SIM_ARTNET`) with
  no hardware. (Wokwi has no SSD1351 part, so the **color panel is verified on hardware.**)
- **Hardware**: the **Retoo SH1106** on `esp32dev`/`esp32s3dev` (I²C 21/22 or 8/9) → mono path,
  address probe, rotation; the **DollaTek SSD1351** on SPI → color path, status palette; trigger
  the conflict banner by pointing two Art-Net sources at one universe.

---

## 9. Build phases

1. ✅ **Config plumbing (mono I²C)** — `Config` fields, NVS, `handleConfigPost`, `handleInfoJson`,
   `/config` "Display" card + JS. (`dispType 0–3` + SDA/SCL/rot; `/info.json` round-trips them)
2. Library swap to Adafruit_GFX family; `initDisplay()` factory + I²C probe + splash; add
   `dispType 4` (SSD1351) + SPI config keys; per-board `DEF_DISP_*`.
3. `displayTask()` + status screen (height-aware mono + color layouts).
4. Auto-rotate banners (conflict / identify / manual) with priority + dwell; color status palette.
5. Wokwi `diagram.json` SSD1306 + README "Display" section.
