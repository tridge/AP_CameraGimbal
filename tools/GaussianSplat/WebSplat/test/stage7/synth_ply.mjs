// A synthetic Brush-layout PLY (Brush's property order, SH 3, no normals) of N Gaussians around an
// orbit target, written in chunks: node synth_ply.mjs N OUT.ply. Deterministic (xorshift).
import { closeSync, openSync, writeSync } from 'node:fs';

const [n, out] = [Number(process.argv[2]), process.argv[3]];
if (!n || !out) throw new Error('usage: node synth_ply.mjs N OUT.ply');
const names = [
  'x',
  'y',
  'z',
  'scale_0',
  'scale_1',
  'scale_2',
  'opacity',
  'rot_0',
  'rot_1',
  'rot_2',
  'rot_3',
];
for (let c = 0; c < 3; c++) names.push(`f_dc_${c}`);
for (let i = 0; i < 45; i++) names.push(`f_rest_${i}`);
const head =
  'ply\nformat binary_little_endian 1.0\ncomment Exported from Brush\ncomment Vertical axis: y\n' +
  `element vertex ${n}\n${names.map((p) => `property float ${p}`).join('\n')}\nend_header\n`;
const fd = openSync(out, 'w');
writeSync(fd, head);
let s = 0x9e3779b9;
const rnd = () => {
  s ^= s << 13;
  s ^= s >>> 17;
  s ^= s << 5;
  return (s >>> 0) / 2 ** 32;
};
const chunk = 65536;
const row = new Float32Array(chunk * names.length);
for (let a = 0; a < n; a += chunk) {
  const m = Math.min(chunk, n - a);
  for (let i = 0; i < m; i++) {
    const r = row.subarray(i * names.length, (i + 1) * names.length);
    const ang = rnd() * 2 * Math.PI;
    const rad = 150 * Math.sqrt(rnd());
    r[0] = rad * Math.cos(ang);
    r[1] = rad * Math.sin(ang);
    r[2] = 10 + 30 * rnd();
    for (let c = 0; c < 3; c++) r[3 + c] = Math.log(0.05 + rnd());
    r[6] = 6 * rnd() - 3;
    for (let c = 0; c < 4; c++) r[7 + c] = rnd() - 0.5;
    const v = (rnd() - 0.5) * 2;
    for (let c = 0; c < 3; c++) r[11 + c] = v + 0.01 * c;
    for (let j = 0; j < 45; j++) r[14 + j] = (rnd() - 0.5) * 0.3;
  }
  writeSync(fd, new Uint8Array(row.buffer, 0, m * names.length * 4));
}
closeSync(fd);
console.log(`wrote ${out}: ${n} Gaussians`);
