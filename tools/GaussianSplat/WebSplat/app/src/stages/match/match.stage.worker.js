/** Stage 4 worker (workers/runtime.js protocol): telemetry pairs, WebGPU matching, then two-view
 * verification in a pool of nested colmap-wasm workers (map/map.worker.js).
 * msg: {selectDir, featuresDir, outDir, match: {ratio, pairs: PAIR_DEFAULTS names, seed, legacy},
 * workers}. Writes outDir/{pairs.json, matches.bin, matches.json, geometry.bin, geometry.json}.
 * Progress in pairs: matching counts the first half, verification the second.
 */
import { serveStage } from '../../workers/runtime.js';
import { readFile, readJson, writeFile, writeJson } from '../../storage/opfs.js';
import { selectPairs, PAIR_DEFAULTS } from './pairs.js';
import { createDevice, matchPairs, MATCH_DEFAULTS } from './matcher.js';
import { encodeMatches, decodeMatches, encodeGeometry } from './format.js';
import { decodeFeatures } from '../features/format.js';
import { verifyInPool } from '../map/pool.js';
import { cameraFromFrames } from '../map/priormap.js';
import { VERIFY_DEFAULTS } from '../map/verify.js';

serveStage(async (msg, api) => {
  const controller = new AbortController();
  const stopCheck = setInterval(() => api.stopped && controller.abort(), 100);
  const signal = controller.signal;
  let device = null;
  try {
    const t0 = performance.now();
    api.phase('load');
    const opts = msg.match ?? {};
    const pairOpts = { ...PAIR_DEFAULTS, ...(opts.pairs ?? {}) };
    const ratio = opts.ratio ?? MATCH_DEFAULTS.ratio;
    const vopts = {
      seed: opts.seed ?? VERIFY_DEFAULTS.seed,
      legacy: opts.legacy ?? VERIFY_DEFAULTS.legacy,
    };
    const info = await readJson(`${msg.selectDir}/frames.json`);
    const { pairs, ambiguous } = selectPairs(info.frames, info.target_enu, pairOpts);
    const np = pairs.length / 2;
    for (const a of ambiguous.slice(0, 5))
      api.log(`pair selection tie: ${JSON.stringify(a)}`, 'warn');
    await writeJson(`${msg.outDir}/pairs.json`, {
      format: 'websplat.pairs',
      version: 1,
      options: {
        max_angle: pairOpts.maxAngle,
        max_scale: pairOpts.maxScale,
        neighbours: pairOpts.neighbours,
        sequential: pairOpts.sequential,
      },
      pairs: Array.from({ length: np }, (_, k) => [pairs[2 * k], pairs[2 * k + 1]]),
      ambiguous,
    });
    const features = decodeFeatures(
      await (await readFile(`${msg.featuresDir}/features.bin`)).arrayBuffer(),
    );
    if (features.numImages !== info.frames.length)
      throw new Error('features and frames disagree on the image count');

    api.phase('compile');
    const dev = await createDevice({ powerPreference: opts.powerPreference });
    device = dev.device;
    const adapter = dev.adapter.info ?? {};
    api.phase('match');
    api.progress(0, 2 * np);
    const t1 = performance.now();
    const { results, stats } = await matchPairs(
      device,
      pairs,
      features.counts,
      (k) => ({
        n: features.counts[k],
        data: features.descriptors.subarray(
          128 * features.offsets[k],
          128 * (features.offsets[k] + features.counts[k]),
        ),
      }),
      { ratio, signal, onBatch: ({ done }) => api.progress(done, 2 * np) },
    );
    const matchMs = performance.now() - t1;
    device.destroy();
    device = null;
    const container = encodeMatches(results);
    await writeFile(`${msg.outDir}/matches.bin`, container);
    await writeJson(`${msg.outDir}/matches.json`, {
      format: 'websplat.matches',
      version: 1,
      ratio,
      rule: 'L2-normalised fp16 descriptors, forward ratio on sqrt(2 - 2 sim), reverse nearest-neighbour identity (sfm.py, CUDA path)',
      fp16: true,
      pairs: np,
      matches: stats.matches,
      adapter: {
        vendor: adapter.vendor,
        architecture: adapter.architecture,
        description: adapter.description,
      },
    });

    api.phase('verify');
    const t2 = performance.now();
    const workers =
      msg.workers ?? Math.max(1, Math.min(8, (navigator.hardwareConcurrency ?? 4) - 1));
    const raw = decodeMatches(container, { counts: features.counts });
    const camera = cameraFromFrames(info);
    const v = await verifyInPool(
      () => new Worker(new URL('../map/map.worker.js', import.meta.url), { type: 'module' }),
      camera,
      features,
      raw,
      { ...vopts, workers, signal, onProgress: (done) => api.progress(np + done, 2 * np) },
    );
    const verifyMs = performance.now() - t2;
    await writeFile(`${msg.outDir}/geometry.bin`, encodeGeometry(v.results));
    const verified = v.results.filter((r) => r.matches.length);
    const inliers = verified.reduce((s, r) => s + r.matches.length / 2, 0);
    await writeJson(`${msg.outDir}/geometry.json`, {
      format: 'websplat.geometry',
      version: 1,
      verifier: `colmap-wasm fork, ${vopts.legacy ? 'COLMAP 3.13 two-view estimators' : 'COLMAP 4.2.1 estimators'}`,
      seed: vopts.seed,
      per_pair_seed: true,
      options:
        'TwoViewGeometryOptions() defaults, min_num_inliers 15 clearing (pycolmap verify_matches)',
      camera,
      verified_pairs: verified.length,
      inliers,
    });
    return {
      pairs: np,
      ambiguous: ambiguous.length,
      matches: stats.matches,
      verified: verified.length,
      inliers,
      workers,
      adapter: adapter.description || adapter.vendor || null,
      timings: { matchMs, verifyMs, totalMs: performance.now() - t0, verifyBusyMs: v.stats.busyMs },
    };
  } finally {
    clearInterval(stopCheck);
    device?.destroy();
  }
});
