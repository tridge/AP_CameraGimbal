// Page controller: device check, job ownership, pipeline controller and stage list.
import { checkDevice } from './device/check.js';
import { formatDuration } from './pipeline/eta.js';
import { Controller } from './pipeline/controller.js';
import { stageSet, stageSets } from './stages/index.js';
import { Job, jobDir } from './storage/job.js';
import { JobLockedError } from './storage/lock.js';
import { ManifestError, loadManifest, saveManifest } from './storage/manifest.js';
import { hashBlob, hashJson } from './storage/hash.js';
import { list, readFile, requestPersistence, storageInfo, writeFile } from './storage/opfs.js';
import { renderStages } from './ui/stage-list.js';

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
  if (d.smoke?.warnings?.length)
    lines.push(`<span class="warn">${d.smoke.warnings.join('<br>')}</span>`);
  if (!d.ok) {
    lines.push(`<b>${d.reason}</b>`);
    if (d.smoke?.errors?.length) lines.push(d.smoke.errors.map((e) => `- ${e}`).join('<br>'));
    if (d.hint) lines.push(`<span class="hint">${d.hint}</span>`);
  }
  el.innerHTML = lines.join('<br>');
}

let controller = null;
let job = null;
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

let pending = false;
function update() {
  if (pending) return;
  pending = true;
  setTimeout(() => {
    pending = false;
    const snap = controller.snapshot();
    renderStages($('stages'), snap);
    const o = snap.overall;
    $('overall').textContent =
      `Elapsed ${formatDuration(o.elapsedMs)}` +
      (o.text === 'done' ? ', all stages done' : `, ${o.text} remaining`);
    $('start').disabled = snap.running || snap.stages.every((s) => s.state === 'done');
    $('start').textContent = snap.stages.some((s) =>
      ['interrupted', 'paused', 'failed'].includes(s.state),
    )
      ? 'Resume'
      : 'Start';
    $('stop').disabled = !snap.running;
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
  await controller.start();
  while (controller.needs === 'select') {
    const proposal = controller.results().ingest?.proposal;
    job.manifest.config.select = await confirmProposal(proposal);
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

// choose a new recording: its hash names the job, so the same file resumes the same job
async function chooseSource(name) {
  $('source').hidden = false;
  const file = await new Promise((r) => ($('file').onchange = () => r($('file').files[0])));
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
function confirmProposal(proposal) {
  const f = $('proposal-form');
  const p = proposal?.proposal ?? {};
  for (const k of ['lat', 'lon', 'target_alt', 'hfov']) f.elements[k].value = p[k] ?? '';
  f.elements.rotation.value = String(p.rotation ?? 0);
  $('proposal-notes').replaceChildren(
    ...[...(proposal?.notes ?? []), ...(proposal?.error ? [proposal.error] : [])].map((n) =>
      Object.assign(document.createElement('li'), { textContent: n }),
    ),
  );
  $('proposal').hidden = false;
  return new Promise((resolve) => {
    f.onsubmit = (e) => {
      e.preventDefault();
      const v = (k) => (f.elements[k].value === '' ? null : Number(f.elements[k].value));
      $('proposal').hidden = true;
      resolve({
        lat: v('lat'),
        lon: v('lon'),
        target_alt: v('target_alt'),
        rotation: v('rotation'),
        hfov: v('hfov'),
      });
    };
  });
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

async function main() {
  window.addEventListener('error', (e) => api.errors.push(String(e.message)));
  window.addEventListener('unhandledrejection', (e) =>
    api.errors.push(String(e.reason?.message ?? e.reason)),
  );
  const powerPreference = params.get('adapter') === 'low-power' ? 'low-power' : 'high-performance';
  const device = await checkDevice({ powerPreference, smoke: params.get('smoke') !== '0' });
  api.device = device;
  showDevice(device);

  const name = params.get('pipeline') ?? (device.ok ? 'thermal' : 'dummy');
  const set = stageSet(name);
  for (const s of stageSets())
    $('pipeline').add(new Option(s.title, s.name, false, s.name === name));
  $('pipeline').onchange = () => {
    params.set('pipeline', $('pipeline').value);
    params.delete('job');
    location.search = params.toString();
  };
  if (!set || (!device.ok && name !== 'dummy')) {
    api.ready = true;
    return;
  }
  const remembered = `websplat.job.${name}`;
  let sourceFile = null;
  try {
    if (name === 'dummy') {
      job = await Job.open(params.get('job') ?? 'dummy', {
        source: DUMMY_SOURCE,
        config: { dummySeed: num('seed', 0) },
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
      ({ job, file: sourceFile } = await chooseSource(name));
    } else if (name !== 'dummy') {
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
  if (name !== 'dummy') localStorage.setItem(remembered, job.id);
  // run-time knobs come from the URL; output-affecting config from the job
  const config = (job.manifest.config ??= {});
  Object.assign(config, { speed: num('speed', 1), ckptMs: num('ckptMs', 1500) });
  if (params.has('selectMode')) config.selectMode = params.get('selectMode'); // same output, any mode
  const env = {
    gpuClass: device.gpuClass ?? 'unknown',
    powerPreference,
    noGpu: params.get('gpu') === '0' || !device.ok,
    sourceSize: job.manifest.source?.size ?? 0,
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
    Job,
    hashJson,
    loadManifest,
    readFile,
    writeFile,
    list,
    saveManifest,
  };
  api.verify = () => verifyJob(job.id);
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
  api.ready = true;
  // autostart begins a fresh job only; an interrupted one waits for Resume
  const fresh = controller.snapshot().stages.every((s) => s.state === 'waiting');
  if (params.get('autostart') === '1' && fresh) start();
}
main().catch((e) => {
  console.error(e);
  api.errors.push(String(e?.message ?? e));
  api.ready = true;
  log(e?.message ?? String(e), 'error');
});
