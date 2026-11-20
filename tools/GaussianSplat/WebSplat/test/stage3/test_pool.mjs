// features/pool.js failure paths with fake workers: a worker failing at startup while the pool
// awaits an image read rejects (no hang) and terminates every worker; the worker error is the
// result; listeners are removed; a throwing onImage and Stop also settle everything.
import assert from 'node:assert/strict';
import { siftInPool } from '../../app/src/stages/features/pool.js';

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
    if (opts.loadFails)
      setTimeout(
        () =>
          this.dispatchEvent(
            Object.assign(new Event('error'), { message: 'module failed to load' }),
          ),
        5,
      );
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
  postMessage(m) {
    if (this.opts.loadFails) return; // never answers
    setTimeout(
      () =>
        !this.terminated &&
        this.dispatchEvent(
          Object.assign(new Event('message'), {
            data: {
              type: 'done',
              id: m.id,
              k: m.k,
              keypoints: new Float32Array(6),
              descriptors: new Uint8Array(128),
              ms: 1,
              heap: 1,
            },
          }),
        ),
      1,
    );
  }
}
const slowImage = () => new Promise((r) => setTimeout(() => r(new Uint8Array(4)), 50));
async function expectReject(name, p, re) {
  all.length = 0;
  const t = Date.now();
  const r = await Promise.race([
    p().then(
      () => 'resolved',
      (e) => e,
    ),
    new Promise((r) => setTimeout(() => r('pending'), 3000)),
  ]);
  await new Promise((r) => setTimeout(r, 100));
  assert.ok(r instanceof Error, `${name}: rejected (got ${r})`);
  if (re) assert.match(r.message, re, name);
  assert.ok(
    all.every((w) => w.terminated),
    `${name}: all workers terminated`,
  );
  assert.equal(
    all.reduce((s, w) => s + w.listeners, 0),
    0,
    `${name}: listeners removed`,
  );
  console.log(`ok ${name}: ${r.name} "${r.message}" after ${Date.now() - t} ms`);
}
{
  const r = await siftInPool(() => new FakeWorker(), 10, slowImage, 2, 2, { workers: 3 });
  assert.equal(r.images.length, 10);
  console.log('ok success path');
}
await expectReject(
  'startup failure during an image read',
  () => siftInPool(() => new FakeWorker({ loadFails: true }), 10, slowImage, 2, 2, { workers: 3 }),
  /failed to load/,
);
let made = 0;
await expectReject(
  'one of three workers fails at startup',
  () => {
    made = 0;
    return siftInPool(() => new FakeWorker({ loadFails: ++made === 2 }), 30, slowImage, 2, 2, {
      workers: 3,
    });
  },
  /failed to load/,
);
await expectReject(
  'onImage throws',
  () =>
    siftInPool(() => new FakeWorker(), 10, slowImage, 2, 2, {
      workers: 2,
      onImage: () => {
        throw new Error('boom');
      },
    }),
  /boom/,
);
const ac = new AbortController();
await expectReject(
  'Stop',
  () =>
    siftInPool(() => new FakeWorker(), 50, slowImage, 2, 2, {
      workers: 2,
      signal: ac.signal,
      onImage: (k, r, done) => done === 3 && ac.abort(),
    }),
  /stopped/,
);
assert.equal(uncaught, 0);
console.log('PASS features/pool.js failure paths');
