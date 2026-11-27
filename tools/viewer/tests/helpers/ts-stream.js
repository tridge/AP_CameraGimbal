// A small MPEG-TS video stream: program tables, then one H.264 access unit per PES.
const hex = (h) => Uint8Array.from(h.match(/../g), (v) => parseInt(v, 16));
const n32 = (n) => Uint8Array.of(n >>> 24, n >>> 16, n >>> 8, n);
export const join = (...parts) => {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
};
function crc(bytes) {
  let n = 0xffffffff;
  for (const b of bytes) {
    n ^= b << 24;
    for (let i = 0; i < 8; i++) n = ((n << 1) ^ (n & 0x80000000 ? 0x04c11db7 : 0)) >>> 0;
  }
  return n32(n);
}
const section = (data) => {
  data = data.slice();
  data[1] = 0xb0 | ((data.length + 1) >> 8);
  data[2] = (data.length + 1) & 255;
  return join(data, crc(data));
};
export const VIDEO_PID = 0x101;
function packet(pid, payload, cc, unitStart) {
  const out = new Uint8Array(188).fill(255);
  out.set([
    0x47,
    (pid >> 8) | (unitStart ? 64 : 0),
    pid & 255,
    (payload.length === 184 ? 0x10 : 0x30) | (cc & 15),
  ]);
  if (payload.length < 184) {
    out[4] = 183 - payload.length;
    if (out[4]) out[5] = 0;
  }
  out.set(payload, 188 - payload.length);
  return out;
}
function stamp(n) {
  n = ((n % 2 ** 33) + 2 ** 33) % 2 ** 33;
  return Uint8Array.of(
    0x21 | (Math.floor(n / 2 ** 29) & 14),
    Math.floor(n / 2 ** 22) & 255,
    1 | (Math.floor(n / 2 ** 14) & 254),
    Math.floor(n / 128) & 255,
    1 | ((n % 128) * 2),
  );
}
/** PAT and PMT, as the relay sends them to every new viewer. */
export function tsTables({ type = 0x1b } = {}) {
  const pat = section(Uint8Array.of(0, 0, 0, 0, 1, 0xc1, 0, 0, 0, 1, 0xf0, 0)),
    pmt = section(
      Uint8Array.of(2, 0, 0, 0, 1, 0xc1, 0, 0, 0xe1, 1, 0xf0, 0, type, 0xe1, 1, 0xf0, 0),
    );
  return join(
    packet(0, join(Uint8Array.of(0), pat), 0, true),
    packet(0x1000, join(Uint8Array.of(0), pmt), 0, true),
  );
}
/** The coded bytes of access unit n; every `gop`th is a keyframe with parameter sets. */
export function accessUnit(n, { gop = 5, bytes = 300 } = {}) {
  const body = new Uint8Array(bytes).fill(0x40 + (n % 32));
  body[0] = n & 255;
  return n % gop === 0
    ? join(
        Uint8Array.of(0, 0, 1, 9, 0xf0),
        Uint8Array.of(0, 0, 1),
        hex('6764000bacb20283f6022000000300200000030281e28549'), // SPS of a real 320x240 stream
        Uint8Array.of(0, 0, 1),
        hex('68ebc3cb22c0'),
        Uint8Array.of(0, 0, 1, 0x65, 0x88),
        body,
      )
    : join(Uint8Array.of(0, 0, 1, 9, 0xf0), Uint8Array.of(0, 0, 1, 0x41, 0x9a), body);
}
/** Transport packets of access unit n at `ticks`; counter is {value}, kept across calls. */
export function tsFrame(n, ticks, counter, options) {
  const data = accessUnit(n, options),
    pes = join(Uint8Array.of(0, 0, 1, 0xe0, 0, 0, 0x84, 0x80, 5), stamp(ticks), data),
    out = [];
  for (let at = 0; at < pes.length; at += 184)
    out.push(packet(VIDEO_PID, pes.subarray(at, at + 184), counter.value++, at === 0));
  return join(...out);
}
