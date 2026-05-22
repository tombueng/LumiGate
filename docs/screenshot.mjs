import { chromium } from '@playwright/test';
import { mkdirSync } from 'fs';

const BASE = 'http://192.168.178.196';
const OUT  = 'C:/dev/DMX/docs';
mkdirSync(OUT, { recursive: true });

const browser = await chromium.launch();
const page    = await browser.newPage();
await page.setViewportSize({ width: 1280, height: 1400 });

// ── Status page ────────────────────────────────────────────────────────────
await page.goto(BASE + '/', { waitUntil: 'networkidle' });
await page.waitForTimeout(2000);
await page.screenshot({ path: OUT + '/screenshot-status.png', fullPage: false });
console.log('status done');

// ── Config page ────────────────────────────────────────────────────────────
await page.goto(BASE + '/config', { waitUntil: 'networkidle' });
await page.waitForTimeout(500);
await page.screenshot({ path: OUT + '/screenshot-config.png', fullPage: true });
console.log('config done');

// ── OTA: click "Update from GitHub" ────────────────────────────────────────
await page.setViewportSize({ width: 1280, height: 800 });
await page.goto(BASE + '/config', { waitUntil: 'networkidle' });
// Submit the OTA form (page will navigate to progress page, device may drop)
await Promise.all([
    page.waitForNavigation({ waitUntil: 'domcontentloaded', timeout: 10000 }).catch(() => {}),
    page.click('form[action="/ota/github"] button[type="submit"]'),
]);
await page.waitForTimeout(1000);
await page.screenshot({ path: OUT + '/screenshot-ota-progress.png', fullPage: false });
console.log('ota progress done');

// ── Wait for device to come back after reboot ──────────────────────────────
console.log('waiting for device to reboot...');
for (let i = 0; i < 60; i++) {
    await page.waitForTimeout(3000);
    try {
        const resp = await page.goto(BASE + '/', { waitUntil: 'domcontentloaded', timeout: 5000 });
        if (resp && resp.ok()) {
            await page.waitForTimeout(2000);
            await page.setViewportSize({ width: 1280, height: 1400 });
            await page.screenshot({ path: OUT + '/screenshot-after-ota.png', fullPage: false });
            console.log('after-ota done');
            break;
        }
    } catch { /* device still rebooting */ }
}

await browser.close();
console.log('all done');
