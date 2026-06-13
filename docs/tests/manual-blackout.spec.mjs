// Manual override + blackout, driven through the real web UI (the same
// WebSocket control path a user clicks), verified via /dmx.json.
import { test, expect } from '@playwright/test';
import { deviceHost, setManual } from './lib/net.mjs';
import { dmx, pollFor } from './lib/device.mjs';

let host;
test.beforeAll(async () => { host = await deviceHost(); });
// Always leave the device back in passthrough mode for the other specs.
test.afterEach(async () => { await setManual(host, false); });

test('manual override + per-channel set reaches the DMX buffer', async ({ page, request }) => {
  await page.goto('/');
  await expect(page.locator('#ws-badge')).toHaveText(/Live/);

  // Enable manual override (freezes network input on the monitored output).
  await page.locator('#modeSwitch').check();
  await expect.poll(async () => (await dmx(request)).manual).toBe(true);

  // Set channel 1 to 200 via the channel modal slider.
  await page.locator('#ch1').click();
  await page.locator('#ch-slider').fill('200');
  await page.locator('#ch-slider').dispatchEvent('input');
  const after = await pollFor(() => dmx(request), (d) => d.ch[0] === 200);
  expect(after.ch[0]).toBe(200);
});

test('blackout zeroes the buffer', async ({ page, request }) => {
  await page.goto('/');
  await expect(page.locator('#ws-badge')).toHaveText(/Live/);
  await page.locator('#modeSwitch').check();
  await expect.poll(async () => (await dmx(request)).manual).toBe(true);

  // Put something non-zero up first, then black out.
  await page.locator('#ch1').click();
  await page.locator('#ch-slider').fill('255');
  await page.locator('#ch-slider').dispatchEvent('input');
  await pollFor(() => dmx(request), (d) => d.ch[0] === 255);
  await page.locator('#modal').getByText('Done').click();

  await page.getByRole('button', { name: 'Blackout' }).click();
  const after = await pollFor(() => dmx(request), (d) => d.ch.every((v) => v === 0));
  expect(after.ch.every((v) => v === 0)).toBeTruthy();
});
