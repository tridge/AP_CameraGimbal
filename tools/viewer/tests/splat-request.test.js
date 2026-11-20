import test from 'node:test';
import assert from 'node:assert/strict';
import {
  amslFromCartographic,
  buildRequest,
  defaultRgbWidth,
  defaultTitle,
  longitudeDelta,
  rgbWidthOptions,
  rgbWorkload,
  sourceKind,
  deepFreeze,
  footprintRadius,
  handoverMessage,
  handoverUrl,
  lookPoints,
  medianPoint,
  newNonce,
  opticalAxisGround,
  packetIndex,
  rangeFrames,
  RequestError,
  requestStep,
  sampleIndices,
  splatUnavailable,
  timingCorrected,
  validateRequest,
  viewFromCamera,
} from '../src/domain/splat-request.js';
import { ElevationModel } from '../src/domain/geometry.js';

const pose = (pitch = -Math.PI / 2) => ({
  position: { lat_e7: -353219444, lon_e7: 1488380556, alt_amsl_m: 1400, age_ms: 0 },
  vehicle_attitude: { roll_rad: 0, pitch_rad: 0, yaw_rad: 0, age_ms: 0 },
  gimbal_attitude: { roll_rad: 0, pitch_rad: pitch, yaw_rad: 0, age_ms: 0 },
});
const frame = (i) => ({
  id: i,
  trackId: 1,
  pts: i * 0.2,
  timestampTicks: i * 200,
  timescale: 1000,
  offset: 4000 + i * 1000,
  size: 900 + i,
  duration: 0.2,
  telemetry: pose(),
  thermalMetadata: { pixel_format: 'gray16le' },
});
const frames = Array.from({ length: 12 }, (_, i) => frame(i));
const source = { name: 'flight2_thermal.mkv', size: 123456789, lastModified: 1762600000000 };
const target = { lat: -35.12345671234, lon: 149.1234567, alt_amsl: 612.4 };

test('range maps times to inclusive frame indices', () => {
  assert.deepEqual(rangeFrames(frames, 0, 2.2), { start: 0, end: 11, count: 12 });
  assert.deepEqual(rangeFrames(frames, 0.2, 0.2), { start: 1, end: 1, count: 1 });
  // between frames: only frames inside the interval
  assert.deepEqual(rangeFrames(frames, 0.25, 0.75), { start: 2, end: 3, count: 2 });
  assert.equal(rangeFrames(frames, 0.25, 0.35), null);
  assert.equal(rangeFrames(frames, 1, 0.5), null);
  assert.equal(rangeFrames(frames, NaN, 1), null);
  // frames without pts are skipped
  const gaps = frames.map((f, i) => (i === 0 || i === 11 ? { ...f, pts: undefined } : f));
  assert.deepEqual(rangeFrames(gaps, 0, 2.2), { start: 1, end: 10, count: 10 });
});

test('packet index counts the track in file order', () => {
  const mixed = [frame(2), frame(0), { ...frame(1), trackId: 2 }, frame(3)];
  assert.equal(packetIndex(mixed, mixed[0]), 1);
  assert.equal(packetIndex(mixed, mixed[3]), 2);
});

test('request is built from the edge frames and validates', () => {
  const request = buildRequest({
    source,
    frames,
    range: { start: 1, end: 10 },
    target,
    refine: false,
    hfov: null,
    view: { camera: { lat: -35.12, lon: 149.12, alt_amsl: 700.123 }, fov_deg: 60 },
    title: '  flight2_thermal ',
  });
  assert.deepEqual(request, {
    format: 'websplat.request',
    version: 1,
    kind: 'thermal',
    source,
    range: {
      timescale: 1000,
      start: { pts: 200, offset: 5000, size: 901, index: 1 },
      end: { pts: 2000, offset: 14000, size: 910, index: 10 },
    },
    target: { lat: -35.1234567, lon: 149.1234567, alt_amsl: 612.4, refine: false },
    hfov: null,
    view: { camera: { lat: -35.12, lon: 149.12, alt_amsl: 700.12 }, fov_deg: 60 },
    title: 'flight2_thermal',
  });
  const plain = buildRequest({ source, frames, range: { start: 0, end: 0 }, target, hfov: 24.2 });
  assert.equal(plain.target.refine, true);
  assert.equal(plain.hfov, 24.2);
  assert.ok(!('view' in plain) && !('title' in plain));
  assert.deepEqual(validateRequest(JSON.parse(JSON.stringify(plain))), plain);
});

test('request building refuses bad ranges and non-monotonic packets', () => {
  assert.throws(() => buildRequest({ source, frames, range: null, target }), RequestError);
  const swapped = frames.slice();
  swapped[4] = { ...frames[4], offset: frames[3].offset - 1 };
  assert.throws(
    () => buildRequest({ source, frames: swapped, range: { start: 0, end: 11 }, target }),
    /not increasing/,
  );
  const missing = frames.slice();
  missing[0] = { ...frames[0], timestampTicks: undefined };
  assert.throws(
    () => buildRequest({ source, frames: missing, range: { start: 0, end: 1 }, target }),
    /range/,
  );
});

test('validation is strict and names the field', () => {
  const good = buildRequest({ source, frames, range: { start: 0, end: 11 }, target });
  const bad = (change, field) => {
    const copy = structuredClone(good);
    change(copy);
    assert.throws(
      () => validateRequest(copy),
      (e) => e instanceof RequestError && e.field === field,
    );
  };
  bad((r) => (r.extra = 1), 'request.extra');
  bad((r) => (r.format = 'x'), 'request.format');
  bad((r) => (r.version = 2), 'request.version');
  bad((r) => delete r.target, 'request.target');
  bad((r) => (r.target.lat = 91), 'request.target.lat');
  bad((r) => (r.target.lon = -181), 'request.target.lon');
  bad((r) => (r.target.alt_amsl = 9001), 'request.target.alt_amsl');
  bad((r) => (r.target.alt_amsl = NaN), 'request.target.alt_amsl');
  bad((r) => (r.target.refine = 'yes'), 'request.target.refine');
  bad((r) => (r.target.colour = 1), 'request.target.colour');
  bad((r) => (r.range.start.pts = 1.5), 'request.range.start.pts');
  bad((r) => (r.range.end.size = 0), 'request.range.end.size');
  bad((r) => ([r.range.start, r.range.end] = [r.range.end, r.range.start]), 'request.range');
  bad((r) => (r.range.timescale = 0), 'request.range.timescale');
  bad((r) => (r.source.name = ''), 'request.source.name');
  bad((r) => (r.hfov = 5), 'request.hfov');
  bad(
    (r) => (r.view = { camera: { lat: 0, lon: 0, alt_amsl: 0 }, fov_deg: 121 }),
    'request.view.fov_deg',
  );
  bad(
    (r) => (r.view = { camera: { lat: 0, lon: 0 }, fov_deg: 60 }),
    'request.view.camera.alt_amsl',
  );
  bad((r) => (r.title = 'x'.repeat(121)), 'request.title');
  bad((r) => (r.radius = 5), 'request.radius');
  bad((r) => (r.radius = 10001), 'request.radius');
  bad((r) => (r.radius = '700'), 'request.radius');
  assert.equal(validateRequest({ ...good, radius: 700 }).radius, 700);
  bad((r) => (r.max_height = 0.5), 'request.max_height');
  bad((r) => (r.max_height = null), 'request.max_height');
  assert.equal(validateRequest({ ...good, max_height: 60 }).max_height, 60);
  assert.equal('max_height' in good, false);
  assert.equal(
    buildRequest({ source, frames, range: { start: 0, end: 11 }, target, maxHeight: 60 })
      .max_height,
    60,
  );
  assert.equal(
    'max_height' in
      buildRequest({ source, frames, range: { start: 0, end: 11 }, target, maxHeight: null }),
    false,
  );
  assert.equal('radius' in good, false);
});

test('radius: in the request when set, default from the ground footprint', () => {
  const request = buildRequest({
    source,
    frames,
    range: { start: 0, end: 11 },
    target,
    radius: 704.6,
  });
  assert.equal(request.radius, 705);
  // camera 800 m above the target (straight down), HFOV 60: 2 * 800 * tan(30) = 923.8 -> 920
  const camera = { ...target, alt_amsl: target.alt_amsl + 800 };
  const record = (alt, hfov = 60) => ({
    ...pose(),
    position: {
      lat_e7: Math.round(camera.lat * 1e7),
      lon_e7: Math.round(camera.lon * 1e7),
      alt_amsl_m: alt,
      age_ms: 0,
    },
    hfov_deg: hfov,
  });
  const at = (records) =>
    footprintRadius(frames, { start: 0, end: 11 }, (f) => records[f.id], target);
  assert.equal(at(frames.map(() => record(camera.alt_amsl))), 920);
  // medians of distance and FOV over the range; frames without a pose or FOV are skipped
  const mixed = frames.map((f, i) =>
    i < 4 ? record(target.alt_amsl + 400, 40) : i < 9 ? record(camera.alt_amsl) : { hfov_deg: 60 },
  );
  assert.equal(at(mixed), 920);
  // scales with height and FOV, clamped to [100, 2000]
  assert.equal(at(frames.map(() => record(target.alt_amsl + 400))), 460);
  assert.equal(at(frames.map(() => record(target.alt_amsl + 50, 30))), 100);
  assert.equal(at(frames.map(() => record(target.alt_amsl + 3000, 90))), 2000);
  assert.equal(at(frames.map(() => record(camera.alt_amsl, null))), null);
  assert.equal(
    footprintRadius(frames, null, () => record(800), target),
    null,
  );
  assert.equal(
    footprintRadius(frames, { start: 0, end: 11 }, () => record(800), null),
    null,
  );
});

test('optical axis ground points, sampling and the median target', () => {
  const elevation = new ElevationModel({ at: () => 600, description: 'flat' });
  const nadir = opticalAxisGround(frames[0].telemetry, elevation).point;
  assert.ok(Math.abs(nadir.lat - -35.3219444) < 1e-5 && Math.abs(nadir.lon - 148.8380556) < 1e-5);
  assert.equal(nadir.altMsl, 600);
  // 45 degrees down from 800 m above ground lands ~800 m north
  const ahead = opticalAxisGround(pose(-Math.PI / 4), elevation).point;
  assert.ok(Math.abs((ahead.lat - -35.3219444) * 111000 - 800) < 20);
  assert.equal(opticalAxisGround({}, elevation).point, null);
  const loading = new ElevationModel({
    at: () => null,
    lastIssue: { state: 'loading', reason: 'loading' },
  });
  assert.equal(opticalAxisGround(frames[0].telemetry, loading).status, 'loading');
  assert.deepEqual(sampleIndices(3, 5, 10), [3, 4, 5]);
  const sampled = sampleIndices(0, 9999, 500);
  assert.equal(sampled.length, 500);
  assert.equal(sampled[0], 0);
  assert.equal(sampled.at(-1), 9999);
  const result = lookPoints(frames, { start: 0, end: 11 }, (f) => f.telemetry, elevation, 5);
  assert.equal(result.points.length, 5);
  assert.equal(result.loading, false);
  assert.deepEqual(
    medianPoint([
      { lat: 1, lon: 10, altMsl: 100 },
      { lat: 3, lon: 30, altMsl: 300 },
      { lat: 2, lon: 20, altMsl: 200 },
      { lat: NaN, lon: 0, altMsl: 0 },
    ]),
    { lat: 2, lon: 20, altMsl: 200 },
  );
  assert.equal(medianPoint([]), null);
});

test('view display from the target', () => {
  const t = { lat: -35, lon: 149, alt_amsl: 600 };
  const north = viewFromCamera({ lat: -35 + 1000 / 110950, lon: 149, alt_amsl: 1600 }, t);
  assert.ok(Math.abs(north.bearingDeg) < 0.5 || Math.abs(north.bearingDeg - 360) < 0.5);
  assert.ok(Math.abs(north.elevationDeg - 45) < 0.5);
  assert.ok(Math.abs(north.distanceM - 1414) < 5);
  const west = viewFromCamera({ lat: -35, lon: 148.99, alt_amsl: 600 }, t);
  assert.ok(Math.abs(west.bearingDeg - 270) < 0.1);
  assert.ok(Math.abs(west.elevationDeg) < 1e-9);
});

test('AMSL inverts the map offset', () => {
  // ellipsoid terrain with a 30 m geoid offset draws AMSL 700 at 730 m
  assert.equal(amslFromCartographic(730, 30), 700);
  // AMSL terrain (plot): offset 0 even when a geoid offset is configured
  assert.equal(amslFromCartographic(700, 0), 700);
});

test('availability, step, title and timing helpers', () => {
  const ok = {
    frames,
    indexed: true,
    indexFailed: false,
    format: 'matroska',
    codec: 'ffv1',
    decodedKind: 'gray16',
  };
  assert.equal(splatUnavailable(ok), null);
  assert.match(splatUnavailable({ ...ok, frames: [] }), /Open/);
  assert.match(splatUnavailable({ ...ok, indexed: false }), /indexing/);
  assert.match(splatUnavailable({ ...ok, indexFailed: true }), /completely/);
  assert.match(splatUnavailable({ ...ok, format: 'matroska', codec: 'avc' }), /FFV1/);
  assert.match(splatUnavailable({ ...ok, decodedKind: 'rgba' }), /gray16/);
  assert.match(
    splatUnavailable({ ...ok, frames: frames.map((f) => ({ ...f, thermalMetadata: null })) }),
    /thermal metadata/,
  );
  assert.match(
    splatUnavailable({ ...ok, frames: frames.map((f) => ({ ...f, telemetry: null })) }),
    /pose/,
  );
  assert.equal(requestStep(1), 1);
  assert.equal(requestStep(1000), 1);
  assert.equal(requestStep(1001), 2);
  assert.equal(requestStep(3456), 4);
  assert.equal(defaultTitle('flight2_thermal.mkv'), 'flight2_thermal');
  assert.equal(defaultTitle('a.b.mkv'), 'a.b');
  assert.equal(defaultTitle('x'.repeat(200)).length, 120);
  assert.equal(timingCorrected(0, []), false);
  assert.equal(timingCorrected(0.5, []), true);
  assert.equal(timingCorrected(0, [{}]), true);
});

test('handover nonce, URL and message filter', () => {
  const nonce = newNonce();
  assert.match(nonce, /^[0-9a-f]{32}$/);
  assert.notEqual(newNonce(), nonce);
  const url = handoverUrl('../websplat/', 'https://plot.ardupilot.org/video/?url=x#y', nonce);
  assert.equal(url.href, `https://plot.ardupilot.org/websplat/?request=${nonce}`);
  const popup = {},
    expected = { popup, origin: url.origin, nonce };
  const event = (data, source = popup, origin = url.origin) => ({ data, source, origin });
  const ready = { type: 'websplat.ready', nonce };
  assert.equal(handoverMessage(event(ready), expected), ready);
  assert.equal(handoverMessage(event({ ...ready, nonce: 'x' }), expected), null);
  assert.equal(handoverMessage(event(ready, {}), expected), null);
  assert.equal(handoverMessage(event(ready, popup, 'https://evil.example'), expected), null);
  assert.equal(handoverMessage(event('websplat.ready'), expected), null);
  assert.equal(handoverMessage(event({ nonce }), expected), null);
});

test('frozen requests cannot change', () => {
  const request = deepFreeze(
    buildRequest({ source, frames, range: { start: 0, end: 11 }, target }),
  );
  assert.throws(() => {
    request.target.lat = 0;
  }, TypeError);
});

test('rgb requests carry kind, width and refine false', () => {
  const request = buildRequest({
    kind: 'rgb',
    rgbWidth: 1280,
    source,
    frames,
    range: { start: 0, end: 11 },
    target,
    refine: true,
    hfov: null,
  });
  assert.equal(request.kind, 'rgb');
  assert.deepEqual(request.rgb, { width: 1280 });
  assert.equal(request.target.refine, false);
  assert.deepEqual(validateRequest(structuredClone(request)), request);
  const bad = (change, field) => {
    const copy = structuredClone(request);
    change(copy);
    assert.throws(
      () => validateRequest(copy),
      (e) => e.field === field,
    );
  };
  bad((r) => delete r.rgb, 'request.rgb');
  bad((r) => (r.rgb.width = 1280.5), 'request.rgb.width');
  bad((r) => (r.rgb.height = 720), 'request.rgb.height');
  bad((r) => (r.target.refine = true), 'request.target.refine');
  bad((r) => (r.kind = 'lidar'), 'request.kind');
  bad((r) => ((r.kind = 'thermal'), (r.target.refine = true)), 'request.rgb');
  // kind defaults to thermal
  const thermal = buildRequest({ source, frames, range: { start: 0, end: 1 }, target });
  delete thermal.kind;
  assert.equal(validateRequest(thermal).kind, 'thermal');
});

test('source kind, RGB widths and workload', () => {
  assert.equal(sourceKind('matroska', 'ffv1'), 'thermal');
  assert.equal(sourceKind('mpegts', 'hevc'), 'rgb');
  assert.equal(sourceKind('mp4', 'avc'), 'rgb');
  assert.equal(sourceKind('matroska', 'avc'), null);
  assert.deepEqual(
    rgbWidthOptions(2560).map((o) => o.width),
    [2560, 1920, 1280, 960, 640],
  );
  assert.equal(rgbWidthOptions(2560)[0].label, 'Full (2560)');
  assert.deepEqual(
    rgbWidthOptions(1280).map((o) => o.width),
    [1280, 960, 640],
  );
  assert.deepEqual(
    rgbWidthOptions(320).map((o) => o.width),
    [320],
  );
  assert.equal(defaultRgbWidth(2560), 1280);
  assert.equal(defaultRgbWidth(960), 960);
  // 3 fps over 120 s, under the 500 frame budget
  assert.deepEqual(
    rgbWorkload({
      count: 3600,
      durationS: 120,
      width: 1280,
      sourceWidth: 2560,
      sourceHeight: 1440,
    }),
    { frames: 360, width: 1280, height: 720, megapixels: (360 * 1280 * 720) / 1e6 },
  );
  // 954 sampled, every 2nd kept
  assert.equal(
    rgbWorkload({ count: 9540, durationS: 318, width: 640, sourceWidth: 1280, sourceHeight: 720 })
      .frames,
    477,
  );
  assert.equal(
    rgbWorkload({ count: 30000, durationS: 1000, width: 640, sourceWidth: 1280, sourceHeight: 720 })
      .frames,
    500,
  );
  assert.equal(
    rgbWorkload({ count: 20, durationS: 2, width: 320, sourceWidth: 320, sourceHeight: 240 })
      .frames,
    6,
  );
});

test('RGB availability needs pose and an HFOV, recorded or overridden', () => {
  const rgb = frames.map((f) => ({
    ...f,
    thermalMetadata: null,
    telemetry: { ...pose(), hfov_deg: 58.1 },
  }));
  const base = { frames: rgb, indexed: true, indexFailed: false, format: 'mpegts', codec: 'hevc' };
  assert.equal(splatUnavailable({ ...base, decodedKind: 'rgba' }), null);
  const noFov = rgb.map((f) => ({ ...f, telemetry: pose() }));
  assert.match(splatUnavailable({ ...base, frames: noFov }), /FOV/);
  assert.equal(splatUnavailable({ ...base, frames: noFov, fovOverride: 60 }), null);
  assert.match(
    splatUnavailable({ ...base, frames: rgb.map((f) => ({ ...f, telemetry: null })) }),
    /pose/,
  );
  assert.match(splatUnavailable({ ...base, format: 'matroska', codec: 'avc' }), /MPEG-TS or MP4/);
});

test('longitudes unwrap across the antimeridian', () => {
  assert.equal(longitudeDelta(-179.9, 179.9).toFixed(6), '0.200000');
  assert.equal(longitudeDelta(179.9, -179.9).toFixed(6), '-0.200000');
  assert.equal(longitudeDelta(10, 350), 20);
  const m = medianPoint([
    { lat: 0, lon: 179.9, altMsl: 0 },
    { lat: 0, lon: -179.9, altMsl: 0 },
    { lat: 0, lon: -179.8, altMsl: 0 },
  ]);
  assert.ok(Math.abs(m.lon - -179.9) < 1e-9);
  const across = medianPoint([
    { lat: 0, lon: 179.95, altMsl: 0 },
    { lat: 0, lon: 179.99, altMsl: 0 },
    { lat: 0, lon: -179.99, altMsl: 0 },
    { lat: 0, lon: -179.95, altMsl: 0 },
  ]);
  assert.ok(Math.abs(Math.abs(across.lon) - 180) < 1e-9);
  // a camera 0.002 deg east across 180 is ~200 m away, bearing east
  const v = viewFromCamera(
    { lat: 0, lon: -179.999, alt_amsl: 0 },
    { lat: 0, lon: 179.999, alt_amsl: 0 },
  );
  assert.ok(Math.abs(v.distanceM - 222.6) < 1);
  assert.ok(Math.abs(v.bearingDeg - 90) < 1e-6);
});
