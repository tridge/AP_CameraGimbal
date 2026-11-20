// SIFT over a pool of sift.worker.js instances, one image per message to whichever worker is
// free. opts.signal terminates the workers at once and rejects with AbortError; any failure
// (worker, callback) settles everything and terminates every worker.
import { call, abortError } from '../map/pool.js';

let nextId = 1;

// image(k) -> Uint8Array(width * height) (may be async; transferred to the worker).
// opts: {workers, maxNumFeatures, signal, onImage(k, result, done)}. Returns [{keypoints,
// descriptors}] in image order unless opts.keep is false (onImage then owns the results).
export async function siftInPool(makeWorker, n, image, width, height, opts = {}) {
  if (opts.signal?.aborted) throw abortError('feature extraction');
  const local = new AbortController();
  const onAbort = () => local.abort();
  opts.signal?.addEventListener('abort', onAbort);
  const signal = local.signal;
  const nw = Math.max(1, Math.min(opts.workers ?? 4, n));
  const workers = [];
  const out = opts.keep === false ? null : new Array(n);
  const stats = { workers: nw, busyMs: new Array(nw).fill(0), heap: new Array(nw).fill(0) };
  let next = 0,
    done = 0;
  // worker failures (e.g. a module that fails to load) are watched from creation: between
  // calls (awaiting an image read) no call() handler is installed
  let workerError = null;
  const onWorkerError = (e) => {
    e.preventDefault?.();
    workerError ??= new Error(`SIFT worker failed: ${e.message || e.type}`);
    local.abort(); // settles every pending call and stops the loops
  };
  try {
    for (let w = 0; w < nw; w++) {
      const wk = makeWorker();
      workers.push(wk);
      wk.addEventListener('error', onWorkerError);
      wk.addEventListener('messageerror', onWorkerError);
    }
    const run = Promise.all(
      workers.map(async (wk, w) => {
        try {
          while (next < n) {
            if (signal.aborted) throw abortError('feature extraction');
            const k = next++;
            const grey = await image(k);
            const r = await call(
              wk,
              {
                type: 'sift',
                id: nextId++,
                k,
                grey,
                width,
                height,
                maxNumFeatures: opts.maxNumFeatures ?? 8192,
              },
              { signal, what: 'feature extraction' },
            );
            stats.busyMs[w] += r.ms;
            stats.heap[w] = r.heap;
            done++;
            const res = { keypoints: r.keypoints, descriptors: r.descriptors };
            if (out) out[k] = res;
            opts.onImage?.(k, res, done);
          }
        } catch (e) {
          local.abort();
          throw e;
        }
      }),
    );
    try {
      await run;
    } catch (e) {
      throw workerError ?? e; // the worker error, not the AbortError it causes in the calls
    }
    return { images: out, stats };
  } finally {
    opts.signal?.removeEventListener('abort', onAbort);
    local.abort();
    for (const w of workers) {
      w.removeEventListener('error', onWorkerError);
      w.removeEventListener('messageerror', onWorkerError);
      w.terminate();
    }
  }
}
