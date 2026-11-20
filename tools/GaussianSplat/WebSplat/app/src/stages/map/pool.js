// Verification in a pool of map.worker.js instances (pairs handed out in batches to whichever
// worker is free; results are per-pair seeded, so they do not depend on the pool) and prior
// mapping in one worker. opts.signal stops either at once: the workers are terminated (a
// running wasm call cannot see a message) and the promise rejects with an AbortError.
let nextId = 1;

// a keypoint view into the whole WSFT buffer would clone the descriptors too
const own = (a) => (a.byteOffset === 0 && a.byteLength === a.buffer.byteLength ? a : a.slice());

export const abortError = (what) =>
  Object.assign(new Error(`${what} stopped`), { name: 'AbortError' });

// Sends msg and settles on the first reply with msg.id for which onMessage (if given) returns
// truthy, on an application error, on a worker 'error'/'messageerror' event, on abort, or when
// postMessage or onMessage throws. Handlers are removed when the call settles.
export function call(worker, msg, { onMessage, signal, what } = {}) {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) {
      reject(abortError(what));
      return;
    }
    let settled = false;
    const done = (fn, v) => {
      if (settled) return;
      settled = true;
      worker.removeEventListener('message', message);
      worker.removeEventListener('error', error);
      worker.removeEventListener('messageerror', error);
      signal?.removeEventListener('abort', abort);
      fn(v);
    };
    const message = ({ data }) => {
      if (data.id !== msg.id) return;
      if (data.type === 'error')
        done(reject, Object.assign(new Error(data.message), { name: data.name }));
      else {
        let finished;
        try {
          finished = !onMessage || onMessage(data);
        } catch (e) {
          done(reject, e);
          return;
        }
        if (finished) done(resolve, data);
      }
    };
    const error = (e) => {
      e.preventDefault?.();
      done(reject, new Error(`map worker failed: ${e.message || e.type}`));
    };
    const abort = () => {
      worker.terminate();
      done(reject, abortError(what));
    };
    worker.addEventListener('message', message);
    worker.addEventListener('error', error);
    worker.addEventListener('messageerror', error);
    signal?.addEventListener('abort', abort);
    try {
      worker.postMessage(msg);
    } catch (e) {
      done(reject, e);
    }
  });
}

// makeWorker: () => Worker (map.worker.js); features: decodeFeatures(); pairs: decodeMatches()
// entries. Returns {results (pairs order), stats}.
export async function verifyInPool(makeWorker, camera, features, pairs, opts = {}) {
  if (opts.signal?.aborted) throw abortError('verification');
  // a failure anywhere (worker, postMessage, a callback) settles every pending call; the first
  // error is the result
  const local = new AbortController();
  const signal = local.signal;
  const onAbort = () => local.abort();
  opts.signal?.addEventListener('abort', onAbort);
  const n = Math.max(1, Math.min(opts.workers ?? 4, pairs.length));
  const batch = opts.batch ?? 48;
  const id = nextId++;
  const workers = [];
  const results = new Array(pairs.length);
  const stats = { workers: n, batch, loadMs: [], heap: [], busyMs: new Array(n).fill(0) };
  const c = { signal, what: 'verification' };
  let next = 0,
    done = 0;
  const guard = (p) =>
    p.catch((e) => {
      local.abort();
      throw e;
    });
  try {
    for (let k = 0; k < n; k++) workers.push(makeWorker());
    const init = {
      type: 'verify-init',
      id,
      camera,
      keypoints: own(features.keypoints),
      counts: features.counts,
      offsets: features.offsets,
      seed: opts.seed ?? 0,
      legacy: opts.legacy ?? true,
    };
    const ready = await Promise.all(workers.map((w) => guard(call(w, init, c))));
    ready.forEach((r, k) => (stats.loadMs[k] = r.loadMs));
    await Promise.all(
      workers.map((w, k) =>
        guard(
          (async () => {
            while (next < pairs.length) {
              if (signal.aborted) throw abortError('verification');
              const start = next;
              next = Math.min(pairs.length, next + batch);
              const r = await call(
                w,
                { type: 'verify-batch', id, pairs: pairs.slice(start, next) },
                c,
              );
              r.results.forEach((e, q) => (results[start + q] = e));
              stats.busyMs[k] += r.ms;
              done += r.results.length;
              opts.onProgress?.(done, pairs.length);
            }
            const end = await call(w, { type: 'verify-end', id }, c);
            stats.heap[k] = end.heap;
          })(),
        ),
      ),
    );
    return { results, stats };
  } finally {
    opts.signal?.removeEventListener('abort', onAbort);
    local.abort(); // settles anything still pending (their handlers are removed)
    workers.forEach((w) => w.terminate());
  }
}

// prior mapping in one worker; opts.onProgress(phase, done, total, round), opts.onRound(stats)
export async function mapInWorker(makeWorker, camera, frames, features, geometry, opts = {}) {
  const { onProgress, onRound, signal, ...mapOpts } = opts;
  if (signal?.aborted) throw abortError('mapping');
  const id = nextId++;
  let w = null;
  try {
    w = makeWorker();
    return await call(
      w,
      {
        type: 'map',
        id,
        camera,
        frames,
        keypoints: own(features.keypoints),
        counts: features.counts,
        offsets: features.offsets,
        geometry,
        opts: mapOpts,
      },
      {
        signal,
        what: 'mapping',
        onMessage: (d) => {
          if (d.type === 'progress') onProgress?.(d.phase, d.done, d.total, d.round);
          else if (d.type === 'round') onRound?.(d.stats);
          return d.type === 'done';
        },
      },
    );
  } finally {
    w?.terminate();
  }
}
