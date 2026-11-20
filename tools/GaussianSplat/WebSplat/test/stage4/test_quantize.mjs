// The JS descriptor normalisation + fp16 cast equals sfm.py's (numpy) bit for bit.
// usage: WEBSPLAT_DATA=... node test_quantize.mjs   (after export_native.py)
import { readFileSync } from 'node:fs';
import {
  quantizeDescriptors,
  f32ToF16Bits,
  f16BitsToF32,
} from '../../app/src/stages/match/quantize.js';

const dir = `${process.env.WEBSPLAT_DATA}/stage4/native`;
const index = JSON.parse(readFileSync(`${dir}/index.json`, 'utf8'));
const n = index.counts.slice(0, index.quant_images).reduce((a, b) => a + b, 0);
const fd = readFileSync(`${dir}/descriptors.u8`);
const desc = new Uint8Array(fd.buffer, fd.byteOffset, n * 128);
const q = readFileSync(`${dir}/quant.bin`);
const want = new Uint16Array(q.buffer, q.byteOffset, n * 128);

const t0 = performance.now();
const got = new Uint16Array(quantizeDescriptors(desc, n).buffer);
const ms = performance.now() - t0;
let bad = 0;
for (let k = 0; k < want.length; k++) if (got[k] !== want[k]) bad++;

// f16 conversion round trip and rounding edge cases
let convBad = 0;
for (let h = 0; h < 0x7c00; h++) if (f32ToF16Bits(f16BitsToF32(h)) !== h) convBad++;
const edge = [
  [1 + 2 ** -11, 0x3c00],
  [1 + 3 * 2 ** -11, 0x3c02],
  [2 ** -25, 0],
  [3 * 2 ** -25, 2],
  [2 ** -14 - 2 ** -25, 0x400],
];
for (const [x, h] of edge) if (f32ToF16Bits(x) !== h) convBad++;

console.log(
  `${n} descriptors quantised in ${ms.toFixed(0)} ms (${((ms / n) * 8192).toFixed(1)} ms per 8192), ${bad} of ${want.length} values differ; conversion errors ${convBad}`,
);
console.log(bad === 0 && convBad === 0 ? 'PASS quantisation bit-exact' : 'FAIL');
process.exit(bad === 0 && convBad === 0 ? 0 : 1);
