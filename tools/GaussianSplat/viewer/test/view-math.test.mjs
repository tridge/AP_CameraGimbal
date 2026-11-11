import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  DEFAULT_SETTINGS,
  SETTINGS_KEY,
  TRIM_FRACTION,
  clampCamera,
  dollyDistance,
  enuToLatLon,
  estimateTrim,
  groundAt,
  groundGrid,
  loadSettings,
  mercatorPixel,
  saveSettings,
  tileLayout,
  trimFromMeta,
} from '../src/view-math.js';

// deterministic random numbers
function rng(seed = 1) {
  return () => (seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648;
}

// a disc of uniform areal density out to radius, plus a sparse fringe out to 2 radius
function disc(n, radius, fringe = 0, centre = [0, 0], r = rng()) {
  const xs = [];
  const ys = [];
  for (let i = 0; i < n + fringe; i++) {
    const d = i < n ? radius * Math.sqrt(r()) : radius * (1 + r());
    const a = 2 * Math.PI * r();
    xs.push(centre[0] + d * Math.cos(a));
    ys.push(centre[1] + d * Math.sin(a));
  }
  return { xs, ys };
}

test('estimateTrim: a uniform disc trims a little inside its edge', () => {
  const { xs, ys } = disc(50000, 100, 0, [10, -5]);
  const t = estimateTrim(xs, ys, [10, -5]);
  assert.ok(t.edge > 95 && t.edge <= 100, `edge ${t.edge}`);
  assert.ok(Math.abs(t.radius - TRIM_FRACTION * t.edge) < 1e-9);
});

test('estimateTrim: a sparse fringe beyond the dense disc is cut at the density drop', () => {
  const { xs, ys } = disc(50000, 100, 4000);
  const t = estimateTrim(xs, ys, [0, 0]);
  assert.ok(t.edge > 90 && t.edge < 110, `edge ${t.edge}`);
});

test('estimateTrim: a dense centre does not pull the edge in', () => {
  const a = disc(40000, 100, 0, [0, 0], rng(2));
  const b = disc(40000, 15, 0, [0, 0], rng(3));
  const t = estimateTrim([...a.xs, ...b.xs], [...a.ys, ...b.ys], [0, 0]);
  assert.ok(t.edge > 90, `edge ${t.edge}`);
});

test('estimateTrim: empty or degenerate input', () => {
  assert.equal(estimateTrim([], [], [0, 0]), null);
  assert.equal(estimateTrim([1, 1], [2, 2], [1, 2]), null);
});

test('trimFromMeta: trim_centre, else the target; invalid radii ignored', () => {
  assert.deepEqual(trimFromMeta({ trim_radius: 120, trim_centre: [1, 2], target: [5, 6, 7] }), {
    centre: [1, 2],
    radius: 120,
  });
  assert.deepEqual(trimFromMeta({ trim_radius: 50, target: [5, 6, 7] }), {
    centre: [5, 6],
    radius: 50,
  });
  assert.equal(trimFromMeta({ target: [0, 0, 0] }), null); // older files: estimated
  assert.equal(trimFromMeta({ trim_radius: 0, target: [0, 0, 0] }), null);
  assert.equal(trimFromMeta({ trim_radius: 'x', target: [0, 0, 0] }), null);
  assert.equal(trimFromMeta({ trim_radius: 10 }), null);
  assert.equal(trimFromMeta(null), null);
});

test('groundGrid: low percentile per cell, trees above a flat ground at 12 m', () => {
  const r = rng(4);
  const xs = [];
  const ys = [];
  const zs = [];
  for (let i = 0; i < 40000; i++) {
    xs.push(200 * r() - 100);
    ys.push(200 * r() - 100);
    zs.push(r() < 0.5 ? 12 + 0.2 * r() : 12 + 25 * r());
  }
  const g = groundGrid(xs, ys, zs, [0, 0], 100);
  assert.ok(Math.abs(g.ground - 12) < 0.5, `ground ${g.ground}`);
  assert.ok(Math.abs(groundAt(g, 30, -40) - 12) < 0.5);
  assert.equal(groundAt(g, 1e6, 0), g.ground); // outside the grid
  assert.equal(groundAt(null, 0, 0), -Infinity);
  assert.equal(groundGrid([], [], [], [0, 0], 10), null);
});

test('clampCamera: pitch limited above the horizon at the same distance', () => {
  const out = clampCamera([100, 0, -50], [0, 0, 0], { minPitchDeg: 5 });
  const d = Math.hypot(...out);
  assert.ok(Math.abs(d - Math.hypot(100, 50)) < 1e-9);
  assert.ok(Math.abs(Math.atan2(out[2], out[0]) - (5 * Math.PI) / 180) < 1e-9);
  assert.ok(Math.abs(out[1]) < 1e-12); // azimuth kept
  // straight below the target: lifted on some side, distance kept
  const under = clampCamera([0, 0, -10], [0, 0, 0]);
  assert.ok(Math.abs(Math.hypot(...under) - 10) < 1e-9 && under[2] > 0);
});

test('clampCamera: a camera above the limit is unchanged', () => {
  assert.deepEqual(clampCamera([10, 20, 30], [0, 0, 0]), [10, 20, 30]);
});

test('clampCamera: never below the ground plus clearance', () => {
  const ground = (x) => (x > 50 ? 40 : 0); // a hill on one side
  const out = clampCamera([100, 0, 20], [0, 0, 0], { ground, clearance: 2 });
  assert.deepEqual(out, [100, 0, 42]);
  assert.deepEqual(clampCamera([10, 0, 20], [0, 0, 0], { ground, clearance: 2 }), [10, 0, 20]);
});

test('dollyDistance: 0.75 per step within limits', () => {
  assert.equal(dollyDistance(100, true), 75);
  assert.ok(Math.abs(dollyDistance(75, false) - 100) < 1e-9);
  assert.equal(dollyDistance(1, true, 0.9), 0.9);
  assert.equal(dollyDistance(1000, false, 0, 1200), 1200);
});

test('enuToLatLon and tileLayout: corners map to the mosaic', () => {
  const origin = { lat: -35.5799, lon: 148.9066 };
  const [lat, lon] = enuToLatLon(origin, 1000, 500);
  assert.ok(Math.abs(lat - origin.lat - ((500 / 6378137) * 180) / Math.PI) < 1e-12);
  assert.ok(lon > origin.lon);
  const L = tileLayout(origin, 600);
  assert.ok(L.width <= 2048 + 256 && L.height <= 2048 + 256);
  assert.equal(L.tiles.length, (L.width / 256) * (L.height / 256));
  for (const [e, n] of [
    [-600, 600],
    [600, -600],
    [0, 0],
  ]) {
    const [u, v] = L.uv(e, n);
    assert.ok(u > 0 && u < 1 && v > 0 && v < 1, `${e},${n}: ${u},${v}`);
  }
  // east is +u, north is +v
  assert.ok(L.uv(10, 0)[0] > L.uv(0, 0)[0]);
  assert.ok(L.uv(0, 10)[1] > L.uv(0, 0)[1]);
  // metres per texel near the requested size
  const [u0] = L.uv(0, 0);
  const [u1] = L.uv(100, 0);
  const mpp = 100 / ((u1 - u0) * L.width);
  assert.ok(mpp > (2 * 600) / 2048 / 1.01 && mpp < (4 * 600) / 2048, `mpp ${mpp}`);
});

test('mercatorPixel: zoom 0 corners and centre', () => {
  assert.deepEqual(mercatorPixel(0, 0, 0), [128, 128]);
  const [x, y] = mercatorPixel(85.05112878, -180, 0);
  assert.ok(Math.abs(x) < 1e-9 && Math.abs(y) < 1e-6);
});

test('settings: defaults, round trip, bad or blocked storage', () => {
  const mem = new Map();
  const storage = { getItem: (k) => mem.get(k) ?? null, setItem: (k, v) => mem.set(k, v) };
  assert.deepEqual(loadSettings(storage), DEFAULT_SETTINGS);
  saveSettings(storage, { level: false, trim: true, satellite: true, panel: 'collapsed' });
  assert.deepEqual(loadSettings(storage), {
    level: false,
    trim: true,
    satellite: true,
    panel: 'collapsed',
  });
  mem.set(SETTINGS_KEY, '{not json');
  assert.deepEqual(loadSettings(storage), DEFAULT_SETTINGS);
  mem.set(SETTINGS_KEY, JSON.stringify({ level: 'yes', panel: 'sideways' }));
  assert.deepEqual(loadSettings(storage), DEFAULT_SETTINGS);
  const blocked = {
    getItem() {
      throw new Error('SecurityError');
    },
    setItem() {
      throw new Error('QuotaExceededError');
    },
  };
  assert.deepEqual(loadSettings(blocked), DEFAULT_SETTINGS);
  saveSettings(blocked, DEFAULT_SETTINGS); // no throw
  assert.deepEqual(loadSettings(null), DEFAULT_SETTINGS);
});
