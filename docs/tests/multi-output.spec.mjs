// Multi-universe / multi-output feature tests (issue #4).
//
// Runs against a live LumiGate (see playwright.config.mjs for target resolution).
// The default tests are read-only (safe to run any time). The config round-trip
// test mutates + reboots the device and is opt-in via LUMIGATE_WRITE=1.
import { test, expect } from '@playwright/test';

const OUT_KEYS = ['en', 'uni', 'port', 'tx', 'rx', 'rts'];

async function getInfo(request) {
  const res = await request.get('/info.json');
  expect(res.ok(), 'GET /info.json should succeed').toBeTruthy();
  return res.json();
}

// Poll /info.json across a reboot until `pred(info)` holds (or time out).
async function waitForState(request, pred, ms = 45_000) {
  const t0 = Date.now();
  await new Promise((r) => setTimeout(r, 2_000)); // let the reboot begin
  while (Date.now() - t0 < ms) {
    try {
      const res = await request.get('/info.json', { timeout: 3_000 });
      if (res.ok()) { const d = await res.json(); if (pred(d)) return d; }
    } catch { /* device is mid-reboot — keep polling */ }
    await new Promise((r) => setTimeout(r, 2_000));
  }
  throw new Error('device did not reach the expected state in time');
}

// Rebuild a full /config form body from an /info.json snapshot, applying
// overrides to output 1. Sending every field avoids clobbering other settings.
function configForm(info, o1Overrides = {}) {
  const f = {
    protocol: String(info.protocol),
    hostname: info.hostname,
    otapw: info.otapw,
    ledtype: String(info.ledType),
    ledpin: String(info.ledPin),
    ip: info.sip || '',
    gateway: info.gateway || '',
    subnet: info.subnet || '',
    dns: info.dns || '',
  };
  if (info.staticIp) f.staticip = '1';
  const outs = [info.outputs[0], { ...info.outputs[1], ...o1Overrides }];
  outs.forEach((o, i) => {
    if (o.en) f[`o${i}_en`] = '1';          // omitted key == disabled
    f[`o${i}_uni`]  = String(o.uni);
    f[`o${i}_port`] = String(o.port);
    f[`o${i}_tx`]   = String(o.tx);
    f[`o${i}_rx`]   = String(o.rx);
    f[`o${i}_rts`]  = String(o.rts);
  });
  return f;
}

test.describe('Multi-output (issue #4)', () => {
  test('/info.json exposes a 2-output array with the right shape', async ({ request }) => {
    const d = await getInfo(request);
    expect(Array.isArray(d.outputs), 'outputs should be an array').toBeTruthy();
    expect(d.outputs.length).toBe(2);
    for (const o of d.outputs) {
      for (const k of OUT_KEYS) expect(o, `output missing "${k}"`).toHaveProperty(k);
      expect(o.port === 1 || o.port === 2, 'port is 1 or 2').toBeTruthy();
      expect(o.uni).toBeGreaterThanOrEqual(0);
      expect(o.uni).toBeLessThanOrEqual(15);
    }
    expect(d).toHaveProperty('rdmOut');
  });

  test('legacy "universe" field mirrors output A (back-compat)', async ({ request }) => {
    const d = await getInfo(request);
    expect(d.universe).toBe(d.outputs[0].uni);
  });

  test('migration leaves Output A enabled', async ({ request }) => {
    // A device updated from single-universe firmware must keep driving its line.
    const d = await getInfo(request);
    expect(d.outputs[0].en).toBe(true);
  });

  test('rdmOut points at an enabled output with an RTS pin, or is -1', async ({ request }) => {
    const d = await getInfo(request);
    if (d.rdmOut === -1) return;
    expect(d.rdmOut).toBeGreaterThanOrEqual(0);
    expect(d.rdmOut).toBeLessThan(d.outputs.length);
    const o = d.outputs[d.rdmOut];
    expect(o.en, 'RDM output must be enabled').toBe(true);
    expect(o.rts, 'RDM output must have an RTS pin').toBeGreaterThanOrEqual(0);
  });

  test('enabled outputs use distinct UART ports', async ({ request }) => {
    const d = await getInfo(request);
    const ports = d.outputs.filter((o) => o.en).map((o) => o.port);
    expect(new Set(ports).size, 'no two enabled outputs share a UART').toBe(ports.length);
  });

  test('config page builds an Output A and Output B block', async ({ page }) => {
    await page.goto('/config');
    await expect(page.locator('.out-card')).toHaveCount(2);
    await expect(page.locator('.out-card .out-title').nth(0)).toHaveText(/Output A/);
    await expect(page.locator('.out-card .out-title').nth(1)).toHaveText(/Output B/);
    // Cloned-template fields are renamed per output index.
    for (const n of ['o0_uni', 'o0_port', 'o0_tx', 'o0_rx', 'o0_rts',
                     'o1_uni', 'o1_port', 'o1_tx', 'o1_rx', 'o1_rts']) {
      await expect(page.locator(`[name="${n}"]`)).toHaveCount(1);
    }
    await expect(page.locator('#o0_en')).toBeChecked(); // Output A enabled
  });

  test('status page View selector matches the number of enabled outputs', async ({ page, request }) => {
    const d = await getInfo(request);
    const enabled = d.outputs.filter((o) => o.en).length;
    await page.goto('/');
    const wrap = page.locator('#out-sel-wrap');
    if (enabled > 1) {
      await expect(wrap).toBeVisible();
      await expect(wrap.locator('#out-sel button')).toHaveCount(enabled);
    } else {
      await expect(wrap).toBeHidden();
    }
  });

  // Opt-in: mutates and reboots the device (twice). Enable Output B as a
  // same-universe splitter, confirm it persists, then restore the original.
  test('config round-trip: enable Output B as a splitter, then restore', async ({ request }) => {
    test.skip(process.env.LUMIGATE_WRITE !== '1',
      'set LUMIGATE_WRITE=1 to run device-mutating tests (reboots the device twice)');
    test.setTimeout(120_000);   // two reboots
    // Output B needs a real TX GPIO to be accepted (the sanitizer drops pin-less
    // outputs). Default is an ESP32-S3-safe free pin; override per board.
    const txB = Number(process.env.LUMIGATE_TXB || 18);
    const before = await getInfo(request);
    try {
      await request.post('/config', {
        form: configForm(before, { en: true, uni: before.outputs[0].uni, port: 2, tx: txB, rx: -1, rts: -1 }),
      });
      const mid = await waitForState(request, (d) => d.outputs[1].en === true);
      expect(mid.outputs[1].en).toBe(true);
      expect(mid.outputs[1].port).toBe(2);
      expect(mid.outputs[1].tx).toBe(txB);
      expect(mid.outputs[1].uni, 'splitter shares output A universe').toBe(before.outputs[0].uni);
    } finally {
      await request.post('/config', { form: configForm(before) });
      await waitForState(request, (d) => d.outputs[1].en === before.outputs[1].en);
    }
  });

  // Regression: enabling an output with no TX pin (tx=-1) once boot-looped the
  // device (esp_dmx crashed in initDmx). It must now be sanitized to disabled
  // and the device must stay reachable.
  test('enabling an output with no TX pin is sanitized, not bricked', async ({ request }) => {
    test.skip(process.env.LUMIGATE_WRITE !== '1',
      'set LUMIGATE_WRITE=1 to run device-mutating tests (reboots the device)');
    test.setTimeout(120_000);   // two reboots
    const before = await getInfo(request);
    try {
      await request.post('/config', {
        form: configForm(before, { en: true, port: 2, tx: -1, rx: -1, rts: -1 }),
      });
      // Device must come back online at all (proves no boot loop)...
      const after = await waitForState(request, (d) => d && Array.isArray(d.outputs));
      // ...and the pin-less output must have been forced off.
      expect(after.outputs[1].en, 'pin-less output disabled by the sanitizer').toBe(false);
    } finally {
      await request.post('/config', { form: configForm(before) });
      await waitForState(request, (d) => d.outputs[1].en === before.outputs[1].en);
    }
  });
});
