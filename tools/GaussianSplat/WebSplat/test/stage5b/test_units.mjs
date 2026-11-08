// Unit checks for the stage 5b helpers that the dataset comparison cannot isolate.
import { F16, f16bits } from '../../app/src/stages/prepare/scene.js';

let fails = 0;
const check = (name, ok, detail = '') => {
  console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? ': ' + detail : ''}`);
  if (!ok) fails++;
};
// float32 -> float16 against Float16Array (round to nearest even) on every float32 exponent
const f16 = new Float16Array(1);
const u16 = new Uint16Array(f16.buffer);
const f32 = new Float32Array(1);
const u32 = new Uint32Array(f32.buffer);
let bad = 0,
  n = 0;
for (let e = 0; e < 256; e++)
  for (let k = 0; k < 4096; k++) {
    const m =
      k < 16
        ? [0, 1, 0x1000, 0x0fff, 0x1001, 0x2000, 0x3000, 0x7fffff][k % 8]
        : (k * 2654435761) & 0x7fffff;
    u32[0] = (e << 23) | m | (k & 1 ? 0x80000000 : 0);
    f16[0] = f32[0];
    const want = Number.isNaN(f32[0]) ? null : u16[0];
    const got = f16bits(f32[0]);
    if (want != null && got !== want) bad++;
    n++;
  }
check('f16bits = Float16Array conversion', bad === 0, `${bad} of ${n} differ`);
let tbad = 0;
for (let h = 0; h < 65536; h++) {
  u16[0] = h;
  if (!(Object.is(F16[h], f16[0]) || (Number.isNaN(F16[h]) && Number.isNaN(f16[0])))) tbad++;
}
check('F16 decode table', tbad === 0, `${tbad} differ`);
process.exit(fails ? 1 : 0);
