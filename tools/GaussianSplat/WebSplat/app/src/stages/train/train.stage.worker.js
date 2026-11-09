// Stage 6 worker: training with the Brush fork from stage 5b's dataset (thermal numeric targets,
// or RGB images through Brush's stock colour path).
// msg: {prepareDir, selectDir, outDir, pkgURL, options, accept, exportConfig, checkpoint, ckptBase,
//       firstCheckpointGeneration}
//   options: {checkpointEvery (iterations, default 5000), previewEvery (default 2000),
//             growthGradThreshold, meanNoiseWeight, refineEvery}
// Writes outDir/{train.json, splat.ply, eval/NAME.f32, preview.png} (CONTRACTS.md 6-7); the Brush
// input directory is built in outDir/brush/ for the attempt and removed before returning.
// Checkpoints: ckpt-<g>/{params.bin (WSSP raw parameters), state.json}; a resumed attempt starts
// Brush at the checkpoint's iteration from those parameters (no optimiser moments).
// Commands (api.onCommand): 'preview' (the current splat as a viewable PLY, preview.js) and
// 'finish' (stop and accept: training ends at the next batch boundary and the run completes as
// usual with the iterations done). msg.accept (an accepted stop recorded in the manifest) ends a
// resumed run at once after the checkpoint, or without one at accept.iteration.
// The Brush wasm runs in a nested module worker (brush-worker.js): Stop and stage failure
// terminate it with this worker. Any GPU error, shader error or device loss fails the stage.
import { serveStage } from '../../workers/runtime.js';
import { readFile, readJson, remove, writeFile, writeJson } from '../../storage/opfs.js';
import { RGB_CAP, brushConfig, writeBrushDataset } from './brush-dataset.js';
import { plyToWssp, wsspHeader, wsspToPly } from './params.js';
import { apcgComment, trainInfo } from '../export/meta.js';
import { previewPly } from './preview.js';

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
  const rgb = dataset.kind === 'rgb';
  const px = dataset.width * dataset.height;
  const targets = await readFile(`${msg.prepareDir}/${dataset.targets.file}`);
  const targetBytes = rgb ? px * 3 : px * 2;
  const buf = async (f) => (await readFile(`${msg.prepareDir}/${f}`)).arrayBuffer();
  const brushDir = `${msg.outDir}/brush`;
  await writeBrushDataset(dataset, {
    points: new Float32Array(await buf(dataset.points.file)),
    ceiling: new Float32Array(await buf(dataset.bounds.ceiling.file)),
    readTargets: async (i) => {
      const b = await targets.slice(i * targetBytes, (i + 1) * targetBytes).arrayBuffer();
      return rgb ? new Uint8Array(b) : new Uint16Array(b);
    },
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
    maxSplats: rgb
      ? Math.min(dataset.training.options.cap, RGB_CAP[msg.env?.gpuClass] ?? RGB_CAP.unknown)
      : null,
  });
  // stop and accept, recorded before this attempt: right after the checkpoint (Brush may run a
  // few steps: it reports them in groups), else where asked
  const stopAt = msg.accept
    ? msg.checkpoint
      ? startIter + 1
      : Math.max(1, msg.accept.iteration ?? 1)
    : null;
  if (stopAt != null) api.log(`stopping early at iteration ${stopAt} (accepted)`);
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
  let doneIter = total;
  let trainingStarted = false;
  let finishing = false;
  let snapshotId = 0;
  const snapshots = new Map();
  api.onCommand = async (name) => {
    if (name === 'finish') {
      if (!trainingStarted) throw new Error('training has not started yet');
      if (!finishing) worker.postMessage({ type: 'finish' });
      finishing = true;
      return { value: { requested: true } };
    }
    if (name === 'preview') {
      if (!trainingStarted) throw new Error('training has not started yet');
      const id = ++snapshotId;
      const snap = await new Promise((res, rej) => {
        snapshots.set(id, { res, rej });
        worker.postMessage({ type: 'snapshotPly', id });
      });
      const p = await previewPly({
        splat: snap.bytes,
        dataset,
        frames,
        view: msg.exportConfig?.view ?? null,
        targetPolicy: msg.exportConfig?.target ?? null,
        title: `${msg.exportConfig?.title ?? (rgb ? 'RGB splat' : 'Thermal splat')} (training preview)`,
      });
      return {
        value: { bytes: p.bytes, iteration: snap.iter, total, gaussians: p.count },
        transfer: [p.bytes.buffer],
      };
    }
    throw new Error(`unknown command ${name}`);
  };
  // RGB: Brush's eval renders return channel 0 only, so the preview is the red channel in grey
  const previewView = dataset.images.find((im) => im.split === 'test' && (rgb || im.hot))?.name;
  const evalViews = dataset.images.filter((im) => im.split === 'test').map((im) => im.name);
  try {
    return await new Promise((resolve, reject) => {
      let finished = false;
      const fail = (e) => {
        if (finished) return;
        finished = true;
        clearInterval(poll);
        for (const s of snapshots.values()) s.rej(e);
        snapshots.clear();
        reject(e);
      };
      const poll = setInterval(() => api.stopped && fail(stopped()), 250);
      const run = (fn) => Promise.resolve().then(fn).catch(fail);
      // an uncaught error in the Brush worker (e.g. a wasm panic once the device is lost) can arrive
      // before its own 'failed' message, which names the cause: give that a moment first
      worker.onerror = (e) => {
        e.preventDefault?.();
        setTimeout(() => fail(new Error(`training worker: ${e.message}`)), 2000);
      };
      worker.onmessage = ({ data: m }) =>
        run(async () => {
          if (finished) return;
          if (m.type === 'progress' || m.type === 'refine') {
            if (m.splats != null) gaussians = m.splats;
            if (m.type === 'refine') return;
            if (m.iter > startIter) {
              trainingStarted = true;
              api.phase('train');
            }
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
          } else if (m.type === 'snapshotPly') {
            const s = snapshots.get(m.id);
            snapshots.delete(m.id);
            s?.res(m);
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
            doneIter = m.iter;
            trainingStarted = false; // no more snapshots: the run moves on to its results
            if (doneIter < total)
              api.log(`training stopped early at iteration ${doneIter} of ${total}`);
            api.progress(doneIter, total, { gaussians });
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
            const info = rgb
              ? {
                  kind: 'rgb',
                  up: [0, 0, 1],
                  target: frames.target_enu,
                  origin: frames.origin,
                  rasterize_mode: 'classic',
                }
              : trainInfo({
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
          } else if (m.type === 'failed' || m.type === 'failure') {
            fail(new Error(`training failed: ${m.reason}`));
          } else if (m.type === 'error') {
            // a snapshot refused because the run already failed: its 'failed' message names the cause
            if (
              /^(Error: )?(no snapshot: run is failed|run failed (before|during) a snapshot)/.test(
                m.message,
              )
            )
              return;
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
        stopAt,
        testFault: msg.env?.testFault?.train ?? null,
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
        iterations: doneIter,
        ...(doneIter < total ? { iterations_planned: total, stopped_early: true } : {}),
        eval: { dir: 'eval', views: evalViews },
        standin: false,
        trainer: TRAINER,
        brush_config: config,
        resumed,
      };
      await writeJson(`${msg.outDir}/train.json`, train);
      return {
        gaussians,
        iterations: doneIter,
        iterations_planned: total,
        stopped_early: doneIter < total,
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
