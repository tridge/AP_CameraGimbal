// Stage 4 definition (pipeline/registry.js): telemetry pairs, WebGPU matching (this directory)
// and two-view verification (map/verify.js in a worker pool).
// config.match: {ratio, pairs: {maxAngle, maxScale, neighbours, sequential}, seed, legacy,
// powerPreference}; config.cpuWorkers: verification pool size (not part of the key).
import { defineStage } from '../../pipeline/registry.js';

export const MATCH_STAGE_VERSION = 1;

export const matchStage = defineStage({
  id: 'match',
  title: 'Match features',
  version: MATCH_STAGE_VERSION,
  unit: 'pairs',
  inputs: ['select', 'features'],
  configKeys: ['match'],
  // measured (G2, stage 4): matching 8.7 ms/pair laptop, 2.5 ms/pair desktop GPU;
  // verification about 80 ms/pair per worker
  estimate: (config, env) => {
    const pairs = 16 * (env.frames ?? 200);
    const workers = Math.max(1, Math.min(8, (env.cores ?? 8) - 1));
    const gpu = env.gpuClass === 'desktop' ? 0.0025 : env.gpuClass === 'integrated' ? 0.1 : 0.009;
    return { total: 2 * pairs, seconds: pairs * gpu + (pairs * 0.08) / workers, overhead: 2 };
  },
  run: (ctx) =>
    ctx.runWorker(
      () => new Worker(new URL('./match.stage.worker.js', import.meta.url), { type: 'module' }),
      {
        selectDir: ctx.input('select').dir,
        featuresDir: ctx.input('features').dir,
        match: ctx.config.match ?? {},
        workers: ctx.config.cpuWorkers,
      },
    ),
});
