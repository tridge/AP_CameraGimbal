// G2 browser test page (run_browser.mjs serves it): verification in a pool of module
// workers, then prior mapping in one worker, from the files under /g2data, /stage4 and /ref.
// Query: matches=native_raw|<stage4/out name>  geometry=native (map the native verified
//        geometry; skips verification)  workers=N  mode=verify|map|both  legacy=0|1  tag=T
import { decodeFeatures } from '../../app/src/stages/map/features.js';
import { verifyInPool, mapInWorker } from '../../app/src/stages/map/pool.js';
import { siftInPool } from '../../app/src/stages/features/pool.js';
import { cameraFromFrames, modelJson } from '../../app/src/stages/map/priormap.js';
import {
  decodeMatches,
  decodeGeometry,
  encodeGeometry,
} from '../../app/src/stages/match/format.js';

const q = new URLSearchParams(location.search);
const mode = q.get('mode') ?? 'both';
const tag = q.get('tag') ?? 'local';
const workers = Number(q.get('workers') ?? 4);
const legacy = q.get('legacy') !== '0';
const logEl = document.getElementById('log');
const log = (t) => {
  logEl.textContent += t + '\n';
  console.log(t);
};
const get = async (url) => {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${url}: ${r.status}`);
  return r.arrayBuffer();
};
const upload = (name, data) =>
  fetch(`/upload?tag=${encodeURIComponent(tag)}&name=${encodeURIComponent(name)}`, {
    method: 'POST',
    body: data,
  });
const makeWorker = () =>
  new Worker(new URL('../../app/src/stages/map/map.worker.js', import.meta.url), {
    type: 'module',
  });

async function main() {
  const result = {
    tag,
    mode,
    workers,
    legacy,
    ua: navigator.userAgent,
    timings: {},
  };
  const t0 = performance.now();
  const info = JSON.parse(new TextDecoder().decode(await get('/ref/frames.json')));
  const features = decodeFeatures(await get('/g2data/native/features.bin'));
  const camera = cameraFromFrames(info);
  result.camera = camera;
  result.timings.load_ms = performance.now() - t0;
  log(`loaded ${features.total} keypoints in ${result.timings.load_ms.toFixed(0)} ms`);

  let geometry;
  if (q.get('geometry') === 'native') {
    geometry = decodeGeometry(await get('/stage4/native/geometry.bin'), {
      counts: features.counts,
    });
  } else {
    const name = q.get('matches') ?? 'native_raw';
    const url = name === 'native_raw' ? '/stage4/native/native_raw.bin' : `/stage4/out/${name}.bin`;
    const pairs = decodeMatches(await get(url), { counts: features.counts });
    const t1 = performance.now();
    let last = 0;
    const { results, stats } = await verifyInPool(makeWorker, camera, features, pairs, {
      workers,
      legacy,
      onProgress: (done, total) => {
        if (done - last >= 400 || done === total) {
          last = done;
          log(`progress verify ${done}/${total}`);
        }
      },
    });
    result.timings.verify_ms = performance.now() - t1;
    result.verify = {
      matches: name,
      ...stats,
      pairs: results.length,
      verified: results.filter((r) => r.matches.length).length,
      inliers: results.reduce((s, r) => s + r.matches.length / 2, 0),
    };
    const container = encodeGeometry(results);
    await upload('geometry.bin', container);
    geometry = decodeGeometry(container, { counts: features.counts });
    log(
      `verified ${result.verify.verified} pairs, ${result.verify.inliers} inliers in ${result.timings.verify_ms.toFixed(0)} ms`,
    );
  }
  if (mode !== 'verify') {
    const t2 = performance.now();
    const phases = {};
    const res = await mapInWorker(makeWorker, camera, info.frames, features, geometry, {
      onProgress: (phase, done, total, round) => {
        phases[`${round}:${phase}`] ??= performance.now() - t2;
      },
      onRound: (s) =>
        log(
          `progress round ${s.round}: ${s.threshold_px} px, ${s.triangulated} triangulated, ${s.filtered} filtered, ${s.mean_reproj_px.toFixed(3)} px, ${s.seconds.toFixed(1)} s`,
        ),
    });
    result.timings.map_ms = performance.now() - t2;
    result.map = {
      rounds: res.rounds,
      heap: res.heap,
      workerMs: res.ms,
      loadMs: res.loadMs,
      phaseStartMs: phases,
    };
    for (const [name, data] of Object.entries(res.files)) await upload(`model/${name}`, data);
    await upload('model/model.json', JSON.stringify(modelJson(camera, res.rounds), null, 1));
    log(`mapped in ${result.timings.map_ms.toFixed(0)} ms`);
  }
  result.timings.total_ms = performance.now() - t0;
  if (performance.memory)
    result.jsHeap = {
      used: performance.memory.usedJSHeapSize,
      total: performance.memory.totalJSHeapSize,
    };
  result.pass = true;
  await upload('result.json', JSON.stringify(result, null, 1));
  window.result = result;
}

// mode=stop: the Stop path through pool.js in real workers. Already-aborted signals, an abort
// during verification and during a mapping round (workers must be terminated and the promise
// rejected with AbortError within 3 s), and a worker that fails to load (rejects, not pending).
async function stopTests() {
  const result = { tag, mode, ua: navigator.userAgent, checks: {} };
  const info = JSON.parse(new TextDecoder().decode(await get('/ref/frames.json')));
  const features = decodeFeatures(await get('/g2data/native/features.bin'));
  const camera = cameraFromFrames(info);
  const pairs = decodeMatches(await get('/stage4/native/native_raw.bin'), {
    counts: features.counts,
  });
  const geometry = decodeGeometry(await get('/stage4/native/geometry.bin'), {
    counts: features.counts,
  });
  const live = new Set();
  let lateMessages = 0; // messages from workers after terminate(): must stay 0
  const tracked = () => {
    const w = makeWorker();
    live.add(w);
    let gone = false;
    w.addEventListener('message', () => gone && lateMessages++);
    const t = w.terminate.bind(w);
    w.terminate = () => ((gone = true), live.delete(w), t());
    return w;
  };
  const expectAbort = async (name, p, abortAt) => {
    let err = null;
    try {
      await p;
    } catch (e) {
      err = e;
    }
    const ms = abortAt === undefined ? 0 : performance.now() - abortAt;
    result.checks[name] = { error: err?.name, ms, liveWorkers: live.size };
    log(`stop ${name}: ${err?.name} after ${ms.toFixed(0)} ms, ${live.size} live workers`);
    return err?.name === 'AbortError' && ms < 3000 && live.size === 0;
  };
  let ok = true;
  const pre = AbortSignal.abort();
  ok &= await expectAbort(
    'verify pre-aborted',
    verifyInPool(tracked, camera, features, pairs, { signal: pre }),
  );
  ok &= await expectAbort(
    'map pre-aborted',
    mapInWorker(tracked, camera, info.frames, features, geometry, { signal: pre }),
  );

  let ac = new AbortController(),
    at;
  const v = verifyInPool(tracked, camera, features, pairs, {
    workers,
    signal: ac.signal,
    onProgress: (done) => {
      if (done >= 200 && !ac.signal.aborted) {
        at = performance.now();
        ac.abort();
      }
    },
  });
  ok &= await expectAbort('verify running', v, (await v.catch(() => 0), at));

  ac = new AbortController();
  const m = mapInWorker(tracked, camera, info.frames, features, geometry, {
    signal: ac.signal,
    onProgress: (phase, done, total, round) => {
      // inside round 1's bundle adjustment: the worker is busy in one synchronous wasm call
      if (round === 1 && phase === 'ba' && !ac.signal.aborted) {
        at = performance.now();
        ac.abort();
      }
    },
  });
  ok &= await expectAbort('map running', m, (await m.catch(() => 0), at));
  // a still-running mapping worker posts BA progress every 200 ms
  await new Promise((r) => setTimeout(r, 2000));
  result.checks.lateMessages = lateMessages;
  log(`stop late messages from terminated workers in 2 s: ${lateMessages}`);
  ok &= lateMessages === 0;

  let err = null;
  const bad = () => new Worker(new URL('./missing.worker.js', import.meta.url), { type: 'module' });
  const timeout = new Promise((r) => setTimeout(() => r('pending'), 10000));
  const r = await Promise.race([
    mapInWorker(bad, camera, info.frames, features, geometry).catch((e) => (err = e)),
    timeout,
  ]);
  result.checks['worker load failure'] = { settled: r !== 'pending', error: err?.message };
  log(`stop worker load failure: ${r === 'pending' ? 'PENDING' : err?.message}`);
  // SIFT pool: nested workers that fail to load while the pool awaits a (slow) image read
  let serr = null;
  const slow = () => new Promise((res) => setTimeout(() => res(new Uint8Array(640 * 512)), 300));
  const sr = await Promise.race([
    siftInPool(bad, 5, slow, 640, 512, { workers: 2 }).catch((e) => (serr = e)),
    new Promise((res) => setTimeout(() => res('pending'), 10000)),
  ]);
  result.checks['sift worker load failure'] = { settled: sr !== 'pending', error: serr?.message };
  log(`stop sift worker load failure: ${sr === 'pending' ? 'PENDING' : serr?.message}`);
  ok &= sr !== 'pending' && !!serr;
  ok &= r !== 'pending' && !!err;

  result.pass = !!ok;
  await upload('result.json', JSON.stringify(result, null, 1));
  window.result = result;
}

(mode === 'stop' ? stopTests() : main()).catch((e) => {
  log(`error ${e.name}: ${e.message}`);
  window.result = { pass: false, error: `${e.name}: ${e.message}` };
});
