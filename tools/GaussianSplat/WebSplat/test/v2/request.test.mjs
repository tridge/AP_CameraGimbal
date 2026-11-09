// websplat.request v1 (app/src/request.js) and the export's captured view (export/view.js).
// usage (WebSplat directory): node --test test/v2/*.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  FRAME_BUDGET,
  RequestError,
  requestIdentity,
  requestKey,
  resolveRange,
  selectConfig,
  PICKED_MARGIN,
  trustedOrigin,
  validNonce,
  validateRequest,
} from '../../app/src/request.js';
import { viewCamera } from '../../app/src/stages/export/view.js';
import { exportInputs } from '../../app/src/stages/export/export.js';
import { LocalFrame } from '../../app/src/stages/select/geometry.js';

const base = () => ({
  format: 'websplat.request',
  version: 1,
  source: { name: 'flight2_thermal.mkv', size: 123456789, lastModified: 1762600000000 },
  range: {
    timescale: 1000,
    start: { pts: 1234, offset: 40960, size: 81234, index: 12 },
    end: { pts: 56789, offset: 98765432, size: 80111, index: 3456 },
  },
  target: { lat: -35.1234567, lon: 149.1234567, alt_amsl: 612.4, refine: true },
  hfov: null,
  view: { camera: { lat: -35.12, lon: 149.12, alt_amsl: 700.0 }, fov_deg: 60.0 },
  title: 'flight2_thermal',
});

// a deep copy with one change: edit(obj) mutates it
const variant = (edit) => {
  const r = structuredClone(base());
  edit(r);
  return r;
};

test('the plan example validates and normalises', () => {
  const r = validateRequest(base());
  assert.equal(r.title, 'flight2_thermal');
  assert.equal(r.range.start.index, 12);
  assert.deepEqual(r.view.camera, { lat: -35.12, lon: 149.12, alt_amsl: 700 });
});

test('optional fields may be absent or null', () => {
  const r = validateRequest(
    variant((o) => {
      delete o.hfov;
      delete o.view;
      delete o.title;
      delete o.source.lastModified;
      delete o.range.start.index;
    }),
  );
  assert.equal(r.hfov, null);
  assert.equal(r.view, null);
  assert.equal(r.title, null);
  assert.equal(r.source.lastModified, null);
  assert.equal(r.range.start.index, null);
  assert.equal(validateRequest(variant((o) => (o.view = null))).view, null);
  assert.equal(validateRequest(variant((o) => (o.title = '   '))).title, null);
  assert.equal(validateRequest(variant((o) => (o.hfov = 32))).hfov, 32);
});

const rejects = [
  ['not an object', () => null, /request: must be an object/],
  ['array', () => [], /request: must be an object/],
  ['format', (o) => (o.format = 'other'), /request format/],
  ['version', (o) => (o.version = 2), /request version/],
  ['unknown top key', (o) => (o.extra = 1), /request: unknown key "extra"/],
  ['missing target', (o) => delete o.target, /request: missing "target"/],
  ['unknown nested key', (o) => (o.target.x = 1), /request target: unknown key "x"/],
  ['source name', (o) => (o.source.name = ''), /request source.name/],
  ['source size', (o) => (o.source.size = 1.5), /request source.size/],
  ['negative size', (o) => (o.source.size = -1), /request source.size/],
  ['lat range', (o) => (o.target.lat = 91), /request target.lat: 91 is outside/],
  ['lon range', (o) => (o.target.lon = -180.5), /request target.lon/],
  ['NaN', (o) => (o.target.lon = NaN), /request target.lon: must be a finite number/],
  ['string number', (o) => (o.target.alt_amsl = '612'), /request target.alt_amsl/],
  ['height low', (o) => (o.target.alt_amsl = -501), /request target.alt_amsl/],
  ['height high', (o) => (o.view.camera.alt_amsl = 9001), /request view.camera.alt_amsl/],
  ['refine', (o) => (o.target.refine = 'yes'), /request target.refine/],
  ['refine missing', (o) => delete o.target.refine, /request target: missing "refine"/],
  ['fov low', (o) => (o.view.fov_deg = 9), /request view.fov_deg/],
  ['hfov high', (o) => (o.hfov = 121), /request hfov/],
  ['pts not integer', (o) => (o.range.start.pts = 1.5), /request range.start.pts/],
  ['offset negative', (o) => (o.range.end.offset = -1), /request range.end.offset/],
  ['size zero', (o) => (o.range.end.size = 0), /request range.end.size/],
  ['timescale', (o) => (o.range.timescale = 0), /request range.timescale/],
  ['start after end', (o) => (o.range.start.pts = 60000), /request range: start is after end/],
  ['title type', (o) => (o.title = 5), /request title: must be a string/],
  ['title length', (o) => (o.title = 'x'.repeat(121)), /request title: longer than 120/],
  ['view camera key', (o) => (o.view.camera.heading = 1), /request view.camera: unknown key/],
];
for (const [name, edit, re] of rejects) {
  test(`rejects: ${name}`, () => {
    const obj = name === 'not an object' || name === 'array' ? edit() : variant(edit);
    assert.throws(
      () => validateRequest(obj),
      (e) => e instanceof RequestError && re.test(e.message),
    );
  });
}

test('requestKey: stable, 16 hex digits, only output-affecting fields', async () => {
  const k = await requestKey(validateRequest(base()));
  assert.match(k, /^[0-9a-f]{16}$/);
  // key order, title, source and the index hint do not matter
  const same = variant((o) => {
    o.title = 'another';
    o.source.name = 'renamed.mkv';
    o.range.start.index = 99;
    o.target = { refine: true, alt_amsl: 612.4, lon: 149.1234567, lat: -35.1234567 };
  });
  assert.equal(await requestKey(validateRequest(same)), k);
  for (const edit of [
    (o) => (o.target.refine = false),
    (o) => (o.target.alt_amsl = 613),
    (o) => (o.range.end.offset = 98765433),
    (o) => (o.hfov = 30),
    (o) => (o.view = null),
    (o) => (o.view.fov_deg = 50),
  ])
    assert.notEqual(await requestKey(validateRequest(variant(edit))), k);
  // a fixed value: changing the identity's canonical form would orphan existing jobs
  assert.equal(k, '84d589776b098834');
  assert.deepEqual(Object.keys(requestIdentity(validateRequest(base()))).sort(), [
    'hfov',
    'range',
    'target',
    'view',
  ]);
});

// index.json-like frames: pts in ticks
const frames = (n, pts = (i) => 1000 + 40 * i) =>
  Array.from({ length: n }, (_, i) => ({
    index: i,
    pts: pts(i),
    offset: 5000 + 100 * i,
    size: 90 + (i % 7),
  }));
const ep = (f) => ({ pts: f.pts, offset: f.offset, size: f.size, index: f.index });
const range = (fs, a, b, timescale = 1000) => ({ timescale, start: ep(fs[a]), end: ep(fs[b]) });

test('resolveRange: interior, edge and single frames', () => {
  const fs = frames(50);
  assert.deepEqual(resolveRange(fs, range(fs, 10, 20), 1000), { start: 10, end: 20 });
  assert.deepEqual(resolveRange(fs, range(fs, 0, 49), 1000), { start: 0, end: 49 });
  assert.deepEqual(resolveRange(fs, range(fs, 7, 7), 1000), { start: 7, end: 7 });
  // the index hint is ignored
  const r = range(fs, 3, 4);
  r.start.index = 40;
  assert.deepEqual(resolveRange(fs, r, 1000), { start: 3, end: 4 });
});

test('resolveRange: mismatches fail clearly', () => {
  const fs = frames(50);
  const bad = (r, re, ts = 1000) =>
    assert.throws(
      () => resolveRange(fs, r, ts),
      (e) => e instanceof RequestError && re.test(e.message),
    );
  bad(range(fs, 1, 2), /timescale 1000 is not the recording's \(90000\)/, 90000);
  let r = range(fs, 1, 2);
  r.start.offset += 1;
  bad(r, /range start \(offset \d+\) is not a video frame/);
  r = range(fs, 1, 2);
  r.end.size += 1;
  bad(r, /range end does not match frame 2/);
  r = range(fs, 1, 2);
  r.end.pts += 1;
  bad(r, /range end does not match frame 2/);
  bad(range(fs, 5, 2), /start \(frame 5\) is after its end \(2\)/);
});

test('resolveRange: non-monotonic or missing pts in the range', () => {
  const fs = frames(30, (i) => (i === 15 ? 1000 : 1000 + 40 * i));
  assert.throws(() => resolveRange(fs, range(fs, 10, 20), 1000), /not increasing \(frame 15/);
  // outside the range it does not matter
  assert.deepEqual(resolveRange(fs, range(fs, 16, 20), 1000), { start: 16, end: 20 });
  const dup = frames(30, (i) => 1000 + 40 * Math.floor(i / 2));
  assert.throws(() => resolveRange(dup, range(dup, 2, 5), 1000), /not increasing/);
  const missing = frames(30);
  missing[12].pts = null;
  assert.throws(
    () =>
      resolveRange(
        missing,
        { timescale: 1000, start: ep(missing[10]), end: ep(missing[14]) },
        1000,
      ),
    /frame 12 in the range has no timestamp/,
  );
});

test('selectConfig: from the request and ingest', () => {
  const id = requestIdentity(validateRequest(base()));
  const ingest = {
    range: { start: 100, end: 2599, frames: 2500 },
    homeAltitude: 580.4,
    proposal: { proposal: { rotation: 180, hfov: 24.2 } },
  };
  const c = selectConfig(id, ingest);
  assert.equal(c.lat, -35.1234567);
  assert.equal(c.lon, 149.1234567);
  assert.ok(Math.abs(c.target_alt - 32) < 1e-9);
  assert.equal(c.locate_hotspot, true);
  assert.equal(c.rotation, 180);
  assert.equal(c.hfov, 24.2);
  assert.equal(c.start, 100);
  assert.equal(c.end, 2599);
  assert.equal(c.step, Math.ceil(2500 / FRAME_BUDGET));
  // request hfov wins; a failed proposal falls back to select's defaults
  const failed = {
    ...ingest,
    range: { start: 0, end: 999, frames: 1000 },
    proposal: { error: 'x' },
  };
  const c2 = selectConfig({ ...id, hfov: 30, target: { ...id.target, refine: false } }, failed);
  assert.equal(c2.hfov, 30);
  assert.equal(c2.rotation, null);
  assert.equal(c2.locate_hotspot, false);
  assert.equal(c2.margin, PICKED_MARGIN);
  assert.equal('margin' in c, false);
  assert.equal(c2.step, 1);
  assert.equal(selectConfig(id, { ...failed, proposal: null }).hfov, null);
  assert.throws(() => selectConfig(id, { ...ingest, range: null }), RequestError);
  assert.throws(() => selectConfig(id, { ...ingest, homeAltitude: null }), /takeoff altitude/);
});

test('origins and nonces', () => {
  const site = 'https://plot.ardupilot.org';
  assert.ok(trustedOrigin(site, site));
  assert.ok(!trustedOrigin('https://evil.example', site));
  assert.ok(!trustedOrigin('http://localhost:8800', site));
  assert.ok(trustedOrigin('http://localhost:5173', 'http://127.0.0.1:8800'));
  assert.ok(trustedOrigin('http://127.0.0.1:1', 'http://localhost'));
  assert.ok(!trustedOrigin('http://127.0.0.2:8800', 'http://127.0.0.1:8800'));
  assert.ok(!trustedOrigin('https://localhost:8800', 'http://127.0.0.1:8800'));
  assert.ok(!trustedOrigin('null', 'http://127.0.0.1:8800'));
  assert.ok(validNonce('0123456789abcdef0123456789ABCDEF'));
  assert.ok(!validNonce('0123'));
  assert.ok(!validNonce(null));
});

const origin = { lat: -35.0, lon: 149.0, alt_amsl: 600.0 };
const local = new LocalFrame(origin.lat, origin.lon, origin.alt_amsl);
const at = (enu) => {
  const [lat, lon, alt_amsl] = local.toGeodetic(enu);
  return { camera: { lat, lon, alt_amsl }, fov_deg: 50 };
};
const close = (a, b, eps = 1e-6) => a.every((x, i) => Math.abs(x - b[i]) < eps);

test('viewCamera: the captured position in frames.origin ENU', () => {
  const target = [5, -3, 20];
  const { camera, note } = viewCamera(at([300, 400, 220]), origin, target, 150);
  assert.ok(close(camera.position, [300, 400, 220]));
  assert.deepEqual(camera.target, target);
  assert.equal(camera.fov_deg, 50);
  assert.equal(note, null);
});

test('viewCamera: distance clamped to [1, 8] x radius along the same direction', () => {
  const target = [0, 0, 10];
  const near = viewCamera(at([30, 40, 10]), origin, target, 100); // 50 m
  assert.ok(close(near.camera.position, [60, 80, 10]));
  assert.match(near.note, /moved to 100 m/);
  const far = viewCamera(at([3000, 4000, 10]), origin, target, 100); // 5000 m
  assert.ok(close(far.camera.position, [480, 640, 10], 1e-3));
  assert.match(far.note, /moved to 800 m/);
  const zero = viewCamera(at(target), origin, target, 100);
  assert.ok(close(zero.camera.position, [0, 0, 110]));
  for (const r of [NaN, 0, -1, undefined]) {
    const v = viewCamera(at([30, 40, 10]), origin, target, r);
    assert.ok(close(v.camera.position, [30, 40, 10]));
    assert.match(v.note, /not usable/);
  }
});

test("exportInputs: 'picked' uses frames.target_enu over prepare's hot spot", () => {
  const dataset = {
    images: [{ name: '00001.png', hot: true, viewmat: Array(16).fill(0) }],
    K: [
      [1, 0, 0],
      [0, 1, 0],
      [0, 0, 1],
    ],
    height: 512,
    hotspot: { enu: [9, 9, 9] },
    scale: {},
  };
  const frames = { target_enu: [0, 0, 30], origin };
  assert.deepEqual(exportInputs(dataset, frames).target, [9, 9, 9]);
  assert.deepEqual(exportInputs(dataset, frames, 'picked').target, [0, 0, 30]);
  assert.deepEqual(exportInputs({ ...dataset, hotspot: null }, frames).target, [0, 0, 30]);
});

test('viewCamera: longitude unwrapped across the antimeridian', () => {
  const east = { lat: -17.0, lon: 179.999, alt_amsl: 100.0 };
  const lfe = new LocalFrame(east.lat, east.lon, east.alt_amsl);
  // 223 m east of the origin is past 180 degrees
  const [lat, lon] = lfe.toGeodetic([223, 0, 0]);
  const v = { camera: { lat, lon: lon - 360, alt_amsl: 300 }, fov_deg: 50 };
  assert.ok(v.camera.lon < -179.99);
  const { camera, note } = viewCamera(v, east, [0, 0, 0], 100);
  assert.ok(close(camera.position, [223, 0, 200], 1e-6), JSON.stringify(camera.position));
  assert.equal(note, null);
  const west = viewCamera(
    { camera: { lat: east.lat, lon: -179.0, alt_amsl: 100 }, fov_deg: 50 },
    { ...east, lon: -179.5 },
    [0, 0, 0],
    NaN, // no clamp
  );
  assert.ok(Math.abs(west.camera.position[0] - 53200) < 200, west.camera.position[0]);
});
