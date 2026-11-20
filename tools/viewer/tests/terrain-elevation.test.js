import test from 'node:test';
import assert from 'node:assert/strict';
import { QuantizedMeshTerrainData, BoundingSphere, Cartesian3, Rectangle } from 'cesium';
import { meshSampler } from '../src/map/mesh-sampler.js';
import { TerrainElevation } from '../src/map/terrain-elevation.js';
import { ElevationModel } from '../src/domain/geometry.js';
const tick = () => new Promise((resolve) => setImmediate(resolve));
const mesh = (height = 900) => ({ interpolateHeight: () => height });
const provider = (request = async () => mesh()) => ({
  availability: { computeMaximumLevelAtPosition: () => 12, isTileAvailable: () => true },
  tilingScheme: {
    positionToTileXY: (p) => ({
      x: Math.floor(p.longitude * 100),
      y: Math.floor(p.latitude * 100),
    }),
    tileXYToRectangle: () => ({}),
  },
  requestTileGeometry: request,
});
const config = { terrainUrl: '/quantized/', terrainDatum: 'amsl', amslToEllipsoidMetres: 50 };
test('terrain loads deduplicate, fixed ray provenance and datum conversion remain independent of map mode', async () => {
  let count = 0;
  const dem = new TerrainElevation();
  dem.configure(
    provider(async () => {
      count++;
      return mesh();
    }),
    config,
  );
  for (let i = 0; i < 20; i++) assert.equal(dem.at(-35, 149), null);
  assert.equal(dem.lastIssue.state, 'loading');
  await tick();
  assert.equal(count, 1);
  assert.equal(dem.at(-35, 149), 900);
  const ray = dem.forRay(-35, 149);
  assert.equal(ray.at(-35, 149), 900);
  assert.equal(ray.provenance().terrainLevel, 12);
  assert.equal(ray.provenance().tileKeys.length, 1);
  dem.configure(provider(), { ...config, terrainDatum: 'ellipsoid' });
  dem.at(-35, 149);
  await tick();
  assert.equal(dem.at(-35, 149), 850);
  dem.close();
});
test('failed/missing terrain has no height fallback, and errors are negative cached', async () => {
  let count = 0;
  const dem = new TerrainElevation();
  dem.configure(
    provider(async () => {
      count++;
      throw new Error('HTTP404');
    }),
    config,
  );
  dem.at(-35, 149);
  await tick();
  assert.equal(dem.at(-35, 149), null);
  assert.equal(dem.lastIssue.state, 'unavailable');
  for (let i = 0; i < 10; i++) dem.at(-35, 149);
  await tick();
  assert.equal(count, 1);
  dem.configure({ ...provider(), availability: undefined }, config);
  assert.equal(dem.at(-35, 149), null);
  assert.match(dem.lastIssue.reason, /availability/);
  dem.configure(
    {
      ...provider(),
      availability: { computeMaximumLevelAtPosition: () => -1, isTileAvailable: () => false },
    },
    config,
  );
  assert.equal(dem.at(-35, 149), null);
  assert.match(dem.lastIssue.reason, /coverage/);
  dem.close();
});
test('old requests cannot populate a changed provider and the queue stays bounded', async () => {
  const pending = [];
  let canceled = 0;
  const dem = new TerrainElevation(
    () => {},
    () => ({ cancel: () => canceled++ }),
  );
  dem.maxTiles = 8;
  dem.maxActive = 2;
  dem.configure(
    provider(() => new Promise((resolve) => pending.push(resolve))),
    config,
  );
  for (let i = 0; i < 100; i++) dem.at(0, i);
  assert.ok(dem.cache.size <= 8);
  assert.equal(dem.active, 2);
  await tick();
  assert.equal(pending.length, 2);
  dem.configure(
    provider(async () => mesh(800)),
    config,
  );
  assert.equal(canceled, 2);
  pending.forEach((resolve) => resolve(mesh(123)));
  dem.at(0, 1);
  await tick();
  assert.equal(dem.at(0, 1), 800);
  assert.equal(dem.active, 0);
  dem.close();
});
test('finding waits for terrain using captured pose and fails explicitly after a configuration change', async () => {
  const pose = {
    position: { lat_e7: -350000000, lon_e7: 1490000000, alt_amsl_m: 1000, age_ms: 0 },
    vehicle_attitude: { yaw_rad: 0, age_ms: 0 },
    gimbal_attitude: { roll_rad: 0, pitch_rad: -Math.PI / 2, yaw_rad: 0, age_ms: 0 },
    hfov_deg: 60,
  };
  let release;
  const terrain = new TerrainElevation();
  terrain.configure(
    provider(
      () =>
        new Promise((resolve) => {
          release = resolve;
        }),
    ),
    config,
  );
  const dem = new ElevationModel(terrain);
  const waiting = dem.resolve(pose, 319.5, 255.5, 640, 512);
  await tick();
  release(mesh(900));
  const result = await waiting;
  assert.equal(result.status, 'ready');
  assert.ok(Math.abs(result.point.range - 100) < 0.001);
  assert.equal(result.point.terrain.terrainDatum, 'amsl');
  terrain.configure(
    provider(() => new Promise(() => {})),
    config,
  );
  const changed = dem.resolve(pose, 319.5, 255.5, 640, 512);
  terrain.configure(provider(), config);
  assert.match((await changed).reason, /settings changed/);
  terrain.close();
});
test('indexed mesh interpolation matches the pinned Cesium implementation across boundaries and random points', (t) => {
  const side = 65,
    length = side * side,
    q = new Uint16Array(length * 3),
    indices = [];
  for (let y = 0; y < side; y++)
    for (let x = 0; x < side; x++) {
      const i = y * side + x;
      q[i] = Math.round((x / (side - 1)) * 32767);
      q[length + i] = Math.round((y / (side - 1)) * 32767);
      q[2 * length + i] = Math.round((Math.sin(x * 0.2) * Math.cos(y * 0.15) + 1) * 16000);
      if (x < side - 1 && y < side - 1)
        indices.push(i, i + 1, i + side, i + 1, i + side + 1, i + side);
    }
  const data = new QuantizedMeshTerrainData({
    minimumHeight: 100,
    maximumHeight: 1500,
    quantizedVertices: q,
    indices: Uint16Array.from(indices),
    boundingSphere: new BoundingSphere(new Cartesian3(), 1e7),
    horizonOcclusionPoint: new Cartesian3(),
    westIndices: [],
    southIndices: [],
    eastIndices: [],
    northIndices: [],
    westSkirtHeight: 0,
    southSkirtHeight: 0,
    eastSkirtHeight: 0,
    northSkirtHeight: 0,
  });
  const rectangle = new Rectangle(0, 0, 1, 1),
    sampler = meshSampler(data),
    points = [
      [0, 0],
      [1, 0],
      [0, 1],
      [1, 1],
      [0.5, 0.5],
    ];
  let seed = 123;
  for (let i = 0; i < 2000; i++) {
    seed = (seed * 1664525 + 1013904223) >>> 0;
    const x = seed / 2 ** 32;
    seed = (seed * 1664525 + 1013904223) >>> 0;
    points.push([x, seed / 2 ** 32]);
  }
  const start = performance.now();
  const expected = points.map(([x, y]) => data.interpolateHeight(rectangle, x, y));
  const referenceMs = performance.now() - start;
  const began = performance.now();
  const actual = points.map(([x, y]) => sampler.sample(rectangle, x, y));
  const acceleratedMs = performance.now() - began;
  actual.forEach((h, i) =>
    assert.ok(Math.abs(h - expected[i]) < 1e-8, `height mismatch at ${points[i]}`),
  );
  assert.ok(sampler.bytes < 2 * 1024 * 1024);
  t.diagnostic(
    JSON.stringify({
      samples: points.length,
      referenceMs,
      acceleratedMs,
      indexBytes: sampler.bytes,
    }),
  );
});

test('temporarily deferred provider requests retry without fabricating terrain', async () => {
  let count = 0;
  const dem = new TerrainElevation();
  dem.configure(
    provider(() => (++count === 1 ? undefined : Promise.resolve(mesh()))),
    config,
  );
  assert.equal(dem.at(-35, 149), null);
  await new Promise((resolve) => setTimeout(resolve, 150));
  assert.equal(dem.at(-35, 149), 900);
  assert.equal(count, 2);
  dem.close();
});
test('a tile larger than the byte budget is rejected rather than retained', async () => {
  const dem = new TerrainElevation();
  dem.maxBytes = 1024;
  dem.configure(
    provider(async () => ({ ...mesh(), extra: new Uint8Array(2048) })),
    config,
  );
  dem.at(-35, 149);
  await tick();
  assert.equal(dem.at(-35, 149), null);
  assert.equal([...dem.cache.values()][0].data, undefined);
  dem.close();
});
test('curvature moves a shallow ground intersection farther along the ray', () => {
  const angle = (10 * Math.PI) / 180,
    alt = 500;
  const pose = {
    position: { lat_e7: 0, lon_e7: 0, alt_amsl_m: alt, age_ms: 0 },
    vehicle_attitude: { yaw_rad: 0, age_ms: 0 },
    gimbal_attitude: { roll_rad: 0, pitch_rad: -angle, yaw_rad: 0, age_ms: 0 },
    hfov_deg: 60,
  };
  const dem = new ElevationModel({ at: () => 0, description: 'Synthetic curved sea level' });
  const point = dem.ground(pose, 319.5, 255.5, 640, 512),
    s = Math.sin(angle),
    a = Math.cos(angle) ** 2 / (2 * 6371008.8);
  const expected = (2 * alt) / (s + Math.sqrt(s * s - 4 * a * alt));
  assert.ok(point.range > alt / s);
  assert.ok(Math.abs(point.range - expected) < 0.001);
});

test('missions spanning more tiles than the mesh cache converge without refetch storms', async () => {
  const requests = new Map();
  const points = Array.from({ length: 65 }, (_, lon) => ({ lat: 0, lon }));
  let heights = new Map(),
    changes = 0;
  const terrain = new TerrainElevation(() => {
    if (terrain.provider) {
      changes++;
      heights = terrain.samplePoints(points);
    }
  });
  terrain.configure(
    provider(async (x, y, level) => {
      const key = `${level}/${x}/${y}`;
      requests.set(key, (requests.get(key) || 0) + 1);
      return mesh();
    }),
    config,
  );
  for (let i = 0; i < 100 && heights.size !== points.length; i++) await tick();
  assert.equal(heights.size, 65);
  assert.equal(requests.size, 65);
  assert.ok([...requests.values()].every((n) => n === 1));
  assert.ok(terrain.cache.size <= 64);
  const settled = changes;
  await tick();
  await tick();
  assert.equal(changes, settled);
  terrain.close();
});

test('terrain queries wrap both sides of the antimeridian', async () => {
  const terrain = new TerrainElevation();
  terrain.configure(provider(), config);
  terrain.at(0, 180.1);
  terrain.at(0, -180.1);
  await tick();
  assert.equal(terrain.at(0, -179.9), 900);
  assert.equal(terrain.at(0, 179.9), 900);
  assert.equal(terrain.cache.size, 2);
  terrain.close();
});
