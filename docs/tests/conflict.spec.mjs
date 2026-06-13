// Conflict detection: two active senders on the same universe must raise the
// banner. We drive Art-Net and sACN at once — same source IP, different
// protocol = two tracked senders.
import { test, expect } from '@playwright/test';
import {
  deviceHost, UdpSender, streamFor, artDmxPacket, e131Packet, prepInput, ART_PORT, SACN_PORT,
} from './lib/net.mjs';
import { senders, universes, pollFor } from './lib/device.mjs';

let host;
test.beforeAll(async () => { host = await deviceHost(); await prepInput(host); });

test('two simultaneous senders are both tracked', async ({ request }) => {
  const { art, sacn } = await universes(request);
  const a = new UdpSender(host), s = new UdpSender(host);
  const data = Buffer.alloc(512); data[0] = 50;
  try {
    await Promise.all([
      streamFor(a, ART_PORT,  (i) => artDmxPacket(art, data, i), { ms: 2000 }),
      streamFor(s, SACN_PORT, (i) => e131Packet(sacn, data, i),  { ms: 2000 }),
    ]);
    const list = await pollFor(() => senders(request),
      (l) => l.filter((x) => x.ago <= 3).length >= 2);
    const active = list.filter((x) => x.ago <= 3);
    expect(active.length, 'two active senders expected').toBeGreaterThanOrEqual(2);
    expect(active.some((x) => x.p === 0), 'Art-Net sender present').toBeTruthy();
    expect(active.some((x) => x.p === 1), 'sACN sender present').toBeTruthy();
  } finally { a.close(); s.close(); }
});

test('conflict banner shows on the status page during a clash', async ({ page, request }) => {
  const { art, sacn } = await universes(request);
  await page.goto('/');
  await expect(page.locator('#ws-badge')).toHaveText(/Live/);
  const a = new UdpSender(host), s = new UdpSender(host);
  const data = Buffer.alloc(512); data[0] = 60;
  try {
    const streaming = Promise.all([
      streamFor(a, ART_PORT,  (i) => artDmxPacket(art, data, i), { ms: 4000 }),
      streamFor(s, SACN_PORT, (i) => e131Packet(sacn, data, i),  { ms: 4000 }),
    ]);
    await expect(page.locator('#conflict-banner')).toBeVisible({ timeout: 6000 });
    await streaming;
  } finally { a.close(); s.close(); }
});
