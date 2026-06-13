// REST helpers built on the Playwright `request` fixture, plus config-form
// round-trip utilities. baseURL comes from playwright.config.mjs.
import { sleep } from './net.mjs';

export async function info(request)    { return (await request.get('/info.json')).json(); }
export async function dmx(request)     { return (await request.get('/dmx.json')).json(); }
export async function senders(request) { return (await request.get('/senders.json')).json(); }
export async function changelog(request) { return (await request.get('/log.json')).json(); }

// The Art-Net universe of output A, and the matching sACN universe (+1).
export async function universes(request) {
  const d = await info(request);
  const art = d.outputs?.[0]?.uni ?? d.universe ?? 0;
  return { art, sacn: art + 1, info: d };
}

// Retry `fetch()` until `pred(result)` is truthy (or time out). Returns the
// last result so the caller can assert on it for a clear failure message.
export async function pollFor(fetchFn, pred, { ms = 8000, every = 300 } = {}) {
  const t0 = Date.now();
  let last;
  while (Date.now() - t0 < ms) {
    try { last = await fetchFn(); if (pred(last)) return last; } catch {}
    await sleep(every);
  }
  return last;
}

// Rebuild a complete /config form body from an /info.json snapshot, applying
// overrides to output 1. Sending every field avoids clobbering other settings.
export function configForm(snapshot, o1Overrides = {}) {
  const f = {
    protocol: String(snapshot.protocol),
    hostname: snapshot.hostname,
    otapw: snapshot.otapw,
    ledtype: String(snapshot.ledType),
    ledpin: String(snapshot.ledPin),
    ip: snapshot.sip || '',
    gateway: snapshot.gateway || '',
    subnet: snapshot.subnet || '',
    dns: snapshot.dns || '',
  };
  if (snapshot.staticIp) f.staticip = '1';
  const outs = [snapshot.outputs[0], { ...snapshot.outputs[1], ...o1Overrides }];
  outs.forEach((o, i) => {
    if (o.en) f[`o${i}_en`] = '1';     // omitted key == disabled
    f[`o${i}_uni`]  = String(o.uni);
    f[`o${i}_port`] = String(o.port);
    f[`o${i}_tx`]   = String(o.tx);
    f[`o${i}_rx`]   = String(o.rx);
    f[`o${i}_rts`]  = String(o.rts);
  });
  return f;
}

// Poll /info.json across a reboot until `pred(info)` holds.
export async function waitForState(request, pred, ms = 45000) {
  await sleep(2000); // let the reboot begin
  const d = await pollFor(() => info(request), pred, { ms, every: 2000 });
  if (!d || !pred(d)) throw new Error('device did not reach the expected state in time');
  return d;
}
