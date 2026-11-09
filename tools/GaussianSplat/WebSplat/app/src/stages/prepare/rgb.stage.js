// Stage 5b for RGB recordings (pipeline/registry.js); config.prepare: rgb-scene.js option names.
import { defineStage } from '../../pipeline/registry.js';

export const RGB_PREPARE_VERSION = 1;

export const rgbPrepareStage = defineStage({
  id: 'prepare',
  title: 'Prepare training data',
  version: `rgb-${RGB_PREPARE_VERSION}`,
  unit: 'images',
  inputs: ['select', 'map'],
  configKeys: ['prepare'],
  estimate: (config, env) => ({
    total: env.frames ?? null,
    seconds: 0.1 * (env.frames ?? 300),
    overhead: 1,
  }),
  run: (ctx) =>
    ctx.runWorker(
      () => new Worker(new URL('./rgb.prepare.worker.js', import.meta.url), { type: 'module' }),
      {
        selectDir: ctx.input('select').dir,
        mapDir: ctx.input('map').dir,
        prepare: ctx.config.prepare ?? {},
      },
    ),
});
