// Training preview, stop and accept, and the viewer's ceiling download.
// usage (WebSplat directory): node --test test/v2/*.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Controller } from '../../app/src/pipeline/controller.js';
import { defineStage } from '../../app/src/pipeline/registry.js';
import { previewPly } from '../../app/src/stages/train/preview.js';
import { wsspHeader } from '../../app/src/stages/train/params.js';
import { clipPly } from '../../../viewer/ply-clip.mjs';
import { Job } from '../../app/src/storage/job.js';
import { embedViewer } from '../../app/src/stages/export/embed.js';

const ident = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
const origin = {
  lat: -35.58,
  lon: 148.9,
  alt_amsl: 978.4,
  frame: 'ENU, up from takeoff altitude',
};

function dataset(kind) {
  const d = {
    format: 'websplat.dataset',
    version: 1,
    width: 640,
    height: 512,
    K: [
      [800, 0, 320],
      [0, 800, 256],
      [0, 0, 1],
    ],
    images: [
      {
        name: '00000.png',
        split: 'train',
        hot: true,
        viewmat: [...ident.slice(0, 11), 100, ...ident.slice(12)],
      },
      {
        name: '00001.png',
        split: 'train',
        hot: false,
        viewmat: [...ident.slice(0, 11), 90, ...ident.slice(12)],
      },
    ],
    bounds: { centre: [0, 0], radius: 100 },
    hotspot: kind === 'rgb' ? null : { enu: [1, 2, 3], frames: 5, median_ray_error_px: 1 },
  };
  if (kind === 'rgb') d.kind = 'rgb';
  else d.scale = { kind: 'log1p', t0_c: 10, t1_c: 120, k_c: 8 };
  return d;
}

const frames = {
  origin,
  target_enu: [0, 0, 30],
  frames: [
    { name: '00000.png', maximum_c: 90 },
    { name: '00001.png', maximum_c: 20 },
  ],
};

// a WSSP params.bin with n Gaussians of SH degree 1 at heights z[i]
function wssp(z) {
  const n = z.length;
  const floats = 3 + 3 + 4 + 1 + 3 + 9;
  const buf = new ArrayBuffer(16 + 4 * floats * n);
  new Uint32Array(buf, 0, 4).set([0x50535357, 1, n, 1]);
  const f = new Float32Array(buf, 16);
  for (let i = 0; i < n; i++) {
    f[3 * i] = i;
    f[3 * i + 2] = z[i];
    f[6 * n + 4 * i] = 1; // quaternion w
    f[14 * n + 3 * i] = 0.5; // sh0
  }
  return new Uint8Array(buf);
}

const headerOf = (bytes) => {
  const t = new TextDecoder('latin1').decode(bytes.subarray(0, 4096));
  return t.slice(0, t.indexOf('end_header'));
};
const apcg = (bytes) =>
  JSON.parse(
    headerOf(bytes)
      .split('\n')
      .find((l) => l.startsWith('comment apcg_splat '))
      .slice(19),
  );

test('a thermal preview is the export PLY form, with the temperature scale', async () => {
  const p = await previewPly({
    splat: wssp([1, 2, 3]),
    dataset: dataset('thermal'),
    frames,
  });
  assert.equal(p.count, 3);
  const info = apcg(p.bytes);
  assert.equal(info.kind, 'thermal');
  assert.deepEqual(info.temperature_scale, dataset('thermal').scale);
  assert.deepEqual(info.target, [1, 2, 3]); // the hot spot
  assert.match(headerOf(p.bytes), /element vertex 3\n/);
});

test('an RGB preview keeps colours and starts at the captured view', async () => {
  const view = {
    camera: { lat: -35.579, lon: 148.9, alt_amsl: 1100 },
    fov_deg: 50,
  };
  const p = await previewPly({
    splat: wssp([1, 2]),
    dataset: dataset('rgb'),
    frames,
    view,
    targetPolicy: 'picked',
    title: 'x (training preview)',
  });
  const info = apcg(p.bytes);
  assert.equal(info.kind, 'rgb');
  assert.equal(info.title, 'x (training preview)');
  assert.equal(info.camera.fov_deg, 50);
  assert.deepEqual(info.camera.target, [0, 0, 30]);
  assert.ok(info.camera.position[2] > 100);
});

test('clipPly drops the Gaussians above the ceiling and keeps the rest byte for byte', async () => {
  const p = await previewPly({
    splat: wssp([5, 50, 10, 200, 30]),
    dataset: dataset('thermal'),
    frames,
    title: 'Ünïcode',
  });
  const r = clipPly(p.bytes, [0, 0, 1], 40);
  assert.equal(r.kept, 3);
  assert.equal(r.removed, 2);
  const h = headerOf(r.bytes);
  assert.match(h, /element vertex 3\n/);
  const info = apcg(r.bytes);
  assert.equal(info.gaussians, 3);
  assert.equal(info.clipped_above, 40);
  assert.equal(info.title, 'Ünïcode');
  assert.match(headerOf(r.bytes), /^[\x20-\x7e\n]*$/, 'the header stays ASCII');
  const stride = (p.bytes.length - headerOf(p.bytes).length - 11) / 5;
  const body = (b, n) => b.subarray(b.length - n * stride);
  const rows = (b, n) =>
    Array.from({ length: n }, (_, i) => body(b, n).subarray(i * stride, (i + 1) * stride));
  const src = rows(p.bytes, 5);
  assert.deepEqual(rows(r.bytes, 3), [src[0], src[2], src[4]]);
  // another up axis: heights along +x (x = index)
  assert.equal(clipPly(p.bytes, [1, 0, 0], 2.5).kept, 3);
  assert.equal(clipPly(new TextEncoder().encode('not a ply'), [0, 0, 1], 1), null);
  // a header written as raw UTF-8 (other tools): read as UTF-8, written back escaped
  const enc = new TextEncoder();
  const raw = enc.encode(
    'ply\nformat binary_little_endian 1.0\ncomment apcg_splat {"title":"Ünï"}\n' +
      'element vertex 2\nproperty float x\nproperty float y\nproperty float z\nend_header\n',
  );
  const two = new Uint8Array(raw.length + 24);
  two.set(raw);
  new Float32Array(two.buffer, raw.length, 6).set([0, 0, 1, 0, 0, 9]);
  const c = clipPly(two, [0, 0, 1], 5);
  assert.equal(c.kept, 1);
  assert.equal(apcg(c.bytes).title, 'Ünï');
  assert.deepEqual([...new Float32Array(c.bytes.slice(c.bytes.length - 12).buffer)], [0, 0, 1]);
});

// a one-stage set and the Job methods runStage uses, in memory
class MemoryJob {
  constructor() {
    this.manifest = { source: { hash: 'sha256c:00' }, config: {}, stages: {} };
    this.saves = 0;
    this.dir = 'jobs/x';
    this.saving = Promise.resolve();
  }
  stage(id) {
    return (this.manifest.stages[id] ??= { state: 'waiting' });
  }
  durableStage(id) {
    return this.manifest.stages[id] ?? null;
  }
  async reset(id) {
    this.manifest.stages[id] = { state: 'waiting' };
  }
  async save() {
    this.saves++;
  }
  nextGeneration() {
    return 1;
  }
  path(id, kind, g) {
    return `${this.dir}/${id}/${kind}-${g}`;
  }
  async removeUnpublished() {}
  // the real write queue and copy-on-write transactions; write() is the test's hook
  enqueue(fn) {
    return Job.prototype.enqueue.call(this, fn);
  }
  transact(id, fn) {
    return Job.prototype.transact.call(this, id, fn);
  }
  fault() {}
  async write(m) {
    this.written = JSON.parse(JSON.stringify(m));
    await this.writeHook?.();
  }
  async publishOutputs(id, generation, fields) {
    Object.assign(this.stage(id), fields, {
      generation,
      outputsHash: 'sha256:out',
      checkpoint: null,
    });
  }
}

function trainSet(run) {
  const def = defineStage({
    id: 'train',
    title: 'Train',
    version: 1,
    unit: 'iterations',
    inputs: ['source'],
    configKeys: ['train'],
    estimate: () => ({ total: 1000, seconds: 10 }),
    run,
  });
  return { name: 'test', stages: [def] };
}

test('accepting a stopped stage needs a checkpoint and is passed to its next run', async () => {
  const job = new MemoryJob();
  let seen;
  const c = new Controller({
    set: trainSet(async (ctx) => {
      seen = ctx.accept;
      ctx.progress(401, 1000);
      return { iterations: 401, iterations_planned: 1000, stopped_early: true };
    }),
    job,
    config: {},
  });
  await assert.rejects(c.acceptEarly('train'), /no checkpoint/);
  Object.assign(job.stage('train'), {
    state: 'paused',
    version: 1,
    key: await c.key(c.stages[0]),
    checkpoint: { generation: 2, done: 400 },
  });
  await c.acceptEarly('train');
  assert.equal(job.stage('train').accept.iteration, 400);
  assert.equal(job.written?.stages.train.accept?.iteration, 400, 'written before it runs');
  await c.start();
  assert.equal(seen.iteration, 400);
  const r = job.stage('train');
  assert.equal(r.state, 'done');
  // the count reached, not the planned total
  assert.equal(r.progress.done, 401);
  assert.equal(r.result.stopped_early, true);
  // reload: a done stage under its key does not run again
  let runs = 0;
  const c2 = new Controller({
    set: trainSet(async () => runs++),
    job,
    config: {},
  });
  assert.deepEqual(await c2.validate(), []);
  await c2.start();
  assert.equal(runs, 0);
  await assert.rejects(c2.acceptEarly('train'), /already done/);
});

test('accepting a running stage asks its worker first, and a refusal records nothing', async () => {
  const job = new MemoryJob();
  const c = new Controller({
    set: trainSet(async () => ({})),
    job,
    config: {},
  });
  Object.assign(job.stage('train'), {
    state: 'running',
    progress: { done: 1234, total: 5000 },
  });
  const asked = [];
  c.channels.set('train', {
    request: async (name) => {
      asked.push(name);
      if (asked.length === 1) throw new Error('training has not started yet');
      return { requested: true };
    },
  });
  await assert.rejects(c.acceptEarly('train'), /not started/);
  assert.equal(job.stage('train').accept, undefined);
  await c.acceptEarly('train');
  assert.deepEqual(asked, ['finish', 'finish']);
  assert.equal(job.stage('train').accept.iteration, 1234);
});

test('accepting a resumed stage before its worker starts is refused, not lost', async () => {
  const job = new MemoryJob();
  const c = new Controller({ set: trainSet(async () => ({})), job, config: {} });
  // resumed from a checkpoint: running, but the run has read record.accept and has no worker yet
  Object.assign(job.stage('train'), {
    state: 'running',
    progress: { done: 400, total: 1000 },
    checkpoint: { generation: 2, done: 400 },
  });
  c.channels.set('train', {});
  await assert.rejects(c.acceptEarly('train'), /starting/);
  assert.equal(job.stage('train').accept, undefined);
});

test('a config change still invalidates an accepted stage and clears the acceptance', async () => {
  const job = new MemoryJob();
  const config = { train: { refineEvery: 100 } };
  const c = new Controller({ set: trainSet(async () => ({})), job, config });
  Object.assign(job.stage('train'), {
    state: 'done',
    version: 1,
    key: await c.key(c.stages[0]),
    outputsHash: 'sha256:out',
    accept: { iteration: 400 },
    result: { stopped_early: true },
  });
  assert.deepEqual(await c.validate(), []);
  config.train = { refineEvery: 200 };
  assert.deepEqual(await c.validate(), ['train']);
  assert.equal(job.stage('train').accept, undefined);
  assert.equal(job.stage('train').state, 'waiting');
});

test('wssp fixture is valid', () => {
  assert.equal(wsspHeader(wssp([1, 2])).count, 2);
});

test('an acceptance survives a checkpoint publication that was in flight', async () => {
  const job = new MemoryJob();
  const c = new Controller({ set: trainSet(async () => ({})), job, config: {} });
  Object.assign(job.stage('train'), { state: 'running', progress: { done: 900, total: 5000 } });
  c.channels.set('train', { request: async () => ({ requested: true }) });
  // a checkpoint publication whose manifest write is slow: it copied the record before the accept
  let release;
  job.writeHook = () => new Promise((r) => (release = r));
  const ckpt = job.transact('train', (rec) => {
    rec.checkpoint = { generation: 3, done: 800 };
  });
  await new Promise((r) => setTimeout(r, 0));
  const accepting = c.acceptEarly('train');
  await new Promise((r) => setTimeout(r, 0));
  job.writeHook = null;
  release();
  await ckpt;
  await accepting;
  const r = job.stage('train');
  assert.equal(r.checkpoint.done, 800);
  assert.equal(r.accept?.iteration, 900, 'the acceptance is kept');
  assert.equal(job.written.stages.train.accept?.iteration, 900, 'and written');
});

test('an acceptance without a checkpoint is dropped when the config changes', async () => {
  const job = new MemoryJob();
  const config = { train: { refineEvery: 100 } };
  const c = new Controller({ set: trainSet(async () => ({})), job, config });
  // an accepted run interrupted before its first checkpoint
  Object.assign(job.stage('train'), {
    state: 'interrupted',
    version: 1,
    key: await c.key(c.stages[0]),
    accept: { iteration: 400 },
  });
  assert.deepEqual(await c.validate(), []);
  assert.equal(job.stage('train').accept.iteration, 400, 'kept under the same key');
  config.train = { refineEvery: 200 };
  assert.deepEqual(await c.validate(), ['train']);
  assert.equal(job.stage('train').accept, undefined);
});

test('clipPly: a vertex declaration with tabs, and a UTF-8 title with a euro sign', () => {
  const enc = new TextEncoder();
  const head = enc.encode(
    'ply\nformat binary_little_endian 1.0\ncomment apcg_splat {"title":"5 \u20ac","gaussians":3}\n' +
      'comment other \u20ac\n  element\tvertex\t3\nproperty float x\nproperty float y\n' +
      'property float z\nend_header\n',
  );
  const body = new Float32Array([0, 0, 1, 0, 0, 9, 0, 0, 2]);
  const ply = new Uint8Array(head.length + body.byteLength);
  ply.set(head);
  ply.set(new Uint8Array(body.buffer), head.length);
  const r = clipPly(ply, [0, 0, 1], 5);
  assert.equal(r.kept, 2);
  const text = new TextDecoder().decode(r.bytes.subarray(0, r.bytes.length - 24));
  assert.match(text, /\nelement vertex 2\n/);
  assert.doesNotMatch(text, /vertex\t3/);
  assert.ok(text.includes('comment other \u20ac\n'), 'other lines are copied byte for byte');
  assert.deepEqual(apcg(r.bytes), { title: '5 \u20ac', gaussians: 2, clipped_above: 5 });
  assert.deepEqual(
    [...new Float32Array(r.bytes.slice(r.bytes.length - 24).buffer)],
    [0, 0, 1, 0, 0, 2],
  );
});

test('embedViewer: dispose rejects an open still waiting and removes the listener', async () => {
  const listeners = new Set();
  const saved = {
    window: globalThis.window,
    document: globalThis.document,
    location: globalThis.location,
  };
  let removed = false;
  globalThis.location = { href: 'http://127.0.0.1/websplat/' };
  globalThis.window = {
    addEventListener: (t, f) => listeners.add(f),
    removeEventListener: (t, f) => listeners.delete(f),
  };
  globalThis.document = {
    createElement: () => ({ remove: () => (removed = true), contentWindow: {} }),
  };
  try {
    const holder = { append() {} };
    const v = embedViewer(holder, 'vendor/splat-viewer/index.html');
    assert.equal(listeners.size, 1);
    const opening = v.open(new Uint8Array(1), 'x.ply'); // the viewer never says it is ready
    v.dispose();
    await assert.rejects(opening, /closed/);
    assert.equal(listeners.size, 0);
    assert.ok(removed);
  } finally {
    Object.assign(globalThis, saved);
  }
});
