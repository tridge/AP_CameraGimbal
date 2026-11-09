// Stage 6 definition (pipeline/registry.js): thermal training with the Brush fork (WebGPU,
// brush-worker.js) from stage 5b's dataset, train.py's rules enforced as notes/stage6.md lists.
// config.train: {checkpointEvery, previewEvery, growthGradThreshold, meanNoiseWeight,
// refineEvery} (train.stage.worker.js).
import { defineStage } from '../../pipeline/registry.js';

export const TRAIN_VERSION = 1;

// the Brush fork's wasm package, served by the app under vendor/brush/ (scripts/prepare-assets.mjs)
export const brushURL = () =>
  new URL('vendor/brush/brush_js.js', globalThis.document?.baseURI ?? location.href).href;

// measured: G1's repeat runs of this configuration (notes/g1.md), training seconds for 30k
// iterations, and the normalised cumulative time at every 2000 iterations (training slows as
// Gaussians grow; laptop and win11 curves agree within 0.007)
const SECONDS_30K = { laptop: 844, desktop: 461, integrated: 3000, unknown: 1500 };
const CURVE = [
  0, 0.0376, 0.0838, 0.1443, 0.2092, 0.2753, 0.343, 0.4128, 0.4846, 0.5561, 0.6314, 0.706, 0.7804,
  0.8531, 0.9255, 1,
];

const iterationsOf = (config) => config.prepare?.iterations ?? 30000;

export function cumCost(total) {
  return (u) => {
    const x = Math.max(0, Math.min(1, u / total)) * (CURVE.length - 1);
    const k = Math.min(CURVE.length - 2, Math.floor(x));
    return total * (CURVE[k] + (CURVE[k + 1] - CURVE[k]) * (x - k));
  };
}

export const trainStage = defineStage({
  id: 'train',
  title: 'Train the splat',
  version: TRAIN_VERSION,
  unit: 'iterations',
  inputs: ['select', 'prepare'],
  configKeys: ['train'],
  estimate: (config, env) => {
    const total = iterationsOf(config);
    return {
      total,
      seconds: (SECONDS_30K[env.gpuClass] ?? SECONDS_30K.unknown) * (total / 30000),
      // Brush dataset (PNG16 targets, init.ply), wasm load and shader compiles, eval and export
      overhead: 40,
      cumCost: cumCost(total),
      checkpointEvery: config.train?.checkpointEvery ?? 5000,
      checkpointSeconds: 6,
    };
  },
  run: (ctx) =>
    ctx.runWorker(
      () => new Worker(new URL('./train.stage.worker.js', import.meta.url), { type: 'module' }),
      {
        prepareDir: ctx.input('prepare').dir,
        selectDir: ctx.input('select').dir,
        pkgURL: brushURL(),
        options: ctx.config.train ?? {},
      },
    ),
});
