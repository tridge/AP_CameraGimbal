// Stage 3 definition (pipeline/registry.js): COLMAP SIFT on select's CLAHE images.
// config.features: {maxNumFeatures (8192)}; config.cpuWorkers: pool size (not part of the key).
import { defineStage } from '../../pipeline/registry.js';

export const FEATURES_VERSION = 1;

export const featuresStage = defineStage({
  id: 'features',
  title: 'Find features',
  version: FEATURES_VERSION,
  unit: 'images',
  inputs: ['select'],
  configKeys: ['features'],
  // measured: about 0.75 s per image per worker (laptop), pool of up to 8
  estimate: (config, env) => ({
    total: env.frames ?? null,
    seconds: (0.75 * (env.frames ?? 200)) / Math.max(1, Math.min(8, (env.cores ?? 8) - 1)),
  }),
  run: (ctx) =>
    ctx.runWorker(
      () => new Worker(new URL('./features.worker.js', import.meta.url), { type: 'module' }),
      {
        selectDir: ctx.input('select').dir,
        features: ctx.config.features ?? {},
        workers: ctx.config.cpuWorkers,
      },
    ),
});
