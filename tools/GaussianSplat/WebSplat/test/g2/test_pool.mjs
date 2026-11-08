// pool.js failure paths with fake workers: every failure rejects, terminates every worker created
// and leaves no message/error/abort listeners behind; no uncaught exceptions.
import assert from 'node:assert/strict';
import { verifyInPool, mapInWorker } from '../../app/src/stages/map/pool.js';

let uncaught = 0;
process.on('uncaughtException', () => uncaught++);
process.on('unhandledRejection', () => uncaught++);

const all = [];
class FakeWorker extends EventTarget {
  constructor(opts = {}) {
    super();
    this.opts = opts;
    this.listeners = 0;
    this.terminated = false;
    all.push(this);
  }
  addEventListener(t, f) {
    this.listeners++;
    super.addEventListener(t, f);
  }
  removeEventListener(t, f) {
    this.listeners--;
    super.removeEventListener(t, f);
  }
  terminate() {
    this.terminated = true;
  }
  reply(data) {
    setTimeout(
      () => !this.terminated && this.dispatchEvent(Object.assign(new Event('message'), { data })),
      1,
    );
  }
  postMessage(msg) {
    if (this.opts.postThrows) throw new DOMException('could not clone', 'DataCloneError');
    if (this.opts.errorEvent) {
      setTimeout(
        () => this.dispatchEvent(Object.assign(new Event('error'), { message: 'load failed' })),
        1,
      );
      return;
    }
    const id = msg.id;
    if (msg.type === 'verify-init') this.reply({ type: 'ready', id, loadMs: 1 });
    else if (msg.type === 'verify-batch')
      this.reply({
        type: 'batch',
        id,
        ms: 1,
        results: msg.pairs.map((e) => ({ i: e.i, j: e.j, config: 2, matches: new Uint32Array(0) })),
      });
    else if (msg.type === 'verify-end') this.reply({ type: 'done', id, heap: 1 });
    else if (msg.type === 'map') {
      this.reply({ type: 'progress', id, phase: 'ba', done: 1, total: 50, round: 0 });
      this.reply({ type: 'round', id, stats: { round: 0 } });
      setTimeout(() => this.reply({ type: 'done', id, rounds: [], files: {} }), 5);
    }
  }
}
const features = {
  keypoints: new Float32Array(12),
  counts: new Uint32Array([1, 1]),
  offsets: new Uint32Array([0, 1]),
};
const pairs = Array.from({ length: 200 }, (_, k) => ({
  i: 0,
  j: 1,
  status: 0,
  matches: new Uint32Array(0),
  k,
}));
const camera = { model: 'SIMPLE_RADIAL', width: 640, height: 512, params: [1, 2, 3, 0] };

function signalCounter() {
  const ac = new AbortController();
  let n = 0;
  const add = ac.signal.addEventListener.bind(ac.signal),
    rem = ac.signal.removeEventListener.bind(ac.signal);
  ac.signal.addEventListener = (...a) => (n++, add(...a));
  ac.signal.removeEventListener = (...a) => (n--, rem(...a));
  return { ac, count: () => n };
}

async function expectReject(name, p, re) {
  all.length = 0;
  const s = signalCounter();
  let err = null;
  try {
    await p(s.ac.signal, s.ac);
  } catch (e) {
    err = e;
  }
  await new Promise((r) => setTimeout(r, 30));
  assert.ok(err, `${name}: rejected`);
  if (re) assert.match(String(err.message), re, name);
  assert.ok(
    all.every((w) => w.terminated),
    `${name}: all ${all.length} workers terminated`,
  );
  assert.equal(
    all.reduce((t, w) => t + w.listeners, 0),
    0,
    `${name}: no worker listeners left`,
  );
  assert.equal(s.count(), 0, `${name}: no signal listeners left`);
  console.log(`ok ${name}: ${err.name} "${err.message}", ${all.length} workers`);
}

// sanity: success paths resolve and leave nothing behind
{
  all.length = 0;
  const r = await verifyInPool(() => new FakeWorker(), camera, features, pairs, {
    workers: 3,
    batch: 16,
  });
  assert.equal(r.results.length, 200);
  const m = await mapInWorker(() => new FakeWorker(), camera, [], features, []);
  assert.equal(m.type, 'done');
  assert.ok(all.every((w) => w.terminated && w.listeners === 0));
  console.log('ok success paths');
}
let made = 0;
await expectReject(
  'factory throws on the 2nd worker',
  (signal) => {
    made = 0;
    return verifyInPool(
      () =>
        ++made === 2
          ? (() => {
              throw new Error('no worker');
            })()
          : new FakeWorker(),
      camera,
      features,
      pairs,
      { workers: 3, signal },
    );
  },
  /no worker/,
);
await expectReject(
  'map factory throws',
  (signal) =>
    mapInWorker(
      () => {
        throw new Error('no worker');
      },
      camera,
      [],
      features,
      [],
      { signal },
    ),
  /no worker/,
);
await expectReject(
  'verify postMessage throws',
  (signal) =>
    verifyInPool(() => new FakeWorker({ postThrows: true }), camera, features, pairs, {
      workers: 2,
      signal,
    }),
  /clone/,
);
await expectReject(
  'map postMessage throws',
  (signal) =>
    mapInWorker(() => new FakeWorker({ postThrows: true }), camera, [], features, [], { signal }),
  /clone/,
);
await expectReject(
  'verify worker error event',
  (signal) =>
    verifyInPool(() => new FakeWorker({ errorEvent: true }), camera, features, pairs, {
      workers: 2,
      signal,
    }),
  /load failed/,
);
await expectReject(
  'verify onProgress throws',
  (signal) =>
    verifyInPool(() => new FakeWorker(), camera, features, pairs, {
      workers: 3,
      batch: 16,
      signal,
      onProgress: (done) => {
        if (done > 40) throw new Error('progress boom');
      },
    }),
  /progress boom/,
);
await expectReject(
  'map onProgress throws',
  (signal) =>
    mapInWorker(() => new FakeWorker(), camera, [], features, [], {
      signal,
      onProgress: () => {
        throw new Error('progress boom');
      },
    }),
  /progress boom/,
);
await expectReject(
  'map onRound throws',
  (signal) =>
    mapInWorker(() => new FakeWorker(), camera, [], features, [], {
      signal,
      onRound: () => {
        throw new Error('round boom');
      },
    }),
  /round boom/,
);
await expectReject(
  'verify abort mid-run',
  (signal, ac) =>
    verifyInPool(() => new FakeWorker(), camera, features, pairs, {
      workers: 2,
      batch: 8,
      signal,
      onProgress: (d) => d > 30 && ac.abort(),
    }),
  /stopped/,
);
assert.equal(uncaught, 0, 'no uncaught exceptions or unhandled rejections');
console.log('PASS pool.js failure paths');
