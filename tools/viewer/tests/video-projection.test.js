import test from 'node:test';
import assert from 'node:assert/strict';
import { ElevationModel } from '../src/domain/geometry.js';
import { buildVideoProjection } from '../src/domain/video-projection.js';

const record = () => ({
  position: { lat_e7: -350000000, lon_e7: 1490000000, alt_amsl_m: 1000, age_ms: 0 },
  gimbal_attitude: { roll_rad: 0, pitch_rad: -Math.PI / 2, yaw_rad: 0, age_ms: 0 },
  vehicle_attitude: { yaw_rad: 0, age_ms: 0 },
  hfov_deg: 60,
});
const elevation = (at = () => 900) => new ElevationModel({ at, description: 'Test DEM' });

test('video projection puts image edges and raw pixel directions on terrain with unflipped UVs', () => {
  const dem = elevation(),
    pose = record(),
    mesh = buildVideoProjection(dem, pose, 640, 512, 2);
  assert.equal(mesh.complete, true);
  assert.equal(mesh.indices.length, 24);
  const topLeft = mesh.vertices.find((p) => p.u === 0 && p.v === 1),
    bottomRight = mesh.vertices.find((p) => p.u === 1 && p.v === 0),
    center = mesh.vertices.find((p) => p.u === 0.5 && p.v === 0.5);
  assert.ok(topLeft.lat > -35 && topLeft.lon < 149);
  assert.ok(bottomRight.lat < -35 && bottomRight.lon > 149);
  assert.ok(Math.abs(center.lat + 35) < 1e-10 && Math.abs(center.lon - 149) < 1e-10);
  assert.deepEqual(topLeft, { ...dem.project(pose, -0.5, -0.5, 640, 512).point, u: 0, v: 1 });
  assert.ok(mesh.vertices.every((p) => p.altMsl === 900));
});

test('video projection samples sloping terrain rather than stretching four corners over a flat plane', () => {
  const dem = elevation((lat) => 900 + (lat + 35) * 10000),
    mesh = buildVideoProjection(dem, record(), 640, 512, 4);
  assert.equal(mesh.complete, true);
  const heights = mesh.vertices.map((p) => p.altMsl);
  assert.ok(Math.max(...heights) - Math.min(...heights) > 5);
  for (const point of mesh.vertices)
    assert.ok(Math.abs(point.altMsl - dem.at(point.lat, point.lon)) < 1e-9);
});

test('partial-horizon video projection only draws triangles with ground-visible vertices', () => {
  const pose = record();
  pose.gimbal_attitude.pitch_rad = 0;
  pose.hfov_deg = 90;
  const mesh = buildVideoProjection(elevation(), pose, 640, 512, 8);
  assert.equal(mesh.complete, false);
  assert.ok(mesh.indices.length > 0 && mesh.indices.length < 8 * 8 * 6);
  assert.match(mesh.reason, /horizon/);
  assert.ok(mesh.indices.every((i) => mesh.vertices[i].v < 0.5));
});

test('missing terrain leaves holes and resampling after loading fills the projection', () => {
  let ready = false;
  const dem = elevation((lat, lon) => (!ready && lon > 149 ? null : 900));
  const partial = buildVideoProjection(dem, record(), 640, 512, 4);
  assert.equal(partial.complete, false);
  assert.ok(partial.indices.length > 0);
  assert.ok(partial.indices.every((i) => partial.vertices[i].lon <= 149));
  ready = true;
  assert.equal(buildVideoProjection(dem, record(), 640, 512, 4).complete, true);
  assert.equal(buildVideoProjection(new ElevationModel(), record(), 640, 512).indices.length, 0);
});

test('invalid pose, stale telemetry, missing lens and missing frames never produce a video mesh', () => {
  const dem = {
    project() {
      throw new Error('invalid inputs should not request terrain');
    },
  };
  const stale = record();
  stale.gimbal_attitude.age_ms = 3000;
  for (const pose of [null, stale, { ...record(), hfov_deg: null }])
    assert.equal(buildVideoProjection(dem, pose, 640, 512).indices.length, 0);
  assert.equal(buildVideoProjection(dem, record(), 0, 0).indices.length, 0);
  assert.throws(() => buildVideoProjection(dem, record(), 640, 512, 100), RangeError);
});
