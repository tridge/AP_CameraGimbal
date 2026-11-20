import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  Terrain,
  decodeQuantizedMesh,
  latLonToEnu,
  loadTile,
  openLayer,
  parseLayer,
  terrainLevel,
  terrainOffset,
  tileRect,
  tileXY,
} from '../src/terrain.js';
import { enuToLatLon, groundGrid } from '../src/view-math.js';
import { meshSampler } from '../../../viewer/src/map/mesh-sampler.js';

const ORIGIN = { lat: -35.579911, lon: 148.906652, alt_amsl: 978.4 };
const zig = (d) => ((d << 1) ^ (d >> 31)) & 0xffff;

// quantized-mesh-1.0 of an n x n grid over a tile, heights from fn(u, v) in 0..1 of the tile;
// vertices numbered in order of first use, as the high-water-mark index coding needs
function tile(n, fn, min = 900, max = 1100) {
  const S = 32767;
  const tris = [];
  for (let j = 0; j < n - 1; j++)
    for (let i = 0; i < n - 1; i++) {
      const a = j * n + i;
      tris.push(a, a + 1, a + n + 1, a, a + n + 1, a + n);
    }
  const order = new Map();
  for (const t of tris) if (!order.has(t)) order.set(t, order.size);
  const grid = [...order.keys()];
  const u = grid.map((k) => Math.round(((k % n) / (n - 1)) * S));
  const v = grid.map((k) => Math.round((Math.floor(k / n) / (n - 1)) * S));
  const h = grid.map((k) =>
    Math.round(((fn((k % n) / (n - 1), Math.floor(k / n) / (n - 1)) - min) / (max - min)) * S),
  );
  const count = u.length;
  const buf = new ArrayBuffer(88 + 4 + 6 * count + 4 + 2 * tris.length);
  const dv = new DataView(buf);
  dv.setFloat32(24, min, true);
  dv.setFloat32(28, max, true);
  let at = 88;
  dv.setUint32(at, count, true);
  at += 4;
  for (const a of [u, v, h]) {
    let prev = 0;
    for (const x of a) {
      dv.setUint16(at, zig(x - prev), true);
      prev = x;
      at += 2;
    }
  }
  dv.setUint32(at, tris.length / 3, true);
  at += 4;
  let highest = 0;
  for (const t of tris) {
    const k = order.get(t);
    dv.setUint16(at, highest - k, true);
    if (k === highest) highest++;
    at += 2;
  }
  return buf;
}

test('decodeQuantizedMesh: high-water-mark indices and zigzag deltas round trip', () => {
  const d = decodeQuantizedMesh(tile(5, () => 1000));
  assert.equal(d._uValues.length, 25);
  assert.equal(d._indices.length, 4 * 4 * 6);
  assert.equal(Math.max(...d._uValues), 32767);
  assert.equal(d._minimumHeight, 900);
  for (const i of d._indices) assert.ok(i < 25);
  const s = meshSampler(d);
  const r = { west: 0, south: 0, width: 1, height: 1 };
  assert.ok(Math.abs(s.sample(r, 0.3, 0.7) - 1000) < 0.01);
  assert.throws(() => decodeQuantizedMesh(new ArrayBuffer(92)));
});

test('tileXY / tileRect: geographic TMS tiling, y from the south', () => {
  const [x, y] = tileXY(12, ORIGIN.lat, ORIGIN.lon);
  assert.deepEqual([x, y], [7484, 1238]); // the plot terrain's tile here
  const r = tileRect(12, x, y);
  assert.ok(ORIGIN.lon >= r.west && ORIGIN.lon < r.east);
  assert.ok(ORIGIN.lat >= r.south && ORIGIN.lat < r.north);
  assert.deepEqual(tileRect(0, 1, 0), { west: 0, south: -90, east: 180, north: 90 });
});

test('tileXY wraps longitudes past the antimeridian', () => {
  assert.deepEqual(tileXY(3, -17, 181), tileXY(3, -17, -179));
  assert.deepEqual(tileXY(3, -17, -181), tileXY(3, -17, 179));
});

const layer = (avail) => ({
  format: 'quantized-mesh-1.0',
  projection: 'EPSG:4326',
  maxzoom: 12,
  tiles: ['{z}/{x}/{y}.terrain'],
  available: avail,
});

test('terrainLevel: about the requested span, coarser where unavailable', () => {
  const all = Array.from({ length: 13 }, (_, z) => [
    { startX: 0, startY: 0, endX: 2 ** (z + 1) - 1, endY: 2 ** z - 1 },
  ]);
  const L = parseLayer(layer(all));
  assert.equal(terrainLevel(L, ORIGIN.lat, ORIGIN.lon, 500), 12); // capped at maxzoom
  assert.equal(terrainLevel(L, ORIGIN.lat, ORIGIN.lon, 4 * 4892), 10); // level 12 tiles are 4.9 km
  assert.equal(terrainLevel(L, ORIGIN.lat, ORIGIN.lon, 64000), 8);
  const holes = all.map((r, z) => (z >= 11 ? [] : r));
  assert.equal(terrainLevel(parseLayer(layer(holes)), ORIGIN.lat, ORIGIN.lon, 500), 10);
  assert.equal(terrainLevel(parseLayer(layer(all.map(() => []))), 0, 0, 500), null);
  assert.throws(() => parseLayer({ format: 'x' }));
});

test('latLonToEnu inverts enuToLatLon', () => {
  const [lat, lon] = enuToLatLon(ORIGIN, 1234.5, -678.9);
  const [e, n] = latLonToEnu(ORIGIN, lat, lon);
  assert.ok(Math.abs(e - 1234.5) < 1e-6 && Math.abs(n + 678.9) < 1e-6);
});

function tileAt(z, fn) {
  const [x, y] = tileXY(z, ORIGIN.lat, ORIGIN.lon);
  const rect = tileRect(z, x, y);
  const data = decodeQuantizedMesh(tile(17, fn, 0, 3000));
  const DEG = Math.PI / 180;
  return {
    key: `${z}/${x}/${y}`,
    level: z,
    rect,
    data,
    sampler: meshSampler(data),
    radians: {
      west: rect.west * DEG,
      south: rect.south * DEG,
      width: (rect.east - rect.west) * DEG,
      height: (rect.north - rect.south) * DEG,
    },
  };
}

test('Terrain: heights in the splat frame (AMSL - origin.alt_amsl), finest tile first', () => {
  const fine = tileAt(12, (u) => 950 + 100 * u); // a slope rising 100 m west to east
  const coarse = tileAt(10, () => 2000);
  const t = new Terrain(ORIGIN, [coarse, fine]);
  const u = (ORIGIN.lon - fine.rect.west) / (fine.rect.east - fine.rect.west);
  assert.ok(Math.abs(t.heightAt(0, 0) - (950 + 100 * u - ORIGIN.alt_amsl)) < 0.05);
  // beyond the fine tile, the coarse one
  const lon =
    fine.rect.east + 0.0005 < coarse.rect.east ? fine.rect.east + 0.0005 : fine.rect.west - 0.0005;
  const [w] = latLonToEnu(ORIGIN, ORIGIN.lat, lon);
  assert.ok(Math.abs(t.heightAt(w, 0) - (2000 - ORIGIN.alt_amsl)) < 0.05);
  assert.ok(Number.isNaN(t.heightAt(1e6, 0)));
  assert.ok(Number.isNaN(t.heightAt(w, 0, 12))); // only tiles at level 12 or finer
  t.add(fine); // no duplicates
  assert.equal(t.tiles.length, 2);
});

test('terrainOffset: median of splat ground minus terrain over the circle', () => {
  const xs = [];
  const ys = [];
  const zs = [];
  for (let i = 0; i < 20000; i++) {
    const x = ((i * 7919) % 200) - 100;
    const y = ((i * 104729) % 200) - 100;
    xs.push(x);
    ys.push(y);
    zs.push(5 + 0.01 * x + (i % 3 ? 0 : 20)); // ground plus some trees
  }
  const grid = groundGrid(xs, ys, zs, [0, 0], 100);
  const o = terrainOffset(grid, (x) => 3 + 0.01 * x, [0, 0], 100);
  assert.ok(Math.abs(o.median - 2) < 0.3, `offset ${o.median}`);
  assert.equal(
    terrainOffset(grid, () => NaN, [0, 0], 100),
    null,
  );
});

test('openLayer / loadTile: fetched once, reasons on failure', async () => {
  const all = Array.from({ length: 13 }, (_, z) => [
    { startX: 0, startY: 0, endX: 2 ** (z + 1) - 1, endY: 2 ** z - 1 },
  ]);
  const fetched = [];
  const fetchFn = async (u) => {
    fetched.push(u);
    if (u.endsWith('layer.json')) return { ok: true, json: async () => layer(all) };
    return { ok: true, arrayBuffer: async () => tile(9, () => 1000) };
  };
  const url = 'https://terrain.test/q/';
  const L = await openLayer(url, fetchFn);
  const [x, y] = tileXY(12, ORIGIN.lat, ORIGIN.lon);
  const t = await loadTile(L, 12, x, y, url, fetchFn);
  assert.equal(t.key, `12/${x}/${y}`);
  assert.ok(fetched.includes(`${url}12/${x}/${y}.terrain`));
  const terrain = new Terrain(ORIGIN, [t]);
  assert.ok(Math.abs(terrain.heightAt(0, 0) - (1000 - ORIGIN.alt_amsl)) < 0.05);
  const n = fetched.length;
  await openLayer(url, fetchFn);
  await loadTile(L, 12, x, y, url, fetchFn);
  assert.equal(fetched.length, n); // cached
  await assert.rejects(
    openLayer('https://down.test/', async () => ({ ok: false, status: 503 })),
    /HTTP 503/,
  );
  // a failure is not cached
  let calls = 0;
  const flaky = async () =>
    ++calls === 1 ? { ok: false, status: 500 } : { ok: true, json: async () => layer(all) };
  await assert.rejects(openLayer('https://flaky.test/', flaky));
  await new Promise((r) => setTimeout(r, 0));
  assert.ok(await openLayer('https://flaky.test/', flaky));
});
