/** Stage 5b for RGB recordings: train_rgb.py's scene setup on the map stage's model and select's
 * RGB frames. Images are undistorted to the pinhole camera (cv2.initUndistortRectifyMap + remap
 * INTER_LINEAR, constant border, as train_rgb.py; frames are already at the processing width),
 * initial points are the model's within `radius` of its median point, coloured from the frames
 * that observed them, and the bounds are train_rgb.py's radius and height ceiling. IO-free.
 */
import { readCameras, readImages, readPoints3D } from './colmap-model.js';
import { undistortMaps } from './opencv.js';
import { TRAIN_DEFAULTS, canopyCeiling, training } from './scene.js';
import { median } from '../select/numeric.js';

const f = Math.fround;

// train_rgb.py argparse defaults (scene and training); images limited by pixels (Brush keeps
// every training image in memory)
export const RGB_SCENE_DEFAULTS = {
  holdout: 25,
  radius: 200.0,
  ceiling_cell: 5.0,
  ceiling_margin: 5.0,
  max_scale: null, // null: radius / 100 or 16 ground pixels at the median camera distance
  max_pixels: 500 * 1280 * 720,
};
export const RGB_TRAIN_DEFAULTS = {
  iterations: 30000,
  sh_degree: 1,
  ssim_weight: 0.2,
  hot_weight: 0,
  cap: 3000000,
  grow_grad2d: 0.0008,
  refine_every: 100,
};

/** cv2.remap(src u8 [H, W, 3], mapx, mapy, INTER_LINEAR, BORDER_CONSTANT 0): fixed point with
 * 1/32 pixel weights (INTER_TAB_SIZE), as OpenCV does for 8-bit images. */
export function remapRgbLinear(src, W, H, mapx, mapy, out = new Uint8Array(W * H * 3)) {
  const at = (x, y, c) => (x < 0 || y < 0 || x >= W || y >= H ? 0 : src[(y * W + x) * 3 + c]);
  for (let i = 0; i < W * H; i++) {
    const sx = Math.round(mapx[i] * 32),
      sy = Math.round(mapy[i] * 32);
    const x0 = sx >> 5,
      y0 = sy >> 5;
    const ax = (sx & 31) / 32,
      ay = (sy & 31) / 32;
    // weights quantised as cv::initInterTab2D (sum 1 << 15)
    const w11 = Math.round(ax * ay * 32768);
    const w10 = Math.round((1 - ax) * ay * 32768);
    const w01 = Math.round(ax * (1 - ay) * 32768);
    const w00 = 32768 - w11 - w10 - w01;
    for (let c = 0; c < 3; c++) {
      const v =
        at(x0, y0, c) * w00 +
        at(x0 + 1, y0, c) * w01 +
        at(x0, y0 + 1, c) * w10 +
        at(x0 + 1, y0 + 1, c) * w11;
      out[i * 3 + c] = Math.min(255, (v + 16384) >> 15);
    }
  }
  return out;
}

// bilinear sample of RGB8 at COLMAP pixel coordinates (pixel centres at +0.5), 0..1
function sample(rgb, W, H, u, v, out) {
  const x = Math.min(W - 1, Math.max(0, u - 0.5)),
    y = Math.min(H - 1, Math.max(0, v - 0.5));
  const x0 = Math.floor(x),
    y0 = Math.floor(y);
  const x1 = Math.min(W - 1, x0 + 1),
    y1 = Math.min(H - 1, y0 + 1);
  const ax = x - x0,
    ay = y - y0;
  for (let c = 0; c < 3; c++) {
    const p = (xx, yy) => rgb[(yy * W + xx) * 3 + c];
    const a = p(x0, y0) + ax * (p(x1, y0) - p(x0, y0));
    const b = p(x0, y1) + ax * (p(x1, y1) - p(x0, y1));
    out[c] += (a + ay * (b - a)) / 255;
  }
}

const viewmat = (im) =>
  [...im.R[0], im.t[0], ...im.R[1], im.t[1], ...im.R[2], im.t[2], 0, 0, 0, 1].map(f);

const percentile = (sorted, q) => {
  const vi = (sorted.length - 1) * (q / 100);
  const k = Math.floor(vi);
  return k + 1 < sorted.length ? sorted[k] + (vi - k) * (sorted[k + 1] - sorted[k]) : sorted[k];
};

/** Evenly spread choice of m of n (all if m >= n). */
export function evenly(n, m) {
  if (m >= n) return Array.from({ length: n }, (_, i) => i);
  if (m <= 1) return [n >> 1];
  return Array.from({ length: m }, (_, k) => Math.round((k * (n - 1)) / (m - 1)));
}

/**
 * io: {info (frames.json), model {cameras, images, points3D}, readRgb(pos) -> Uint8Array
 *      [H*W*3] of frames list position pos, writeTarget(n, Uint8Array [H*W*3]) (chosen image n,
 *      in order), progress(done, total), checkStop(), log(text)}
 * returns {dataset, points Float32Array [count*6] (x, y, z, r, g, b), ceiling Float32Array}
 */
export async function prepareRgbScene(io, options = {}) {
  const o = { ...TRAIN_DEFAULTS, ...RGB_SCENE_DEFAULTS, ...RGB_TRAIN_DEFAULTS, ...options };
  const log = io.log ?? (() => {});
  const T0 = performance.now();
  const info = io.info;
  const pos = new Map(info.frames.map((fr, k) => [fr.name, k]));
  const cams = readCameras(io.model.cameras);
  if (cams.length !== 1) throw new Error(`expected one shared camera, got ${cams.length}`);
  const cam = cams[0];
  const { width: W, height: H } = cam;
  if (W !== info.width || H !== info.height)
    throw new Error(`camera ${W}x${H} differs from the frames ${info.width}x${info.height}`);
  let fx, fy, cx, cy, k1;
  if (cam.model === 'SIMPLE_RADIAL') (([fx, cx, cy, k1] = cam.params), (fy = fx));
  else if (cam.model === 'PINHOLE') (([fx, fy, cx, cy] = cam.params), (k1 = null));
  else throw new Error(`unsupported camera model ${cam.model}`);
  const K = [
    [fx, 0, cx],
    [0, fy, cy],
    [0, 0, 1],
  ];
  const maps = k1 ? undistortMaps(fx, fy, cx, cy, k1, W, H) : null;
  const { images: all } = readImages(io.model.images);
  for (const im of all)
    if (!pos.has(im.name)) throw new Error(`model image ${im.name} is not in frames.json`);
  all.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
  const maxImages = Math.max(3, Math.floor(o.max_pixels / (W * H)));
  const chosenIdx = evenly(all.length, maxImages);
  const images = chosenIdx.map((i) => all[i]);
  const chosen = new Set(images.map((im) => im.id));
  const n = images.length;
  if (n < 3) throw new Error(`only ${n} registered images`);
  const test = new Set();
  if (o.holdout) for (let i = o.holdout >> 1; i < n; i += o.holdout) test.add(i);
  log(
    `${n} of ${all.length} registered images` +
      (n < all.length ? ` (at most ${maxImages} at ${W}x${H})` : '') +
      `, ${test.size} held out`,
  );

  // initial points: within radius (horizontally) of the median model point
  const pts = readPoints3D(io.model.points3D);
  if (!pts.count) throw new Error('the model has no points');
  const axis = (a) => median(Array.from({ length: pts.count }, (_, p) => pts.xyz[3 * p + a]));
  const centre = [axis(0), axis(1), axis(2)];
  const keep = [];
  for (let p = 0; p < pts.count; p++)
    if (Math.hypot(pts.xyz[3 * p] - centre[0], pts.xyz[3 * p + 1] - centre[1]) < o.radius)
      keep.push(p);
  if (!keep.length) throw new Error(`no model points within ${o.radius} m of the model centre`);
  const near = new Float64Array(keep.length * 3);
  keep.forEach((p, k) => near.set(pts.xyz.subarray(3 * p, 3 * p + 3), 3 * k));
  const zs = Float64Array.from(keep, (p) => pts.xyz[3 * p + 2]).sort();
  const top = percentile(zs, 99.5) + o.ceiling_margin;
  const ceiling =
    o.ceiling_margin > 0
      ? canopyCeiling(near, centre, o.radius, o.ceiling_cell, o.ceiling_margin, top)
      : null;
  if (ceiling) log(`height ceiling: max ${top.toFixed(1)} m above takeoff`);

  // one pass over the registered images: point colours from their observations, targets of the
  // chosen ones
  const slot = new Map(keep.map((p, k) => [pts.ids[p], k]));
  const colour = new Float64Array(keep.length * 3);
  const seen = new Uint8Array(keep.length);
  const imgBuf = io.model.images;
  const dv = ArrayBuffer.isView(imgBuf)
    ? new DataView(imgBuf.buffer, imgBuf.byteOffset, imgBuf.byteLength)
    : new DataView(imgBuf);
  const acc = [0, 0, 0];
  let written = 0;
  for (let i = 0; i < all.length; i++) {
    io.checkStop?.();
    const im = all[i];
    const rgb = await io.readRgb(pos.get(im.name));
    if (rgb.length !== W * H * 3) throw new Error(`frame ${im.name}: ${rgb.length} bytes`);
    for (let k = 0; k < im.numPoints2D; k++) {
      const o2 = im.points2D + 24 * k;
      const lo = dv.getUint32(o2 + 16, true),
        hi = dv.getInt32(o2 + 20, true);
      if (hi === -1 && lo === 0xffffffff) continue;
      const s = slot.get(hi * 2 ** 32 + lo);
      if (s === undefined || seen[s] >= 3) continue;
      acc[0] = acc[1] = acc[2] = 0;
      sample(rgb, W, H, dv.getFloat64(o2, true), dv.getFloat64(o2 + 8, true), acc);
      for (let c = 0; c < 3; c++) colour[3 * s + c] += acc[c];
      seen[s]++;
    }
    if (chosen.has(im.id)) {
      await io.writeTarget(written++, maps ? remapRgbLinear(rgb, W, H, maps.mapx, maps.mapy) : rgb);
    }
    io.progress?.(i + 1, all.length);
  }
  const points = new Float32Array(keep.length * 6);
  keep.forEach((p, k) => {
    for (let a = 0; a < 3; a++) points[6 * k + a] = pts.xyz[3 * p + a];
    for (let c = 0; c < 3; c++) points[6 * k + 3 + c] = seen[k] ? colour[3 * k + c] / seen[k] : 0.5;
  });

  // train_rgb.py's settings: means LR from the camera extent, the too-big rule from max_scale
  const centres = images.map((im) => im.centre);
  const mean = [0, 1, 2].map((a) => centres.reduce((s, c) => s + c[a], 0) / n);
  const extent = Math.max(...centres.map((c) => Math.hypot(...c.map((v, a) => v - mean[a]))));
  const groundPx = median(centres.map((c) => Math.hypot(...c.map((v, a) => v - centre[a])))) / fx;
  const maxScale = o.max_scale ?? Math.max(o.radius / 100, 16 * groundPx);
  log(`${keep.length} initial points; pruning Gaussians larger than ${maxScale.toFixed(1)} m`);
  const train = training({ ...o, max_scale: maxScale }, extent, 1.0);
  const zMin = zs[0] - o.radius; // train_rgb.py has no floor
  const dataset = {
    format: 'websplat.dataset',
    version: 1,
    kind: 'rgb',
    width: W,
    height: H,
    K,
    undistortion: k1
      ? {
          from: { model: cam.model, params: cam.params },
          method:
            'cv2.initUndistortRectifyMap(K, [k1,0,0,0], newK=K) + remap INTER_LINEAR, BORDER_CONSTANT, on RGB8',
        }
      : null,
    images: images.map((im, k) => ({
      name: im.name,
      frame: pos.get(im.name),
      image_id: im.id,
      split: test.has(k) ? 'test' : 'train',
      hot: false,
      viewmat: viewmat(im),
    })),
    bounds: {
      centre: [centre[0], centre[1]],
      radius: o.radius,
      z_min: zMin,
      z_max: top,
      ceiling: ceiling
        ? { origin: ceiling.origin, cell: ceiling.cell, n: ceiling.n, file: 'ceiling.f32' }
        : {
            origin: [centre[0] - o.radius, centre[1] - o.radius],
            cell: 2 * o.radius,
            n: 1,
            file: 'ceiling.f32',
          },
    },
    hotspot: null,
    extent_m: extent,
    lr: train.lr,
    training: train,
    targets: { file: 'targets.u8', dtype: 'u8', shape: [n, H, W, 3] },
    points: { count: keep.length, file: 'points.f32', dtype: 'f32', shape: [keep.length, 6] },
    options: o,
    stats: { registered: all.length, test: test.size, points_in_bounds: keep.length },
  };
  return {
    dataset,
    points,
    ceiling: ceiling ? ceiling.grid : Float32Array.of(1e9),
    timings: { totalMs: Math.round(performance.now() - T0) },
  };
}
