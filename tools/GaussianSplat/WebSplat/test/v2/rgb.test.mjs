// RGB sources: request kind, KLV poses, takeoff altitude, frame selection, decode grouping,
// undistortion and the RGB export.
// usage (WebSplat directory): node --test test/v2/*.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  RequestError,
  requestIdentity,
  requestKey,
  selectConfig,
  validateRequest,
} from '../../app/src/request.js';
import { normalizeTelemetry, parseKLV } from '../../../../viewer/src/media/telemetry.js';
import { poseTelemetry, rgbHomeAltitude } from '../../app/src/stages/ingest/rgb.js';
import {
  LocalFrame,
  R_BODY_CAM,
  R_ENU_NED,
  cameraRotation,
  matmul,
  rotX,
  rotY,
  rotZ,
} from '../../app/src/stages/select/geometry.js';
import {
  finitePose,
  processingSize,
  proposeRgbTarget,
  selectRgb,
} from '../../app/src/stages/select/rgb-select.js';
import { groupsByKeyframe, splitRgba } from '../../app/src/stages/select/rgb-decoder.js';
import { evenly, remapRgbLinear } from '../../app/src/stages/prepare/rgb-scene.js';
import { initPly, encodePngRgb8 } from '../../app/src/stages/train/brush-dataset.js';
import { plyPass } from '../../app/src/stages/export/stream.js';
import { emptySplat, readPly, splatFromPly, writePly } from '../../app/src/stages/export/ply.js';

const thermal = {
  format: 'websplat.request',
  version: 1,
  source: { name: 'orbit.mkv', size: 100 },
  range: {
    timescale: 1000,
    start: { pts: 1234, offset: 40960, size: 81234, index: 12 },
    end: { pts: 56789, offset: 98765432, size: 80111, index: 3456 },
  },
  target: { lat: -35.1234567, lon: 149.1234567, alt_amsl: 612.4, refine: true },
  view: { camera: { lat: -35.12, lon: 149.12, alt_amsl: 700.0 }, fov_deg: 60.0 },
};
const rgbReq = (over = {}) => ({
  ...structuredClone(thermal),
  kind: 'rgb',
  rgb: { width: 1280 },
  source: { name: 'orbit.ts', size: 100 },
  range: {
    timescale: 90000,
    start: { pts: 900000, offset: 188 * 10, size: 50000 },
    end: { pts: 990000, offset: 188 * 900, size: 40000 },
  },
  target: { lat: -35.32207, lon: 148.8349, alt_amsl: 790, refine: false },
  ...over,
});

test('request kind: thermal by default or explicit, rgb with rgb.width', () => {
  assert.equal(validateRequest(thermal).kind, 'thermal');
  assert.equal(validateRequest({ ...thermal, kind: 'thermal' }).kind, 'thermal');
  const r = validateRequest(rgbReq());
  assert.equal(r.kind, 'rgb');
  assert.deepEqual(r.rgb, { width: 1280 });
  const bad = (o, re) =>
    assert.throws(
      () => validateRequest(o),
      (e) => e instanceof RequestError && re.test(e.message),
    );
  bad({ ...thermal, kind: 'lidar' }, /request kind/);
  bad({ ...thermal, rgb: { width: 640 } }, /request rgb: is only allowed/);
  bad(rgbReq({ rgb: undefined }), /request rgb: is required/);
  bad(rgbReq({ rgb: { width: 640, height: 360 } }), /request rgb: unknown key "height"/);
  bad(rgbReq({ rgb: { width: 63 } }), /request rgb.width/);
  bad(rgbReq({ rgb: { width: 16385 } }), /request rgb.width/);
  bad(rgbReq({ rgb: { width: 640.5 } }), /request rgb.width/);
  bad(rgbReq({ target: { ...rgbReq().target, refine: true } }), /target.refine: must be false/);
});

test('request identity: thermal keeps the v1 form, rgb adds kind and width', async () => {
  // the fixed thermal key of request.test.mjs's example (kind absent or explicit)
  const t = validateRequest({ ...thermal, kind: 'thermal' });
  assert.ok(!('kind' in requestIdentity(t)));
  assert.equal(await requestKey(t), await requestKey(validateRequest(thermal)));
  const a = validateRequest(rgbReq());
  const b = validateRequest(rgbReq({ rgb: { width: 640 } }));
  assert.deepEqual(requestIdentity(a).rgb, { width: 1280 });
  assert.notEqual(await requestKey(a), await requestKey(b));
  const c = selectConfig(requestIdentity(a), {
    range: { start: 30, end: 930, frames: 901 },
    homeAltitude: 585,
  });
  assert.deepEqual(c, {
    lat: -35.32207,
    lon: 148.8349,
    target_alt: 205,
    hfov: null,
    width: 1280,
    start: 30,
    end: 930,
    budget: 500,
  });
});

// a MISB ST 0601 local set with the pose tags (5, 90, 91 platform; 18, 19, 20 sensor; 16 HFOV)
function st0601(v) {
  const uenc = (x, size, scale, bias = 0) => {
    const raw = Math.round(((x - bias) / scale) * (2 ** (8 * size) - 1));
    return Array.from(
      { length: size },
      (_, i) => Math.floor(raw / 2 ** (8 * (size - 1 - i))) % 256,
    );
  };
  const senc = (x, size, scale) => {
    let raw = Math.round((x / scale) * (2 ** (8 * size - 1) - 1));
    if (raw < 0) raw += 2 ** (8 * size);
    return Array.from(
      { length: size },
      (_, i) => Math.floor(raw / 2 ** (8 * (size - 1 - i))) % 256,
    );
  };
  const items = [
    [2, [0, 0x06, 0x41, 0x6d, 0, 0, 0, 0]],
    [65, [17]],
    [5, uenc(v.yaw, 2, 360)],
    [90, senc(v.pitch, 4, 90)],
    [91, senc(v.roll, 4, 90)],
    [13, senc(v.lat, 4, 90)],
    [14, senc(v.lon, 4, 180)],
    [15, uenc(v.alt, 2, 19900, -900)],
    [16, uenc(v.hfov, 2, 180)],
    [18, uenc(v.saz, 4, 360)],
    [19, senc(v.sel, 4, 180)],
    [20, uenc(v.sroll, 4, 360)],
  ];
  const body = items.flatMap(([t, b]) => [t, b.length, ...b]);
  const key = [6, 14, 43, 52, 2, 11, 1, 1, 14, 1, 3, 1, 1, 0, 0, 0];
  const len = body.length + 4;
  const bytes = [...key, len, ...body, 1, 2];
  let sum = 0;
  for (let i = 0; i < bytes.length; i++) sum = (sum + bytes[i] * (i % 2 ? 1 : 256)) & 65535;
  return Uint8Array.from([...bytes, sum >> 8, sum & 255]);
}

// extract_ts_frames.py camera_rotation
const nativeRotation = (k) => {
  const r = (d) => (d * Math.PI) / 180;
  const body = matmul(matmul(rotZ(r(k.yaw)), rotY(r(k.pitch))), rotX(r(k.roll)));
  const sensor = matmul(matmul(rotZ(r(k.saz)), rotY(r(k.sel))), rotX(r(k.sroll)));
  return matmul(matmul(matmul(R_ENU_NED, body), sensor), R_BODY_CAM);
};

test("KLV poses: the viewer's normalised telemetry gives extract_ts_frames.py's rotation", () => {
  for (const k of [
    { yaw: 302.6, pitch: 8.1, roll: 16.3, saz: 282.8, sel: -50.0, sroll: 0 },
    { yaw: 10, pitch: -3, roll: 25, saz: 45, sel: -89, sroll: 2 },
    { yaw: 180, pitch: 0, roll: 0, saz: 0, sel: -30, sroll: 359 },
  ]) {
    const errors = [];
    const [rec] = parseKLV(st0601({ ...k, lat: -35.3, lon: 148.8, alt: 1473, hfov: 58.1 }), {
      errors,
    });
    assert.deepEqual(errors, []);
    const t = poseTelemetry(normalizeTelemetry(rec, { format: 'misb-klv' }));
    assert.ok(Math.abs(t.hfov_deg - 58.1) < 0.01);
    assert.equal(t.position.alt_relative_m, null);
    const R = cameraRotation(t);
    const N = nativeRotation(k);
    for (let i = 0; i < 3; i++)
      // KLV quantisation: 360 / 65535 degrees of platform yaw
      for (let j = 0; j < 3; j++)
        assert.ok(Math.abs(R[i][j] - N[i][j]) < 2e-4, `${JSON.stringify(k)} [${i}][${j}]`);
  }
});

const pose = (lat, lon, alt, rel = null) => ({
  position: {
    lat_e7: Math.round(lat * 1e7),
    lon_e7: Math.round(lon * 1e7),
    alt_amsl_m: alt,
    alt_relative_m: rel,
  },
  vehicle_attitude: { roll_rad: 0, pitch_rad: 0, yaw_rad: 0 },
  gimbal_attitude: { roll_rad: 0, pitch_rad: -Math.PI / 2, yaw_rad: 0 },
  hfov_deg: 60,
  offset_ms: 0,
});

test('takeoff altitude: relative altitude (APCG SEI) or the first record (KLV)', () => {
  const sei = [100, 101, 102].map((a) => ({ telemetry: pose(-35, 148, 600 + a, a) }));
  assert.deepEqual(rgbHomeAltitude(sei), { alt: 600, rule: 'median of alt_amsl - alt_relative' });
  const klv = [{ telemetry: null }, ...[1473, 1470].map((a) => ({ telemetry: pose(-35, 148, a) }))];
  const h = rgbHomeAltitude(klv);
  assert.equal(h.alt, 1473);
  assert.match(h.rule, /first record/);
  assert.throws(() => rgbHomeAltitude([{ telemetry: null }]), /no frame carries a position/);
});

// a circle of radius 150 m at 200 m above takeoff, 30 fps, the camera pointed at the centre
function orbit(n = 900, { fps = 30, height = 200 } = {}) {
  const local = new LocalFrame(-35.32, 148.83, 600);
  return Array.from({ length: n }, (_, i) => {
    const a = (2 * Math.PI * i) / n;
    const [lat, lon, alt] = local.toGeodetic([150 * Math.cos(a), 150 * Math.sin(a), height]);
    // NED yaw towards the centre, camera pitched down at the centre
    const yaw = Math.atan2(-150 * Math.cos(a), -150 * Math.sin(a));
    const t = pose(lat, lon, alt);
    t.vehicle_attitude.yaw_rad = yaw;
    t.gimbal_attitude = { roll_rad: 0, pitch_rad: -Math.atan2(height, 150), yaw_rad: 0 };
    return {
      index: i,
      pts: 3000 * i,
      keyframe: i % 30 === 0,
      offset: 1000 + i,
      size: 10,
      telemetry: t,
    };
  });
}

test('selectRgb: fps sampling, view test, budget, sizes', () => {
  const frames = orbit();
  const src = { width: 2560, height: 1440, timescale: 90000 };
  const p = { lat: -35.32, lon: 148.83, target_alt: 0 };
  const s = selectRgb(frames, 600, src, { ...p, width: 1280 });
  assert.equal(s.width, 1280);
  assert.equal(s.height, 720);
  // 30 s at 3 fps; as extract_ts_frames.py, a frame exactly 1/fps after the last may round below
  assert.ok(s.counts.sampled >= 85 && s.counts.sampled <= 90, s.counts.sampled);
  assert.equal(s.entries.length, s.counts.sampled);
  assert.ok(
    s.selected.slice(1).every((i, k) => i - s.selected[k] >= 10 && i - s.selected[k] <= 11),
  );
  assert.equal(s.entries[0].name, '00000.png');
  assert.ok(Math.abs(s.K[0][0] - 640 / Math.tan(Math.PI / 6)) < 1e-9);
  assert.deepEqual(s.target_enu, [0, 0, 0]);
  const b = selectRgb(frames, 600, src, { ...p, budget: 40 });
  assert.equal(b.step, 3);
  assert.equal(b.entries.length, Math.ceil(s.entries.length / 3));
  const r = selectRgb(frames, 600, src, { ...p, start: 100, end: 399 });
  assert.equal(r.counts.range, 300);
  assert.ok(r.selected[0] >= 100 && r.selected.at(-1) <= 399);
  // a target far away is out of view; one above the camera is too high
  assert.throws(
    () => selectRgb(frames, 600, src, { ...p, lat: -35.2 }),
    /only 0 frames see the target/,
  );
  assert.throws(
    () => selectRgb(frames, 600, src, { ...p, target_alt: 198 }),
    /lower than 5 m above it/,
  );
  assert.throws(
    () => selectRgb(frames, 600, src, { ...p, width: 4000 }),
    /above the recording's 2560/,
  );
  assert.deepEqual(processingSize(1280, 720, 640), { width: 640, height: 360 });
  // telemetry more than 200 ms away is not used
  const stale = orbit().map((f) => ({ ...f, telemetry: { ...f.telemetry, offset_ms: 300 } }));
  assert.throws(() => selectRgb(stale, 600, src, p), /only 0 frames/);
});

test('proposeRgbTarget: an orbit converges; nadir views meet the takeoff plane', () => {
  const t = proposeRgbTarget(orbit(), 600);
  assert.ok(Math.abs(t.lat - -35.32) < 1e-6 && Math.abs(t.lon - 148.83) < 1e-6, JSON.stringify(t));
  assert.equal(t.rule, 'optical axes converge');
  assert.ok(Math.abs(t.target_alt) < 1e-3);
  // a straight line looking straight down: parallel axes, the plane under the path
  const local = new LocalFrame(-35.32, 148.83, 600);
  const line = Array.from({ length: 50 }, (_, i) => {
    const [lat, lon, alt] = local.toGeodetic([2 * i, 0, 100]);
    return { telemetry: pose(lat, lon, alt) };
  });
  const n = proposeRgbTarget(line, 600);
  assert.equal(n.rule, 'views meet the takeoff-height ground');
  assert.ok(Math.abs(local.toEnu(n.lat, n.lon, 600)[0] - 49) < 1.5, JSON.stringify(n));
});

test('groupsByKeyframe: each keyframe group decoded once, to its last wanted frame', () => {
  const frames = Array.from({ length: 120 }, (_, i) => ({ keyframe: i % 60 === 0, size: 1 }));
  const g = groupsByKeyframe(frames, [5, 15, 55, 65, 70]);
  assert.equal(g.length, 2);
  assert.deepEqual(g[0].wanted, [5, 15, 55]);
  assert.equal(g[0].packets.length, 56);
  assert.equal(g[0].packets[0], 0);
  assert.deepEqual(g[1].wanted, [65, 70]);
  assert.deepEqual([g[1].packets[0], g[1].packets.at(-1)], [60, 70]);
  assert.throws(() => groupsByKeyframe(frames, [100], { maxPackets: 30 }), /limit 30/);
});

test('splitRgba: packed RGB and the cv2 grey', () => {
  const rgba = Uint8ClampedArray.from([
    255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 200, 100, 50, 255,
  ]);
  const { rgb, grey } = splitRgba(rgba, 4);
  assert.deepEqual([...rgb.slice(0, 6)], [255, 0, 0, 0, 255, 0]);
  // cv2.cvtColor(BGR2GRAY): 76, 150, 29, 124
  assert.deepEqual([...grey], [76, 150, 29, 124]);
});

test('remapRgbLinear: identity, half-pixel shift, constant border', () => {
  const W = 4,
    H = 2;
  const src = Uint8Array.from({ length: W * H * 3 }, (_, i) => i * 10);
  const mx = Float32Array.from({ length: W * H }, (_, i) => i % W);
  const my = Float32Array.from({ length: W * H }, (_, i) => Math.floor(i / W));
  assert.deepEqual([...remapRgbLinear(src, W, H, mx, my)], [...src]);
  const half = remapRgbLinear(
    src,
    W,
    H,
    mx.map((x) => x + 0.5),
    my,
  );
  assert.equal(half[0], 15); // between pixels 0 and 1, channel 0: (0 + 30) / 2
  assert.equal(half[9], 45); // last column blends with the black border: 90 / 2
  assert.deepEqual(evenly(10, 4), [0, 3, 6, 9]);
  assert.deepEqual(evenly(3, 5), [0, 1, 2]);
});

test('RGB training input: coloured initial points and 8-bit RGB PNGs', async () => {
  const pts = Float32Array.from([
    0, 0, 0, 1, 0, 0.5, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1, 0.5, 0.5, 0.5,
  ]);
  const ply = initPly(pts, 4, 1, 0, 3);
  const s = splatFromPly(readPly(ply));
  const C0 = 0.28209479177387814;
  assert.ok(Math.abs(s.sh0[0] - 0.5 / C0) < 1e-6 && Math.abs(s.sh0[1] + 0.5 / C0) < 1e-6);
  assert.ok(Math.abs(s.sh0[2] - 0) < 1e-6);
  const png = await encodePngRgb8(Uint8Array.of(1, 2, 3, 4, 5, 6), 2, 1, async (b) => b);
  assert.deepEqual([...png.slice(1, 4)], [80, 78, 71]);
  assert.deepEqual([...png.slice(16 + 8, 16 + 10)], [8, 2]); // 8-bit truecolour
});

test('RGB export: colours kept, rgb metadata, middle view', async () => {
  const n = 3;
  const sp = emptySplat(n, 1);
  for (let i = 0; i < n; i++) {
    sp.means.set([i, 0, 0], 3 * i);
    sp.sh0.set([0.1 * i, -0.2, 0.3], 3 * i);
    sp.quats.set([1, 0, 0, 0], 4 * i);
  }
  const bytes = writePly(sp);
  const sink = { buf: new Uint8Array(1 << 16), size: 0 };
  const ply = {
    truncate: (k) => (sink.size = k),
    write: (b, at) => {
      sink.buf.set(b, at);
      sink.size = Math.max(sink.size, at + b.length);
    },
    flush: () => {},
  };
  const view = (t) => [1, 0, 0, -t, 0, -1, 0, 0, 0, 0, -1, 100];
  const dataset = {
    kind: 'rgb',
    images: [0, 1, 2].map((k) => ({ name: `0000${k}.png`, hot: false, viewmat: view(k) })),
    K: [
      [500, 0, 320],
      [0, 500, 180],
      [0, 0, 1],
    ],
    height: 360,
    hotspot: null,
  };
  const frames = { target_enu: [0, 0, 0], origin: { lat: -35, lon: 148, alt_amsl: 600 } };
  const r = await plyPass({
    input: { size: bytes.length, read: (o, l) => bytes.slice(o, o + l) },
    dataset,
    frames,
    title: 'orbit',
    ply,
  });
  assert.equal(r.info.kind, 'rgb');
  assert.equal(r.info.temperature_scale, undefined);
  assert.equal(r.info.title, 'orbit');
  const out = splatFromPly(readPly(sink.buf.slice(0, sink.size)));
  assert.ok(Math.abs(out.sh0[3] - 0.1) < 1e-6 && Math.abs(out.sh0[4] + 0.2) < 1e-6);
  // export_web.py start_camera: the middle view (looking straight down: an oblique view)
  assert.deepEqual(r.info.camera.target, [0, 0, 0]);
});

test('selectRgb: records without a usable pose are skipped, never NaN', () => {
  const src = { width: 1280, height: 720, timescale: 90000 };
  const p = { lat: -35.32, lon: 148.83, target_alt: 0, fps: 1000 }; // every frame
  const frames = orbit(90).map((f, i) => {
    const t = structuredClone(f.telemetry);
    if (i % 9 === 1) delete t.vehicle_attitude.yaw_rad; // missing yaw
    if (i % 9 === 2) t.gimbal_attitude = {}; // empty object
    if (i % 9 === 3) t.position.lat_e7 = NaN;
    if (i % 9 === 4) t.vehicle_attitude = {};
    if (i % 9 === 5) delete t.gimbal_attitude.yaw_rad; // allowed: sensor yaw 0
    return { ...f, telemetry: t };
  });
  const s = selectRgb(frames, 600, src, p);
  assert.equal(s.counts.noPose, 40);
  for (const e of s.entries) {
    assert.ok(e.position.every(Number.isFinite), JSON.stringify(e));
    assert.ok(e.rotation.flat().every(Number.isFinite));
  }
  assert.ok(s.selected.every((i) => ![1, 2, 3, 4].includes(i % 9)));
  assert.ok(s.selected.some((i) => i % 9 === 5));
  assert.equal(finitePose({ position: {}, vehicle_attitude: {}, gimbal_attitude: {} }), false);
});

test('selectRgb and proposeRgbTarget across the antimeridian', () => {
  // the orbit centred 30 m west of 180 degrees: half its frames have longitudes near -180
  const centre = new LocalFrame(-17.5, 179.9997, 0);
  const local = new LocalFrame(-17.5, 179.9997, 600);
  const frames = orbit(300).map((f) => {
    const t = structuredClone(f.telemetry);
    const enu = new LocalFrame(-35.32, 148.83, 600).toEnu(
      t.position.lat_e7 / 1e7,
      t.position.lon_e7 / 1e7,
      t.position.alt_amsl_m,
    );
    let [lat, lon, alt] = local.toGeodetic(enu);
    if (lon >= 180) lon -= 360;
    t.position = {
      ...t.position,
      lat_e7: Math.round(lat * 1e7),
      lon_e7: Math.round(lon * 1e7),
      alt_amsl_m: alt,
    };
    return { ...f, telemetry: t };
  });
  assert.ok(frames.some((f) => f.telemetry.position.lon_e7 < 0));
  const s = selectRgb(
    frames,
    600,
    { width: 1280, height: 720, timescale: 90000 },
    {
      lat: -17.5,
      lon: 179.9997,
      target_alt: 0,
    },
  );
  assert.ok(s.entries.length > 20);
  for (const e of s.entries)
    assert.ok(Math.hypot(e.position[0], e.position[1]) < 160, JSON.stringify(e.position));
  const t = proposeRgbTarget(frames, 600);
  assert.ok(t.lon >= -180 && t.lon < 180);
  const d = centre.toEnu(t.lat, t.lon < 0 ? t.lon + 360 : t.lon, 0);
  assert.ok(Math.hypot(d[0], d[1]) < 1, JSON.stringify(t));
});

test("SPZ check: Spark's colour bytes near the rounding edges are within bounds", async (t) => {
  let spark;
  try {
    spark = await import('../../../viewer/node_modules/@sparkjsdev/spark/dist/spark.module.js');
  } catch {
    return t.skip('the splat viewer (Spark) is not installed');
  }
  const { decodeSpz, spzErrors } = await import('../../app/src/stages/export/spz.js');
  // coefficients from an RGB training run that Spark rounds to the neighbouring byte
  const vals = [2.5080981254577637, 3.2696807384490967, 2.5081164836883545, -2.2, 0.31, 5.0];
  const s = emptySplat(vals.length, 1);
  vals.forEach((v, i) => {
    s.sh0.set([v, v, v], 3 * i);
    s.quats.set([1, 0, 0, 0], 4 * i);
  });
  const { fileBytes } = await spark.transcodeSpz({
    inputs: [{ fileBytes: writePly(s), pathOrUrl: 'x.ply', fileType: 'ply' }],
    maxSh: 3,
  });
  const d = await decodeSpz(fileBytes);
  const e = spzErrors(s, d.splat, d.fractionalBits);
  assert.deepEqual(e.over, {});
  assert.equal(e.clamped.sh0, 3); // 5.0 is beyond SPZ's colour range
});
