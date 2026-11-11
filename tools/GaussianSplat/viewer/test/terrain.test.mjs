import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  Terrain,
  chooseTiles,
  decodeQuantizedMesh,
  latLonToEnu,
  loadTerrain,
  parseLayer,
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

const layer = (avail) => ({
  format: 'quantized-mesh-1.0',
  projection: 'EPSG:4326',
  maxzoom: 12,
  tiles: ['{z}/{x}/{y}.terrain'],
  available: avail,
});

test('chooseTiles: the finest available level within the tile budget', () => {
  const all = Array.from({ length: 13 }, (_, z) => [
    { startX: 0, startY: 0, endX: 2 ** (z + 1) - 1, endY: 2 ** z - 1 },
  ]);
  const L = parseLayer(layer(all));
  const c = chooseTiles(ORIGIN, 600, L);
  assert.equal(c.z, 12);
  assert.ok(c.tiles.length >= 1 && c.tiles.length <= 4);
  // a disc much larger than a tile: coarser
  assert.ok(chooseTiles(ORIGIN, 40000, L).z < 12);
  // level 12 missing here: level 11
  const holes = all.map((r, z) => (z === 12 ? [{ startX: 0, startY: 0, endX: 10, endY: 10 }] : r));
  assert.equal(chooseTiles(ORIGIN, 600, parseLayer(layer(holes))).z, 11);
  // nothing near: null
  const none = all.map((r, z) => (z >= 6 ? [] : r));
  assert.equal(chooseTiles(ORIGIN, 600, parseLayer(layer(none))), null);
  assert.throws(() => parseLayer({ format: 'x' }));
});

test('latLonToEnu inverts enuToLatLon', () => {
  const [lat, lon] = enuToLatLon(ORIGIN, 1234.5, -678.9);
  const [e, n] = latLonToEnu(ORIGIN, lat, lon);
  assert.ok(Math.abs(e - 1234.5) < 1e-6 && Math.abs(n + 678.9) < 1e-6);
});

test('Terrain: heights in the splat frame (AMSL - origin.alt_amsl) and clipped geometry', () => {
  const [x, y] = tileXY(12, ORIGIN.lat, ORIGIN.lon);
  const rect = tileRect(12, x, y);
  // a slope rising 100 m west to east across the tile
  const data = decodeQuantizedMesh(tile(17, (u) => 950 + 100 * u));
  const DEG = Math.PI / 180;
  const t = new Terrain(ORIGIN, 12, [
    {
      rect,
      data,
      sampler: meshSampler(data),
      radians: {
        west: rect.west * DEG,
        south: rect.south * DEG,
        width: (rect.east - rect.west) * DEG,
        height: (rect.north - rect.south) * DEG,
      },
    },
  ]);
  const u = (ORIGIN.lon - rect.west) / (rect.east - rect.west);
  assert.ok(Math.abs(t.heightAt(0, 0) - (950 + 100 * u - ORIGIN.alt_amsl)) < 0.05);
  assert.ok(Number.isNaN(t.heightAt(1e5, 0))); // outside the tile
  const g = t.geometry(300);
  assert.ok(g.indices.length > 0 && g.indices.length < data._indices.length);
  let checked = 0;
  for (let i = 0; i < g.positions.length; i += 3) {
    const h = t.heightAt(g.positions[i], g.positions[i + 1]);
    if (!Number.isFinite(h)) continue; // on the tile edge after float32 rounding
    assert.ok(Math.abs(g.positions[i + 2] - h) < 0.05);
    checked++;
  }
  assert.ok(checked > 0.8 * (g.positions.length / 3));
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

test('loadTerrain: fetches layer and tiles once, reasons on failure', async () => {
  const [x, y] = tileXY(12, ORIGIN.lat, ORIGIN.lon);
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
  const t = await loadTerrain(ORIGIN, 600, { url, fetchFn });
  assert.equal(t.level, 12);
  assert.ok(fetched.includes(`${url}12/${x}/${y}.terrain`));
  assert.ok(Math.abs(t.heightAt(0, 0) - (1000 - ORIGIN.alt_amsl)) < 0.05);
  const n = fetched.length;
  await loadTerrain(ORIGIN, 600, { url, fetchFn });
  assert.equal(fetched.length, n); // cached
  await assert.rejects(
    loadTerrain(ORIGIN, 600, {
      url: 'https://down.test/',
      fetchFn: async () => ({ ok: false, status: 503 }),
    }),
    /HTTP 503/,
  );
  await assert.rejects(
    loadTerrain(ORIGIN, 600, {
      url: 'https://empty.test/',
      fetchFn: async () => ({ ok: true, json: async () => layer([]) }),
    }),
    /no terrain coverage/,
  );
});
