import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  curvatureDrop,
  mercatorBounds,
  planTiles,
  tileDistance,
  tileMesh,
  tileSize,
} from '../src/map-tiles.js';
import { latLonToEnu } from '../src/terrain.js';

const ORIGIN = { lat: -35.579911, lon: 148.906652, alt_amsl: 978.4 };

test('mercatorBounds and tileSize', () => {
  assert.deepEqual(mercatorBounds(0, 0, 0).west, -180);
  assert.ok(Math.abs(mercatorBounds(0, 0, 0).north - 85.0511287798) < 1e-9);
  const b = mercatorBounds(17, 118000, 80000);
  assert.ok(b.north > b.south && b.east > b.west);
  assert.ok(Math.abs(tileSize(0, 0) - 2 * Math.PI * 6378137) < 1e-6);
  assert.ok(Math.abs(tileSize(18, 60) - (2 * Math.PI * 6378137) / 2 ** 19) < 1e-6);
});

test('planTiles: covers the near circle at the finest zoom, coarser outward, within budget', () => {
  for (const [origin, trim] of [
    [ORIGIN, 30],
    [ORIGIN, 67],
    [{ lat: -35.32, lon: 148.84 }, 515],
    [{ lat: 36.3, lon: 138.6 }, 37],
    [{ lat: 0, lon: 0 }, 1500],
    [{ lat: 65, lon: -18 }, 200],
  ]) {
    const nearRadius = Math.max(150, 1.5 * trim);
    const p = planTiles(origin, { nearRadius, farRadius: 25000 });
    assert.ok(p.leaves.length <= 160, `${p.leaves.length} tiles`);
    // nearest first, and every tile meeting the near circle is at zNear
    for (let i = 1; i < p.leaves.length; i++) assert.ok(p.leaves[i].dist >= p.leaves[i - 1].dist);
    for (const t of p.leaves) if (t.dist < nearRadius) assert.equal(t.z, p.zNear);
    // zoom falls with distance: about one per doubling
    for (const t of p.leaves)
      if (t.dist > 2 * tileSize(p.zNear, origin.lat))
        assert.ok(tileSize(t.z, origin.lat) <= 4 * t.dist + 1, `z${t.z} at ${t.dist}`);
    // no overlap: tiles are quadtree leaves
    const keys = new Set(p.leaves.map((t) => `${t.z}/${t.x}/${t.y}`));
    for (const t of p.leaves)
      for (let z = t.z - 1; z >= p.zRoot; z--)
        assert.ok(!keys.has(`${z}/${t.x >> (t.z - z)}/${t.y >> (t.z - z)}`));
    // coverage: sample points out to the far radius each fall in a tile
    for (let r = 0; r <= 24000; r += 3000)
      for (let a = 0; a < 2 * Math.PI; a += Math.PI / 6) {
        const e = r * Math.cos(a);
        const n = r * Math.sin(a);
        assert.ok(
          p.leaves.some((t) => {
            const b = mercatorBounds(t.z, t.x, t.y);
            const [w, s] = latLonToEnu(origin, b.south, b.west);
            const [ea, no] = latLonToEnu(origin, b.north, b.east);
            return e >= w - 1e-6 && e <= ea + 1e-6 && n >= s - 1e-6 && n <= no + 1e-6;
          }),
          `${e},${n} uncovered (${trim})`,
        );
      }
  }
});

test('tileDistance: zero for the tile holding the origin', () => {
  const n = 2 ** 17;
  const x = Math.floor(((ORIGIN.lon + 180) / 360) * n);
  const lat = (ORIGIN.lat * Math.PI) / 180;
  const y = Math.floor(((1 - Math.log(Math.tan(lat) + 1 / Math.cos(lat)) / Math.PI) / 2) * n);
  assert.equal(tileDistance(ORIGIN, 17, x, y), 0);
  assert.ok(tileDistance(ORIGIN, 17, x + 3, y) > 2 * tileSize(17, ORIGIN.lat));
});

test('curvatureDrop: d^2 / 2R', () => {
  assert.equal(curvatureDrop(0), 0);
  assert.ok(Math.abs(curvatureDrop(25000) - 49.0) < 0.1);
  assert.ok(Math.abs(curvatureDrop(1000) - 0.0784) < 1e-3);
});

test('tileMesh: grid on the height function less curvature, skirts below the edges', () => {
  const z = 14;
  const n2 = 2 ** z;
  const tile = { z, x: Math.floor(((ORIGIN.lon + 180) / 360) * n2) + 4, y: 10000 };
  const lat = (ORIGIN.lat * Math.PI) / 180;
  tile.y = Math.floor(((1 - Math.log(Math.tan(lat) + 1 / Math.cos(lat)) / Math.PI) / 2) * n2);
  const height = (e, n) => 100 + 0.01 * e + 0.02 * n;
  const m = tileMesh(ORIGIN, tile, height, { n: 8, skirt: 7 });
  const grid = 9 * 9;
  assert.equal(m.positions.length, 3 * (grid + 4 * 9));
  assert.equal(m.indices.length, 6 * 64 + 4 * 6 * 8);
  for (let k = 0; k < grid; k++) {
    const [e, n, h] = m.positions.slice(3 * k, 3 * k + 3);
    assert.ok(Math.abs(h - (height(e, n) - curvatureDrop(Math.hypot(e, n)))) < 1e-3);
  }
  for (let k = grid; k < grid + 36; k++) {
    const [e, n, h] = m.positions.slice(3 * k, 3 * k + 3);
    assert.ok(Math.abs(h - (height(e, n) - curvatureDrop(Math.hypot(e, n)) - 7)) < 1e-3);
  }
  // corners: the tile's bounds; uv (0,0) south-west, (1,1) north-east
  const b = mercatorBounds(z, tile.x, tile.y);
  const [w, s] = latLonToEnu(ORIGIN, b.south, b.west);
  assert.ok(Math.abs(m.positions[0] - w) < 1e-2 && Math.abs(m.positions[1] - s) < 1e-2);
  assert.deepEqual([...m.uvs.slice(0, 2)], [0, 0]);
  assert.deepEqual([...m.uvs.slice(2 * (grid - 1), 2 * grid)], [1, 1]);
  for (const i of m.indices) assert.ok(i < m.positions.length / 3);
});
