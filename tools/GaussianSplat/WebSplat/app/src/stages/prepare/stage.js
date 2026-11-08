// Stage 5b definition (pipeline/registry.js): train.py's scene preparation (views, holdouts,
// undistorted radiometric targets, temperature scale, bounds, initial points, hot spot,
// training settings). config.prepare: train.py option values (scene.js SCENE_DEFAULTS,
// TRAIN_DEFAULTS names).
import { defineStage } from '../../pipeline/registry.js';

export const PREPARE_VERSION = 1;

export const prepareStage = defineStage({
  id: 'prepare',
  title: 'Prepare training data',
  version: PREPARE_VERSION,
  unit: 'images',
  inputs: ['select', 'map'],
  configKeys: ['prepare'],
  // measured in the page: 8.1 s laptop, 3.9 s win11 for 201 images (notes/stage5b.md)
  estimate: (config, env) => ({
    total: env.frames ?? null,
    seconds: 0.03 * (env.frames ?? 200),
    overhead: 1,
  }),
  run: (ctx) =>
    ctx.runWorker(
      () => new Worker(new URL('./prepare.worker.js', import.meta.url), { type: 'module' }),
      {
        selectDir: ctx.input('select').dir,
        mapDir: ctx.input('map').dir,
        prepare: ctx.config.prepare ?? {},
      },
    ),
});
