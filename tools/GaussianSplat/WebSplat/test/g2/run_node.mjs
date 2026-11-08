// G2 in Node: the colmap-wasm fork's verification and prior mapping on stage files.
//   node run_node.mjs verify FEATURES.bin MATCHES.bin CAMERA.json OUT.bin [--seed S] [--current]
//   node run_node.mjs map FRAMES.json FEATURES.bin GEOMETRY.bin OUTDIR [--camera CAMERA.json]
// Writes the outputs plus OUT.json / OUTDIR/stats.json (timings, wasm memory, rounds).
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { performance } from 'node:perf_hooks';
import { loadColmap } from '../../app/src/stages/map/colmap.js';
import { decodeFeatures } from '../../app/src/stages/map/features.js';
import { verifyPairs } from '../../app/src/stages/map/verify.js';
import { priorMap, cameraFromFrames, modelJson } from '../../app/src/stages/map/priormap.js';
import {
  decodeMatches,
  decodeGeometry,
  encodeGeometry,
} from '../../app/src/stages/match/format.js';

const wasmUrl = new URL('../../app/src/stages/map/wasm/websplat-colmap.wasm', import.meta.url);
const args = process.argv.slice(2);
const flag = (name, def) => {
  const k = args.indexOf(name);
  if (k < 0) return def;
  const v = args[k + 1];
  args.splice(k, 2);
  return v;
};
const has = (name) => {
  const k = args.indexOf(name);
  if (k < 0) return false;
  args.splice(k, 1);
  return true;
};

const t0 = performance.now();
const cw = await loadColmap({
  wasmBinary: readFileSync(wasmUrl),
  onLog: (t) => console.error(t),
});
const tLoad = performance.now() - t0;
const cmd = args.shift();

if (cmd === 'verify') {
  const seed = Number(flag('--seed', 0));
  const legacy = !has('--current');
  const limit = Number(flag('--limit', 0));
  const [featPath, matchPath, camPath, out] = args;
  const features = decodeFeatures(readFileSync(featPath));
  let pairs = decodeMatches(readFileSync(matchPath), {
    counts: features.counts,
  });
  if (limit) pairs = pairs.filter((_, k) => k % Math.ceil(pairs.length / limit) === 0);
  const camera = JSON.parse(readFileSync(camPath, 'utf8'));
  const t1 = performance.now();
  let last = 0;
  const res = verifyPairs(cw, camera, features, pairs, {
    seed,
    legacy,
    onProgress: (done, total) => {
      if (done - last >= 500 || done === total) {
        last = done;
        console.error(`verify ${done}/${total}`);
      }
    },
  });
  const ms = performance.now() - t1;
  writeFileSync(out, encodeGeometry(res));
  const stats = {
    load_ms: tLoad,
    verify_ms: ms,
    pairs: res.length,
    verified: res.filter((r) => r.matches.length).length,
    inliers: res.reduce((s, r) => s + r.matches.length / 2, 0),
    wasm_heap_bytes: cw.heapBytes,
    rss_bytes: process.memoryUsage().rss,
    seed,
    legacy,
  };
  writeFileSync(out.replace(/\.bin$/, '') + '.json', JSON.stringify(stats, null, 1));
  console.log(JSON.stringify(stats));
} else if (cmd === 'map') {
  const camPath = flag('--camera', null);
  const [framesPath, featPath, geomPath, outDir] = args;
  const info = JSON.parse(readFileSync(framesPath, 'utf8'));
  const features = decodeFeatures(readFileSync(featPath));
  const geometry = decodeGeometry(readFileSync(geomPath), {
    counts: features.counts,
  });
  const camera = camPath ? JSON.parse(readFileSync(camPath, 'utf8')) : cameraFromFrames(info);
  const t1 = performance.now();
  const phases = {};
  const res = priorMap(cw, camera, info.frames, features, geometry, {
    onProgress: (phase, done, total, round) => {
      const key = `${round}:${phase}`;
      phases[key] = phases[key] ?? performance.now();
      return false;
    },
    onRound: (s) => console.error(JSON.stringify(s)),
  });
  const ms = performance.now() - t1;
  mkdirSync(outDir, { recursive: true });
  for (const [name, data] of Object.entries(res.files)) writeFileSync(`${outDir}/${name}`, data);
  writeFileSync(`${outDir}/model.json`, JSON.stringify(modelJson(camera, res.rounds), null, 1));
  const stats = {
    load_ms: tLoad,
    map_ms: ms,
    camera,
    rounds: res.rounds,
    wasm_heap_bytes: cw.heapBytes,
    rss_bytes: process.memoryUsage().rss,
  };
  writeFileSync(`${outDir}/stats.json`, JSON.stringify(stats, null, 1));
  console.log(JSON.stringify({ map_ms: ms, wasm_heap_bytes: cw.heapBytes }));
} else {
  console.error('usage: run_node.mjs verify|map ...');
  process.exit(2);
}
