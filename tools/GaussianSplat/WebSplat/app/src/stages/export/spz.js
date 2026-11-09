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

const collect = async (chunks) => {
  const parts = [];
  let n = 0;
  for await (const c of chunks) {
    parts.push(c);
    n += c.length;
  }
  const out = new Uint8Array(n);
  let at = 0;
  for (const c of parts) {
    out.set(c, at);
    at += c.length;
  }
  return out;
};

const streamBytes = async (bytes, transform) =>
  new Uint8Array(
    await new Response(new Blob([bytes]).stream().pipeThrough(transform)).arrayBuffer(),
  );

async function gunzip(bytes) {
  return streamBytes(bytes, new DecompressionStream('gzip'));
}

let crcTable = null;
function crc32(b, c = 0) {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let x = n;
      for (let k = 0; k < 8; k++) x = x & 1 ? 0xedb88320 ^ (x >>> 1) : x >>> 1;
      crcTable[n] = x;
    }
  }
  c = ~c >>> 0;
  for (let i = 0; i < b.length; i++) c = crcTable[(c ^ b[i]) & 0xff] ^ (c >>> 8);
  return ~c >>> 0;
}

// gzip with a fixed header (no name, mtime 0, OS unknown) around zlib's raw deflate, so Node
// (ply2spz.mjs does the same with zlib) and browsers give the same bytes. chunks: an (async)
// iterable of Uint8Arrays; yields the gzip file in pieces.
export async function* gzipFixedStream(chunks) {
  yield new Uint8Array([0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255]);
  let crc = 0;
  let size = 0;
  const cs = new CompressionStream('deflate-raw');
  const writer = cs.writable.getWriter();
  const feeding = (async () => {
    for await (const c of chunks) {
      crc = crc32(c, crc);
      size += c.length;
      await writer.write(c);
    }
    await writer.close();
  })();
  const reader = cs.readable.getReader();
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    yield value;
  }
  await feeding;
  const t = new Uint8Array(8);
  new DataView(t.buffer).setUint32(0, crc, true);
  new DataView(t.buffer).setUint32(4, size >>> 0, true);
  yield t;
}

export const gzipFixed = (bytes) => collect(gzipFixedStream([bytes]));

// the decompressed SPZ in pieces, with the header's flags byte changed by fn
async function* spzFlagged(spz, fn) {
  const r = new Blob([spz]).stream().pipeThrough(new DecompressionStream('gzip')).getReader();
  let at = 0;
  for (;;) {
    const { done, value } = await r.read();
    if (done) break;
    if (at <= 14 && at + value.length > 14) value[14 - at] = fn(value[14 - at]);
    at += value.length;
    yield value;
  }
}

// an SPZ with its flags byte rewritten, as gzip pieces (streaming) or bytes
export const setSpzFlagsStream = (spz, fn) => gzipFixedStream(spzFlagged(spz, fn));
export const setSpzFlags = (spz, fn) => collect(setSpzFlagsStream(spz, fn));

const SQRT1_2 = Math.SQRT1_2;

// header of a decompressed SPZ
export function spzHeader(u) {
  const dv = new DataView(u.buffer, u.byteOffset, u.byteLength);
  if (u.length < 16 || dv.getUint32(0, true) !== 0x5053474e) throw new Error('not an SPZ file');
  const version = dv.getUint32(4, true);
  if (version !== 2 && version !== 3) throw new Error(`SPZ version ${version} is not supported`);
  const count = dv.getUint32(8, true);
  const shDegree = u[12];
  if (shDegree > 3) throw new Error(`SPZ SH degree ${shDegree}`);
  const fractionalBits = u[13];
  const flags = u[14];
  const k = shCoeffs(shDegree);
  const rot = version === 2 ? 3 : 4;
  const size = 16 + count * (9 + 1 + 3 + 3 + rot + 3 * k);
  if (u.length !== size) throw new Error(`SPZ: ${u.length} bytes, expected ${size}`);
  return { version, count, shDegree, fractionalBits, flags, antialiased: !!(flags & 1), rot, k };
}

// Gaussians [a, b) of a decompressed SPZ (planar layout: positions, alphas, colours, scales,
// rotations, SH)
export function decodeSpzRange(u, h, a, b) {
  const n = h.count;
  const m = b - a;
  const s = emptySplat(m, h.shDegree);
  const scale = 1 / (1 << h.fractionalBits);
  let at = 16 + 9 * a;
  for (let i = 0; i < 3 * m; i++, at += 3) {
    let v = u[at] | (u[at + 1] << 8) | (u[at + 2] << 16);
    if (v & 0x800000) v |= ~0xffffff;
    s.means[i] = v * scale;
  }
  at = 16 + 9 * n + a;
  for (let i = 0; i < m; i++, at++) {
    const x = u[at] / 255;
    s.opacities[i] = Math.log(x / (1 - x));
  }
  at = 16 + 10 * n + 3 * a;
  for (let i = 0; i < 3 * m; i++, at++) s.sh0[i] = (u[at] / 255 - 0.5) / 0.15;
  at = 16 + 13 * n + 3 * a;
  for (let i = 0; i < 3 * m; i++, at++) s.scales[i] = u[at] / 16 - 10;
  at = 16 + 16 * n + h.rot * a;
  for (let i = 0; i < m; i++) {
    const xyzw = [0, 0, 0, 0];
    if (h.version === 2) {
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
  at = 16 + (16 + h.rot) * n + 3 * h.k * a;
  for (let i = 0; i < m * h.k * 3; i++, at++) s.shN[i] = (u[at] - 128) / 128;
  return s;
}

export async function decodeSpz(bytes) {
  const u = await gunzip(bytes);
  const h = spzHeader(u);
  return { splat: decodeSpzRange(u, h, 0, h.count), ...h };
}

export const decompressSpz = gunzip;

// Decode error bounds: half a quantisation step plus Spark's staging before it quantises
// (measured: up to 0.56 steps otherwise): half a float16 ulp of the value entering the quantiser,
// for log scales also two float32 ulps (Spark's exp/ln round trip), and for SH coefficients also
// half of 1/128 (8-bit staging).
// Rotation: smallest-three, 9 bits over [0, sqrt(1/2)] per component: each stored component is
// within e = 0.5 sqrt(1/2)/511 + 2^-12 (staging), the largest (>= 1/2) is rebuilt from the norm
// (error <= sqrt(3)|e3|), so the quaternion moves <= 2 sqrt(3) e and the rotation <= 0.375 deg.
export const SPZ_STEPS = {
  means: (fractionalBits) => 1 / (1 << fractionalBits),
  scales: 1 / 16,
  sh0: 1 / (0.15 * 255),
  alpha: 1 / 255,
  sh1: 8 / 128, // degree 1: 5 bits
  shHigh: 16 / 128, // degree 2-3: 4 bits
};
export const SPZ_QUAT_BOUND_DEG = 0.375;

export function f32Ulp(x) {
  const a = Math.abs(x);
  return a < 2 ** -126 ? 2 ** -149 : 2 ** (Math.floor(Math.log2(a)) - 23);
}

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
      // Spark holds scales linearly and takes ln again before staging it as float16: that float32
      // round trip can move ln(scale) by an ulp, enough to tip a float16 tie (stage 6:
      // -0.8439942 -> -0.84375, byte 147 instead of 146)
      add('scales', Math.abs(sc - dec.scales[j]), f16HalfUlp(sc) + 2 * f32Ulp(sc));
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

// spzErrors results of consecutive chunks combined
export function mergeSpzErrors(acc, e) {
  if (!acc) return structuredClone(e);
  for (const k of Object.keys(e.max)) acc.max[k] = Math.max(acc.max[k], e.max[k]);
  for (const k of Object.keys(e.steps)) acc.steps[k] = Math.max(acc.steps[k], e.steps[k]);
  for (const k of Object.keys(e.clamped)) acc.clamped[k] += e.clamped[k];
  for (const [k, v] of Object.entries(e.over)) acc.over[k] = (acc.over[k] ?? 0) + v;
  return acc;
}
