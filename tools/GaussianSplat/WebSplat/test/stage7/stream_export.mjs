// The stage 7 streaming export in Node, on files:
//   node stream_export.mjs SPLAT DATASET.json FRAMES.json OUTDIR [--title T] [--compare]
// SPLAT: Brush PLY / train.py PLY / WSSP params.bin. Writes OUTDIR/{splat.ply, splat.spz,
// meta.json, result.json}; result.json has the peak RSS. --compare: also the in-memory export
// (export.js exportSplat) and a byte comparison with it (exit 1 if different).
import { mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { args } from '../harness/lib/env.mjs';
import { exportStream } from '../../app/src/stages/export/stream.js';
import { exportInputs, exportSplat, loadSplat } from '../../app/src/stages/export/export.js';
import { startCamera } from '../../app/src/stages/export/meta.js';
import { fileIn, fileSink } from './fsio.mjs';

const spark = fileURLToPath(
  new URL('../../../viewer/node_modules/@sparkjsdev/spark/dist/spark.module.js', import.meta.url),
);
const { transcodeSpz } = await import(spark);
const a = args();
const [splat, datasetPath, framesPath, out] = a._;
if (!out) throw new Error('usage: node stream_export.mjs SPLAT DATASET FRAMES OUTDIR');
mkdirSync(out, { recursive: true });
const dataset = JSON.parse(readFileSync(datasetPath, 'utf8'));
const frames = JSON.parse(readFileSync(framesPath, 'utf8'));
const title = a.title ?? 'Thermal splat';
const input = fileIn(splat);
const sinks = Object.fromEntries(
  [
    ['ply', 'splat.ply'],
    ['spz', 'splat.spz'],
    ['meta', 'meta.json'],
    ['raw', 'spz.raw'],
  ].map(([k, f]) => [k, fileSink(`${out}/${f}`)]),
);
const rss0 = process.memoryUsage().rss;
let peak = rss0;
const timer = setInterval(() => (peak = Math.max(peak, process.memoryUsage().rss)), 50);
const t0 = performance.now();
// peak RSS per phase
const phasePeak = {};
let cur = 'start';
const timer2 = setInterval(() => {
  phasePeak[cur] = Math.max(phasePeak[cur] ?? 0, process.memoryUsage().rss / 2 ** 20);
}, 50);
const res = await exportStream({
  input,
  dataset,
  frames,
  title,
  sinks,
  transcodeSpz,
  phase: (p) => {
    phasePeak[cur] = Math.max(phasePeak[cur] ?? 0, process.memoryUsage().rss / 2 ** 20);
    cur = p;
  },
});
phasePeak[cur] = Math.max(phasePeak[cur] ?? 0, process.memoryUsage().rss / 2 ** 20);
clearInterval(timer2);
const ms = performance.now() - t0;
clearInterval(timer);
for (const s of Object.values(sinks)) s.close();
rmSync(`${out}/spz.raw`);
input.close();
const result = {
  ...res,
  ms,
  rssStartMiB: rss0 / 2 ** 20,
  rssPeakSampledMiB: peak / 2 ** 20,
  maxRssMiB: process.resourceUsage().maxRSS / 1024,
  phasePeakMiB: phasePeak,
};
console.log(
  'peak RSS per phase (MiB):',
  JSON.stringify(phasePeak, (k, v) => (typeof v === 'number' ? Math.round(v) : v)),
);
writeFileSync(`${out}/result.json`, JSON.stringify(result, null, 1));
// export_web.py's start camera (the middle hot view) for the start-view comparison
{
  const inp = exportInputs(dataset, frames);
  writeFileSync(`${out}/native_camera.json`, JSON.stringify(startCamera(inp.views, inp.target)));
}
console.log(
  `${res.info.gaussians} Gaussians (${res.input}): ${(ms / 1000).toFixed(1)} s, peak RSS ${result.maxRssMiB.toFixed(0)} MiB, ` +
    `PLY ${res.downloads.ply} SPZ ${res.downloads.spz} bytes; SPZ over bounds ${JSON.stringify(res.spzReport?.over)}`,
);
let failed = 0;
if (a.compare) {
  const mem = await exportSplat(loadSplat(new Uint8Array(readFileSync(splat))), {
    ...exportInputs(dataset, frames),
    camera: res.info.camera, // stage 7 starts at the hottest view (7a: export_web.py's)
    title,
    transcodeSpz,
  });
  for (const f of ['splat.ply', 'splat.spz', 'meta.json']) {
    const same = Buffer.from(mem.files[f]).equals(readFileSync(`${out}/${f}`));
    console.log(
      `${same ? 'PASS' : 'FAIL'} streaming ${f} = in-memory export (${mem.files[f].length} bytes)`,
    );
    if (!same) failed++;
  }
}
if (res.spzReport && Object.keys(res.spzReport.over).length) {
  console.log(`FAIL SPZ error over bounds: ${JSON.stringify(res.spzReport.over)}`);
  failed++;
}
process.exit(failed ? 1 : 0);
