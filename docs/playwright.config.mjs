// Playwright config for the LumiGate web-UI / API test suite.
//
// Tests run against a *live device* (the same one screenshot.mjs drives).
// Resolve order: LUMIGATE_URL > mDNS lookup of LUMIGATE_HOST > fallback IP.
// Headless Chromium can't resolve *.local itself, so we resolve to an IP here.
import { defineConfig } from '@playwright/test';
import dns from 'dns/promises';

const FALLBACK_IP = '192.168.178.197';

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

const baseURL = await resolveBase();
console.log('LumiGate test target:', baseURL);

export default defineConfig({
  testDir: './tests',
  fullyParallel: false,          // one device, keep requests serial
  workers: 1,
  timeout: 30_000,
  expect: { timeout: 10_000 },
  reporter: [['list']],
  use: {
    baseURL,
    headless: true,
    actionTimeout: 10_000,
    navigationTimeout: 15_000,
  },
});
