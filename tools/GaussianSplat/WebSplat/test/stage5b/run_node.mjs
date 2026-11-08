// Stage 5b in Node: app/src/stages/prepare/scene.js on files.
//   node run_node.mjs MODEL_DIR FRAMES.json THERMAL.u16 OUTDIR [--opt name=value ...]
//       [--options scale.json] [--celsius]
// THERMAL.u16: select's [N, H, W] raw frames in frames.json order. Writes OUTDIR/{dataset.json,
// targets.f16, points.f32, ceiling.f32, stats.json} and with --celsius celsius.f32 (the undistorted
// Celsius images, float32 [n, H, W]).
import {
  closeSync,
  mkdirSync,
  openSync,
  readFileSync,
  readSync,
  writeFileSync,
  writeSync,
} from 'node:fs';
import { performance } from 'node:perf_hooks';
import { prepareScene } from '../../app/src/stages/prepare/scene.js';

const args = process.argv.slice(2);
const options = {};
for (let k; (k = args.indexOf('--opt')) >= 0; ) {
  const [name, value] = args[k + 1].split('=');
  options[name] =
    value === 'null' ? null : value === 'true' ? true : value === 'false' ? false : Number(value);
  args.splice(k, 2);
}
const oi = args.indexOf('--options');
if (oi >= 0) {
  // a train.py scale.json: its recorded options
  const rec = JSON.parse(readFileSync(args[oi + 1], 'utf8')).options;
  for (const [k, v] of Object.entries(rec))
    if (!['workdir', 'sparse', 'out'].includes(k)) options[k] = v;
  args.splice(oi, 2);
}
const ci = args.indexOf('--celsius');
const celsiusOut = ci >= 0 ? args.splice(ci, 1) && true : false;
const [modelDir, framesPath, thermalPath, out] = args;
const info = JSON.parse(readFileSync(framesPath, 'utf8'));
const px = info.width * info.height;
mkdirSync(out, { recursive: true });
const fdIn = openSync(thermalPath, 'r');
const fdOut = openSync(`${out}/targets.f16`, 'w');
const fdC = celsiusOut ? openSync(`${out}/celsius.f32`, 'w') : null;
const t0 = performance.now();
const res = await prepareScene(
  {
    info,
    model: Object.fromEntries(
      ['cameras', 'images', 'points3D'].map((n) => [n, readFileSync(`${modelDir}/${n}.bin`)]),
    ),
    readThermal: async (pos) => {
      const b = Buffer.alloc(px * 2);
      if (readSync(fdIn, b, 0, px * 2, pos * px * 2) !== px * 2) throw new Error('short read');
      return new Uint16Array(b.buffer, b.byteOffset, px);
    },
    writeTarget: async (n, bits) => {
      writeSync(fdOut, new Uint8Array(bits.buffer, bits.byteOffset, bits.byteLength));
    },
    onCelsius: fdC != null ? (n, c) => writeSync(fdC, new Uint8Array(c.buffer)) : undefined,
    log: (t) => console.error(t),
  },
  options,
);
closeSync(fdIn);
closeSync(fdOut);
if (fdC != null) closeSync(fdC);
writeFileSync(`${out}/points.f32`, new Uint8Array(res.points.buffer));
writeFileSync(`${out}/ceiling.f32`, new Uint8Array(res.ceiling.buffer));
writeFileSync(`${out}/dataset.json`, JSON.stringify(res.dataset, null, 1));
const stats = {
  ms: performance.now() - t0,
  timings: res.timings,
  rss: process.memoryUsage().rss,
  options,
};
writeFileSync(`${out}/stats.json`, JSON.stringify(stats, null, 1));
console.log(JSON.stringify(stats));
