<img src="docs/logo.png" alt="LumiGate" width="120">

# LumiGate

**Art-Net → DMX512 Gateway** based on ESP32 with a live web UI, WebSocket push, and manual DMX control via browser.

---

## Features

| Feature | Details |
|---|---|
| **Art-Net → DMX512** | Full 512-channel, unicast or broadcast, universe configurable |
| **Live Web UI** | Dark Tailwind UI, WebSocket push ~25 fps, all 512 channels visible |
| **Manual DMX control** | Click any channel in browser, set value via slider |
| **Blackout button** | Zero all channels instantly from browser |
| **Art-Net / Manual toggle** | Switch between Art-Net passthrough and manual override |
| **WiFi Config Portal** | First-boot AP + captive portal via WiFiManager |
| **OTA Updates** | ArduinoOTA over WiFi, password protected |
| **mDNS** | Reachable as `lumigate.local` (configurable) |
| **REST API** | `GET /dmx.json` — all 512 values + stats as JSON |
| **NVS persistence** | Universe, hostname, OTA password survive reboots |
| **Config reset** | Hold BOOT button 3 s on startup, or via `/reset` page |
| **Status LED** | Off = no WiFi · Heartbeat = idle · Solid = Art-Net active |

---

## Hardware

### Bill of Materials

| # | Component | Description | Link |
|---|---|---|---|
| 1 | **ESP32 DevKit v1** | ESP32-WROOM-32, 30-pin, any CH340/CP2102 variant | [Amazon.de search](https://www.amazon.de/s?k=ESP32+DevKit+WROOM-32) |
| 2 | **Waveshare TTL to RS485 (C)** | Galvanically isolated RS485 transceiver, auto-direction | [Amazon.de – B0D4TZQYVG](https://www.amazon.de/dp/B0D4TZQYVG) |
| 3 | **XLR-5 female panel socket** | Standard DMX output connector (XLR-3 also works) | [Amazon.de search](https://www.amazon.de/s?k=XLR+5+Pin+Buchse+Panel) |
| 4 | **Jumper wires / dupont cables** | Male–male for breadboard, or direct solder | any |
| 5 | **USB-A to Micro-USB cable** | Power + serial flash | any |

**Optional for enclosure:**
- Project box / DIN-rail enclosure
- 120 Ω termination resistor across DMX A/B at the cable end (required for long runs)

---

### The RS485 Module: Waveshare TTL to RS485 (C)

This module is the key interface between the ESP32's UART and the DMX RS485 bus.

| Property | Value |
|---|---|
| Product name | Waveshare TTL to RS485 (C) |
| Isolation | Galvanic, 2500 V RMS (SP3485 + isolation transformer) |
| Direction control | Automatic — no DE/RE pin needed |
| TTL voltage | 3.3 V or 5 V (VCC selects level) |
| Max baudrate | ≥ 250 kbps (verified working at DMX rate) |
| Connector (TTL side) | 6-pin 2.54 mm header: VCC / GND / RXD / TXD / (unused) / (unused) |
| Connector (RS485 side) | Screw terminal: A / B / GND |
| Dimensions | ~38 × 14 mm |

> **Why galvanic isolation?**  
> DMX fixtures are often powered from separate circuits or have ground loops. The isolated module prevents ground noise from corrupting the signal and protects the ESP32 from voltage spikes.

> **Why auto-direction?**  
> Standard RS485 requires toggling a DE/RE pin to switch between transmit and receive. The Waveshare (C) variant handles this internally based on line activity — no GPIO needed, firmware is simpler. Trade-off: RDM (which requires half-duplex arbitration) is not supported by this module.

---

### Pinout Reference

**Waveshare TTL to RS485 (C) — TTL header (left side):**

```
Pin 1  VCC   → ESP32 3.3V
Pin 2  GND   → ESP32 GND
Pin 3  RXD   → ESP32 GPIO16 (UART2 RX)
Pin 4  TXD   → ESP32 GPIO17 (UART2 TX)
Pin 5  —     (not connected)
Pin 6  —     (not connected)
```

**Waveshare TTL to RS485 (C) — RS485 screw terminal (right side):**

```
A  →  DMX XLR pin 3  (Data+)
B  →  DMX XLR pin 2  (Data–)
GND→  DMX XLR pin 1  (Shield/Ground)  [optional but recommended]
```

---

### Wiring Diagram

![LumiGate Wiring Diagram](docs/wiring.svg)

**Connection table:**

| ESP32 pin | → | Module pin | → | XLR pin |
|---|---|---|---|---|
| GPIO17 | → | TXD | — | — |
| GPIO16 | ← | RXD | — | — |
| 3.3V | → | VCC | — | — |
| GND | → | GND | — | — |
| — | — | A (RS485+) | → | Pin 3 |
| — | — | B (RS485–) | → | Pin 2 |
| GND | — | GND | → | Pin 1 (optional) |

---

### Assembly Guide

#### Step 1 — Prepare the ESP32

No soldering required if using a DevKit with pre-soldered headers. Place it on a breadboard or in your enclosure.

#### Step 2 — Connect ESP32 ↔ RS485 module

Using dupont jumper wires:

| Color convention | From | To |
|---|---|---|
| Red | ESP32 **3.3V** | Module **VCC** |
| Black | ESP32 **GND** | Module **GND** |
| Yellow | ESP32 **GPIO17** | Module **TXD** |
| Green | ESP32 **GPIO16** | Module **RXD** |

> Use **3.3V**, not 5V — the ESP32's GPIOs are not 5V tolerant.

#### Step 3 — Wire the XLR connector

The DMX standard uses XLR-5 (5-pin), but most DMX fixtures also accept XLR-3. Pin numbering is the same:

| XLR Pin | Signal | RS485 module |
|---|---|---|
| 1 | Shield / GND | Module GND (optional) |
| 2 | DMX Data– | Module **B** |
| 3 | DMX Data+ | Module **A** |
| 4 | (unused in DMX) | — |
| 5 | (unused in DMX) | — |

Screw the wires into the RS485 module's terminal block firmly.

#### Step 4 — Termination resistor (for longer cable runs)

For DMX cable runs over ~10 m, solder a **120 Ω resistor** between pins 2 and 3 (A and B) at the **far end** of the DMX chain (i.e., inside the last fixture's XLR input). Most professional fixtures include this internally.

#### Step 5 — Power

The ESP32 DevKit is powered via its **Micro-USB port**. Any 5V USB power supply works (500 mA is sufficient). The RS485 module draws its power from the ESP32's 3.3V rail.

---

### Notes

- **Do not connect DE/RE** — the Waveshare (C) handles direction automatically. Connecting them will break transmission.
- **Keep TTL wires short** (< 20 cm). Long unshielded wires on the TTL side pick up noise; the RS485 side handles long distances natively.
- **XLR gender:** Use a **female** XLR panel socket for the DMX output. DMX sources (transmitters) use female XLR; receivers use male XLR. LumiGate is a source.

---

## Software Stack

| Library | Purpose |
|---|---|
| `someweisguy/esp_dmx ^4.1` | DMX512 transmit via UART |
| `rstephan/ArtnetWifi ^1.5` | Art-Net UDP receiver |
| `tzapu/WiFiManager ^2.0` | WiFi config portal |
| `links2004/WebSockets ^2.4` | WebSocket server (port 81) |
| `ArduinoOTA` | OTA firmware updates |
| `ESPmDNS` | mDNS (`lumigate.local`) |
| `Preferences` | NVS persistent config |
| `WebServer` | HTTP server (port 80) |

---

## Build & Flash

### Requirements

- [PlatformIO](https://platformio.org/) with VS Code extension
- ESP32 connected via USB (CH340 or CP2102)

### First Flash (USB)

```bash
pio run --target upload
```

> **If upload fails** ("Wrong boot mode"): Hold **BOOT** button, tap **EN/RST**, release BOOT — chip enters download mode. Then retry upload.
>
> Or use the included flash helper:
> ```bash
> bash ~/.claude/flash_esp32.sh COM3 460800
> ```
> The script gives you an 8-second countdown to press BOOT+EN.

### OTA Updates (after first flash)

Uncomment in `platformio.ini`:
```ini
upload_protocol = espota
upload_port     = lumigate.local
upload_flags    = --auth=dmxota
```

---

## First Setup

### 1. Config Portal

On first boot (or after WiFi reset), LumiGate opens a WiFi access point:

- **SSID:** `DMX-Gateway` (no password)
- Connect with phone or PC → browser auto-opens portal (or go to `192.168.4.1`)
- Select your network, enter password, set Art-Net Universe (default: `0`)
- Click **Save** → LumiGate connects and reboots

### 2. Status Page

Open `http://lumigate.local` (or the IP shown in serial monitor at 115200 baud):

- Live stats: framerate, RSSI, uptime, free heap
- All 512 DMX channels as a color-coded grid (cyan = active)
- Click any cell → slider appears → set value directly

### 3. Config Reset

| Method | How |
|---|---|
| Web | `http://lumigate.local/reset` → button |
| Hardware | Hold **BOOT** during power-on for 3 seconds |

---

## Web UI

| URL | Function |
|---|---|
| `/` | Live status + 512-channel DMX grid |
| `/config` | Change universe, hostname, OTA password |
| `/reset` | Clear WiFi credentials, reboot to AP mode |
| `/dmx.json` | JSON API: all 512 values, fps, rssi, uptime, heap |

### WebSocket (port 81)

Binary push frame at up to 25 fps:

```
Bytes  0–1   fps × 10        uint16 big-endian
Bytes  2–3   RSSI (dBm)      int16  big-endian
Bytes  4–7   free heap       uint32 big-endian
Bytes  8–11  uptime (s)      uint32 big-endian
Bytes 12–523 DMX ch 1–512   uint8[512]
```

Browser → ESP32 (JSON text):
```json
{ "type": "set",     "ch": 1,   "val": 200  }
{ "type": "mode",    "manual":  true         }
{ "type": "blackout"                         }
```

---

## QLC+ Setup

1. Open **Eingänge/Ausgänge** tab
2. Click in the output area right of **Universe 1** → select **ArtNet** → interface `192.168.x.x` (your PC's IP)
3. Configure: IP = LumiGate's IP, Art-Net Universe = `0`
4. Test with **Einfaches Mischpult** → move fader → grid lights up cyan

> **Universe mapping:** QLC+ "Universe 1" = Art-Net Universe `0`. LumiGate defaults to Universe `0`.

---

## Status LED (GPIO2 / D2)

| State | LED |
|---|---|
| No WiFi | Off |
| Connected, no Art-Net | Short pulse every 2 s (heartbeat) |
| Art-Net receiving | Solid on |

---

## Persistent Configuration (NVS)

| Key | Default | Change via |
|---|---|---|
| Art-Net Universe | `0` | Web `/config` or portal |
| Hostname | `dmx-gateway` | Web `/config` |
| OTA Password | `dmxota` | Web `/config` |
| WiFi credentials | — | Config portal or `/reset` |

---

## Roadmap / Ideas

- [ ] Scenes / presets (save & recall DMX snapshots)
- [ ] Fade engine (smooth transitions between scenes)
- [ ] Failsafe scene (auto-load when Art-Net signal lost)
- [ ] E1.31 sACN support
- [ ] MQTT integration (Home Assistant / Node-RED)
- [ ] Channel labels / fixture naming
- [ ] RDM support (requires module with controllable DE/RE pin, e.g. SP3485)
- [ ] Multi-universe (multiple RS485 ports)

---

## License

MIT — do whatever you want, attribution appreciated.
