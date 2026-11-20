// RGB scene preparation (prepare/rgb-scene.js) on a synthetic COLMAP model with SIMPLE_RADIAL
// (and RADIAL) distortion: intrinsics, undistorted targets against an independent
// OpenCV-equivalent map and bilinear reference, point colours from the observations, bounds and
// training settings.
// usage (WebSplat directory): node --test test/v2/*.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { prepareRgbScene } from '../../app/src/stages/prepare/rgb-scene.js';
import { boundsJson } from '../../app/src/stages/train/brush-dataset.js';

const W = 64,
  H = 48,
  F = 50,
  CX = 32.3,
  CY = 23.7,
  K1 = -0.12;

// little-endian COLMAP binary writer
class Bin {
  constructor() {
    this.parts = [];
  }
  u32(v) {
    const b = new DataView(new ArrayBuffer(4));
    b.setUint32(0, v, true);
    this.parts.push(new Uint8Array(b.buffer));
  }
  i32(v) {
    const b = new DataView(new ArrayBuffer(4));
    b.setInt32(0, v, true);
    this.parts.push(new Uint8Array(b.buffer));
  }
  u64(v) {
    const b = new DataView(new ArrayBuffer(8));
    b.setBigUint64(0, BigInt(v), true);
    this.parts.push(new Uint8Array(b.buffer));
  }
  i64(v) {
    const b = new DataView(new ArrayBuffer(8));
    b.setBigInt64(0, BigInt(v), true);
    this.parts.push(new Uint8Array(b.buffer));
  }
  f64(v) {
    const b = new DataView(new ArrayBuffer(8));
    b.setFloat64(0, v, true);
    this.parts.push(new Uint8Array(b.buffer));
  }
  u8(v) {
    this.parts.push(Uint8Array.of(v));
  }
  str(s) {
    this.parts.push(new TextEncoder().encode(s + '\0'));
  }
  bytes() {
    const out = new Uint8Array(this.parts.reduce((n, p) => n + p.length, 0));
    let o = 0;
    for (const p of this.parts) {
      out.set(p, o);
      o += p.length;
    }
    return out.buffer;
  }
}

// cameras straight down at height 60 (cam_from_world: R = diag(1, -1, -1), t = -R C)
const centres = [
  [-10, -5, 60],
  [10, -5, 60],
  [-10, 5, 60],
  [10, 5, 60],
];
const R = [
  [1, 0, 0],
  [0, -1, 0],
  [0, 0, -1],
];
const tOf = (C) => R.map((row) => -(row[0] * C[0] + row[1] * C[1] + row[2] * C[2]));
// distorted pixel of a world point (COLMAP SIMPLE_RADIAL)
function project(C, X) {
  const t = tOf(C);
  const x = R.map((row, i) => row[0] * X[0] + row[1] * X[1] + row[2] * X[2] + t[i]);
  const xn = x[0] / x[2],
    yn = x[1] / x[2];
  const d = 1 + K1 * (xn * xn + yn * yn);
  return [F * xn * d + CX, F * yn * d + CY];
}
const points = [];
for (let i = -4; i <= 4; i++)
  for (let j = -3; j <= 3; j++) points.push([3 * i, 3 * j, (i + j) % 3]);
// a far point outside the radius
points.push([500, 0, 0]);

// COLMAP model id 2 SIMPLE_RADIAL [f, cx, cy, k1], 3 RADIAL [f, cx, cy, k1, k2]
function model(id = 2, params = [F, CX, CY, K1]) {
  const cams = new Bin();
  cams.u64(1);
  cams.u32(1);
  cams.i32(id);
  cams.u64(W);
  cams.u64(H);
  for (const v of params) cams.f64(v);
  const imgs = new Bin();
  imgs.u64(centres.length);
  centres.forEach((C, k) => {
    imgs.u32(k + 1);
    for (const q of [0, 1, 0, 0]) imgs.f64(q); // 180 degrees about x
    for (const v of tOf(C)) imgs.f64(v);
    imgs.u32(1);
    imgs.str(`0000${k}.png`);
    imgs.u64(points.length);
    points.forEach((X, p) => {
      const [u, v] = project(C, X);
      imgs.f64(u);
      imgs.f64(v);
      imgs.i64(p + 1);
    });
  });
  const pts = new Bin();
  pts.u64(points.length);
  points.forEach((X, p) => {
    pts.u64(p + 1);
    for (const v of X) pts.f64(v);
    for (const c of [128, 128, 128]) pts.u8(c);
    pts.f64(0.5);
    pts.u64(centres.length);
    centres.forEach((_, k) => {
      pts.u32(k + 1);
      pts.u32(p);
    });
  });
  return { cameras: cams.bytes(), images: imgs.bytes(), points3D: pts.bytes() };
}

// a smooth synthetic image per frame
const pixel = (k, x, y) => [
  Math.min(255, 3 * x + 10 * k),
  Math.min(255, 4 * y + 5 * k),
  Math.min(255, 2 * (x + y)),
];
const image = (k) => {
  const out = new Uint8Array(W * H * 3);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) out.set(pixel(k, x, y), (y * W + x) * 3);
  return out;
};

// cv2.initUndistortRectifyMap (newK = K) and float bilinear remap with a black border
function reference(k, k2 = 0) {
  const src = image(k);
  const out = new Float64Array(W * H * 3);
  for (let i = 0; i < H; i++)
    for (let j = 0; j < W; j++) {
      const xn = (j - CX) / F,
        yn = (i - CY) / F;
      const r2 = xn * xn + yn * yn;
      const d = 1 + K1 * r2 + k2 * r2 * r2;
      const sx = F * xn * d + CX,
        sy = F * yn * d + CY;
      const x0 = Math.floor(sx),
        y0 = Math.floor(sy),
        ax = sx - x0,
        ay = sy - y0;
      for (let c = 0; c < 3; c++) {
        const at = (x, y) => (x < 0 || y < 0 || x >= W || y >= H ? 0 : src[(y * W + x) * 3 + c]);
        const a = at(x0, y0) * (1 - ax) + at(x0 + 1, y0) * ax;
        const b = at(x0, y0 + 1) * (1 - ax) + at(x0 + 1, y0 + 1) * ax;
        out[(i * W + j) * 3 + c] = a * (1 - ay) + b * ay;
      }
    }
  return out;
}

test('RGB scene preparation with SIMPLE_RADIAL distortion', async () => {
  const info = {
    width: W,
    height: H,
    frames: centres.map((_, k) => ({ name: `0000${k}.png` })),
  };
  const targets = [];
  const res = await prepareRgbScene(
    {
      info,
      model: model(),
      readRgb: async (pos) => image(pos),
      writeTarget: (n, bytes) => targets.push([n, bytes.slice()]),
    },
    { holdout: 2, radius: 50 },
  );
  const ds = res.dataset;
  assert.equal(ds.kind, 'rgb');
  assert.deepEqual(ds.K, [
    [F, 0, CX],
    [0, F, CY],
    [0, 0, 1],
  ]);
  assert.deepEqual(ds.undistortion.from, { model: 'SIMPLE_RADIAL', params: [F, CX, CY, K1] });
  assert.deepEqual(
    ds.images.map((im) => [im.name, im.split]),
    [
      ['00000.png', 'train'],
      ['00001.png', 'test'],
      ['00002.png', 'train'],
      ['00003.png', 'test'],
    ],
  );
  // viewmat: cam_from_world, float32
  assert.deepEqual(ds.images[1].viewmat.slice(0, 4), [1, 0, 0, -10]);
  // undistorted targets: OpenCV's 1/32 pixel fixed-point remap within one level of the float one
  assert.deepEqual(
    targets.map(([n]) => n),
    [0, 1, 2, 3],
  );
  for (const [n, bytes] of targets) {
    const ref = reference(n);
    let worst = 0;
    for (let i = 0; i < ref.length; i++) worst = Math.max(worst, Math.abs(bytes[i] - ref[i]));
    assert.ok(worst <= 2, `image ${n}: worst ${worst}`);
    // the distortion moves edge pixels: not the identity
    assert.ok(bytes.some((v, i) => Math.abs(v - image(n)[i]) > 3));
  }
  // initial points: the 63 within the radius, coloured as the mean of their first 3 observations
  assert.equal(ds.points.count, 63);
  const p = res.points;
  const X = points[0];
  const want = [0, 0, 0];
  for (let k = 0; k < 3; k++) {
    const [u, v] = project(centres[k], X);
    pixel(k, u - 0.5, v - 0.5).forEach((c, i) => (want[i] += c / 255 / 3));
  }
  assert.deepEqual([...p.slice(0, 3)], X.map(Math.fround));
  for (let c = 0; c < 3; c++)
    assert.ok(Math.abs(p[3 + c] - want[c]) < 0.01, `${p[3 + c]} ${want[c]}`);
  // bounds and train_rgb.py's settings
  assert.equal(ds.bounds.radius, 50);
  assert.ok(ds.bounds.z_max > 2 && ds.bounds.z_max < 10, ds.bounds.z_max);
  assert.equal(ds.training.options.sh_degree, 1);
  assert.equal(ds.training.options.ssim_weight, 0.2);
  assert.equal(ds.training.options.hot_weight, 0);
  // max_scale: radius / 100 or 16 ground pixels at the median camera distance, the larger
  const dist = Math.hypot(10, 5, 60 - 0);
  assert.ok(Math.abs(ds.training.scene_scale - 10 * Math.max(0.5, (16 * dist) / F)) < 0.5);
  assert.equal(res.ceiling.length, ds.bounds.ceiling.n ** 2);
  // train_rgb.py's too_tall: max(1, max_scale / 2) from half way; Brush gets it in bounds.json
  assert.equal(ds.bounds.max_vertical, Math.max(1, ds.training.scene_scale / 20));
  assert.equal(ds.bounds.vertical_prune_from, 15000);
  const bj = boundsJson(ds, res.ceiling);
  assert.equal(bj.max_vertical, ds.bounds.max_vertical);
  assert.equal(bj.vertical_prune_from, 15000);
  // the gsplat too-big window is unchanged (train_rgb.py: reset_every < step < refine_stop_iter)
  assert.deepEqual([bj.scale_prune_from, bj.scale_prune_until], [3001, 15000]);
  // thermal datasets have no vertical rule
  const { max_vertical: _, vertical_prune_from: __, ...thermalBounds } = ds.bounds;
  const tj = boundsJson({ ...ds, bounds: thermalBounds }, res.ceiling);
  assert.ok(!('max_vertical' in tj) && !('vertical_prune_from' in tj));
  // max_vertical 0 keeps tall Gaussians
  const off = await prepareRgbScene(
    { info, model: model(), readRgb: async (pos) => image(pos), writeTarget: () => {} },
    { holdout: 2, radius: 50, max_vertical: 0 },
  );
  assert.equal(off.dataset.bounds.max_vertical, null);
  assert.ok(!('max_vertical' in boundsJson(off.dataset, off.ceiling)));
});

test('max_height caps the initial points and the ceiling above the target', async () => {
  // the target 1 m above takeoff: points (z up to 2) above 1 + 0.5 go, and so does the ceiling
  const info = {
    width: W,
    height: H,
    target_enu: [0, 0, 1],
    frames: centres.map((_, k) => ({ name: `0000${k}.png` })),
  };
  const io = { info, model: model(), readRgb: async (pos) => image(pos), writeTarget: () => {} };
  const all = await prepareRgbScene(io, { holdout: 2, radius: 50 });
  const res = await prepareRgbScene(io, { holdout: 2, radius: 50, max_height: 0.5 });
  const kept = points.filter((X) => Math.hypot(X[0], X[1]) < 50 && X[2] <= 1.5).length;
  assert.ok(kept < all.dataset.points.count);
  assert.equal(res.dataset.points.count, kept);
  assert.ok(Math.max(...res.ceiling) <= 1.5);
  assert.ok(Math.max(...all.ceiling) > 1.5);
  assert.equal(res.dataset.bounds.z_max, 1.5);
  assert.equal(boundsJson(res.dataset, res.ceiling).ceiling.every((z) => z <= 1.5), true);
});

test("centre 'target': the radius, points and bounds about the target, not the model median", async () => {
  const info = {
    width: W,
    height: H,
    target_enu: [6, 3, 0],
    frames: centres.map((_, k) => ({ name: `0000${k}.png` })),
  };
  const io = { info, model: model(), readRgb: async (pos) => image(pos), writeTarget: () => {} };
  const res = await prepareRgbScene(io, { holdout: 2, radius: 4, centre: 'target' });
  // the grid points within 4 m of (6, 3): itself and its four 3 m neighbours
  assert.equal(res.dataset.points.count, 5);
  for (let k = 0; k < 5; k++)
    assert.ok(Math.hypot(res.points[6 * k] - 6, res.points[6 * k + 1] - 3) < 4);
  assert.deepEqual(res.dataset.bounds.centre, [6, 3]);
  const med = await prepareRgbScene(io, { holdout: 2, radius: 4 });
  assert.deepEqual(med.dataset.bounds.centre, [0, 0]);
  await assert.rejects(prepareRgbScene(io, { radius: 4, centre: 'middle' }), /centre middle/);
});

test('RGB scene preparation with RADIAL distortion (sfm.py --camera-model RADIAL)', async () => {
  const K2 = -0.2;
  const info = { width: W, height: H, frames: centres.map((_, k) => ({ name: `0000${k}.png` })) };
  const targets = [];
  const res = await prepareRgbScene(
    {
      info,
      model: model(3, [F, CX, CY, K1, K2]),
      readRgb: async (pos) => image(pos),
      writeTarget: (n, bytes) => targets.push([n, bytes.slice()]),
    },
    { holdout: 2, radius: 50 },
  );
  assert.deepEqual(res.dataset.undistortion.from, {
    model: 'RADIAL',
    params: [F, CX, CY, K1, K2],
  });
  for (const [n, bytes] of targets) {
    const ref = reference(n, K2);
    const plain = reference(n);
    let worst = 0,
      apart = 0;
    for (let i = 0; i < ref.length; i++) {
      worst = Math.max(worst, Math.abs(bytes[i] - ref[i]));
      apart = Math.max(apart, Math.abs(ref[i] - plain[i]));
    }
    assert.ok(worst <= 2, `image ${n}: worst ${worst}`);
    assert.ok(apart > 3, 'k2 changes the undistortion');
  }
});
