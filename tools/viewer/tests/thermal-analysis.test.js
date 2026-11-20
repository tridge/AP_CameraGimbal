import test from 'node:test';
import assert from 'node:assert/strict';
import {
  analyzeThermal,
  analysisSettings,
  gaussian,
  histogram,
  METHODS,
  regionOf,
  richardsonLucy,
  wiener,
} from '../src/domain/thermal-analysis.js';
import { ThermalAnalysisClient } from '../src/domain/thermal-analysis-client.js';

const scene = (w = 96, h = 80, amplitude = 12) => {
  let seed = 1321;
  const uniform = () => ((seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0) + 1) / 4294967297;
  return Uint16Array.from({ length: w * h }, (_, i) => {
    const x = i % w,
      y = Math.floor(i / w),
      noise = Math.sqrt(-2 * Math.log(uniform())) * Math.cos(2 * Math.PI * uniform());
    return Math.round(
      18000 +
        0.03 * x +
        0.02 * y +
        noise +
        amplitude * Math.exp(-((x - 47.3) ** 2 + (y - 39.7) ** 2) / (2 * 1.2 ** 2)),
    );
  });
};
test('raw histograms conserve region counts, exact quantiles, endpoints and finite statistics', () => {
  const raw = Uint16Array.of(0, 1, 2, 3, 4, 5, 65535, 7, 8, 9, 10, 11, 12, 13, 14, 15);
  const h = histogram(raw, 4, 4, null, true);
  assert.equal(h.count, 16);
  assert.equal(h.median, 8);
  assert.equal(h.endpoints, 2);
  assert.equal(
    h.bins.reduce((a, b) => a + b),
    16,
  );
  const roi = histogram(raw, 4, 4, { x: 1, y: 1, width: 2, height: 2 }, true);
  assert.equal(roi.count, 4);
  assert.equal(roi.min, 5);
  assert.equal(roi.max, 65535);
  const f = histogram(Float32Array.of(-1, 0, NaN, Infinity), 2, 2);
  assert.equal(f.count, 2);
  assert.equal(f.invalid, 2);
  assert.equal(f.mean, -0.5);
});
test('bounded settings and invalid input cannot create unbounded work', () => {
  assert.deepEqual(analysisSettings(null), analysisSettings({}));
  assert.equal(
    analysisSettings({ method: '__proto__', iterations: Infinity, sigma: 999 }).method,
    'matched',
  );
  assert.equal(analysisSettings({ iterations: 500 }).iterations, 200);
  assert.equal(analysisSettings({ damping: 50 }).damping, 10);
  assert.deepEqual(regionOf({ x: -5, y: 8, width: 1e9, height: -1 }, 10, 10), {
    x: 0,
    y: 8,
    width: 10,
    height: 1,
  });
  assert.throws(() => analyzeThermal(new Float32Array(16), 4, 4), /gray16/);
  assert.throws(() => analyzeThermal(new Uint16Array(16), 8, 8), /gray16/);
  assert.throws(() => wiener(new Float32Array(2048 * 1024), 2048, 1024, 1, 0.1), /budget/);
});
test('every method is finite on zero, constant and saturated data and preserves its raw input', () => {
  for (const value of [0, 18000, 65535])
    for (const method of Object.keys(METHODS)) {
      const raw = new Uint16Array(32 * 32).fill(value),
        before = raw.slice();
      const r = analyzeThermal(raw, 32, 32, { method, iterations: 2 });
      assert.deepEqual(raw, before, method);
      if (r.plane) assert.ok(r.plane.every(Number.isFinite), method);
      assert.equal(r.rawStats.std, 0);
      assert.equal(r.candidates.length, 0);
      if (['background', 'dog', 'tophat', 'matched', 'cfar', 'multiscale'].includes(method))
        assert.ok(
          r.plane.every((v) => Math.abs(v) < 0.01),
          method,
        );
    }
});
test('Gaussian normalization, reflection, centering and restoration preserve constants', () => {
  const impulse = new Float32Array(65 * 65);
  impulse[32 * 65 + 32] = 1;
  const blur = gaussian(impulse, 65, 65, 1);
  assert.ok(Math.abs(blur.reduce((a, b) => a + b) - 1) < 1e-6);
  assert.equal(blur[32 * 65 + 31], blur[32 * 65 + 33]);
  const flat = new Float32Array(16 * 16).fill(18000);
  assert.ok(richardsonLucy(flat, 16, 16, 1, 8, 0).every((v) => Math.abs(v - 18000) < 0.01));
  assert.ok(wiener(flat, 16, 16, 1, 0.01).every((v) => Math.abs(v - 18000) < 0.01));
  const restored = richardsonLucy(blur, 65, 65, 1, 8, 0);
  assert.ok(restored[32 * 65 + 32] > blur[32 * 65 + 32]);
});
test('seeded weak subpixel Gaussian source is localized and restoration never changes candidate ranking', () => {
  const raw = scene(),
    before = raw.slice();
  const matched = analyzeThermal(raw, 96, 80, { method: 'matched', sigma: 1.2 });
  assert.ok(matched.candidates.length);
  assert.ok(Math.hypot(matched.candidates[0].x - 47.3, matched.candidates[0].y - 39.7) < 1.5);
  assert.ok(matched.candidates[0].score > 5);
  for (const method of ['rl', 'wiener', 'clahe']) {
    const r = analyzeThermal(raw, 96, 80, { method, sigma: 1.2, iterations: 3 });
    assert.deepEqual(r.candidates, matched.candidates);
    assert.equal(r.candidateMethod, 'matched');
  }
  assert.deepEqual(raw, before);
  const multi = analyzeThermal(raw, 96, 80, { method: 'multiscale', sigma: 1.2 });
  assert.ok(Math.hypot(multi.candidates[0].x - 47.3, multi.candidates[0].y - 39.7) < 1.5);
});
test('region statistics and candidate selection leave full-frame convolution unchanged', () => {
  const raw = scene(),
    a = analyzeThermal(raw, 96, 80, { method: 'background' });
  const b = analyzeThermal(
    raw,
    96,
    80,
    { method: 'background' },
    { x: 10, y: 10, width: 12, height: 12 },
  );
  assert.deepEqual(a.plane, b.plane);
  assert.equal(b.rawStats.count, 144);
  assert.equal(b.candidates.length, 0);
});
test('analysis worker copies decoder pixels and cancels stale same-frame results from another source', async () => {
  const workers = [],
    client = new ThermalAnalysisClient(() => {
      const w = {
        terminate() {
          this.stopped = true;
        },
        postMessage(message) {
          this.message = message;
        },
      };
      workers.push(w);
      return w;
    });
  const raw = scene(8, 8),
    first = client.run(raw, 8, 8, {}, null, 'file-A:frame-1');
  const rejected = assert.rejects(first, { name: 'AbortError' });
  const second = client.run(raw, 8, 8, {}, null, 'file-B:frame-1');
  await rejected;
  assert.equal(workers[0].stopped, true);
  assert.notEqual(workers[1].message.pixels.buffer, raw.buffer);
  workers[0].onmessage({ data: { ...workers[0].message, result: { stale: true } } });
  workers[1].onmessage({ data: { ...workers[1].message, result: { current: true } } });
  assert.deepEqual(await second, { current: true });
  assert.ok(raw.byteLength);
  const third = client.run(raw, 8, 8, {}, null, 'C');
  const cancelled = assert.rejects(third, { name: 'AbortError' });
  client.cancel();
  await cancelled;
});

test('Gaussian, top-hat and both deconvolutions match independently generated SciPy/NumPy planes', async () => {
  const { readFile } = await import('node:fs/promises');
  const r = JSON.parse(
    await readFile(new URL('./fixtures/thermal-analysis-reference.json', import.meta.url), 'utf8'),
  );
  const data = Uint16Array.from(r.input),
    { width: w, height: h } = r;
  const cases = [
    ['Gaussian', gaussian(data, w, h, 1), r.gaussian, 0.004],
    [
      'top-hat',
      analyzeThermal(data, w, h, { method: 'tophat', background: 6 }).plane,
      r.tophat,
      0.001,
    ],
    ['RL', richardsonLucy(data, w, h, 1, 5, 0.75), r.rl, 0.03],
    ['Wiener', wiener(data, w, h, 1, 0.02), r.wiener, 0.003],
  ];
  for (const [name, actual, expected, tolerance] of cases) {
    const max = Math.max(...actual.map((v, i) => Math.abs(v - expected[i])));
    assert.ok(max < tolerance, `${name}: error ${max}, tolerance ${tolerance}`);
  }
});
test('2D noise normalization recovers known white noise and ROI does not alter scores/restoration', () => {
  const w = 256,
    h = 256;
  const unit = scene(w, h, 0),
    raw = Uint16Array.from(
      unit,
      (v, i) => 18000 + 5 * (v - Math.round(18000 + 0.03 * (i % w) + 0.02 * Math.floor(i / w))),
    );
  const r = analyzeThermal(raw, w, h, { method: 'matched' });
  assert.ok(Math.abs(r.noise - 5) < 0.6, `measured ${r.noise}`);
  for (const method of ['matched', 'cfar', 'multiscale', 'wiener', 'rl']) {
    const raw = scene(),
      a = analyzeThermal(raw, 96, 80, { method, iterations: 2 });
    const b = analyzeThermal(
      raw,
      96,
      80,
      { method, iterations: 2 },
      { x: 38, y: 30, width: 20, height: 20 },
    );
    assert.deepEqual(a.plane, b.plane, method);
    assert.deepEqual(a.score, b.score, method);
    assert.equal(a.noise, b.noise, method);
    assert.equal(a.k, b.k, method);
    assert.ok(
      b.candidates.length &&
        b.candidates.every((p) => p.x >= 38 && p.x < 58 && p.y >= 30 && p.y < 50),
    );
  }
});
test('Wiener limits noise amplification and software hashing matches SHA-256 known and multi-block vectors', async () => {
  const { softwareSha256 } = await import('../src/domain/pixel-hash.js');
  const { createHash } = await import('node:crypto');
  for (const text of ['', 'abc', 'a'.repeat(1000)]) {
    const bytes = new TextEncoder().encode(text);
    assert.equal(softwareSha256(bytes), createHash('sha256').update(bytes).digest('hex'));
  }
  const raw = scene(96, 80, 0),
    r = analyzeThermal(raw, 96, 80, { method: 'wiener', regularization: 0.1 });
  assert.ok(r.k >= 0.001);
  assert.equal(r.wienerGainLimit, 8);
  assert.ok(r.processedStats.std < 8 * r.rawStats.std);
});

test('worker errors, invalid identities and postMessage failures release resources', async () => {
  let worker;
  const client = new ThermalAnalysisClient(
    () =>
      (worker = {
        stopped: false,
        terminate() {
          this.stopped = true;
        },
        postMessage(message) {
          this.message = message;
        },
      }),
  );
  const raw = scene(8, 8);
  await assert.rejects(client.run(raw, 8, 8, {}, null, {}), /string/);
  const failure = client.run(raw, 8, 8, {}, null, 'A');
  worker.onmessage({ data: { ...worker.message, error: 'bounded failure' } });
  await assert.rejects(failure, /bounded failure/);
  assert.equal(worker.stopped, true);
  const crash = client.run(raw, 8, 8, {}, null, 'B');
  worker.onerror({ message: 'worker stopped' });
  await assert.rejects(crash, /worker stopped/);
  assert.equal(worker.stopped, true);
  const throwing = new ThermalAnalysisClient(() => ({
    terminate() {},
    postMessage() {
      throw new Error('clone failure');
    },
  }));
  await assert.rejects(throwing.run(raw, 8, 8, {}, null, 'C'), /clone failure/);
});
test('border sources are explicitly excluded while interior candidates remain reviewable', () => {
  const raw = scene();
  raw[40 * 96 + 3] = 65000;
  const r = analyzeThermal(raw, 96, 80, { method: 'matched' });
  assert.equal(r.border, 24);
  assert.ok(r.candidates.every((p) => p.x >= r.border && p.y >= r.border));
  assert.ok(r.candidates.some((p) => Math.hypot(p.x - 47.3, p.y - 39.7) < 2));
});
