import test from 'node:test';
import assert from 'node:assert/strict';
import {
  correctedUtc,
  validateTiePoints,
  PoseTimeline,
  frameAt,
  rebaseReviewTime,
} from '../src/domain/time.js';
import { celsius, stretch, colorize, rawPixel, thermalPeaks } from '../src/domain/thermal.js';
import { ElevationModel, cameraRay, validPose } from '../src/domain/geometry.js';

test('piecewise clocks preserve labels, refuse extrapolation and reject ambiguous dates', () => {
  const points = validateTiePoints([
    { videoSeconds: 10, utc: '2026-08-28T03:00:00Z' },
    { videoSeconds: 20, utc: '2026-08-28T03:00:30Z' },
  ]);
  assert.equal(correctedUtc(15, 1, 2, points), points[0].utcUs + 17e6);
  assert.equal(correctedUtc(9, 1, 0, points), null);
  assert.equal(correctedUtc(21, 1, 0, points), null);
  assert.equal(correctedUtc(4, 100000000, -2), 98000000);
  assert.throws(() => validateTiePoints([{ videoSeconds: 0, utc: '2026-08-28T03:00:00' }]));
  assert.throws(() =>
    validateTiePoints([
      { videoSeconds: 0, utcUs: 100 },
      { videoSeconds: 1, utcUs: 90 },
    ]),
  );
});
test('pose interpolation wraps angles and does not bridge missing poses, clock gaps or boundaries', () => {
  const record = (utc, yaw, gimbal = true) => ({
    utc_us: utc,
    position: { lat_e7: 0, lon_e7: 100, alt_amsl_m: 100, age_ms: 0 },
    vehicle_attitude: { yaw_rad: yaw, age_ms: 0 },
    gimbal_attitude: gimbal ? { roll_rad: 0, pitch_rad: 0, yaw_rad: yaw, age_ms: 0 } : null,
  });
  const a = record(1e6, (179 * Math.PI) / 180),
    b = record(2e6, (-179 * Math.PI) / 180),
    timeline = new PoseTimeline();
  timeline.rebuild([{ telemetry: a }, { telemetry: b }]);
  assert.equal(timeline.at(1e6, a, true), a);
  assert.ok(Math.abs(Math.abs(timeline.at(1.5e6, a).vehicle_attitude.yaw_rad) - Math.PI) < 1e-12);
  assert.equal(timeline.at(0, a), null);
  assert.equal(timeline.at(3e6, a), null);
  timeline.rebuild([{ telemetry: a }, { telemetry: record(2e6, 0, false) }]);
  assert.equal(timeline.at(1.5e6, a).gimbal_attitude, null);
  timeline.rebuild([{ telemetry: a }, { telemetry: record(5e6, 0) }]);
  assert.equal(timeline.at(3e6, a), null);
  assert.equal(frameAt([{ pts: 600 }, { pts: 601 }, { pts: 604 }], 3, 600), 1);
});
test('rebasing an edited clip preserves recorded-frame identity and the UTC fit, including hidden observations', () => {
  const anchors = validateTiePoints([
    { videoSeconds: 0, utcUs: 1e6 },
    { videoSeconds: 1, utcUs: 2e6 },
  ]);
  const review = rebaseReviewTime(
    [
      { videoSeconds: 0, originalPts: -0.7 },
      { videoSeconds: 1, originalPts: 0.3 },
      { videoSeconds: 1.7, originalPts: 1 },
    ],
    anchors,
    -0.7,
    0,
    1,
  );
  assert.equal(review.findings[0].presentationUnavailable, true);
  assert.equal(review.findings[1].presentationUnavailable, false);
  assert.equal(review.findings[1].videoSeconds, 0.3);
  assert.equal(review.findings[2].presentationUnavailable, true);
  assert.equal(review.findings[0].originalPts, -0.7);
  validateTiePoints(review.anchors);
  assert.equal(correctedUtc(0.1, null, 0, review.anchors), correctedUtc(0.8, null, 0, anchors));
  const emptyLead = rebaseReviewTime([{ originalPts: 3, videoSeconds: 0 }], [], 3, 0, 10, 5);
  assert.equal(emptyLead.findings[0].presentationUnavailable, true);
});
test('thermal palettes never alter raw samples or invent Celsius; rotation maps to raw coordinates', () => {
  const pixels = Uint16Array.of(18000, 19000, 20000, 21000),
    copy = pixels.slice(),
    meta = { temperature_scale_k: 1 / 64, temperature_offset_k: 0 };
  assert.equal(celsius(19000, meta), 23.725000000000023);
  assert.equal(celsius(19000, {}), null);
  const white = colorize(pixels, 'White hot', [18000, 21000]),
    black = colorize(pixels, 'Black hot', [18000, 21000]);
  assert.deepEqual(pixels, copy);
  assert.equal(white[0], 0);
  assert.equal(black[0], 255);
  assert.equal(white[15], 255);
  assert.throws(() => stretch(pixels, [10, 0], meta));
  assert.throws(() => stretch(pixels, [0, 20], {}));
  assert.deepEqual(rawPixel(1, 2, 640, 512, 180), { x: 638, y: 509 });
  assert.equal(rawPixel(-1, 0, 640, 512, 0), null);
  const source = new Uint16Array(32 * 32).fill(19000);
  source[16 * 32 + 16] = 20000;
  const peaks = thermalPeaks(source, 32, 32);
  assert.equal(peaks[0].x, 16);
  assert.equal(peaks[0].y, 16);
});
const nadir = {
  position: { lat_e7: -350000000, lon_e7: 1490000000, alt_amsl_m: 1000, age_ms: 0 },
  vehicle_attitude: { yaw_rad: 0, age_ms: 0 },
  gimbal_attitude: { roll_rad: 0, pitch_rad: -Math.PI / 2, yaw_rad: 0, age_ms: 0 },
  hfov_deg: 60,
};
test('offset interpolation never revives missing fields, missing ages or stale components', () => {
  for (const [component, key, value] of [
    ['gimbal_attitude', 'pitch_rad', null],
    ['position', 'lat_e7', undefined],
    ['vehicle_attitude', 'yaw_rad', NaN],
    ['gimbal_attitude', 'age_ms', undefined],
    ['position', 'age_ms', null],
    ['vehicle_attitude', 'age_ms', 2001],
  ]) {
    const a = structuredClone({ ...nadir, utc_us: 1e6 }),
      b = structuredClone({ ...nadir, utc_us: 2e6 });
    b[component][key] = value;
    const timeline = new PoseTimeline();
    timeline.rebuild([{ telemetry: a }, { telemetry: b }]);
    const mid = timeline.at(1.5e6, a);
    assert.equal(mid[component], null, `${component}.${key}`);
    assert.equal(validPose(mid), null);
  }
});
test('footprints retain open ground-visible image borders and never close across sky or missing terrain', () => {
  const dem = new ElevationModel({ at: () => 900, description: 'Test terrain' });
  const full = dem.footprint(nadir, 640, 512);
  assert.equal(full.complete, true);
  assert.equal(full.segments.length, 1);
  assert.deepEqual(full.segments[0][0], full.segments[0].at(-1));
  const level = { ...nadir, gimbal_attitude: { ...nadir.gimbal_attitude, pitch_rad: 0 } };
  const partial = dem.footprint(level, 640, 512);
  assert.equal(partial.complete, false);
  assert.equal(partial.segments.length, 1);
  assert.ok(partial.segments[0].length >= 3);
  assert.notDeepEqual(partial.segments[0][0], partial.segments[0].at(-1));
  assert.match(partial.reason, /Partial footprint.*above the horizon/);
  dem.terrain = { at: () => null, lastIssue: { state: 'unavailable', reason: 'No coverage.' } };
  assert.deepEqual(dem.footprint(nadir, 640, 512).segments, []);
});
test('NED camera ray and WGS84 flat-ground intersection preserve nadir and exclude absent elevation', () => {
  const dem = new ElevationModel();
  assert.equal(dem.ground(nadir, 319.5, 255.5, 640, 512), null);
  dem.terrain = { at: () => 900, description: 'Test terrain' };
  const ray = cameraRay(nadir, 319.5, 255.5, 640, 512);
  assert.ok(Math.abs(ray.down - 1) < 1e-12);
  const ground = dem.ground(nadir, 319.5, 255.5, 640, 512);
  assert.ok(Math.abs(ground.lat + 35) < 1e-10);
  assert.ok(Math.abs(ground.lon - 149) < 1e-10);
  assert.ok(Math.abs(ground.range - 100) < 0.001);
  const east = dem.ground(nadir, 400, 255.5, 640, 512);
  assert.ok(east.lon > 149);
  assert.ok(Math.abs(east.lat + 35) < 1e-10);
  assert.equal(
    dem.ground(
      { ...nadir, gimbal_attitude: { ...nadir.gimbal_attitude, pitch_rad: Math.PI / 2 } },
      319.5,
      255.5,
      640,
      512,
    ),
    null,
  );
  assert.equal(
    dem.ground({ ...nadir, position: { ...nadir.position, age_ms: 3000 } }, 319.5, 255.5, 640, 512),
    null,
  );
});

test('pose freshness is explicit and missing terrain never acquires a flat fallback', () => {
  const dem = new ElevationModel();
  dem.terrain = { at: () => 900, description: 'Test terrain' };
  assert.equal(
    dem.ground(
      { ...nadir, position: { ...nadir.position, age_ms: undefined } },
      319.5,
      255.5,
      640,
      512,
    ),
    null,
  );
  assert.equal(
    dem.ground(
      { ...nadir, gimbal_attitude: { ...nadir.gimbal_attitude, age_ms: null } },
      319.5,
      255.5,
      640,
      512,
    ),
    null,
  );
  dem.terrain = {
    at: () => null,
    lastIssue: { state: 'unavailable', reason: 'No terrain coverage.' },
  };
  assert.equal(dem.at(-34.5, 149.5), null);
  assert.equal(dem.project(nadir, 319.5, 255.5, 640, 512).reason, 'No terrain coverage.');
  assert.equal(dem.ground(nadir, 319.5, 255.5, 640, 512), null);
});

test('null or nonnumeric raw coordinates never become valid ground poses', () => {
  for (const key of ['lat_e7', 'lon_e7'])
    for (const value of [null, undefined, '', '0', NaN]) {
      const bad = structuredClone(nadir);
      bad.position[key] = value;
      assert.equal(validPose(bad), null);
    }
});

test('pose longitude interpolation crosses the antimeridian along the short arc', () => {
  const a = structuredClone({ ...nadir, utc_us: 1e6 });
  const b = structuredClone({ ...nadir, utc_us: 2e6 });
  a.position.lon_e7 = 179.9e7;
  b.position.lon_e7 = -179.9e7;
  const timeline = new PoseTimeline();
  timeline.rebuild([{ telemetry: a }, { telemetry: b }]);
  assert.ok(Math.abs(Math.abs(timeline.at(1.5e6).position.lon_e7 / 1e7) - 180) < 1e-8);
  assert.ok(timeline.at(1.25e6).position.lon_e7 > 179.9e7);
  assert.ok(timeline.at(1.75e6).position.lon_e7 < -179.9e7);
});
