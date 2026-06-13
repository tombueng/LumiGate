// Network helpers for the LumiGate e2e suite: Art-Net + sACN (E1.31) packet
// builders, a UDP streamer, and a thin WebSocket client (Node's built-in global
// WebSocket, available on Node 21+). All pure Node — no extra dependencies.
import dgram from 'dgram';
import dns from 'dns/promises';

export const ART_PORT  = 6454;
export const SACN_PORT = 5568;

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Resolve the device to a bare IP (headless Chromium / dgram can't use *.local).
export async function deviceHost() {
  if (process.env.LUMIGATE_URL) return new URL(process.env.LUMIGATE_URL).hostname;
  const host = process.env.LUMIGATE_HOST || 'dmx-gateway.local';
  try {
    const { address } = await dns.lookup(host, { family: 4 });
    return address;
  } catch {
    return '192.168.178.197';
  }
}

// ── Art-Net ArtDMX packet (Art-Net 4 spec) ──────────────────────────────────
export function artDmxPacket(universe, data, seq = 0) {
  const len = Math.min(512, data.length);
  const buf = Buffer.alloc(18 + len);
  buf.write('Art-Net\0', 0, 'latin1');     // ID (8)
  buf.writeUInt16LE(0x5000, 8);            // OpCode = OpOutput/ArtDMX (little-endian)
  buf[10] = 0; buf[11] = 14;               // ProtVer hi/lo = 14
  buf[12] = seq & 0xff;                    // Sequence
  buf[13] = 0;                             // Physical
  buf[14] = universe & 0xff;               // SubUni (low 8 bits)
  buf[15] = (universe >> 8) & 0x7f;        // Net (high 7 bits)
  buf.writeUInt16BE(len, 16);              // Length (big-endian)
  Buffer.from(data).copy(buf, 18, 0, len);
  return buf;
}

// ── sACN / E1.31 data packet (638 bytes, E1.31-2016) ────────────────────────
export function e131Packet(universe, data, seq = 0, cid = Buffer.alloc(16, 0x2a)) {
  const buf = Buffer.alloc(638);
  // Root layer
  buf.writeUInt16BE(0x0010, 0);                 // preamble size
  buf.writeUInt16BE(0x0000, 2);                 // postamble size
  buf.write('ASC-E1.17\0\0\0', 4, 'latin1');    // ACN packet identifier (12)
  buf.writeUInt16BE(0x7000 | (638 - 16), 16);   // flags + root PDU length
  buf.writeUInt32BE(0x00000004, 18);            // root vector = VECTOR_ROOT_E131_DATA
  cid.copy(buf, 22);                            // CID (16)
  // Framing layer
  buf.writeUInt16BE(0x7000 | (638 - 38), 38);   // flags + framing PDU length
  buf.writeUInt32BE(0x00000002, 40);            // framing vector = VECTOR_E131_DATA_PACKET
  buf.write('LumiGate e2e', 44, 'latin1');      // source name (64)
  buf[108] = 100;                               // priority
  buf.writeUInt16BE(0, 109);                    // sync address
  buf[111] = seq & 0xff;                        // sequence number
  buf[112] = 0;                                 // options
  buf.writeUInt16BE(universe, 113);             // universe (big-endian)
  // DMP layer
  buf.writeUInt16BE(0x7000 | (638 - 115), 115); // flags + DMP PDU length
  buf[117] = 0x02;                              // DMP vector = SET_PROPERTY
  buf[118] = 0xa1;                              // address + data type
  buf.writeUInt16BE(0x0000, 119);               // first property address
  buf.writeUInt16BE(0x0001, 121);               // address increment
  buf.writeUInt16BE(0x0201, 123);               // property value count = 513
  buf[125] = 0x00;                              // DMX start code
  Buffer.from(data).copy(buf, 126, 0, Math.min(512, data.length));
  return buf;
}

// Persistent UDP socket so a stream of frames reuses one source port.
export class UdpSender {
  constructor(host) { this.host = host; this.sock = dgram.createSocket('udp4'); }
  send(port, buf) {
    return new Promise((res, rej) =>
      this.sock.send(buf, port, this.host, (e) => (e ? rej(e) : res())));
  }
  close() { this.sock.close(); }
}

// Stream frames for `ms` at `hz`. makeBuf(seq) returns the packet for frame seq.
export async function streamFor(sender, port, makeBuf, { ms = 1500, hz = 40 } = {}) {
  const period = 1000 / hz;
  const end = Date.now() + ms;
  let seq = 0;
  while (Date.now() < end) { await sender.send(port, makeBuf(seq++)); await sleep(period); }
  return seq;
}

// ── WebSocket (browser→device control channel) ──────────────────────────────
// Send one or more JSON control messages, then close.
export function wsSend(host, msgs, holdMs = 250) {
  const list = Array.isArray(msgs) ? msgs : [msgs];
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`ws://${host}/ws`);
    ws.onopen = () => {
      for (const m of list) ws.send(JSON.stringify(m));
      setTimeout(() => { try { ws.close(); } catch {} resolve(); }, holdMs);
    };
    ws.onerror = (e) => reject(e.error || new Error('ws error'));
  });
}

// Resolve with the first binary frame (ArrayBuffer) the device pushes.
export function wsFirstBinary(host, timeoutMs = 5000) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`ws://${host}/ws`);
    ws.binaryType = 'arraybuffer';
    const timer = setTimeout(() => { try { ws.close(); } catch {} reject(new Error('no binary frame')); }, timeoutMs);
    ws.onmessage = (ev) => {
      if (ev.data instanceof ArrayBuffer) {
        clearTimeout(timer); try { ws.close(); } catch {}
        resolve(new DataView(ev.data));
      }
    };
    ws.onerror = (e) => { clearTimeout(timer); reject(e.error || new Error('ws error')); };
  });
}

// Set manual override on/off (network input is ignored on the monitored output
// while manual is on). Used to keep network tests deterministic.
export function setManual(host, on) {
  return wsSend(host, { type: 'mode', manual: !!on });
}

// Put the device in a known input state before injecting DMX: passthrough (not
// manual) and the monitor pinned to output A, so /dmx.json and the grid reflect
// what we send even if a previous test left the monitor on another output.
export function prepInput(host) {
  return wsSend(host, [{ type: 'mode', manual: false }, { type: 'viewout', out: 0 }]);
}
