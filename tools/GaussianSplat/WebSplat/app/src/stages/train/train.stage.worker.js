// Stage 6 worker: thermal training with the Brush fork from stage 5b's dataset.
// msg: {prepareDir, selectDir, outDir, pkgURL, options, checkpoint, ckptBase, firstCheckpointGeneration}
//   options: {checkpointEvery (iterations, default 5000), previewEvery (default 2000),
//             growthGradThreshold, meanNoiseWeight, refineEvery}
// Writes outDir/{train.json, splat.ply, eval/NAME.f32, preview.png} (CONTRACTS.md 6-7); the Brush
// input directory is built in outDir/brush/ for the attempt and removed before returning.
// Checkpoints: ckpt-<g>/{params.bin (WSSP raw parameters), state.json}; a resumed attempt starts
// Brush at the checkpoint's iteration from those parameters (no optimiser moments).
// The Brush wasm runs in a nested module worker (brush-worker.js): Stop and stage failure
// terminate it with this worker. Any GPU error, shader error or device loss fails the stage.
import { serveStage } from '../../workers/runtime.js';
import { readFile, readJson, remove, writeFile, writeJson } from '../../storage/opfs.js';
import { brushConfig, writeBrushDataset } from './brush-dataset.js';
import { plyToWssp, wsspHeader, wsspToPly } from './params.js';
import { apcgComment, trainInfo } from '../export/meta.js';

export const TRAINER =
  'Brush 1388f74 + WebSplat patches (brush-patches/brush 0001-0016), CubeCL 467522442c7b + 0001';

async function deflate(bytes) {
  const s = new Blob([bytes]).stream().pipeThrough(new CompressionStream('deflate'));
  return new Uint8Array(await new Response(s).arrayBuffer());
}

const stopped = () => Object.assign(new Error('stopped'), { name: 'AbortError' });

// a capped (half size) grey preview of one render, as PNG bytes
async function previewPng(view) {
  const w = view.width >> 1;
  const h = view.height >> 1;
  const img = new ImageData(w, h);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const v = Math.max(0, Math.min(1, view.data[2 * y * view.width + 2 * x]));
      const o = (y * w + x) * 4;
      img.data[o] = img.data[o + 1] = img.data[o + 2] = Math.round(255 * v);
      img.data[o + 3] = 255;
    }
  }
  const c = new OffscreenCanvas(w, h);
  c.getContext('2d').putImageData(img, 0, 0);
  return new Uint8Array(await (await c.convertToBlob({ type: 'image/png' })).arrayBuffer());
}

serveStage(async (msg, api) => {
  const t0 = performance.now();
  const opt = { checkpointEvery: 5000, previewEvery: 2000, ...msg.options };
  api.phase('load');
  const dataset = await readJson(`${msg.prepareDir}/dataset.json`);
  const frames = await readJson(`${msg.selectDir}/frames.json`);
  const px = dataset.width * dataset.height;
  const targets = await readFile(`${msg.prepareDir}/${dataset.targets.file}`);
  const buf = async (f) => (await readFile(`${msg.prepareDir}/${f}`)).arrayBuffer();
  const brushDir = `${msg.outDir}/brush`;
  await writeBrushDataset(dataset, {
    points: new Float32Array(await buf(dataset.points.file)),
    ceiling: new Float32Array(await buf(dataset.bounds.ceiling.file)),
    readTargets: async (i) =>
      new Uint16Array(await targets.slice(i * px * 2, (i + 1) * px * 2).arrayBuffer()),
    write: (path, data) => writeFile(`${brushDir}/${path}`, data),
    deflate,
    signal: { throwIfAborted: () => api.throwIfStopped() },
  });
  const total = dataset.training.options.iterations;
  let startIter = 0;
  const resumed = [];
  if (msg.checkpoint) {
    const state = await readJson(`${msg.checkpoint.dir}/state.json`);
    const params = new Uint8Array(
      await (await readFile(`${msg.checkpoint.dir}/params.bin`)).arrayBuffer(),
    );
    const { count } = wsspHeader(params);
    await writeFile(`${brushDir}/init.ply`, wsspToPly(params));
    startIter = state.iteration;
    resumed.push(...(state.resumed ?? []), { iteration: startIter, gaussians: count });
    api.log(`resuming at iteration ${startIter} from ${count} Gaussians (parameters only)`);
  }
  const config = brushConfig(dataset, {
    startIter,
    growthGradThreshold: opt.growthGradThreshold,
    meanNoiseWeight: opt.meanNoiseWeight,
    refineEvery: opt.refineEvery,
  });
  const datasetMs = performance.now() - t0;
  api.progress(startIter, total, { gaussians: null });
  api.phase('compile');

  const worker = new Worker(new URL('./brush-worker.js', import.meta.url), { type: 'module' });
  let generation = msg.firstCheckpointGeneration;
  let nextCkpt = (Math.floor(startIter / opt.checkpointEvery) + 1) * opt.checkpointEvery;
  let nextPreview = startIter + opt.previewEvery;
  let busy = false; // one snapshot (checkpoint or preview) at a time
  let previews = 0;
  let gaussians = null;
  let trainMs = null;
  let checkpoints = 0;
  const previewView = dataset.images.find((im) => im.split === 'test' && im.hot)?.name;
  const evalViews = dataset.images.filter((im) => im.split === 'test').map((im) => im.name);
  try {
    return await new Promise((resolve, reject) => {
      let finished = false;
      const fail = (e) => {
        if (finished) return;
        finished = true;
        clearInterval(poll);
        reject(e);
      };
      const poll = setInterval(() => api.stopped && fail(stopped()), 250);
      const run = (fn) => Promise.resolve().then(fn).catch(fail);
      worker.onerror = (e) => fail(new Error(`training worker: ${e.message}`));
      worker.onmessage = ({ data: m }) =>
        run(async () => {
          if (finished) return;
          if (m.type === 'progress' || m.type === 'refine') {
            if (m.splats != null) gaussians = m.splats;
            if (m.type === 'refine') return;
            if (m.iter > startIter) api.phase('train');
            api.progress(m.iter, total, { gaussians });
            if (!busy && m.iter < total && m.iter >= nextCkpt) {
              busy = true;
              nextCkpt += opt.checkpointEvery;
              worker.postMessage({ type: 'checkpoint' });
            } else if (!busy && m.iter < total && m.iter >= nextPreview && previewView) {
              busy = true;
              nextPreview += opt.previewEvery;
              worker.postMessage({ type: 'previewRender' });
            }
          } else if (m.type === 'checkpoint') {
            const params = plyToWssp(m.bytes);
            const g = generation++;
            await api.checkpoint(
              g,
              async () => {
                await writeFile(`${msg.ckptBase}${g}/params.bin`, params);
                await writeJson(`${msg.ckptBase}${g}/state.json`, {
                  format: 'websplat.train-checkpoint',
                  version: 1,
                  iteration: m.iter,
                  gaussians: wsspHeader(params).count,
                  seed: 42,
                  resumed,
                  note: 'parameters only: Adam moments, refine statistics and the data order restart on resume',
                });
              },
              { done: m.iter, gaussians: wsspHeader(params).count },
            );
            checkpoints++;
            api.phase('train');
            busy = false;
          } else if (m.type === 'previewRender') {
            const v = m.views.find((x) => x.name === previewView);
            if (v) {
              await writeFile(`${msg.outDir}/preview.png`, await previewPng(v));
              api.progress(m.iter, total, {
                gaussians,
                preview: {
                  path: `${msg.outDir}/preview.png`,
                  seq: ++previews,
                  view: v.name,
                  iteration: m.iter,
                },
              });
            }
            busy = false;
          } else if (m.type === 'done') {
            trainMs = m.elapsedMs;
            gaussians = m.splats;
            api.progress(total, total, { gaussians });
            api.phase('eval');
            const wait = () =>
              new Promise((r) => {
                const t = setInterval(() => !busy && (clearInterval(t), r()), 50);
              });
            await wait();
            worker.postMessage({ type: 'renderEval' });
          } else if (m.type === 'evalRenders') {
            for (const v of m.views)
              await writeFile(`${msg.outDir}/eval/${v.name}.f32`, new Uint8Array(v.data.buffer));
            // train.py's splat.ply header
            const info = trainInfo({
              target: dataset.hotspot?.enu ?? frames.target_enu,
              scale: dataset.scale,
              origin: frames.origin,
            });
            worker.postMessage({ type: 'exportPly', comments: [apcgComment(info)] });
            evalViews.length = 0;
            evalViews.push(...m.views.map((v) => v.name));
          } else if (m.type === 'ply') {
            await writeFile(`${msg.outDir}/splat.ply`, m.bytes);
            worker.postMessage({ type: 'dispose' });
          } else if (m.type === 'disposed') {
            if (!m.ok) return fail(new Error(`training run invalid: ${m.reason}`));
            finished = true;
            clearInterval(poll);
            resolve(m);
          } else if (m.type === 'failed') {
            fail(new Error(`training failed: ${m.reason}`));
          } else if (m.type === 'error') {
            fail(new Error(m.message.split('\n')[0]));
          } else if (m.type === 'gpuerror' || m.type === 'shaderError') {
            api.log(`${m.type}: ${m.message ?? m.messages?.join('; ')}`, 'error');
          } else if (m.type === 'warning') {
            api.log(m.text, 'warn');
          }
        });
      worker.postMessage({
        type: 'start',
        pkg: msg.pkgURL,
        dataset: ['websplat', ...brushDir.split('/').filter(Boolean)],
        config,
        stepsPerMessage: 50,
      });
    }).then(async () => {
      api.phase('write');
      await remove(brushDir);
      const train = {
        format: 'websplat.train',
        version: 1,
        splat: 'splat.ply',
        gaussians,
        sh_degree: dataset.training.options.sh_degree,
        rasterize_mode: 'classic',
        iterations: total,
        eval: { dir: 'eval', views: evalViews },
        standin: false,
        trainer: TRAINER,
        brush_config: config,
        resumed,
      };
      await writeJson(`${msg.outDir}/train.json`, train);
      return {
        gaussians,
        iterations: total,
        resumed_from: resumed.map((r) => r.iteration),
        train_seconds: trainMs != null ? trainMs / 1000 : null,
        dataset_seconds: datasetMs / 1000,
        checkpoints,
      };
    });
  } finally {
    worker.terminate();
  }
});
