// Stage 4 module worker: telemetry pairs + WebGPU matching. One run at a time; every message
// carries the caller's run id.
// in:  {type: 'run', id, frames, target, pairOptions, pairs?: Uint32Array, counts: Uint32Array,
//       descriptors: Uint8Array (all frames' uint8 descriptors concatenated in frame order),
//       matchOptions}
//      {type: 'cancel', id}
// out: {type: 'pairs', id, pairs: Uint32Array, ambiguous: [...] (pairs.js tie policy; null
//       when the caller supplied the pairs)}
//      {type: 'progress', id, done, total, matches}
//      {type: 'done', id, container: Uint32Array (format.js), ambiguous, stats, adapter}
//      {type: 'error', id, name, message, done}   name 'Busy' if a run is already active
import { selectPairs } from './pairs.js';
import { createDevice, matchPairs } from './matcher.js';
import { encodeMatches } from './format.js';

let active = null; // {id, abort}

self.onmessage = async ({ data: msg }) => {
  if (msg.type === 'cancel') {
    if (active && active.id === msg.id) active.abort.abort();
    return;
  }
  if (msg.type !== 'run') return;
  const id = msg.id;
  if (active) {
    self.postMessage({
      type: 'error',
      id,
      name: 'Busy',
      message: `run ${active.id} is active`,
      done: 0,
    });
    return;
  }
  const run = { id, abort: new AbortController() };
  active = run;
  let device;
  try {
    let pairs = msg.pairs,
      ambiguous = null;
    if (!pairs) ({ pairs, ambiguous } = selectPairs(msg.frames, msg.target, msg.pairOptions));
    self.postMessage({ type: 'pairs', id, pairs: pairs.slice(), ambiguous });
    const { counts, descriptors } = msg;
    const offsets = new Float64Array(counts.length + 1);
    for (let k = 0; k < counts.length; k++) offsets[k + 1] = offsets[k] + counts[k] * 128;
    const dev = await createDevice(msg.matchOptions);
    device = dev.device;
    const info = dev.adapter.info || {};
    const t0 = performance.now();
    const { results, stats } = await matchPairs(
      device,
      pairs,
      counts,
      (k) => ({ n: counts[k], data: descriptors.subarray(offsets[k], offsets[k + 1]) }),
      {
        ...msg.matchOptions,
        signal: run.abort.signal,
        onBatch: ({ done, total, stats: s }) =>
          self.postMessage({ type: 'progress', id, done, total, matches: s.matches }),
      },
    );
    stats.ms = performance.now() - t0;
    const container = encodeMatches(results);
    self.postMessage(
      {
        type: 'done',
        id,
        container,
        ambiguous,
        stats,
        adapter: {
          vendor: info.vendor,
          architecture: info.architecture,
          description: info.description,
        },
      },
      [container.buffer],
    );
  } catch (e) {
    self.postMessage({ type: 'error', id, name: e.name, message: e.message, done: e.done ?? 0 });
  } finally {
    device && device.destroy();
    active = null;
  }
};
