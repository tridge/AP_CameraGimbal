/** Stage 2 robustness on synthetic inputs: degenerate geometry, isolated optional estimates,
 * the hottest-pixel shortcut, telemetry validation, and output cleanup on failure,
 * interruption and cancellation (handles closed, no partial files, the store reusable). */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { dataDir } from '../stage1/harness.mjs';
import { FsStore } from './node-io.mjs';
import {
  triangulateRays,
  LocalFrame,
  cameraRotation,
  intrinsics,
  project,
} from '../../app/src/stages/select/geometry.js';
import {
  propose,
  selectFrames,
  hottestAll,
  rotate,
  SelectError,
  checkTelemetry,
} from '../../app/src/stages/select/select.js';

let passed = 0;
const test = async (name, fn) => {
  await fn();
  passed++;
  console.log('PASS ' + name);
};

// synthetic scene: cameras looking straight down (parallel optical axes) over a hot point
const W = 64,
  H = 48,
  HFOV = 24.2;
// aim: cameras on a ring, each yawed and pitched at the centre (converging axes)
function scene({ n = 12, aim = false, hot = [3, -2, 5] } = {}) {
  const lat0 = -35.58,
    lon0 = 148.9,
    home = 900;
  const local = new LocalFrame(lat0, lon0, home);
  const K = intrinsics(W, H, HFOV);
  const frames = [];
  for (let i = 0; i < n; i++) {
    const e = aim ? 60 * Math.cos(i) : 6 * Math.cos(i),
      nn = aim ? 60 * Math.sin(i) : 6 * Math.sin(i * 1.3);
    // NED yaw from north; pitch down to the centre
    const yaw = aim ? Math.atan2(-e, -nn) : 0;
    const pitch = aim ? -Math.atan2(100, Math.hypot(e, nn)) : -Math.PI / 2;
    const [lat, lon] = local.toGeodetic([e, nn, 0]);
    const telemetry = {
      utc_us: 1e15 + i * 1e6,
      position: {
        lat_e7: Math.round(lat * 1e7),
        lon_e7: Math.round(lon * 1e7),
        alt_amsl_m: home + 100,
        alt_relative_m: 100,
      },
      vehicle_attitude: { roll_rad: 0, pitch_rad: 0, yaw_rad: yaw },
      gimbal_attitude: { roll_rad: 0, pitch_rad: pitch, yaw_rad: 0 },
    };
    const meta = {
      schema: 'apcg.thermal.v1',
      width: W,
      height: H,
      rotation_deg: 0,
      hfov_deg: HFOV,
      temperature_scale_k: 1 / 64,
      temperature_offset_k: 0,
      maximum_c: 100,
      telemetry,
    };
    frames.push({ index: i, meta });
  }
  // pixels: a gradient (not blank) and the hot point where each camera sees it
  const pixels = frames.map(({ meta }) => {
    const t = meta.telemetry,
      p = t.position;
    const c = local.toEnu(p.lat_e7 / 1e7, p.lon_e7 / 1e7, p.alt_amsl_m);
    const pr = project(K, c, cameraRotation(t), hot);
    const data = new Uint16Array(W * H);
    for (let y = 0; y < H; y++)
      for (let x = 0; x < W; x++) data[y * W + x] = 18000 + 10 * x + 10 * y;
    if (pr) {
      const [[u, v]] = pr;
      data[Math.floor(v) * W + Math.floor(u)] = 30000;
    }
    return data;
  });
  const ingest = { frames, track: { width: W, height: H } };
  const decoder = {
    decoded: 0,
    async *decode(wanted, { signal } = {}) {
      for (const i of [...new Set(wanted)].sort((a, b) => a - b)) {
        if (signal?.aborted) throw new DOMException('cancelled', 'AbortError');
        this.decoded++;
        yield { index: i, meta: frames[i].meta, data: pixels[i].slice() };
      }
    },
  };
  return { ingest, decoder, local, hot };
}

await test('exactly parallel rays: singular, as np.linalg.solve', () => {
  const c = [
    [0, 0, 0],
    [1, 0, 0],
    [0, 1, 0],
    [2, 3, 0],
    [5, 1, 0],
  ];
  const d = c.map(() => [0, 0, -1]);
  assert.throws(() => triangulateRays(c, d, 1000), /singular/);
});
await test('nearly parallel rays: rejected when conditioning is checked', () => {
  const c = [
    [0, 0, 0],
    [1, 0, 0],
    [0, 1, 0],
    [2, 3, 0],
    [5, 1, 0],
  ];
  const d = c.map((_, k) => [1e-13 * k, 0, -1]);
  assert.throws(() => triangulateRays(c, d, 1000, 5, { rcond: 1e-9 }));
});
await test('parallel optical axes do not destroy a valid hot spot proposal (review 6)', async () => {
  const s = scene();
  const r = await propose(s.ingest, s.decoder);
  assert.ok(r.hotspot, 'hot spot kept');
  assert.equal(r.lookAt, null);
  assert.ok(r.notes.some((n) => /optical axes do not converge/.test(n)));
  // the proposal is the hot point (within the pixel quantisation at 95 m)
  const enu = s.local.toEnu(r.proposal.lat, r.proposal.lon, 900);
  assert.ok(Math.hypot(enu[0] - s.hot[0], enu[1] - s.hot[1]) < 1, `hot spot at ${enu}`);
});
await test('no hot spot and no convergence: a clear error', async () => {
  const s = scene();
  for (const f of s.ingest.frames) f.meta.maximum_c = 20;
  await assert.rejects(
    propose(s.ingest, s.decoder),
    /no hot spot and the optical axes do not converge/,
  );
});
await test('fallback proposal carries the optical-axis height (review 1)', async () => {
  // cameras tilted towards a common point: axes converge; no hot frames
  const s = scene({ aim: true });
  for (const f of s.ingest.frames) f.meta.maximum_c = 20;
  const r = await propose(s.ingest, s.decoder);
  assert.equal(r.hotspot, null);
  assert.ok(r.lookAt);
  assert.equal(r.proposal.target_alt, r.lookAt.enu[2]);
});
await test('proposal reports the effective HFOV (review 7)', async () => {
  const s = scene();
  assert.equal((await propose(s.ingest, s.decoder, { params: { hfov: 30 } })).proposal.hfov, 30);
  assert.equal((await propose(s.ingest, s.decoder)).proposal.hfov, HFOV);
});
await test('hottest pixel per rotation without the frame = argmax of the rotated frame (review 4)', () => {
  let seed = 5;
  const rand = () => ((seed = Math.imul(seed ^ (seed >>> 15), 2246822519) + 7) >>> 0) / 2 ** 32;
  for (let t = 0; t < 300; t++) {
    const w = 1 + Math.floor(rand() * 9),
      h = 1 + Math.floor(rand() * 9);
    // few levels so the maximum is often tied
    const raw = Uint16Array.from({ length: w * h }, () => Math.floor(rand() * 3));
    const fast = hottestAll(raw, w, h);
    for (let k = 0; k < 4; k++) {
      const r = rotate(raw, w, h, k * 90);
      let best = 0;
      for (let i = 1; i < r.data.length; i++) if (r.data[i] > r.data[best]) best = i;
      assert.deepEqual(fast[k], [best % r.width, Math.floor(best / r.width)], `${w}x${h} k=${k}`);
    }
  }
});
await test('hot samples keep no frame data (review 4)', async () => {
  const s = scene();
  const r = await propose(s.ingest, s.decoder);
  for (const v of r.samples.values())
    assert.ok(!Object.values(v).some((x) => ArrayBuffer.isView(x) || typeof x === 'function'));
});
await test('incomplete telemetry is rejected with its frames (review 2)', async () => {
  const s = scene();
  s.ingest.frames[3].meta.telemetry.vehicle_attitude.yaw_rad = null;
  s.ingest.frames[5].meta.telemetry.position.alt_amsl_m = null;
  s.ingest.frames[6].meta.telemetry.position.alt_amsl_m = null;
  const e = await propose(s.ingest, s.decoder).catch((e) => e);
  assert.ok(e instanceof SelectError);
  assert.match(e.message, /vehicle_attitude\.yaw_rad in frame 3/);
  assert.match(e.message, /position\.alt_amsl_m in frames 5, 6/);
  assert.deepEqual(
    e.problems.map((p) => p.index),
    [3, 5, 6],
  );
  await assert.rejects(
    selectFrames(s.ingest, s.decoder, null, { lat: -35.58, lon: 148.9 }),
    /incomplete telemetry/,
  );
  // without a hot spot search native never reads maximum_c
  {
    const t = scene();
    t.ingest.frames[2].meta.maximum_c = null;
    assert.doesNotThrow(() =>
      checkTelemetry(
        t.ingest.frames.map((f) => f.meta),
        0,
        11,
        { locate: false },
      ),
    );
    assert.throws(
      () =>
        checkTelemetry(
          t.ingest.frames.map((f) => f.meta),
          0,
          11,
        ),
      /maximum_c in frame 2/,
    );
  }
  // outside start..end only the takeoff-altitude fields matter, as in native
  assert.doesNotThrow(() =>
    checkTelemetry(
      scene().ingest.frames.map((f, i) => {
        if (i === 9) f.meta.telemetry.vehicle_attitude.yaw_rad = null;
        return f.meta;
      }),
      0,
      8,
    ),
  );
});

// output cleanup: a store whose handles are counted and which fails on demand
class FaultStore extends FsStore {
  constructor(root, { writeLimit = Infinity, closeThrows = false, failFile = null } = {}) {
    super(root);
    this.open_ = 0;
    this.writeLimit = writeLimit;
    this.closeThrows = closeThrows;
    this.failFile = failFile;
  }
  // whole-file writes (frames.json) fail like a full disk, after writing part of the file
  async write(name, data) {
    if (name === this.failFile) {
      await super.write(
        name,
        typeof data === 'string' ? data.slice(0, 100) : data.subarray(0, 100),
      );
      throw new DOMException('quota (test)', 'QuotaExceededError');
    }
    return super.write(name, data);
  }
  async open(name) {
    const h = await super.open(name);
    this.open_++;
    const store = this;
    return {
      ...h,
      write(bytes, at) {
        if (store.bytesWritten + bytes.byteLength > store.writeLimit)
          throw new DOMException('quota (test)', 'QuotaExceededError');
        h.write(bytes, at);
      },
      close() {
        store.open_--;
        h.close();
        if (store.closeThrows && name === 'features.u8') throw new Error('close failed (test)');
      },
    };
  }
  files() {
    return fs.readdirSync(this.root);
  }
}
const params = (s) => {
  const [lat, lon] = s.local.toGeodetic([s.hot[0], s.hot[1], 0]);
  return { lat, lon, target_alt: s.hot[2], locate_hotspot: false };
};
const root = path.join(dataDir(), 'stage12', 'robust');
for (const mode of ['single', 'redecode', 'cache']) {
  await test(`quota failure mid-write (${mode}): handles closed, no partial outputs, store reusable`, async () => {
    const s = scene();
    const store = new FaultStore(path.join(root, mode), { writeLimit: 5 * W * H * 3 });
    await assert.rejects(selectFrames(s.ingest, s.decoder, store, params(s), { mode }), {
      name: 'QuotaExceededError',
    });
    assert.equal(store.open_, 0);
    assert.deepEqual(store.files(), []);
    store.writeLimit = Infinity;
    const r = await selectFrames(s.ingest, s.decoder, store, params(s), { mode });
    assert.equal(r.info.frames.length, 12);
    assert.deepEqual(store.files().sort(), ['features.u8', 'frames.json', 'thermal.u16']);
  });
}
await test('interrupted decode (decoder failure) leaves nothing behind', async () => {
  const s = scene();
  const store = new FaultStore(path.join(root, 'interrupted'));
  const inner = s.decoder.decode.bind(s.decoder);
  const failing = {
    async *decode(w, o) {
      let n = 0;
      for await (const f of inner(w, o)) {
        if (++n === 7) throw new Error('decoder worker failed (test)');
        yield f;
      }
    },
  };
  await assert.rejects(selectFrames(s.ingest, failing, store, params(s)), /decoder worker failed/);
  assert.equal(store.open_, 0);
  assert.deepEqual(store.files(), []);
});
await test('cancellation then rerun into the same store', async () => {
  const s = scene();
  const store = new FaultStore(path.join(root, 'cancel'));
  const c = new AbortController();
  const p = selectFrames(s.ingest, s.decoder, store, params(s), {
    signal: c.signal,
    onProgress: ({ done }) => done === 4 && c.abort(),
  });
  await assert.rejects(p, { name: 'AbortError' });
  assert.equal(store.open_, 0);
  assert.deepEqual(store.files(), []);
  assert.equal((await selectFrames(s.ingest, s.decoder, store, params(s))).info.frames.length, 12);
});
for (const mode of ['single', 'cache']) {
  await test(`frames.json write failure (${mode}): no outputs left, store reusable`, async () => {
    const s = scene();
    const store = new FaultStore(path.join(root, 'meta-' + mode), { failFile: 'frames.json' });
    await assert.rejects(selectFrames(s.ingest, s.decoder, store, params(s), { mode }), {
      name: 'QuotaExceededError',
    });
    assert.equal(store.open_, 0);
    assert.deepEqual(store.files(), []);
    store.failFile = null;
    assert.equal(
      (await selectFrames(s.ingest, s.decoder, store, params(s), { mode })).info.frames.length,
      12,
    );
  });
}
await test('an outputs-closed failure after the pixel files (no usable frames) leaves nothing', async () => {
  const s = scene();
  const store = new FaultStore(path.join(root, 'nokept'));
  await assert.rejects(
    selectFrames(s.ingest, s.decoder, store, { ...params(s), max_median_dev: -1 }),
    /no usable frames/,
  );
  assert.deepEqual(store.files(), []);
});
await test('a failing close does not keep other handles open', async () => {
  const s = scene();
  const store = new FaultStore(path.join(root, 'close'), { closeThrows: true });
  await assert.rejects(selectFrames(s.ingest, s.decoder, store, params(s)), /close failed/);
  assert.equal(store.open_, 0);
  assert.deepEqual(store.files(), []);
});
console.log(`robustness: ${passed} checks pass`);
