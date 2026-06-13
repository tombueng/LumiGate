// Web UI loads + REST API contract (no network input required).
import { test, expect } from '@playwright/test';

test.describe('Web UI + REST', () => {
  test('status page loads with the channel grid and key cards', async ({ page }) => {
    await page.goto('/');
    await expect(page).toHaveTitle(/LumiGate/i);
    await expect(page.locator('#grid .ch')).toHaveCount(512);
    await expect(page.locator('#senders-body')).toBeVisible();
    await expect(page.locator('#log-body')).toBeVisible();
    // Subtitle is filled from /info.json (hostname · ip · Universe N · version)
    await expect(page.locator('#nav-sub')).toContainText('Universe');
  });

  test('settings page loads with protocol + outputs + network cards', async ({ page }) => {
    await page.goto('/config');
    await expect(page.locator('select[name="protocol"]')).toBeVisible();
    await expect(page.locator('.out-card')).toHaveCount(2);
    await expect(page.locator('input[name="hostname"]')).toBeVisible();
    await expect(page.locator('#save-btn')).toBeEnabled();
  });

  test('live status badge connects (WebSocket)', async ({ page }) => {
    await page.goto('/');
    await expect(page.locator('#ws-badge')).toHaveText(/Live/, { timeout: 10000 });
  });

  test('/info.json has the expected top-level fields', async ({ request }) => {
    const d = await (await request.get('/info.json')).json();
    for (const k of ['ip', 'hostname', 'version', 'protocol', 'outputs', 'ledType', 'staticIp']) {
      expect(d, `info.json missing "${k}"`).toHaveProperty(k);
    }
    expect(d.protocol).toBeGreaterThanOrEqual(0);
    expect(d.protocol).toBeLessThanOrEqual(2);
  });

  test('/dmx.json returns 512 channel values + fps + manual flag', async ({ request }) => {
    const d = await (await request.get('/dmx.json')).json();
    expect(Array.isArray(d.ch)).toBeTruthy();
    expect(d.ch.length).toBe(512);
    for (const v of d.ch) { expect(v).toBeGreaterThanOrEqual(0); expect(v).toBeLessThanOrEqual(255); }
    expect(typeof d.manual).toBe('boolean');
    expect(d).toHaveProperty('fps');
  });

  test('/senders.json and /log.json return arrays', async ({ request }) => {
    expect(Array.isArray(await (await request.get('/senders.json')).json())).toBeTruthy();
    expect(Array.isArray(await (await request.get('/log.json')).json())).toBeTruthy();
  });

  test('/version.json reports current + latest', async ({ request }) => {
    const d = await (await request.get('/version.json')).json();
    expect(d).toHaveProperty('current');
    expect(d).toHaveProperty('latest');
    expect(typeof d.update).toBe('boolean');
  });

  test('/labels.json returns a JSON object', async ({ request }) => {
    const res = await request.get('/labels.json');
    expect(res.ok()).toBeTruthy();
    expect(typeof await res.json()).toBe('object');
  });

  test('/rdm.json exposes availability + a devices array', async ({ request }) => {
    const d = await (await request.get('/rdm.json')).json();
    expect(typeof d.available).toBe('boolean');
    expect(Array.isArray(d.devices)).toBeTruthy();
  });
});
