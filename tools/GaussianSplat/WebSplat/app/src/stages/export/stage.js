// Stage 7 definition (pipeline/registry.js): the downloads (splat.ply, splat.spz, meta.json, as
// export_web.py makes them) from stage 6's trained splat, streamed (stream.js). Two workers in
// turn: the PLY, then the SPZ (Spark in chunks, checked chunk by chunk) and meta.json.
// config.export: {title, description, view (a request's captured start view), target ('picked':
// the selection's target, not prepare's hot spot)}.
import { defineStage } from '../../pipeline/registry.js';
import { remove } from '../../storage/opfs.js'; // retries while a worker's handle is released

export const EXPORT_VERSION = 2;

const worker = () =>
  new Worker(new URL('./export.stage.worker.js', import.meta.url), { type: 'module' });

export const exportStage = defineStage({
  id: 'export',
  title: 'Export',
  version: EXPORT_VERSION,
  unit: 'Gaussians',
  inputs: ['select', 'prepare', 'train'],
  configKeys: ['export'],
  // measured in the page (notes/stage7.md): about 15 s per million Gaussians on the laptop
  estimate: (config, env) => {
    const n = env.results?.train?.gaussians ?? 600000;
    return { total: 2 * n, seconds: (15 * n) / 1e6, overhead: 2 };
  },
  async run(ctx) {
    const dirs = {
      selectDir: ctx.input('select').dir,
      prepareDir: ctx.input('prepare').dir,
      trainDir: ctx.input('train').dir,
      title: ctx.config.export?.title ?? 'Thermal splat',
      description: ctx.config.export?.description ?? '',
      view: ctx.config.export?.view ?? null,
      targetPolicy: ctx.config.export?.target ?? null,
    };
    const t0 = performance.now();
    const ply = await ctx.runWorker(worker, { ...dirs, pass: 'ply' });
    const t1 = performance.now();
    const fin = await ctx.runWorker(worker, { ...dirs, pass: 'spz', ply });
    await remove(`${ctx.outDir}/spz.raw`);
    const t2 = performance.now();
    // a decoded SPZ outside the quantisation bounds (or not finite) is a broken file; values the
    // format clamps are counted apart (spz.clamped) and shown in the result view
    if (Object.keys(fin.spz.over).length)
      throw new Error(`the SPZ does not decode within its bounds: ${JSON.stringify(fin.spz.over)}`);
    const train = ctx.input('train').result ?? {};
    const s = ply.info.temperature_scale;
    return {
      kind: ply.info.kind,
      title: dirs.title,
      trainStandin: !!train.standin, // made from a stand-in's splat, not stage 6's training
      gaussians: ply.count,
      shDegree: ply.info.sh_degree,
      input: ply.input,
      temperatureC: s ? [s.t0_c, s.t1_c] : null,
      displayRangeC: ply.info.display_range_c ?? null,
      rasterizeMode: ply.info.rasterize_mode,
      stoppedEarly: ply.info.stopped_early ?? null, // {iterations, iterations_planned}
      files: { 'splat.ply': ply.plySize, 'splat.spz': fin.spzSize, 'meta.json': fin.metaSize },
      spz: fin.spz,
      timesMs: { ply: t1 - t0, spz: t2 - t1 },
      workerTimesMs: { ply: ply.ms, spz: fin.ms },
    };
  },
});
