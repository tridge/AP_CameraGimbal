// Stage 6 definition (pipeline/registry.js): thermal training with the Brush fork (WebGPU,
// brush-worker.js) from stage 5b's dataset, train.py's rules enforced as notes/stage6.md lists.
// config.train: {checkpointEvery, previewEvery, growthGradThreshold, meanNoiseWeight,
// refineEvery} (train.stage.worker.js).
import { defineStage } from '../../pipeline/registry.js';
import { assetURL } from '../../asset-url.js';

export const TRAIN_VERSION = 1;

// the Brush fork's wasm package, served by the app under vendor/brush/ (scripts/prepare-assets.mjs)
export const brushURL = () => assetURL('vendor/brush/brush_js.js');

// measured: training seconds for 30k iterations (laptop: stage 6 series 4 median, notes/stage6.md;
// desktop: stage 8 win11 run 1 with its GPU idle, notes/stage8.md), and the normalised
// cumulative time at every 2000 iterations (stage 8 laptop runs 1-2 and win11 run 1: training
// slows while Gaussians grow until 15000, then holds its rate; the curves agree within 0.027)
const SECONDS_30K = { laptop: 736, desktop: 407, integrated: 3000, unknown: 1500 };
const CURVE = [
  0, 0.0324, 0.0704, 0.1173, 0.1702, 0.2328, 0.3015, 0.3806, 0.4641, 0.5425, 0.6225, 0.7008, 0.7774,
  0.8531, 0.9304, 1,
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
      // (series 4: load 4-5 s, compile 4-6 s, eval 1 s)
      overhead: 12,
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
