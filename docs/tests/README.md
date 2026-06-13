# LumiGate end-to-end test suite

Playwright tests that drive a **live LumiGate device** end-to-end: they send real
Art-Net and sACN/E1.31 packets over the network and assert the device's REST API,
WebSocket, and web UI react correctly.

## Prerequisites

- A LumiGate on the same LAN, reachable and powered.
- Node 21+ (uses the built-in global `WebSocket`).
- Dependencies + the Chromium browser:

```bash
cd docs
npm install
npx playwright install chromium
```

## Run

```bash
cd docs
npm test                 # all default (non-destructive) tests
npm run test:headed      # watch the browser
npm run report           # open the last HTML report
```

### Targeting the device

Resolution order: `LUMIGATE_URL` → mDNS lookup of `LUMIGATE_HOST` → fallback IP
(`192.168.178.197`).

```bash
LUMIGATE_HOST=dmx-gateway.local npm test
LUMIGATE_URL=http://192.168.1.50 npm test
```

### Device-mutating tests (opt-in)

A few tests change config and reboot the device (multi-output round-trip, the
boot-loop regression). They are skipped unless you opt in:

```bash
LUMIGATE_WRITE=1 npm test
```

They always restore the original configuration afterwards.

## What's covered

| Spec | Feature (network → web UI) |
|---|---|
| `web-ui.spec.mjs` | Pages load; REST contract (`/info`, `/dmx`, `/senders`, `/log`, `/version`, `/labels`, `/rdm`) |
| `artnet.spec.mjs` | Art-Net ArtDMX → DMX values, live grid, sender + FPS tracking |
| `sacn.spec.mjs` | sACN / E1.31 → DMX values, live grid, sender tracking |
| `conflict.spec.mjs` | Two simultaneous senders → conflict banner |
| `changelog.spec.mjs` | DMX changes appear in `/log.json` + the change-log card |
| `manual-blackout.spec.mjs` | Manual override + per-channel set + blackout (via the UI) |
| `labels.spec.mjs` | Channel labels round-trip + grid rendering |
| `multi-output.spec.mjs` | Issue #4: `outputs[2]` shape, migration, RDM binding, UI; splitter + pin-less-output regression (opt-in) |

## Notes

- sACN frames are sent **unicast** to the device's port 5568 (it binds
  `INADDR_ANY`), which avoids host multicast-routing quirks while still
  exercising the full E1.31 parse path.
- Tests run serially (`workers: 1`) since they share one physical device, and
  network specs reset manual override so they don't interfere with each other.
