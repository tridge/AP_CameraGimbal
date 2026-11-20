// sfm.py's descriptor preparation, bit-exact: uint8 -> float32 / max(L2 norm, 1e-6) -> fp16
// (the CUDA path). The squared norm is an exact integer in float32, so the order of summation
// does not matter; f64 division and sqrt rounded once to f32 equal the f32 operations.

const f32 = Math.fround;
const EPS = f32(1e-6);
const fbuf = new Float32Array(1),
  ubuf = new Uint32Array(fbuf.buffer);

// float32 value (as a JS number) -> fp16 bits, round to nearest even.
export function f32ToF16Bits(x) {
  fbuf[0] = x;
  const u = ubuf[0],
    sign = (u >>> 16) & 0x8000,
    a = u & 0x7fffffff;
  if (a >= 0x7f800000) return sign | (a > 0x7f800000 ? 0x7e00 : 0x7c00);
  if (a >= 0x477ff000) return sign | 0x7c00; // rounds to >= 65520: inf
  if (a >= 0x38800000) {
    // normal fp16
    const r = a - 0x38000000 + 0xfff + ((a >>> 13) & 1);
    return sign | (r >>> 13);
  }
  // subnormal fp16: quantum 2^-24 (|x| * 2^24 is exact); round half to even
  const v = Math.abs(fbuf[0]) * 16777216,
    fl = Math.floor(v),
    d = v - fl;
  return sign | (d > 0.5 || (d === 0.5 && fl & 1) ? fl + 1 : fl);
}

export function f16BitsToF32(h) {
  const s = h & 0x8000 ? -1 : 1,
    e = (h >>> 10) & 0x1f,
    m = h & 0x3ff;
  if (e === 0) return s * m * 2 ** -24;
  if (e === 31) return m ? NaN : s * Infinity;
  return s * (1024 + m) * 2 ** (e - 25);
}

// desc: Uint8Array(n * 128). Returns Uint32Array(n * 64): fp16 pairs, element 2k in the low half.
export function quantizeDescriptors(desc, n, out = new Uint32Array(n * 64)) {
  for (let r = 0; r < n; r++) {
    const o = r * 128;
    let ss = 0;
    for (let k = 0; k < 128; k++) ss += desc[o + k] * desc[o + k];
    const norm = Math.max(f32(Math.sqrt(ss)), EPS);
    for (let k = 0; k < 128; k += 2) {
      const lo = f32ToF16Bits(f32(desc[o + k] / norm)),
        hi = f32ToF16Bits(f32(desc[o + k + 1] / norm));
      out[r * 64 + (k >> 1)] = lo | (hi << 16);
    }
  }
  return out;
}
