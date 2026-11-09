/** Stage 5 worker (workers/runtime.js protocol): sfm.py's prior mapping with the colmap-wasm fork,
 * in this worker. msg: {selectDir, featuresDir, matchDir, outDir, map: {thresholds, priorSigma,
 * maxIterations}, checkpoint, ckptBase, firstCheckpointGeneration}. Writes outDir/final/
 * {cameras,images,points3D,rigs,frames}.bin and outDir/model.json. A checkpoint after every
 * round holds the state the next round starts from (poses, camera, PRNG), so a resumed run gives
 * the same model. Progress: rounds, fractional within a round (triangulation, then BA iterations).
 * Stop: a round is one synchronous wasm call; the host terminates the worker (3 s rule).
 */
import { serveStage } from '../../workers/runtime.js';
import { readFile, readJson, writeFile, writeJson } from '../../storage/opfs.js';
import { decodeFeatures } from '../features/format.js';
import { decodeGeometry } from '../match/format.js';
import { loadColmap } from './colmap.js';
import {
  priorMap,
  cameraFromFrames,
  modelJson,
  thinMatches,
  PRIOR_MAP_DEFAULTS,
} from './priormap.js';

const TRI = 0.15; // share of a round's progress for triangulation

// the 4 GB WebAssembly heap limits images x correspondences: say so
function outOfMemory(e, images, geometry) {
  if (!/bad_alloc/.test(e?.message ?? '')) return e;
  const matches = geometry.reduce((n, g) => n + (g.matches?.length ?? 0) / 2, 0);
  return new Error(
    `mapping ran out of memory (the 4 GB WebAssembly heap) with ${images} images and ${matches} ` +
      'matches: use a shorter time range (or, for RGB, a smaller processing width)',
  );
}

serveStage(async (msg, api) => {
  const t0 = performance.now();
  api.phase('load');
  const o = { ...PRIOR_MAP_DEFAULTS, ...(msg.map ?? {}) };
  const nr = o.thresholds.length;
  const info = await readJson(`${msg.selectDir}/frames.json`);
  const features = decodeFeatures(
    await (await readFile(`${msg.featuresDir}/features.bin`)).arrayBuffer(),
  );
  const geometry = decodeGeometry(
    await (await readFile(`${msg.matchDir}/geometry.bin`)).arrayBuffer(),
    {
      counts: features.counts,
    },
  );
  const thinned = thinMatches(geometry);
  if (thinned.kept < 1)
    api.log(
      `${thinned.total} matches: mapping with ${(100 * thinned.kept).toFixed(0)}% of each ` +
        "image's keypoints to stay inside the WebAssembly heap",
      'warn',
    );
  let start = null;
  if (msg.checkpoint) {
    const s = await readJson(`${msg.checkpoint.dir}/state.json`);
    start = { ...s, poses: new Float64Array(s.poses), params: new Float64Array(s.params) };
    api.log(`resuming after round ${s.round}`);
  }
  const cw = await loadColmap({
    onLog: (text, level) => level === 'warn' && api.log(text, 'warn'),
  });
  const loadMs = performance.now() - t0;
  const camera = cameraFromFrames(info);
  let generation = msg.firstCheckpointGeneration;
  let lastPost = 0;
  let phase = null;
  const res = await priorMap(cw, camera, info.frames, features, thinned.geometry, {
    ...o,
    start,
    onProgress: (p, raw, total, round) => {
      // Ceres reports its initial state as a callback too: BA iterations are raw - 1, at most total
      const done = p === 'ba' ? Math.max(0, Math.min(total, raw - 1)) : raw;
      if (p !== phase) api.phase((phase = p === 'ba' ? 'bundle adjustment' : 'triangulate'));
      const now = performance.now();
      if (now - lastPost < 250 && done !== total) return false;
      lastPost = now;
      const frac = total > 0 ? Math.min(1, done / total) : 1;
      const f = p === 'ba' ? TRI + (1 - TRI) * frac : TRI * frac;
      api.progress(round + f, nr, { round, phase: p, done, total });
      return false;
    },
    onRound: async (stats, state) => {
      api.log(
        `round ${stats.round}: ${stats.threshold_px} px, ${stats.triangulated} triangulated, ${stats.filtered} filtered, ` +
          `mean reprojection ${stats.mean_reproj_px.toFixed(3)} px, f ${stats.focal.toFixed(2)} (${stats.seconds.toFixed(1)} s)`,
      );
      api.progress(stats.round + 1, nr, { round: stats.round, phase: 'done' });
      if (stats.round + 1 < nr) {
        const s = state();
        const g = generation++;
        await api.checkpoint(
          g,
          () =>
            writeJson(`${msg.ckptBase}${g}/state.json`, {
              ...s,
              poses: Array.from(s.poses),
              params: Array.from(s.params),
            }),
          { done: stats.round + 1, round: stats.round },
        );
      }
      api.throwIfStopped();
    },
  }).catch((e) => {
    throw outOfMemory(e, info.frames.length, thinned.geometry);
  });
  api.phase('write');
  for (const [name, data] of Object.entries(res.files))
    await writeFile(`${msg.outDir}/final/${name}`, data);
  await writeJson(`${msg.outDir}/model.json`, modelJson(camera, res.rounds, o));
  const last = res.rounds.at(-1);
  return {
    images: info.frames.length,
    points: Number(new DataView(res.files['points3D.bin'].buffer).getBigUint64(0, true)),
    observations: last.observations,
    mean_reproj_px: last.mean_reproj_px,
    focal: last.focal,
    k1: last.k1,
    rounds: res.rounds.map((r) => ({
      threshold_px: r.threshold_px,
      triangulated: r.triangulated,
      filtered: r.filtered,
      seconds: r.seconds,
    })),
    resumedAfter: start ? start.round : null,
    matches: thinned.total,
    keptKeypoints: thinned.kept,
    heap: cw.heapBytes,
    timings: { loadMs, totalMs: performance.now() - t0 },
  };
});
