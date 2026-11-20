// Stage 5 definition (pipeline/registry.js): telemetry-seeded mapping (sfm.py --mapper prior)
// with per-round checkpoints. config.map: {thresholds, priorSigma, maxIterations, camera
// (SIMPLE_RADIAL, RADIAL or PINHOLE)}.
import { defineStage } from '../../pipeline/registry.js';

export const MAP_VERSION = 1;

export const mapStage = defineStage({
  id: 'map',
  title: 'Map cameras and points',
  version: MAP_VERSION,
  unit: 'BA rounds',
  inputs: ['select', 'features', 'match'],
  configKeys: ['map'],
  checkpoints: true,
  // measured: 75-89 s for 4 rounds in the page (series 4, notes/stage6.md; G2 62-74 s); one checkpoint per round
  estimate: (config) => {
    const rounds = config.map?.thresholds?.length ?? 4;
    // BA may stop before its iteration limit, so the work left in a round is uncertain
    return {
      total: rounds,
      uncertain: true,
      seconds: 19.5 * rounds,
      overhead: 2,
      checkpointEvery: 1,
      checkpointSeconds: 0.2,
    };
  },
  run: (ctx) =>
    ctx.runWorker(
      () => new Worker(new URL('./map.stage.worker.js', import.meta.url), { type: 'module' }),
      {
        selectDir: ctx.input('select').dir,
        featuresDir: ctx.input('features').dir,
        matchDir: ctx.input('match').dir,
        map: ctx.config.map ?? {},
      },
    ),
});
