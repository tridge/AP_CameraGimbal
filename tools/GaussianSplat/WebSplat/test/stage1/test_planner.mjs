/** Batch planner: hard bounds and complete decode chains, on synthetic GOP layouts. */
import assert from 'node:assert/strict';
import { planBatches, BatchLimitError } from '../../app/src/stages/ingest/frame-decoder.js';

const layout = (n, gop, size = 1000) =>
  Array.from({ length: n }, (_, i) => ({ keyframe: i % gop === 0, size: size + (i % 7) * 10 }));
let seed = 1;
const rand = () => ((seed = Math.imul(seed ^ (seed >>> 15), 2246822519) + 12345) >>> 0) / 2 ** 32;

function check(frames, wanted, opts) {
  const batches = planBatches(frames, wanted, opts);
  const { batchFrames = 16, maxChainFrames = 64, batchBytes = 24 << 20 } = opts;
  const seen = [];
  for (const b of batches) {
    assert.ok(frames[b.packets[0]].keyframe, 'batch starts at a keyframe');
    for (let k = 1; k < b.packets.length; k++)
      assert.ok(b.packets[k] > b.packets[k - 1], 'packets ascending');
    const chained = b.wanted.some((w) => !frames[w].keyframe);
    assert.ok(
      b.packets.length <= (chained ? maxChainFrames : Math.min(batchFrames, maxChainFrames)),
      `batch of ${b.packets.length} packets`,
    );
    assert.ok(b.packets.reduce((n, i) => n + frames[i].size, 0) <= batchBytes, 'bytes bound');
    for (const w of b.wanted) {
      let first = w;
      while (!frames[first].keyframe) first--;
      // every packet of the chain is in the batch, in decode order without gaps
      const at = b.packets.indexOf(first);
      assert.ok(at >= 0, `keyframe ${first} of ${w} in batch`);
      for (let i = first; i <= w; i++) assert.equal(b.packets[at + i - first], i, `chain of ${w}`);
      seen.push(w);
    }
  }
  assert.deepEqual(
    seen,
    [...new Set(wanted)].sort((a, b) => a - b),
  );
  return batches;
}

// all keyframes: plain batches of batchFrames
let b = check(layout(201, 1), [...Array(201).keys()], {});
assert.equal(b.length, 13);
// the review's case: frame 199 of a 200-frame GOP is rejected before any read
assert.throws(() => planBatches(layout(200, 200), [199], {}), BatchLimitError);
// ...and allowed when the chain limit permits, as one bounded batch
b = check(layout(200, 200), [199], { maxChainFrames: 200 });
assert.equal(b[0].packets.length, 200);
// byte and decoded-size bounds
assert.throws(() => planBatches(layout(40, 8, 4000), [7], { batchBytes: 20000 }), BatchLimitError);
assert.throws(
  () => planBatches(layout(40, 8), [7], { frameBytes: 1 << 20, maxOutputBytes: 4 << 20 }),
  BatchLimitError,
);
check(layout(40, 8), [3, 4, 5], { frameBytes: 1 << 20, maxOutputBytes: 6 << 20 });
// a single frame larger than the output limit fits no batch
assert.throws(
  () => planBatches(layout(10, 1), [0], { frameBytes: 8 << 20, maxOutputBytes: 4 << 20 }),
  BatchLimitError,
);
// random layouts and requests
for (let t = 0; t < 2000; t++) {
  const n = 1 + Math.floor(rand() * 120);
  const frames = Array.from({ length: n }, (_, i) => ({
    keyframe: i === 0 || rand() < 0.15,
    size: 500 + Math.floor(rand() * 2000),
  }));
  const wanted = [...Array(n).keys()].filter(() => rand() < 0.4);
  const opts = {
    batchFrames: 1 + Math.floor(rand() * 10),
    maxChainFrames: 1 + Math.floor(rand() * 40),
    batchBytes: 20000 + Math.floor(rand() * 60000),
  };
  try {
    check(frames, wanted, opts);
  } catch (e) {
    if (!(e instanceof BatchLimitError)) throw e;
    // rejected only when some chain really exceeds a bound
    const over = wanted.some((w) => {
      let f = w;
      while (!frames[f].keyframe) f--;
      let bytes = 0;
      for (let i = f; i <= w; i++) bytes += frames[i].size;
      return w - f + 1 > opts.maxChainFrames || bytes > opts.batchBytes;
    });
    assert.ok(over, 'rejection is justified');
  }
}
console.log('planner: PASS (fixed cases and 2000 random layouts)');
