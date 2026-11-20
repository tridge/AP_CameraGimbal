// Stage 5 / G2 module worker: one colmap-wasm fork instance. Every message carries the
// caller's run id.
// in:  {type: 'verify-init', id, camera, keypoints: Float32Array, counts, offsets, seed, legacy}
//      {type: 'verify-batch', id, pairs: [decodeMatches() entries]}
//      {type: 'verify-end', id}
//      {type: 'map', id, camera, frames, keypoints, counts, offsets, geometry, opts}
//      {type: 'cancel', id}   honoured between pairs; a mapping round runs to its end. pool.js
//                             stops by terminating the worker instead (opts.signal)
// out: {type: 'ready', id, loadMs, heap}  {type: 'batch', id, results, ms, heap}
//      {type: 'progress', id, phase, done, total, round}  {type: 'round', id, stats}
//      {type: 'done', id, rounds, files, ms, heap}  {type: 'error', id, name, message}
import { loadColmap } from './colmap.js';
import { createVerifier } from './verify.js';
import { priorMap } from './priormap.js';

let cw = null,
  loadMs = 0,
  verifier = null;
const cancelled = new Set();

async function colmap() {
  if (!cw) {
    const t = performance.now();
    cw = await loadColmap({ onLog: (text, level) => level === 'warn' && console.warn(text) });
    loadMs = performance.now() - t;
  }
  return cw;
}

const yieldNow = () => new Promise((r) => setTimeout(r, 0));
const abortError = (what) => Object.assign(new Error(`${what} stopped`), { name: 'AbortError' });

async function handle(msg) {
  const id = msg.id;
  const post = (m, transfer) => self.postMessage({ ...m, id }, transfer ?? []);
  if (msg.type === 'verify-init') {
    const c = await colmap();
    if (verifier) verifier.end();
    const features = { keypoints: msg.keypoints, counts: msg.counts, offsets: msg.offsets };
    verifier = createVerifier(c, msg.camera, features, { seed: msg.seed, legacy: msg.legacy });
    post({ type: 'ready', loadMs, heap: c.heapBytes });
  } else if (msg.type === 'verify-batch') {
    const t = performance.now();
    const results = [];
    for (let k = 0; k < msg.pairs.length; k++) {
      if (k % 8 === 0) {
        await yieldNow();
        if (cancelled.has(id)) throw abortError('verification');
      }
      results.push(verifier.pair(msg.pairs[k]));
    }
    const transfer = results.flatMap((r) => [r.matches.buffer, r.model.buffer]);
    post({ type: 'batch', results, ms: performance.now() - t, heap: cw.heapBytes }, transfer);
  } else if (msg.type === 'verify-end') {
    if (verifier) verifier.end();
    verifier = null;
    post({ type: 'done', heap: cw ? cw.heapBytes : 0 });
  } else if (msg.type === 'map') {
    const c = await colmap();
    const t = performance.now();
    const features = { keypoints: msg.keypoints, counts: msg.counts, offsets: msg.offsets };
    let lastPost = 0;
    const res = await priorMap(c, msg.camera, msg.frames, features, msg.geometry, {
      ...msg.opts,
      onProgress: (phase, done, total, round) => {
        const now = performance.now();
        if (now - lastPost > 200 || done === total) {
          lastPost = now;
          post({ type: 'progress', phase, done, total, round });
        }
        return false;
      },
      onRound: (stats) => post({ type: 'round', stats }),
    });
    post(
      {
        type: 'done',
        rounds: res.rounds,
        files: res.files,
        ms: performance.now() - t,
        loadMs,
        heap: c.heapBytes,
      },
      Object.values(res.files).map((f) => f.buffer),
    );
  }
}

self.onmessage = async ({ data: msg }) => {
  if (msg.type === 'cancel') {
    cancelled.add(msg.id);
    return;
  }
  try {
    await handle(msg);
  } catch (e) {
    self.postMessage({ type: 'error', id: msg.id, name: e.name, message: e.message });
  }
};
