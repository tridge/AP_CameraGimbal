// Page controller: device check, job ownership, pipeline controller and stage list.
import { checkDevice } from './device/check.js';
import { formatDuration } from './pipeline/eta.js';
import { Controller } from './pipeline/controller.js';
import { stageSet, stageSets } from './stages/index.js';
import { Job, jobDir } from './storage/job.js';
import { JobLockedError } from './storage/lock.js';
import { ManifestError, loadManifest, saveManifest } from './storage/manifest.js';
import { hashBlob, hashJson, stableStringify } from './storage/hash.js';
import {
  RGB_FRAME_BUDGET,
  RequestError,
  requestIdentity,
  requestKey,
  selectConfig,
  validNonce,
  validateRequest,
} from './request.js';
import {
  HandoverError,
  moveFile,
  receiveRequest,
  refuse,
  removeStaleIncoming,
  reply,
  withStagedCopy,
} from './handover.js';
import {
  exists,
  list,
  readFile,
  requestPersistence,
  storageInfo,
  writeFile,
} from './storage/opfs.js';
import { renderStages } from './ui/stage-list.js';
import { RGB_MAX_FEATURES } from './stages/features/sift.js';
import { showResult } from './ui/result.js';
import { trainActions } from './ui/train-actions.js';

const $ = (id) => document.getElementById(id);
const params = new URLSearchParams(location.search);
const num = (k, d) => (params.has(k) ? Number(params.get(k)) : d);

// test hooks for the harness (test/harness); also handy from the console
const api = (window.websplat = { ready: false, blocked: false, device: null, errors: [] });

function log(text, level = 'info') {
  const line = `${new Date().toISOString().slice(11, 19)} ${level === 'info' ? '' : level + ': '}${text}`;
  $('log').textContent = (line + '\n' + $('log').textContent).slice(0, 20000);
}

function showDevice(d) {
  const el = $('device');
  el.className = 'panel' + (d.ok ? '' : ' error');
  const a = d.adapter;
  const lines = [];
  if (a) {
    lines.push(
      `<b>GPU</b>: ${a.description || a.device || a.vendor || 'unknown'} (${a.vendor ?? '?'} ${a.architecture ?? ''}), ` +
        `${d.powerPreference} adapter, class ${d.gpuClass}`,
    );
    lines.push(
      `max buffer ${(d.limits.maxBufferSize / 2 ** 20) | 0} MiB, max storage binding ` +
        `${(d.limits.maxStorageBufferBindingSize / 2 ** 20) | 0} MiB` +
        (d.smoke ? `, smoke test ${d.smoke.ok ? 'passed' : 'FAILED'} in ${d.smoke.ms} ms` : ''),
    );
  }
  // self-test notes for developers (the stages already work around them)
  if (d.smoke?.warnings?.length) console.info('WebGPU self-test:', d.smoke.warnings.join('; '));
  if (!d.ok) {
    lines.push(`<b>${d.reason}</b>`);
    if (d.smoke?.errors?.length) lines.push(d.smoke.errors.map((e) => `- ${e}`).join('<br>'));
    if (d.hint) lines.push(`<span class="hint">${d.hint}</span>`);
  }
  el.innerHTML = lines.join('<br>');
}

let controller = null;
let job = null;
let actions = null;
let wakeLock = null;

async function wake(on) {
  try {
    if (on && !wakeLock && document.visibilityState === 'visible' && navigator.wakeLock) {
      wakeLock = await navigator.wakeLock.request('screen');
      wakeLock.addEventListener('release', () => (wakeLock = null));
    } else if (!on && wakeLock) {
      await wakeLock.release();
    }
  } catch {
    // not granted (e.g. headless); the hidden-tab warning still applies
  }
}

document.addEventListener('visibilitychange', () => {
  const hidden = document.visibilityState === 'hidden';
  $('hidden-warning').hidden = !(hidden && controller?.running);
  if (!hidden && controller?.running) wake(true);
});

async function showStorage() {
  const s = await storageInfo();
  const mb = (b) => (b == null ? '?' : `${(b / 2 ** 20).toFixed(0)} MiB`);
  $('storage').textContent =
    `Storage: ${mb(s.usage)} used of ${mb(s.quota)}; ` +
    (s.persisted ? 'persistent' : 'not persistent (the browser may evict it when space is short)');
  api.storage = s;
}

// what a stage waiting for the user asks of them (controller.needs)
const NEEDS = {
  select: {
    row: 'confirm the target and camera above, then press Continue',
    overall: 'confirm the target and camera',
  },
  train: { row: 'choose a trained splat above', overall: 'choose a trained splat' },
};

let pending = false;
function update() {
  if (pending) return;
  pending = true;
  setTimeout(() => {
    pending = false;
    const snap = controller.snapshot();
    const needs = !snap.running && snap.needs ? NEEDS[snap.needs] : null;
    renderStages($('stages'), snap, needs && { stage: snap.needs, text: needs.row });
    const o = snap.overall;
    $('overall').textContent =
      `Elapsed ${formatDuration(o.elapsedMs)}` +
      (o.text === 'done'
        ? ', all stages done'
        : needs
          ? `; waiting for you to ${needs.overall} (then ${o.text})`
          : `, ${o.text} remaining`);
    $('start').disabled = snap.running || snap.stages.every((s) => s.state === 'done');
    $('start').textContent = snap.stages.some((s) =>
      ['interrupted', 'paused', 'failed'].includes(s.state),
    )
      ? 'Resume'
      : 'Start';
    $('stop').disabled = !snap.running;
    actions?.update(snap);
    showRequestSummary();
  }, 100);
}

async function start() {
  const granted = await requestPersistence();
  if (job.manifest.storage?.persistRequested !== true) {
    job.manifest.storage = { persistRequested: true, persisted: granted };
    if (!granted)
      log('persistent storage was not granted; data may be evicted when space is short', 'warn');
  }
  showStorage();
  wake(true);
  log('started');
  await checkInputs();
  await controller.start();
  for (;;) {
    if (controller.needs === 'select') {
      const ingest = controller.results().ingest;
      const request = job.manifest.config.request;
      try {
        // a request job: the user chose the target in the viewer
        job.manifest.config.select = request
          ? selectConfig(request, ingest)
          : await confirmProposal(
              ingest?.proposal,
              ingest?.range,
              ingest?.kind === 'rgb' && ingest.track,
            );
      } catch (e) {
        if (!(e instanceof RequestError)) throw e;
        log(`cannot select frames: ${e.message}`, 'error');
        break;
      }
    } else if (controller.needs === 'train') {
      await chooseTrained();
    } else break;
    await job.save();
    await controller.start();
  }
  wake(false);
  const snap = controller.snapshot();
  const failed = snap.stages.find((s) => s.state === 'failed');
  if (snap.storageError) log(`storage: ${snap.storageError.message}`, 'error');
  if (failed) log(`${failed.title} failed: ${failed.error.message}`, 'error');
  else if (snap.stages.every((s) => s.state === 'done')) log('all stages done');
  else log('stopped');
  showStorage();
  await result();
}

// the result view once the export stage is done
async function result() {
  const r = job.manifest.stages?.export;
  if (r?.state !== 'done') return;
  const res = await showResult($('result'), r, controller.snapshot(), job.manifest.stages?.map);
  api.result = {
    meta: res.meta,
    sizes: Object.fromEntries(Object.entries(res.files).map(([k, f]) => [k, f.size])),
  };
  await res.opened;
  api.result.opened = true;
}

// stand-in inputs the page copied into storage must still be there; a missing one is asked for
// again (the train stand-in then waits for a file)
async function checkInputs() {
  const st = job.manifest.config.standin ?? {};
  for (const [id, v] of Object.entries(st)) {
    if (job.manifest.stages?.[id]?.state === 'done') continue; // loaded already
    const urls = Object.values(v.files ?? {}).filter((u) => u.startsWith('opfs:'));
    for (const u of urls) {
      if (await exists(u.slice(5))) continue;
      log(`${id}: ${u.slice(5)} is no longer in browser storage; choose it again`, 'warn');
      delete st[id];
      await job.save();
      break;
    }
  }
}

// stage 6 stand-in: a trained splat chosen by the user, copied into the job's storage
async function chooseTrained() {
  $('trained').hidden = false;
  const file = await new Promise(
    (r) => ($('trained-file').onchange = () => r($('trained-file').files[0])),
  );
  const name = file.name.endsWith('.bin') ? 'params.bin' : 'splat.ply';
  log(`copying ${file.name} into browser storage`);
  // under source/, which recovery keeps (stage directories lose unpublished files)
  const path = `${job.dir}/source/inputs/${name}`;
  await writeFile(path, file);
  const config = job.manifest.config;
  config.standin = { ...config.standin, train: { files: { [name]: `opfs:${path}` } } };
  $('trained').hidden = true;
}

const DUMMY_SOURCE = {
  format: 'websplat.source',
  version: 1,
  name: 'dummy',
  size: 0,
  hash: 'sha256:dummy',
  copied: false,
};

// the job's source File: the OPFS copy, or the user's file after checking it is the same one
async function sourceFor(job) {
  const src = job.manifest.source;
  if (src.copied) return readFile(`${job.dir}/source/source.mkv`);
  $('source').hidden = false;
  $('copy').closest('label').hidden = true;
  $('source-status').textContent =
    `To resume, select ${src.name} again (it was not copied into browser storage).`;
  for (;;) {
    const file = await new Promise((r) => ($('file').onchange = () => r($('file').files[0])));
    $('source-status').textContent = 'checking…';
    if (file.size === src.size && (await hashBlob(file, { chunkedAlways: true })) === src.hash) {
      $('source').hidden = true;
      return file;
    }
    $('source-status').textContent =
      `${file.name} is not the recording this job was made from; select ${src.name}.`;
  }
}

// a job id names its pipeline: rgb-<hash>... for RGB recordings, else thermal
const pipelineOf = (jobId) => (jobId.startsWith('rgb-') ? 'rgb' : 'thermal');
const RGB_FILE = /\.(ts|m2ts|mts|mp4|m4v|mov)$/i;

// choose a new recording: its hash names the job, so the same file resumes the same job; an
// MPEG-TS or MP4 makes an RGB splat, an MKV a thermal one
async function chooseSource() {
  $('source').hidden = false;
  const file = await new Promise((r) => ($('file').onchange = () => r($('file').files[0])));
  const name = RGB_FILE.test(file.name) ? 'rgb' : 'thermal';
  $('source-status').textContent = `reading ${file.name}…`;
  const hash = await hashBlob(file, { chunkedAlways: true });
  const source = {
    format: 'websplat.source',
    version: 1,
    name: file.name,
    size: file.size,
    lastModified: file.lastModified,
    hash,
    copied: false,
  };
  const jobId = `${name}-${hash.split(':')[1].slice(0, 16)}`;
  const opened = await Job.open(jobId, { source, config: {} });
  if (!opened.manifest.source.copied && $('copy').checked) {
    $('source-status').textContent = `copying ${file.name} into browser storage…`;
    await writeFile(`${opened.dir}/source/source.mkv`, file);
    opened.manifest.source.copied = true;
    await opened.save();
  }
  $('source').hidden = true;
  return {
    job: opened,
    file: opened.manifest.source.copied ? await readFile(`${opened.dir}/source/source.mkv`) : file,
  };
}

// the selection options form, filled from ingest's proposal; resolves when the user continues
// (range: ingest's resolved request range, kept as start/end; rgb: the RGB track, which adds
// the processing width and drops rotation)
function confirmProposal(proposal, range = null, rgb = null) {
  const f = $('proposal-form');
  const p = proposal?.proposal ?? {};
  for (const k of ['lat', 'lon', 'target_alt', 'hfov']) f.elements[k].value = p[k] ?? '';
  f.elements.rotation.value = String(p.rotation ?? 0);
  $('rotation-label').hidden = !!rgb;
  $('width-label').hidden = !rgb;
  if (rgb) {
    f.elements.width.max = rgb.width;
    f.elements.width.value = Math.min(1280, rgb.width);
  }
  $('proposal-notes').replaceChildren(
    ...[...(proposal?.notes ?? []), ...(proposal?.error ? [proposal.error] : [])].map((n) =>
      Object.assign(document.createElement('li'), { textContent: n }),
    ),
  );
  $('proposal').hidden = false;
  $('proposal').scrollIntoView({ block: 'nearest', behavior: 'smooth' });
  f.elements.lat.focus({ preventScroll: true });
  update();
  return new Promise((resolve) => {
    f.onsubmit = (e) => {
      e.preventDefault();
      const v = (k) => (f.elements[k].value === '' ? null : Number(f.elements[k].value));
      $('proposal').hidden = true;
      const r = range && { start: range.start, end: range.end };
      if (rgb)
        return resolve({
          lat: v('lat'),
          lon: v('lon'),
          target_alt: v('target_alt') ?? 0,
          hfov: v('hfov'),
          width: v('width'),
          budget: RGB_FRAME_BUDGET,
          ...r,
        });
      resolve({
        lat: v('lat'),
        lon: v('lon'),
        target_alt: v('target_alt'),
        rotation: v('rotation'),
        hfov: v('hfov'),
        ...r,
      });
    };
  });
}

// ?request=<nonce>: the video viewer's Create splat hands over a request and its recording.
// Returns {job, file}, or null after showing why not (the page then offers a file chooser).
async function fromViewer(nonce, remembered) {
  api.request = { state: 'waiting' };
  $('request').hidden = false;
  const status = (t) => ($('request-status').textContent = t);
  status('Waiting for the video viewer…');
  let msg;
  try {
    if (!validNonce(nonce)) throw new HandoverError('the request id in the address is not valid');
    msg = await receiveRequest(nonce, { waitMs: num('requestWaitMs', 30000) });
  } catch (e) {
    if (!(e instanceof HandoverError)) throw e;
    return handoverFailed(e.message, remembered);
  }
  const answer = (type, extra) => reply(msg.origin, { type, nonce, ...extra });
  try {
    const request = validateRequest(msg.data.request);
    const file = msg.data.file;
    if (!(file instanceof File)) throw new RequestError('no recording was handed over');
    if (file.name !== request.source.name || file.size !== request.source.size)
      throw new RequestError(
        `the file handed over (${file.name}, ${file.size} bytes) is not the request's ` +
          `${request.source.name} (${request.source.size} bytes)`,
      );
    api.request = { state: 'copying' };
    await removeStaleIncoming();
    const opened = await withStagedCopy(
      file,
      nonce,
      async (staged) => {
        const identity = requestIdentity(request);
        const jobId = `${request.kind}-${staged.hash.split(':')[1].slice(0, 16)}-${await requestKey(request)}`;
        const title = request.title ?? file.name.replace(/\.[^.]*$/, '');
        const exportConfig = { title };
        if (request.view) exportConfig.view = request.view;
        if (!request.target.refine) exportConfig.target = 'picked';
        const proposal = { range: identity.range };
        if (request.hfov != null) proposal.hfov = request.hfov;
        const source = {
          format: 'websplat.source',
          version: 1,
          name: file.name,
          size: file.size,
          lastModified: file.lastModified,
          hash: staged.hash,
          copied: false,
        };
        const config = {
          request: { ...identity, source: { name: file.name, size: file.size } },
          proposal,
          export: exportConfig,
        };
        const existed = await exists(jobDir(jobId));
        const j = await Job.open(jobId, { source, config });
        try {
          const { source: _, ...stored } = j.manifest.config?.request ?? {};
          if (stableStringify(stored) !== stableStringify(identity))
            throw new RequestError(
              `job ${jobId} already exists for a different request; delete it or change the request`,
            );
          if (!j.manifest.source.copied) {
            status(`saving ${file.name}…`);
            await moveFile(staged.path, `${j.dir}/source/source.mkv`);
            j.manifest.source.copied = true;
          }
          // only the title may differ on reopen (re-runs export alone)
          j.manifest.config.export = { ...j.manifest.config.export, title };
          await j.save();
          return j;
        } catch (e) {
          // a job made for this handover is removed again (no half-copied job is left)
          if (existed) j.close();
          else await j.destroy().catch(() => j.close());
          throw e;
        }
      },
      {
        onProgress: (done, total) =>
          status(`copying ${file.name} into browser storage: ${((100 * done) / total) | 0}%`),
      },
    );
    params.delete('request');
    params.set('job', opened.id);
    history.replaceState(null, '', `?${params}`);
    answer('websplat.accepted', { job: opened.id });
    api.request = { state: 'accepted', job: opened.id };
    $('request').hidden = true;
    return { job: opened, file: await readFile(`${opened.dir}/source/source.mkv`) };
  } catch (e) {
    answer('websplat.rejected', { reason: e.message });
    if (e instanceof JobLockedError || e instanceof ManifestError) {
      $('request').hidden = true;
      api.request = { state: 'rejected', error: e.message };
      throw e;
    }
    return handoverFailed(e.message, remembered);
  }
}

function handoverFailed(reason, remembered) {
  api.request = { state: 'failed', error: reason };
  $('request-status').textContent =
    `Could not take over the splat request (${reason}). Open this from the video viewer's ` +
    'Create splat again, or choose a recording below.';
  const latest = localStorage.getItem(remembered);
  $('request-latest').hidden = !latest;
  $('request-latest').onclick = () => {
    params.delete('request');
    params.set('job', latest);
    location.search = params.toString();
  };
  return null;
}

// a request job's summary under the job header
function showRequestSummary() {
  const r = job?.manifest.config?.request;
  const el = $('request-summary');
  el.hidden = !r;
  if (!r) return;
  const range = job.manifest.stages?.ingest?.result?.range;
  const secs = (r.range.end.pts - r.range.start.pts) / r.range.timescale;
  const t = r.target;
  el.textContent =
    `From the video viewer: ${secs.toFixed(1)} s of video` +
    (range ? ` (frames ${range.start}..${range.end}, ${range.frames} frames)` : '') +
    `; target ${t.lat.toFixed(6)}, ${t.lon.toFixed(6)} at ${t.alt_amsl.toFixed(1)} m AMSL` +
    (t.refine ? ' (refined to the hot spot)' : ' (as picked)') +
    `; start view ${r.view ? 'captured' : r.kind === 'rgb' ? 'default (the middle view)' : 'default (the hottest view)'}` +
    (r.rgb ? `; processing width ${r.rgb.width} px.` : '.');
}

// re-reads the job through the production validator and re-hashes every published file
async function verifyJob(jobId) {
  const found = await loadManifest(jobDir(jobId));
  if (!found) return { ok: false, error: 'no manifest' };
  const bad = [];
  let files = 0;
  for (const [id, st] of Object.entries(found.manifest.stages)) {
    const sets = [st.outputs ?? {}, st.checkpoint?.outputs ?? {}];
    for (const outputs of sets) {
      for (const [name, o] of Object.entries(outputs)) {
        files++;
        const f = await readFile(o.path).catch(() => null);
        if (!f || f.size !== o.bytes || (await hashBlob(f)) !== o.hash) bad.push(`${id}/${name}`);
      }
    }
  }
  return {
    ok: bad.length === 0,
    seq: found.manifest.seq,
    slot: found.slot,
    files,
    bad,
    manifest: found.manifest,
  };
}

const snapDone = () => controller.snapshot().stages.every((s) => s.state === 'done');

async function main() {
  window.addEventListener('error', (e) => api.errors.push(String(e.message)));
  window.addEventListener('unhandledrejection', (e) =>
    api.errors.push(String(e.reason?.message ?? e.reason)),
  );
  // tests only: storage faults in the page itself (?testFault= applies to the workers)
  if (params.get('pageFault')) globalThis.websplatTestFault = JSON.parse(params.get('pageFault'));
  const powerPreference = params.get('adapter') === 'low-power' ? 'low-power' : 'high-performance';
  const device = await checkDevice({ powerPreference, smoke: params.get('smoke') !== '0' });
  api.device = device;
  showDevice(device);

  // test and development pipelines are only offered when the URL names one
  let name = params.get('pipeline') ?? 'thermal';
  $('pipeline').closest('label').hidden = !params.has('pipeline');
  let set = stageSet(name);
  const noSource = name === 'dummy' || set?.source === false;
  for (const s of stageSets())
    $('pipeline').add(new Option(s.title, s.name, false, s.name === name));
  $('pipeline').onchange = () => {
    params.set('pipeline', $('pipeline').value);
    params.delete('job');
    location.search = params.toString();
  };
  // the export-test set needs no WebGPU (the viewer is WebGL)
  const nonce = noSource ? null : params.get('request');
  if (!set || (!device.ok && !noSource)) {
    if (nonce) refuse(nonce, device.reason ?? 'this browser cannot run WebSplat');
    api.ready = true;
    return;
  }
  // thermal and RGB jobs share the page's remembered job (the job id names its pipeline)
  const remembered = `websplat.job.${noSource ? name : 'thermal'}`;
  let sourceFile = null;
  let handedOver = false;
  try {
    if (nonce) {
      // ?request= takes precedence over ?job= and the remembered job
      ({ job, file: sourceFile } = (await fromViewer(nonce, remembered)) ?? {});
      handedOver = !!job;
    } else if (name === 'dummy') {
      job = await Job.open(params.get('job') ?? 'dummy', {
        source: DUMMY_SOURCE,
        config: { dummySeed: num('seed', 0) },
      });
    } else if (noSource) {
      job = await Job.open(params.get('job') ?? name, {
        source: { ...DUMMY_SOURCE, name, hash: `sha256:${name}` },
        config: {},
      });
    } else if (params.get('job') ?? localStorage.getItem(remembered)) {
      job = await Job.open(params.get('job') ?? localStorage.getItem(remembered));
      if (job.missing) {
        job.close(); // nothing stored under that id: start a new job
        job = null;
      }
    }
    if (!job) {
      api.ready = true; // waiting for a file
      ({ job, file: sourceFile } = await chooseSource());
      if (params.has('request')) {
        // a failed handover: this job is the user's own choice now
        params.delete('request');
        params.set('job', job.id);
        history.replaceState(null, '', `?${params}`);
        $('request').hidden = true;
      }
    } else if (!noSource && !sourceFile) {
      api.ready = true;
      sourceFile = await sourceFor(job);
    }
  } catch (e) {
    if (e instanceof ManifestError) {
      // keep the data: the user decides whether to delete it
      const id = e.jobId;
      $('unreadable-text').textContent = `${e.message}. Its data has been left as it is.`;
      $('unreadable').hidden = false;
      $('discard').onclick = async () => {
        await Job.discard(id);
        localStorage.removeItem(remembered);
        location.reload();
      };
      api.unreadable = { kind: e.kind, message: e.message };
      api.ready = true;
      return;
    }
    if (!(e instanceof JobLockedError)) throw e;
    $('blocked').hidden = false;
    api.blocked = true;
    api.ready = true;
    return;
  }
  api.ready = false;
  if (!noSource) {
    name = pipelineOf(job.id);
    set = stageSet(name);
    localStorage.setItem(remembered, job.id);
  }
  // run-time knobs come from the URL; output-affecting config from the job
  const config = (job.manifest.config ??= {});
  Object.assign(config, { speed: num('speed', 1), ckptMs: num('ckptMs', 1500) });
  if (params.has('selectMode')) config.selectMode = params.get('selectMode'); // same output, any mode
  // stand-in inputs (stage 6 and the export-test set): ?standin=URL of {id: {files: {name: url}}}
  if (params.get('standin')) {
    // relative to the page's URL, not the release <base>
    const r = await fetch(new URL(params.get('standin'), location.href));
    if (!r.ok) throw new Error(`standin: HTTP ${r.status}`);
    const urls = await r.json();
    for (const v of Object.values(urls)) {
      for (const [k, u] of Object.entries(v.files)) v.files[k] = new URL(u, r.url).href;
    }
    config.standin = { ...config.standin, ...urls };
  }
  // RGB frames are feature-rich: fewer keypoints keep mapping inside its 4 GB wasm heap
  if (name === 'rgb') config.features ??= { maxNumFeatures: RGB_MAX_FEATURES };
  config.export ??= {
    title: params.get('title') ?? (job.manifest.source?.name ?? 'splat').replace(/\.[^.]*$/, ''),
  };
  const env = {
    gpuClass: device.gpuClass ?? 'unknown',
    powerPreference,
    noGpu: params.get('gpu') === '0' || !device.ok,
    sourceSize: job.manifest.source?.size ?? 0,
    // recovery tests only: injected faults (storage/opfs.js, stages/train/brush-worker.js)
    testFault: params.get('testFault') ? JSON.parse(params.get('testFault')) : null,
  };
  controller = new Controller({ set, job, config, env, sourceFile });
  controller.addEventListener('update', update);
  controller.addEventListener('log', (e) =>
    log(`${e.detail.stage}: ${e.detail.text}`, e.detail.level),
  );
  api.snapshot = () => controller.snapshot();
  api.events = () => controller.events;
  api.start = () => start();
  api.stop = () => controller.stop();
  api.job = () => job.manifest;
  api.shutdown = () => controller.shutdown();
  // storage internals for the harness's fault and recovery tests
  api.testing = {
    job: () => job,
    controller: () => controller, // its 'update' events (stage 8 recovery tests)
    Job,
    hashJson,
    loadManifest,
    readFile,
    writeFile,
    list,
    saveManifest,
  };
  api.verify = () => verifyJob(job.id);
  if (set.stages.some((d) => d.id === 'train') && !config.standin?.train)
    actions = trainActions({ controller, job, resume: () => start(), log });
  api.trainPreview = () => actions?.preview();
  api.acceptTraining = () => controller.acceptEarly('train');
  $('job').hidden = false;
  $('stages-panel').hidden = false;
  $('start').onclick = () => start();
  $('stop').onclick = () => controller.stop();
  $('reset').onclick = async () => {
    await controller.shutdown(); // no work, publication or manifest write left in flight
    await job.destroy();
    localStorage.removeItem(remembered);
    location.reload();
  };
  await controller.validate();
  showStorage();
  update();
  const interrupted = controller.snapshot().stages.filter((s) => s.state === 'interrupted');
  if (interrupted.length)
    log(`interrupted: ${interrupted.map((s) => s.title).join(', ')}; press Resume`);
  await result().catch((e) => log(`result: ${e.message}`, 'error'));
  api.ready = true;
  // autostart begins a fresh job only; an interrupted one waits for Resume
  const fresh = controller.snapshot().stages.every((s) => s.state === 'waiting');
  if (params.get('autostart') === '1' && fresh) start();
  // a handed-over request starts (or resumes) at once: the user pressed Go in the viewer
  else if (handedOver && params.get('autostart') !== '0' && !snapDone()) start();
}
main().catch((e) => {
  console.error(e);
  api.errors.push(String(e?.message ?? e));
  api.ready = true;
  log(e?.message ?? String(e), 'error');
});
