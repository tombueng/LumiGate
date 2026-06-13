// Art-Net → DMX end-to-end: real ArtDMX UDP packets on the wire, observed
// through the REST API and the live status-page grid.
import { test, expect } from '@playwright/test';
import { deviceHost, UdpSender, streamFor, artDmxPacket, prepInput, ART_PORT } from './lib/net.mjs';
import { dmx, senders, universes, pollFor } from './lib/device.mjs';

let host;
test.beforeAll(async () => { host = await deviceHost(); await prepInput(host); });

// A recognisable pattern: ch1=255, ch2=128, ch10=64 (1-based), rest 0.
function pattern() {
  const d = Buffer.alloc(512);
  d[0] = 255; d[1] = 128; d[9] = 64;
  return d;
}

test('Art-Net frame updates the monitored DMX buffer', async ({ request }) => {
  const { art } = await universes(request);
  const sender = new UdpSender(host);
  try {
    const p = pattern();
    await streamFor(sender, ART_PORT, () => artDmxPacket(art, p), { ms: 1200 });
    const d = await pollFor(() => dmx(request),
      (x) => x.ch[0] === 255 && x.ch[1] === 128 && x.ch[9] === 64);
    expect(d.ch[0]).toBe(255);
    expect(d.ch[1]).toBe(128);
    expect(d.ch[9]).toBe(64);
  } finally { sender.close(); }
});

test('status-page grid reflects Art-Net values live', async ({ page, request }) => {
  const { art } = await universes(request);
  await page.goto('/');
  await expect(page.locator('#ws-badge')).toHaveText(/Live/);
  const sender = new UdpSender(host);
  try {
    const d = Buffer.alloc(512);
    d[0] = 200; d[4] = 99;                      // ch1=200, ch5=99
    const streaming = streamFor(sender, ART_PORT, () => artDmxPacket(art, d), { ms: 2500 });
    await expect(page.locator('#v1')).toHaveText('200');
    await expect(page.locator('#v5')).toHaveText('99');
    await streaming;
  } finally { sender.close(); }
});

test('Art-Net sender is tracked with FPS', async ({ request }) => {
  const { art } = await universes(request);
  const sender = new UdpSender(host);
  try {
    const p = pattern();
    await streamFor(sender, ART_PORT, () => artDmxPacket(art, p), { ms: 1800, hz: 40 });
    const list = await pollFor(() => senders(request),
      (s) => s.some((x) => x.p === 0 && x.ago <= 2));
    const art0 = list.find((x) => x.p === 0);
    expect(art0, 'an Art-Net sender should be listed').toBeTruthy();
    expect(art0.fps).toBeGreaterThan(0);
    // global fps stat should also have climbed
    const d = await dmx(request);
    expect(Number(d.fps)).toBeGreaterThan(0);
  } finally { sender.close(); }
});
