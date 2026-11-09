// Stage 6 unit tests (Node): params.js WSSP <-> PLY, brush-dataset.js float16 decoding, PNG16
// transport rounding, option mapping, and the ETA cost curve. Exits nonzero on any failure.
// usage: node test_units.mjs [PREPARE_DIR]   (a 5b output for the mapping test)
import fs from 'node:fs';
import zlib from 'node:zlib';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const app = join(dirname(fileURLToPath(import.meta.url)), '../../app/src/stages/train');
const { plyToWssp, wsspToPly, wsspHeader } = await import(join(app, 'params.js'));
const { halfToFloat, targetToU16, brushConfig, boundsJson, encodePng16 } = await import(join(app, 'brush-dataset.js'));
const { cumCost } = await import(join(app, 'stage.js'));

let failures = 0;
const check = (name, ok, detail = '') => {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${ok ? '' : ': ' + detail}`);
  if (!ok) failures++;
};

// a PLY in Brush's export property order (x y z scale rot opacity f_dc f_rest), degree 3
function brushPly(n, seed = 1) {
  let a = seed;
  const r = () => ((a = (a * 1103515245 + 12345) >>> 0) / 4294967296) * 2 - 1;
  const props = ['x', 'y', 'z', 'scale_0', 'scale_1', 'scale_2', 'opacity', 'rot_0', 'rot_1', 'rot_2', 'rot_3',
    'f_dc_0', 'f_dc_1', 'f_dc_2', ...Array.from({ length: 45 }, (_, i) => `f_rest_${i}`)];
  const head = `ply\nformat binary_little_endian 1.0\ncomment Exported from Brush\nelement vertex ${n}\n`
    + props.map((p) => `property float ${p}\n`).join('') + 'end_header\n';
  const body = Float32Array.from({ length: n * props.length }, r);
  const h = new TextEncoder().encode(head);
  const out = new Uint8Array(h.length + body.byteLength);
  out.set(h);
  out.set(new Uint8Array(body.buffer), h.length);
  return { bytes: out, props, body };
}
function columns(bytes) {
  const text = new TextDecoder('latin1').decode(bytes.subarray(0, 8192));
  const end = text.indexOf('end_header\n') + 11;
  const props = text.slice(0, end).split('\n').filter((l) => l.startsWith('property')).map((l) => l.split(' ')[2]);
  const n = Number(text.match(/element vertex (\d+)/)[1]);
  const f = new Float32Array(bytes.slice(end).buffer);
  return Object.fromEntries(props.map((p, k) => [p, Array.from({ length: n }, (_, i) => f[i * props.length + k])]));
}
{
  const { bytes } = brushPly(1000);
  const w = plyToWssp(bytes);
  const { count, degree } = wsspHeader(w);
  check('WSSP header (count, degree)', count === 1000 && degree === 3 && w.byteLength === 16 + 1000 * 59 * 4);
  const a = columns(bytes);
  const b = columns(wsspToPly(w));
  const same = Object.keys(a).every((k) => b[k] && a[k].every((v, i) => Object.is(v, b[k][i])));
  check('PLY -> WSSP -> PLY keeps every property bit for bit', same && Object.keys(b).length === Object.keys(a).length);
  // WSSP layout: shN coefficient-major, channel last: shN[i][k][c] = f_rest_{c*K + k}
  const f = new Float32Array(w.buffer, 16);
  const shStart = 1000 * (3 + 3 + 4 + 1 + 3);
  const i = 7, k = 4, c = 2;
  check('WSSP shN layout [count, K, 3] from channel-major f_rest', f[shStart + (i * 15 + k) * 3 + c] === a[`f_rest_${c * 15 + k}`][i]);
  check('WSSP quaternion order w, x, y, z = rot_0..3', [0, 1, 2, 3].every((q) => f[1000 * 6 + 7 * 4 + q] === a[`rot_${q}`][7]));
  let threw = false;
  try { plyToWssp(bytes.subarray(0, bytes.length - 4)); } catch { threw = true; }
  check('truncated PLY refused', threw);
}
{
  let ok = true;
  const F16 = globalThis.Float16Array;
  if (F16) {
    const u = new Uint16Array(65536).map((_, i) => i);
    const ref = new F16(u.buffer);
    for (let i = 0; i < 65536; i++) {
      const x = halfToFloat(i), y = ref[i];
      if (!(Object.is(x, y) || (Number.isNaN(x) && Number.isNaN(y)))) { ok = false; break; }
    }
  }
  check(`float16 decoding exact for all 65536 codes${F16 ? '' : ' (no Float16Array: skipped)'}`, ok);
  const codes = new Uint16Array(65536).map((_, i) => i).filter((i) => { const v = halfToFloat(i); return v >= 0 && v <= 1; });
  const t = targetToU16(codes);
  const worst = Math.max(...Array.from(codes, (h, i) => Math.abs(t[i] / 65535 - halfToFloat(h))));
  check(`PNG16 round(65535 v) within half a step of every float16 in [0, 1] (max ${worst.toExponential(3)})`, worst <= 0.5 / 65535 + 1e-15);
  const png = await encodePng16(new Uint16Array([0, 1, 65535, 32768, 7, 9]), 3, 2, async (b) => new Uint8Array(zlib.deflateSync(b)));
  check('PNG16 signature and IHDR (16-bit gray)', png[0] === 137 && png[25 - 1] === 16 && png[25] === 0);
}
{
  const c = cumCost(30000);
  let mono = true;
  for (let u = 0; u < 30000; u += 100) if (c(u + 100) < c(u)) mono = false;
  check('ETA cost curve: 0 at start, total at end, monotone', c(0) === 0 && Math.abs(c(30000) - 30000) < 1e-9 && mono);
  check('ETA cost curve: training slows (second half costs more than the first)', c(30000) - c(15000) > c(15000) - c(0));
}
const prep = process.argv[2];
if (prep) {
  const d = JSON.parse(fs.readFileSync(join(prep, 'dataset.json'), 'utf8'));
  const cfg = brushConfig(d);
  const t = d.training;
  check('Brush learning rates = train.py rates (absolute means)', cfg['lr-mean'] === t.lr.means && cfg['lr-mean-absolute']
    && cfg['lr-scale'] === t.lr.scales && cfg['lr-rotation'] === t.lr.quats && cfg['lr-opac'] === t.lr.opacities
    && cfg['lr-coeffs-dc'] === t.lr.sh0 && Math.abs(cfg['lr-coeffs-dc'] / cfg['lr-coeffs-sh-scale'] - t.lr.shN) < 1e-18);
  check('means schedule: end rate = rate * gamma^iterations (1%)', Math.abs(cfg['lr-mean-end'] / cfg['lr-mean'] - 0.01) < 1e-9);
  check('refine period 200 (Brush), growth stop, cap, SSIM, hot weight/threshold, SH degree from the dataset',
    cfg['refine-every'] === 200 && cfg['growth-stop-iter'] === t.strategy.refine_stop_iter
    && cfg['max-splats'] === t.options.cap && cfg['ssim-weight'] === t.options.ssim_weight
    && cfg['hot-weight'] === t.options.hot_weight && cfg['hot-threshold'] === t.v_hot && cfg['sh-degree'] === t.options.sh_degree
    && cfg['total-train-iters'] === t.options.iterations && cfg['background-noise-strength'] === 0);
  check('refine window and bounds pruning from train.py: refine 500 < step < 15000, prune every 100',
    cfg['refine-start-iter'] === t.strategy.refine_start_iter && cfg['refine-start-iter'] === 500
    && cfg['growth-start-iter'] === t.strategy.refine_start_iter + 1 && cfg['refine-stop-iter'] === t.strategy.refine_stop_iter
    && cfg['growth-stop-iter'] === t.strategy.refine_stop_iter && cfg['prune-bounds-every'] === t.prune_outside_bounds_every
    && cfg['prune-bounds-every'] === 100);
  check('SH degree schedule: train.py min(step // 1000, sh_degree) as --sh-degree-interval 1000',
    t.sh_degree_schedule === 'min(step // 1000, sh_degree)' && cfg['sh-degree-interval'] === 1000);
  let refused = false;
  try { brushConfig({ ...d, training: { ...t, sh_degree_schedule: 'step // 500' } }); } catch { refused = true; }
  check('an SH schedule Brush cannot follow is refused', refused);
  const ceiling = new Float32Array(fs.readFileSync(join(prep, d.bounds.ceiling.file)).buffer.slice(0));
  const b = boundsJson(d, ceiling);
  check('bounds: radius, floor, ceiling grid, too-big rule', b.radius === d.bounds.radius && b.z_min === d.bounds.z_min
    && b.ceiling.length === d.bounds.ceiling.n ** 2 && b.max_scale === 5);
}
console.log(failures ? `${failures} check(s) failed` : 'stage 6 units: PASS');
process.exit(failures ? 1 : 0);
