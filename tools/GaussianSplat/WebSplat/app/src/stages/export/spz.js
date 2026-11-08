// SPZ: encoded with Spark's transcodeSpz from the exported PLY, as export_web.py does (ply2spz.mjs),
// so browser and native SPZ files match. The decoder here (Niantic SPZ v2/v3 layout) measures the
// quantisation error of an exported file against the splat it came from.
import { emptySplat, shCoeffs } from './ply.js';

export const SPZ_MAX_SH = 3;

// transcodeSpz: Spark's function (the caller imports @sparkjsdev/spark). Spark always sets the
// antialiased flag; antialiased = false (classic-trained splats) clears it.
export async function encodeSpz(plyBytes, transcodeSpz, { antialiased = true } = {}) {
  const { fileBytes } = await transcodeSpz({
    inputs: [{ fileBytes: plyBytes, pathOrUrl: 'splat.ply' }],
    maxSh: SPZ_MAX_SH,
  });
  return antialiased ? fileBytes : setSpzFlags(fileBytes, (f) => f & ~1);
}

const streamBytes = async (bytes, transform) =>
  new Uint8Array(
    await new Response(new Blob([bytes]).stream().pipeThrough(transform)).arrayBuffer(),
  );

async function gunzip(bytes) {
  return streamBytes(bytes, new DecompressionStream('gzip'));
}

let crcTable = null;
function crc32(b) {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      crcTable[n] = c;
    }
  }
  let c = 0xffffffff;
  for (let i = 0; i < b.length; i++) c = crcTable[(c ^ b[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

// gzip with a fixed header (no name, mtime 0, OS unknown) around zlib's raw deflate, so Node
// (ply2spz.mjs does the same with zlib) and browsers give the same bytes
export async function gzipFixed(bytes) {
  const body = await streamBytes(bytes, new CompressionStream('deflate-raw'));
  const out = new Uint8Array(10 + body.length + 8);
  out.set([0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255]);
  out.set(body, 10);
  const dv = new DataView(out.buffer);
  dv.setUint32(10 + body.length, crc32(bytes), true);
  dv.setUint32(14 + body.length, bytes.length >>> 0, true);
  return out;
}

export async function setSpzFlags(spz, fn) {
  const u = await gunzip(spz);
  u[14] = fn(u[14]);
  return gzipFixed(u);
}

const SQRT1_2 = Math.SQRT1_2;

export async function decodeSpz(bytes) {
  const u = await gunzip(bytes);
  const dv = new DataView(u.buffer, u.byteOffset, u.byteLength);
  if (dv.getUint32(0, true) !== 0x5053474e) throw new Error('not an SPZ file');
  const version = dv.getUint32(4, true);
  if (version !== 2 && version !== 3) throw new Error(`SPZ version ${version} is not supported`);
  const n = dv.getUint32(8, true);
  const shDegree = u[12];
  const fractionalBits = u[13];
  const flags = u[14];
  const k = shCoeffs(shDegree);
  const s = emptySplat(n, shDegree);
  let at = 16;
  const scale = 1 / (1 << fractionalBits);
  for (let i = 0; i < 3 * n; i++, at += 3) {
    let v = u[at] | (u[at + 1] << 8) | (u[at + 2] << 16);
    if (v & 0x800000) v |= ~0xffffff;
    s.means[i] = v * scale;
  }
  for (let i = 0; i < n; i++, at++) {
    const a = u[at] / 255;
    s.opacities[i] = Math.log(a / (1 - a));
  }
  for (let i = 0; i < 3 * n; i++, at++) s.sh0[i] = (u[at] / 255 - 0.5) / 0.15;
  for (let i = 0; i < 3 * n; i++, at++) s.scales[i] = u[at] / 16 - 10;
  for (let i = 0; i < n; i++) {
    const xyzw = [0, 0, 0, 0];
    if (version === 2) {
      let ss = 0;
      for (let c = 0; c < 3; c++) {
        xyzw[c] = u[at + c] / 127.5 - 1;
        ss += xyzw[c] ** 2;
      }
      xyzw[3] = Math.sqrt(Math.max(0, 1 - ss));
      at += 3;
    } else {
      let comp = (u[at] | (u[at + 1] << 8) | (u[at + 2] << 16) | (u[at + 3] << 24)) >>> 0;
      const largest = comp >>> 30;
      let ss = 0;
      for (let c = 3; c >= 0; c--) {
        if (c === largest) continue;
        const mag = comp & 511;
        const neg = (comp >>> 9) & 1;
        comp >>>= 10;
        xyzw[c] = ((SQRT1_2 * mag) / 511) * (neg ? -1 : 1);
        ss += xyzw[c] ** 2;
      }
      xyzw[largest] = Math.sqrt(Math.max(0, 1 - ss));
      at += 4;
    }
    s.quats.set([xyzw[3], xyzw[0], xyzw[1], xyzw[2]], 4 * i); // w, x, y, z
  }
  // SH: per point, per coefficient, r g b
  for (let i = 0; i < n * k * 3; i++, at++) s.shN[i] = (u[at] - 128) / 128;
  if (at !== u.length) throw new Error(`SPZ: ${u.length - at} unexpected trailing bytes`);
  return { splat: s, version, fractionalBits, flags, antialiased: !!(flags & 1) };
}

// Decode error bounds: half a quantisation step plus Spark's staging before it quantises
// (measured: up to 0.56 steps otherwise): half a float16 ulp of the value entering the quantiser,
// and for SH coefficients also half of 1/128 (8-bit staging).
// Rotation: smallest-three, 9 bits over [0, sqrt(1/2)] per component.
export const SPZ_STEPS = {
  means: (fractionalBits) => 1 / (1 << fractionalBits),
  scales: 1 / 16,
  sh0: 1 / (0.15 * 255),
  alpha: 1 / 255,
  sh1: 8 / 128, // degree 1: 5 bits
  shHigh: 16 / 128, // degree 2-3: 4 bits
};
export const SPZ_QUAT_BOUND_DEG = 0.25;

export function f16HalfUlp(x) {
  const a = Math.abs(x);
  return a < 2 ** -14 ? 2 ** -25 : 2 ** (Math.floor(Math.log2(a)) - 11);
}

const sigmoid = (x) => 1 / (1 + Math.exp(-x));

// decoded vs source: max abs error per field and the largest error in quantisation steps
// (the rotation as an angle); `over` counts values beyond their bound, `nonFinite` decoded values
// that are not numbers. Values outside the encodable range are compared with the saturated value
// and also counted in `clamped`. The SH degree must be the source's (at most SPZ_MAX_SH).
export function spzErrors(src, dec, fractionalBits) {
  if (src.count !== dec.count) throw new Error(`SPZ has ${dec.count} Gaussians, not ${src.count}`);
  const degree = Math.min(src.shDegree, SPZ_MAX_SH);
  if (dec.shDegree !== degree) throw new Error(`SPZ has SH degree ${dec.shDegree}, not ${degree}`);
  const ks = shCoeffs(src.shDegree);
  const k = shCoeffs(degree);
  const n = src.count;
  for (const [name, w] of [
    ['means', 3],
    ['scales', 3],
    ['quats', 4],
    ['opacities', 1],
    ['sh0', 3],
    ['shN', 3 * k],
  ]) {
    if (dec[name].length !== n * w) throw new Error(`SPZ ${name}: ${dec[name].length} values`);
  }
  const step = { ...SPZ_STEPS, means: SPZ_STEPS.means(fractionalBits) };
  const max = { means: 0, scales: 0, sh0: 0, alpha: 0, sh1: 0, shHigh: 0, quatAngleDeg: 0 };
  const steps = { means: 0, scales: 0, sh0: 0, alpha: 0, sh1: 0, shHigh: 0 };
  const clamped = { means: 0, scales: 0, sh0: 0, sh: 0 };
  const over = {};
  let nonFinite = 0;
  // staged: Spark's staging allowance in the field's units
  const add = (name, err, staged = 0) => {
    if (!Number.isFinite(err)) {
      nonFinite++;
      return;
    }
    if (err > max[name]) max[name] = err;
    if (name in steps) steps[name] = Math.max(steps[name], err / step[name]);
    const bound = name === 'quatAngleDeg' ? SPZ_QUAT_BOUND_DEG : step[name] / 2 + staged;
    if (err > bound * (1 + 1e-6) + 1e-12) over[name] = (over[name] ?? 0) + 1;
  };
  // the value the format can hold, counting saturations
  const sat = (x, lo, hi, field) => {
    if (x >= lo && x <= hi) return x;
    clamped[field]++;
    return x < lo ? lo : hi;
  };
  const mlim = ((1 << 23) - 1) / (1 << fractionalBits);
  const dcLo = -0.5 / 0.15;
  const dcHi = 0.5 / 0.15;
  for (let i = 0; i < n; i++) {
    for (let c = 0; c < 3; c++) {
      const j = 3 * i + c;
      add('means', Math.abs(sat(src.means[j], -mlim - step.means, mlim, 'means') - dec.means[j]));
      const sc = sat(src.scales[j], -10, 255 / 16 - 10, 'scales');
      add('scales', Math.abs(sc - dec.scales[j]), f16HalfUlp(sc));
      const d = sat(src.sh0[j], dcLo, dcHi, 'sh0');
      const rgb = 0.15 * d + 0.5;
      add('sh0', Math.abs(d - dec.sh0[j]), Math.max(f16HalfUlp(d), f16HalfUlp(rgb) / 0.15));
    }
    const a = sigmoid(src.opacities[i]);
    add('alpha', Math.abs(a - sigmoid(dec.opacities[i])), f16HalfUlp(a));
    let dot = 0;
    let ns = 0;
    let nd = 0;
    for (let c = 0; c < 4; c++) {
      dot += src.quats[4 * i + c] * dec.quats[4 * i + c];
      ns += src.quats[4 * i + c] ** 2;
      nd += dec.quats[4 * i + c] ** 2;
    }
    const cos = Math.abs(dot) / Math.sqrt(ns * nd);
    add('quatAngleDeg', (2 * Math.acos(Math.min(1, cos)) * 180) / Math.PI);
    for (let j = 0; j < k; j++) {
      for (let c = 0; c < 3; c++) {
        const x = sat(src.shN[(i * ks + j) * 3 + c], -1, 127 / 128, 'sh');
        const err = Math.abs(x - dec.shN[(i * k + j) * 3 + c]);
        add(j < 3 ? 'sh1' : 'shHigh', err, 0.5 / 128 + f16HalfUlp(x));
      }
    }
  }
  if (nonFinite) over.nonFinite = nonFinite;
  return { max, steps, over, clamped };
}
