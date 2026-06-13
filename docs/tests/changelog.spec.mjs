// Change log: a DMX value change arriving over the network must show up in
// /log.json and the status-page change-log card.
import { test, expect } from '@playwright/test';
import { deviceHost, UdpSender, streamFor, artDmxPacket, prepInput, sleep, ART_PORT } from './lib/net.mjs';
import { changelog, universes, pollFor } from './lib/device.mjs';

let host;
test.beforeAll(async () => { host = await deviceHost(); await prepInput(host); });

test('a channel change is recorded in /log.json', async ({ request }) => {
  const { art } = await universes(request);
  const sender = new UdpSender(host);
  try {
    const base = Buffer.alloc(512);                 // baseline: ch1 = 0
    await streamFor(sender, ART_PORT, () => artDmxPacket(art, base), { ms: 600 });
    await sleep(300);                               // clear the 200 ms log throttle
    const changed = Buffer.alloc(512); changed[0] = 250;  // ch1 → 250
    await streamFor(sender, ART_PORT, () => artDmxPacket(art, changed), { ms: 600 });

    const log = await pollFor(() => changelog(request),
      (l) => l.length && l[0].ch.some((c) => c[0] === 1 && c[1] === 250));
    expect(log.length, 'log should have entries').toBeGreaterThan(0);
    const newest = log[0];
    expect(newest.ch.some((c) => c[0] === 1 && c[1] === 250), 'ch1=250 logged').toBeTruthy();
    expect(newest.u, 'entry tagged with the output universe').toBe(art);
    expect(newest.n).toBeGreaterThanOrEqual(1);
  } finally { sender.close(); }
});

test('change-log card on the status page shows the change', async ({ page, request }) => {
  const { art } = await universes(request);
  await page.goto('/');
  await expect(page.locator('#ws-badge')).toHaveText(/Live/);
  const sender = new UdpSender(host);
  try {
    const base = Buffer.alloc(512);
    await streamFor(sender, ART_PORT, () => artDmxPacket(art, base), { ms: 500 });
    await sleep(300);
    const changed = Buffer.alloc(512); changed[6] = 211;  // ch7 → 211
    const streaming = streamFor(sender, ART_PORT, () => artDmxPacket(art, changed), { ms: 3000 });
    await expect(page.locator('#log-body')).toContainText('211', { timeout: 6000 });
    await streaming;
  } finally { sender.close(); }
});
