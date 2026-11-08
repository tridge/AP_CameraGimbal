// The dummy stage set: fake work in module workers with realistic counts and timings, used to
// exercise the controller, storage, progress/ETA model and harness. ?speed=N runs N times faster.
import { defineStage, registerStageSet } from '../../pipeline/registry.js';
import { DESKTOP_FACTOR, PROFILES, cumCostOf } from './profile.js';

const ids = Object.keys(PROFILES);

const stages = ids.map((id, k) =>
  defineStage({
    id,
    title: PROFILES[id].title,
    version: 1,
    unit: PROFILES[id].unit,
    inputs: k === 0 ? ['source'] : [ids[k - 1]],
    configKeys: ['dummySeed'],
    checkpoints: !!PROFILES[id].checkpointEvery,
    estimate(config, env) {
      const p = PROFILES[id];
      const f = (env.gpuClass === 'desktop' ? DESKTOP_FACTOR : 1) / (config.speed ?? 1);
      const overhead = Object.values(p.overhead).reduce((a, b) => a + b, 0) / (config.speed ?? 1);
      return {
        total: p.total,
        seconds: p.seconds * f,
        overhead,
        cumCost: cumCostOf(p),
        uncertain: !!p.uncertain,
        checkpointEvery: p.checkpointEvery,
        checkpointSeconds: (config.ckptMs ?? 1500) / 1000,
      };
    },
    async run(ctx) {
      const inputs = ctx.def.inputs.map((i) => (i === 'source' ? null : ctx.input(i)));
      return ctx.runWorker(
        () => new Worker(new URL('./dummy.worker.js', import.meta.url), { type: 'module' }),
        {
          stage: id,
          inputHashes: inputs.map(
            (i) => i && Object.fromEntries(Object.entries(i.outputs).map(([n, o]) => [n, o.hash])),
          ),
        },
      );
    },
  }),
);

registerStageSet('dummy', stages, { title: 'Dummy pipeline (fake work)' });
