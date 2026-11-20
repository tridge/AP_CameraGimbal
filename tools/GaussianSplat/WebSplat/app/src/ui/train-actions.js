// Training actions: Preview (the splat so far in the result viewer, from a snapshot of the running
// trainer or, when training is not running, its latest checkpoint) and Stop and accept (training
// ends with the iterations done so far; controller.acceptEarly).
import { embedViewer } from '../stages/export/embed.js';
import { assetURL } from '../asset-url.js';
import { readFile, readJson } from '../storage/opfs.js';
import { previewPly } from '../stages/train/preview.js';

const VIEWER = assetURL('vendor/splat-viewer/index.html');
const $ = (id) => document.getElementById(id);
const ACTIVE = ['running', 'checkpointing'];

// controller, job; resume(): starts the pipeline (main.js start); log(text, level)
export function trainActions({ controller, job, resume, log }) {
  let viewer = null;
  let epoch = 0; // bumped by close(): a preview still loading then stops
  let busy = false;
  const status = (t) => ($('train-preview-status').textContent = t);

  // what can be done now: 'running' (snapshot), 'checkpoint' (stopped, has one) or null
  function mode(snap) {
    const s = snap.stages.find((x) => x.id === 'train');
    if (!s || s.state === 'done') return null;
    if (ACTIVE.includes(s.state) && (s.progress?.done ?? 0) > 0 && s.phase !== 'eval')
      return 'running';
    if (!snap.running && s.checkpoint) return 'checkpoint';
    return null;
  }

  async function checkpointPly() {
    const r = controller.record('train');
    const dir = (id, kind, g) => job.path(id, kind, g);
    const ckpt = dir('train', 'ckpt', r.checkpoint.generation);
    const [params, dataset, frames] = await Promise.all([
      readFile(`${ckpt}/params.bin`).then(async (f) => new Uint8Array(await f.arrayBuffer())),
      readJson(`${dir('prepare', 'gen', controller.record('prepare').generation)}/dataset.json`),
      readJson(`${dir('select', 'gen', controller.record('select').generation)}/frames.json`),
    ]);
    const e = job.manifest.config.export ?? {};
    const p = await previewPly({
      splat: params,
      dataset,
      frames,
      view: e.view ?? null,
      targetPolicy: e.target ?? null,
      title: `${e.title ?? 'splat'} (checkpoint preview)`,
    });
    const total = dataset.training?.options?.iterations ?? null;
    return { bytes: p.bytes, iteration: r.checkpoint.done, total, gaussians: p.count };
  }

  async function preview() {
    if (busy) return;
    busy = true;
    $('train-preview').disabled = true;
    $('preview-refresh').disabled = true;
    const mine = epoch;
    try {
      const m = mode(controller.snapshot());
      if (!m) throw new Error('training is not running and has no checkpoint');
      status(m === 'running' ? 'Taking a snapshot of training…' : 'Reading the checkpoint…');
      const t0 = performance.now();
      const p =
        m === 'running' ? await controller.command('train', 'preview') : await checkpointPly();
      if (mine !== epoch) return; // closed meanwhile
      $('preview').hidden = false;
      if (!viewer) viewer = embedViewer($('preview-viewer'), VIEWER);
      status('Opening the viewer…');
      // a Blob: the viewer reads it, the page does not keep a copy
      await viewer.open(new Blob([p.bytes]), 'preview.ply');
      const what = m === 'running' ? 'iteration' : 'checkpoint at iteration';
      status(
        `${what} ${p.iteration.toLocaleString('en')}${p.total ? ` of ${p.total.toLocaleString('en')}` : ''}, ` +
          `${p.gaussians.toLocaleString('en')} Gaussians (${((performance.now() - t0) / 1000).toFixed(1)} s)` +
          (m === 'running' ? '; training continues: press Refresh for a newer one.' : '.'),
      );
      window.websplat.preview = {
        seq: (window.websplat.preview?.seq ?? 0) + 1,
        iteration: p.iteration,
        gaussians: p.gaussians,
        opened: true,
      };
    } catch (e) {
      if (mine !== epoch) return; // closed while loading: not a failure
      status(`Preview failed: ${e.message}`);
      log(`preview: ${e.message}`, 'warn');
    } finally {
      busy = false;
      update(controller.snapshot());
    }
  }

  // the viewer's WebGL context and splat go with its iframe (training shares the GPU)
  function close() {
    epoch++;
    viewer?.dispose(); // rejects an open still waiting, so busy clears
    viewer = null;
    $('preview').hidden = true;
    status('');
    if (window.websplat.preview) window.websplat.preview.opened = false;
  }

  async function accept() {
    const snap = controller.snapshot();
    const m = mode(snap);
    const s = snap.stages.find((x) => x.id === 'train');
    const at = m === 'running' ? s.progress.done : s.checkpoint?.done;
    const total = s.progress?.total;
    const ok = window.confirm(
      `Stop training now and use the splat as it is (iteration ${at}${total ? ` of ${total}` : ''})?\n\n` +
        'This cannot be undone, short of training again from the start.',
    );
    if (!ok) return;
    try {
      $('train-accept').disabled = true;
      await controller.acceptEarly('train');
      log(`training accepted at iteration ${at}`);
      if (m !== 'running') await resume();
    } catch (e) {
      log(`stop and accept: ${e.message}`, 'error');
    } finally {
      update(controller.snapshot());
    }
  }

  $('train-preview').onclick = preview;
  $('preview-refresh').onclick = preview;
  $('preview-close').onclick = close;
  $('train-accept').onclick = accept;

  function update(snap) {
    const m = mode(snap);
    const accepted = !!controller.record('train').accept;
    $('train-actions').hidden = !m && !viewer;
    $('train-preview').disabled = busy || !m;
    $('preview-refresh').disabled = busy || !m;
    $('train-accept').disabled = !m || accepted;
    $('train-accept').textContent = accepted ? 'Accepted: finishing…' : 'Stop and accept…';
    if (snap.stages.find((x) => x.id === 'train')?.state === 'done' && viewer) close();
  }
  return { update, preview, close };
}
