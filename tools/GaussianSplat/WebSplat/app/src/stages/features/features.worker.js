/** Stage 3 worker (workers/runtime.js protocol). msg: {selectDir, outDir, features: {maxNumFeatures},
 * workers (pool size)}. Reads select's frames.json and features.u8 (CLAHE images), runs COLMAP SIFT in a
 * pool of nested workers and writes outDir/features.bin (WSFT v1) and features.json.
 * Progress in images.
 */
import { serveStage } from '../../workers/runtime.js';
import { readFile, readJson, writeFile, writeJson } from '../../storage/opfs.js';
import { siftInPool } from './pool.js';
import { FEAT_MAGIC } from './format.js';
import { SIFT_DEFAULTS } from './sift.js';

serveStage(async (msg, api) => {
  const controller = new AbortController();
  const stopCheck = setInterval(() => api.stopped && controller.abort(), 100);
  try {
    const t0 = performance.now();
    api.phase('load');
    const info = await readJson(`${msg.selectDir}/frames.json`);
    const pixels = await readFile(`${msg.selectDir}/features.u8`);
    const { width: W, height: H } = info;
    const n = info.frames.length;
    if (pixels.size !== n * W * H)
      throw new Error(`features.u8 is ${pixels.size} bytes, expected ${n * W * H}`);
    const o = { ...SIFT_DEFAULTS, ...(msg.features ?? {}) };
    const workers =
      msg.workers ?? Math.max(1, Math.min(8, (navigator.hardwareConcurrency ?? 4) - 1));
    api.phase('sift');
    api.progress(0, n);
    const { images, stats } = await siftInPool(
      () => new Worker(new URL('./sift.worker.js', import.meta.url), { type: 'module' }),
      n,
      async (k) => new Uint8Array(await pixels.slice(k * W * H, (k + 1) * W * H).arrayBuffer()),
      W,
      H,
      {
        workers,
        maxNumFeatures: o.maxNumFeatures,
        signal: controller.signal,
        onImage: (k, r, done) => api.progress(done, n),
      },
    );
    const siftMs = performance.now() - t0;
    api.phase('write');
    const counts = images.map((im) => im.keypoints.length / 6);
    const total = counts.reduce((a, b) => a + b, 0);
    // streamed WSFT: header + index, keypoints, descriptors (no full-size concatenation)
    const head = new Uint32Array(4 + 2 * n);
    head.set([FEAT_MAGIC, 1, n, 128]);
    let off = 0;
    counts.forEach((c, k) => {
      head[4 + 2 * k] = c;
      head[5 + 2 * k] = off;
      off += c;
    });
    await writeFile(
      `${msg.outDir}/features.bin`,
      (function* () {
        yield head;
        for (const im of images) yield im.keypoints;
        for (const im of images) yield im.descriptors;
      })(),
    );
    await writeJson(`${msg.outDir}/features.json`, {
      format: 'websplat.features',
      version: 1,
      extractor: 'COLMAP 4.2.1 SIFT (VLFeat, CPU) in the colmap-wasm fork',
      max_num_features: o.maxNumFeatures,
      options: 'FeatureExtractionOptions() defaults (sfm.py), on frames/features.u8',
      images: n,
      keypoints: total,
      counts,
    });
    return {
      images: n,
      keypoints: total,
      workers,
      timings: { siftMs, totalMs: performance.now() - t0, busyMs: stats.busyMs },
      heap: Math.max(...stats.heap),
    };
  } finally {
    clearInterval(stopCheck);
  }
});
