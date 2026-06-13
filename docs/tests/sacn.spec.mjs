// sACN / E1.31 → DMX end-to-end: real E1.31 data packets (unicast to the
// device's 5568 socket), observed through the REST API and the live grid.
import { test, expect } from '@playwright/test';
import { deviceHost, UdpSender, streamFor, e131Packet, prepInput, SACN_PORT } from './lib/net.mjs';
import { dmx, senders, universes, pollFor } from './lib/device.mjs';

let host;
test.beforeAll(async () => { host = await deviceHost(); await prepInput(host); });

test('sACN frame updates the monitored DMX buffer', async ({ request }) => {
  const { sacn } = await universes(request);
  const sender = new UdpSender(host);
  try {
    const d = Buffer.alloc(512);
    d[0] = 17; d[2] = 222; d[100] = 5;          // ch1=17, ch3=222, ch101=5
    await streamFor(sender, SACN_PORT, (i) => e131Packet(sacn, d, i), { ms: 1200 });
    const got = await pollFor(() => dmx(request),
      (x) => x.ch[0] === 17 && x.ch[2] === 222 && x.ch[100] === 5);
    expect(got.ch[0]).toBe(17);
    expect(got.ch[2]).toBe(222);
    expect(got.ch[100]).toBe(5);
  } finally { sender.close(); }
});

test('status-page grid reflects sACN values live', async ({ page, request }) => {
  const { sacn } = await universes(request);
  await page.goto('/');
  await expect(page.locator('#ws-badge')).toHaveText(/Live/);
  const sender = new UdpSender(host);
  try {
    const d = Buffer.alloc(512);
    d[1] = 77; d[2] = 33;                        // ch2=77, ch3=33
    const streaming = streamFor(sender, SACN_PORT, (i) => e131Packet(sacn, d, i), { ms: 2500 });
    await expect(page.locator('#v2')).toHaveText('77');
    await expect(page.locator('#v3')).toHaveText('33');
    await streaming;
  } finally { sender.close(); }
});

test('sACN sender is tracked as protocol sACN', async ({ request }) => {
  const { sacn } = await universes(request);
  const sender = new UdpSender(host);
  try {
    const d = Buffer.alloc(512); d[0] = 10;
    await streamFor(sender, SACN_PORT, (i) => e131Packet(sacn, d, i), { ms: 1800, hz: 40 });
    const list = await pollFor(() => senders(request),
      (s) => s.some((x) => x.p === 1 && x.ago <= 2));
    const sacnSender = list.find((x) => x.p === 1);
    expect(sacnSender, 'a sACN sender should be listed').toBeTruthy();
    expect(sacnSender.fps).toBeGreaterThan(0);
  } finally { sender.close(); }
});
