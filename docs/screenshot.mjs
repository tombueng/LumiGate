import { chromium } from '@playwright/test';
import { mkdirSync, readdirSync, rmSync, existsSync } from 'fs';
import { execSync } from 'child_process';
import dns from 'dns/promises';

// ── Config ───────────────────────────────────────────────────────────────────
// Resolve the device: LUMIGATE_URL > mDNS lookup of LUMIGATE_HOST > fallback IP.
// (Headless Chromium can't resolve *.local itself, so we resolve to an IP here.)
const FALLBACK_IP = '192.168.178.197';
const OUT         = 'C:/dev/DMX/docs';
const VID_RAW     = OUT + '/video-raw';
const RUN_OTA     = process.env.LUMIGATE_OTA === '1';   // off by default (reflashes device)
const RUN_VIDEO   = process.env.LUMIGATE_NOVIDEO !== '1';

async function resolveBase() {
  if (process.env.LUMIGATE_URL) return process.env.LUMIGATE_URL;
  const host = process.env.LUMIGATE_HOST || 'dmx-gateway.local';
  try {
    const { address } = await dns.lookup(host, { family: 4 });
    return 'http://' + address;
  } catch {
    return 'http://' + FALLBACK_IP;
  }
}

const BASE = await resolveBase();
mkdirSync(OUT, { recursive: true });
console.log('device:', BASE);

// A visible cursor + click ripple so the walkthrough video is readable.
const CURSOR_JS = `(() => {
  if (window.__cursor) return; window.__cursor = 1;
  const dot = document.createElement('div');
  dot.style.cssText = 'position:fixed;z-index:2147483647;width:20px;height:20px;left:0;top:0;'
    + 'margin:-10px 0 0 -10px;border-radius:50%;background:rgba(88,166,255,.55);'
    + 'border:2px solid #fff;box-shadow:0 0 8px rgba(0,0,0,.6);pointer-events:none;'
    + 'transition:width .08s,height .08s,margin .08s;';
  const add = () => (document.body || document.documentElement).appendChild(dot);
  if (document.body) add(); else addEventListener('DOMContentLoaded', add);
  addEventListener('mousemove', e => { dot.style.left = e.clientX + 'px'; dot.style.top = e.clientY + 'px'; }, true);
  const press = down => { const s = down ? '12px' : '20px', m = down ? '-6px' : '-10px';
    dot.style.width = dot.style.height = s; dot.style.margin = m + ' 0 0 ' + m; };
  addEventListener('mousedown', () => press(true), true);
  addEventListener('mouseup',   () => press(false), true);
})();`;

// ─────────────────────────────────────────────────────────────────────────────
// 1. Still screenshots (status + settings; OTA pair only when LUMIGATE_OTA=1)
// ─────────────────────────────────────────────────────────────────────────────
async function shoot(browser) {
  const page = await browser.newPage();
  await page.setViewportSize({ width: 1280, height: 1400 });

  // Status page — staged with a lit, labelled grid for a compelling hero image.
  // We snapshot the device's existing labels and restore them afterward, and
  // blackout + leave manual mode so the device ends exactly as we found it.
  await page.goto(BASE + '/', { waitUntil: 'domcontentloaded' });
  await page.waitForTimeout(3000);

  const DEMO_LABELS = {
    '1': 'Front Wash L', '2': 'Front Wash R', '5': 'Haze',
    '9': 'Mover 1', '10': 'Mover 2', '17': 'Blinder',
  };
  const origLabels = await page.evaluate(async () => {
    try { return await (await fetch('/labels.json')).json(); } catch { return {}; }
  });
  await page.evaluate((demo) => {
    send({ type: 'mode', manual: true });
    // Smooth value sweep across the first rows so the grid looks alive
    for (let ch = 1; ch <= 160; ch++) {
      const v = Math.round(70 + 120 * (0.5 + 0.5 * Math.sin(ch / 7)) + (ch % 5) * 8);
      send({ type: 'set', ch, val: Math.min(255, v) });
    }
    labels = demo;
    applyLabels();
    fetch('/labels', { method: 'POST', headers: { 'Content-Type': 'application/json' },
                       body: JSON.stringify(demo) });
  }, DEMO_LABELS);
  await page.waitForTimeout(2500);              // let WS echo values back + render
  await page.screenshot({ path: OUT + '/screenshot-status.png', fullPage: false });
  console.log('status done');

  // Restore device: original labels, blackout, manual mode off
  await page.evaluate((orig) => {
    labels = orig || {};
    applyLabels();
    fetch('/labels', { method: 'POST', headers: { 'Content-Type': 'application/json' },
                       body: JSON.stringify(labels) });
    send({ type: 'blackout' });
    send({ type: 'mode', manual: false });
  }, origLabels);
  await page.waitForTimeout(1000);

  await page.goto(BASE + '/config', { waitUntil: 'domcontentloaded' });
  await page.waitForTimeout(2500);
  await page.screenshot({ path: OUT + '/screenshot-config.png', fullPage: true });
  console.log('config done');

  if (RUN_OTA) {
    await page.setViewportSize({ width: 1280, height: 800 });
    await page.goto(BASE + '/config', { waitUntil: 'domcontentloaded' });
    await Promise.all([
      page.waitForNavigation({ waitUntil: 'domcontentloaded', timeout: 10000 }).catch(() => {}),
      page.click('form[action="/ota/github"] button[type="submit"]'),
    ]);
    await page.waitForTimeout(1000);
    await page.screenshot({ path: OUT + '/screenshot-ota-progress.png', fullPage: false });
    console.log('ota progress done');

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
      } catch { /* still rebooting */ }
    }
  }
  await page.close();
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Web-UI walkthrough video — demonstrates every interactive control.
//    Does NOT submit any form that reboots/flashes (Save, Reset, OTA install).
//    Leaves the device as found (manual mode off, blackout, demo label cleared).
// ─────────────────────────────────────────────────────────────────────────────
async function recordDemo(browser) {
  const ctx = await browser.newContext({
    viewport: { width: 1280, height: 800 },
    recordVideo: { dir: VID_RAW, size: { width: 1280, height: 800 } },
  });
  await ctx.addInitScript(CURSOR_JS);
  const page = await ctx.newPage();
  page.setDefaultNavigationTimeout(60000);
  page.setDefaultTimeout(20000);

  const pause = ms => page.waitForTimeout(ms);
  async function moveTo(sel, frac = 0.5) {
    const el = page.locator(sel).first();
    await el.scrollIntoViewIfNeeded();
    await pause(150);
    const b = await el.boundingBox();
    if (!b) return null;
    const x = b.x + b.width * frac, y = b.y + b.height / 2;
    await page.mouse.move(x, y, { steps: 22 });
    return { x, y, b };
  }
  async function click(sel, frac = 0.5) {
    const p = await moveTo(sel, frac);
    if (!p) return;
    await pause(250); await page.mouse.click(p.x, p.y); await pause(450);
  }

  try {
  // ── STATUS PAGE ────────────────────────────────────────────────────────────
  await page.goto(BASE + '/', { waitUntil: 'domcontentloaded' });
  await pause(3000);                            // let live stats + WebSocket tick in

  // Enable manual override so we can drive the grid for the demo
  await click('#modeSwitch');
  await pause(600);

  // Light up the grid with a colourful pattern (over the live WebSocket)
  await page.evaluate(() => {
    let i = 1;
    const t = setInterval(() => {
      for (let k = 0; k < 24 && i <= 240; k++, i++) {
        const v = Math.round(127 + 127 * Math.sin(i / 9));
        send({ type: 'set', ch: i, val: v });
      }
      if (i > 240) clearInterval(t);
    }, 90);
  });
  await pause(2600);

  // Open a channel, name it, sweep the slider, use quick buttons, identify
  await click('#ch7');
  await pause(700);
  await click('#ch-label');
  await page.fill('#ch-label', '');
  await page.type('#ch-label', 'Front Wash L', { delay: 90 });
  await page.keyboard.press('Tab');            // commit label (onchange)
  await pause(800);

  // Sweep the slider thumb left→right→mid for a visible fade
  {
    const s = await page.locator('#ch-slider').boundingBox();
    if (s) {
      const y = s.y + s.height / 2;
      await page.mouse.move(s.x + 4, y, { steps: 10 });
      await page.mouse.down();
      await page.mouse.move(s.x + s.width - 4, y, { steps: 40 });
      await pause(400);
      await page.mouse.move(s.x + 4, y, { steps: 40 });
      await pause(300);
      await page.mouse.move(s.x + s.width * 0.6, y, { steps: 30 });
      await page.mouse.up();
      await pause(600);
    }
  }
  await click('button[onclick="setQuick(0)"]');     // Off
  await pause(400);
  await click('button[onclick="setQuick(128)"]');   // 50%
  await pause(400);
  await click('button[onclick="setQuick(255)"]');   // Full
  await pause(500);
  await click('button[onclick="identify()"]');      // Identify flash
  await pause(1600);
  await click('button[onclick="closeModal()"]');    // Done
  await pause(700);

  // Blackout everything
  await click('button[onclick="sendBlackout()"]');
  await pause(1500);

  // ── SETTINGS PAGE ────────────────────────────────────────────────────────────
  await page.goto(BASE + '/config', { waitUntil: 'domcontentloaded' });
  await pause(2200);

  // Protocol dropdown
  await page.selectOption('#proto-sel', '0'); await pause(700);
  await page.selectOption('#proto-sel', '1'); await pause(700);
  await page.selectOption('#proto-sel', '2'); await pause(700);

  // Universe spinner
  await click('#uni-inp');
  await page.fill('#uni-inp', '4'); await page.dispatchEvent('#uni-inp', 'input'); await pause(900);
  await page.fill('#uni-inp', '0'); await page.dispatchEvent('#uni-inp', 'input'); await pause(600);

  // Static IP toggle reveals the network fields
  await click('#static-sw'); await pause(1200);
  await click('#static-sw'); await pause(800);

  // LED type
  await page.selectOption('#led-type', '1'); await pause(700);
  await page.selectOption('#led-type', '2'); await pause(700);

  // Scroll through the firmware version table + auto-update toggle
  await moveTo('#auto-update-sw'); await pause(900);
  await moveTo('#ver-rows'); await pause(1400);

  // Danger zone confirm checkbox (do NOT submit)
  await click('#confirm-reset'); await pause(900);
  await click('#confirm-reset'); await pause(700);  // un-tick again

  // ── Leave the device exactly as found ───────────────────────────────────────
  await page.goto(BASE + '/', { waitUntil: 'domcontentloaded' });
  await pause(1800);
  await page.evaluate(() => {
    delete labels['7'];
    fetch('/labels', { method: 'POST', headers: { 'Content-Type': 'application/json' },
                       body: JSON.stringify(labels) });
    send({ type: 'blackout' });
    send({ type: 'mode', manual: false });
  });
  await pause(1200);
  } finally {
    await ctx.close();   // always flush the .webm + close WS/HTTP, even on error
  }

  // Find the recorded webm
  const webm = readdirSync(VID_RAW).filter(f => f.endsWith('.webm')).map(f => VID_RAW + '/' + f)[0];
  if (!webm) { console.log('no video produced'); return; }
  console.log('raw video:', webm);

  // Convert to MP4 (linked in README) and an inline-playing GIF
  const mp4 = OUT + '/demo.mp4';
  const gif = OUT + '/demo.gif';
  const pal = VID_RAW + '/palette.png';
  console.log('encoding mp4...');
  execSync(`ffmpeg -y -i "${webm}" -movflags +faststart -pix_fmt yuv420p `
         + `-vf "scale=1280:-2" -c:v libx264 -crf 24 "${mp4}"`, { stdio: 'inherit' });
  console.log('encoding gif...');
  const GIF_VF = 'fps=10,scale=760:-1:flags=lanczos';
  execSync(`ffmpeg -y -i "${webm}" -vf "${GIF_VF},palettegen=max_colors=128:stats_mode=diff" "${pal}"`, { stdio: 'inherit' });
  execSync(`ffmpeg -y -i "${webm}" -i "${pal}" -filter_complex `
         + `"${GIF_VF}[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=3" "${gif}"`, { stdio: 'inherit' });
  console.log('video done:', mp4, gif);
}

// ─────────────────────────────────────────────────────────────────────────────
const browser = await chromium.launch();
try {
  await shoot(browser);
  if (RUN_VIDEO) {
    if (existsSync(VID_RAW)) rmSync(VID_RAW, { recursive: true, force: true });
    await recordDemo(browser);
  }
} finally {
  await browser.close();
}
console.log('all done');
