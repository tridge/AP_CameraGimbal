/** Stage 5b core: train.py's scene loading (load_scene, choose_images, canopy_ceiling,
 * locate_hotspot) and its scene-relative training settings, on a COLMAP model and the raw
 * radiometric frames. IO-free: the worker and the Node tests supply readers and writers.
 * Arithmetic follows NumPy 2 promotion (float32 arrays with weak Python scalars stay float32)
 * and OpenCV 5.0 (opencv.js). Output: CONTRACTS.md "5b. Training dataset".
 */
import { readCameras, readImages, readPoints3D } from './colmap-model.js';
import { remapLinearReflect, undistortMaps, undistortPoint } from './opencv.js';
import { triangulateRays } from '../select/geometry.js';
import { median } from '../select/numeric.js';

const f = Math.fround;

// train.py argparse defaults: scene options, then training options
export const SCENE_DEFAULTS = {
  max_images: 500,
  hot_delta: 10.0,
  min_elevation: 20.0,
  min_obs: 30,
  max_pose_offset: 20.0,
  holdout: 20,
  hot_holdout: 8,
  k: 8.0,
  t0: null,
  t1: null,
  radius: 150,
  height_margin: 15.0,
  ceiling_cell: 5.0,
  ceiling_margin: 5.0,
};
export const TRAIN_DEFAULTS = {
  iterations: 30000,
  sh_degree: 3,
  ssim_weight: 0.2,
  hot_weight: 20.0,
  hot_tolerance: 0,
  cap: 800000,
  grow_grad2d: 0.0002,
  refine_every: 100,
  max_scale: 5.0,
  lobe: false,
  source_gain: false,
  source_radius: 3.0,
  gain_smooth: 1.0,
  gain_scope: 'source',
  pose_opt: false,
  appearance: false,
  focus_radius: 0.0,
  focus_enu: null,
  focus_points: 20000,
  focus_grow: 4.0,
};
// gsplat 1.5.3 DefaultStrategy defaults (train.py overrides some, see training())
const GSPLAT_DEFAULT_STRATEGY = {
  prune_opa: 0.005,
  grow_grad2d: 0.0002,
  grow_scale3d: 0.01,
  grow_scale2d: 0.05,
  prune_scale3d: 0.1,
  prune_scale2d: 0.15,
  refine_scale2d_stop_iter: 0,
  refine_start_iter: 500,
  refine_stop_iter: 15000,
  reset_every: 3000,
  refine_every: 100,
  pause_refine_after_reset: 0,
  absgrad: false,
  revised_opacity: false,
  verbose: false,
  key_for_gradient: 'means2d',
};

// ---- NumPy-exact helpers

/** np.percentile(float32 array, scalar q): weak q, float32 lerp. sorted: ascending Float32Array */
function percentileF32(sorted, q) {
  const n = sorted.length;
  const vi = (n - 1) * (q / 100);
  if (vi >= n - 1) return sorted[n - 1];
  const prev = Math.floor(vi);
  const t = vi - prev;
  const a = sorted[prev],
    b = sorted[prev + 1];
  const d = f(b - a);
  return t >= 0.5 ? f(b - f(d * f(1 - t))) : f(a + f(d * f(t)));
}

/** np.percentile(float64 values, q) (float64 lerp). sorted: ascending Float64Array */
function percentileF64(sorted, q) {
  const n = sorted.length;
  const vi = (n - 1) * (q / 100);
  if (vi >= n - 1) return sorted[n - 1];
  const prev = Math.floor(vi);
  const t = vi - prev;
  const a = sorted[prev],
    b = sorted[prev + 1];
  const d = b - a;
  return t >= 0.5 ? b - d * (1 - t) : a + d * t;
}

/** np.linspace(start, stop, num) */
function linspace(start, stop, num) {
  const div = num - 1;
  const step = (stop - start) / div;
  const y = [];
  for (let i = 0; i < num; i++) y.push(div > 0 ? i * step + start : start);
  if (num > 1) y[num - 1] = stop;
  return y;
}

// float32 -> float16 bits, round to nearest even (numpy astype(float16))
const f32buf = new Float32Array(1);
const u32buf = new Uint32Array(f32buf.buffer);
export function f16bits(v) {
  f32buf[0] = v;
  const x = u32buf[0];
  const sign = (x >>> 16) & 0x8000;
  const e = (x >>> 23) & 0xff;
  let m = x & 0x7fffff;
  if (e === 0xff) return sign | 0x7c00 | (m ? 0x200 : 0);
  const exp = e - 127 + 15;
  if (exp >= 0x1f) return sign | 0x7c00;
  if (exp <= 0) {
    if (exp < -10) return sign;
    m |= 0x800000;
    const shift = 14 - exp;
    const half = 1 << (shift - 1);
    const rem = m & ((1 << shift) - 1);
    let r = m >>> shift;
    if (rem > half || (rem === half && r & 1)) r++;
    return sign | r;
  }
  let r = (exp << 10) | (m >>> 13);
  const rem = m & 0x1fff;
  if (rem > 0x1000 || (rem === 0x1000 && r & 1)) r++;
  return sign | r;
}
export const F16 = (() => {
  const t = new Float32Array(65536);
  for (let h = 0; h < 65536; h++) {
    const s = h & 0x8000 ? -1 : 1;
    const e = (h >> 10) & 0x1f;
    const m = h & 0x3ff;
    t[h] =
      e === 0
        ? s * m * 2 ** -24
        : e === 31
          ? m
            ? NaN
            : s * Infinity
          : s * (1 + m / 1024) * 2 ** (e - 15);
  }
  return t;
})();

/** TempScale (train.py) */
export function tempScale(t0, t1, k) {
  const norm = Math.log1p((t1 - t0) / k);
  const t0f = f(t0),
    kf = f(k),
    nf = f(norm);
  return {
    t0,
    t1,
    k,
    norm,
    // forward on a float32 value: float32 arithmetic (weak Python scalars)
    forward32: (c) => {
      let x = f(c - t0f);
      if (x < 0) x = 0;
      x = f(x / kf);
      const v = f(f(Math.log1p(x)) / nf);
      return v < 0 ? 0 : v > 1 ? 1 : v;
    },
    // forward on a float64 value (np.array(python float))
    forward64: (c) => {
      const x = Math.max(c - t0, 0) / k;
      return Math.min(Math.max(Math.log1p(x) / norm, 0), 1);
    },
    todict: () => ({ kind: 'log1p', t0_c: t0, t1_c: t1, k_c: k }),
  };
}

// ---- train.py

function chooseImages(images, frames, o, target, log) {
  const skipped = { elevation: [], min_obs: [], pose_offset: [], budget: [] };
  const elevation = (im) => {
    const d = [0, 1, 2].map((i) => im.centre[i] - target[i]);
    return Math.atan2(d[2], Math.hypot(d[0], d[1])) * (180 / Math.PI);
  };
  let ims = [];
  for (const im of images) (elevation(im) >= o.min_elevation ? ims : skipped.elevation).push(im);
  for (const im of ims) if (im.numPoints3D < o.min_obs) skipped.min_obs.push(im);
  if (skipped.min_obs.length)
    log(`skipping ${skipped.min_obs.length} frames with fewer than ${o.min_obs} SfM observations`);
  ims = ims.filter((im) => im.numPoints3D >= o.min_obs);
  const offset = ims.map((im) => {
    const p = frames.get(im.name).position;
    return norm3(im.centre[0] - p[0], im.centre[1] - p[1], im.centre[2] - p[2]);
  });
  const limit = ims.length ? Math.max(o.max_pose_offset, 4 * median(offset)) : o.max_pose_offset;
  ims.forEach((im, k) => offset[k] > limit && skipped.pose_offset.push(im));
  if (skipped.pose_offset.length)
    log(
      `skipping ${skipped.pose_offset.length} frames more than ${limit.toFixed(0)} m from telemetry: ` +
        skipped.pose_offset.map((im) => im.name).join(', '),
    );
  ims = ims.filter((im, k) => offset[k] <= limit);
  const maxima = [...frames.values()].map((fr) => fr.maximum_c);
  const hot_c = median(maxima) + o.hot_delta;
  const hot = ims.filter((im) => frames.get(im.name).maximum_c > hot_c);
  let cold = ims.filter((im) => frames.get(im.name).maximum_c <= hot_c);
  const nCold = o.max_images ? Math.max(0, o.max_images - hot.length) : cold.length;
  if (nCold < cold.length) {
    const keep = linspace(0, cold.length - 1, nCold).map((x) => cold[Math.trunc(x)]);
    const kept = new Set(keep);
    skipped.budget = cold.filter((im) => !kept.has(im));
    cold = keep;
  }
  const byName = (a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0);
  const chosen = [...hot, ...cold].sort(byName);
  const test = new Set();
  if (o.holdout)
    for (let i = Math.floor(o.holdout / 2); i < cold.length; i += o.holdout) test.add(cold[i].name);
  if (o.hot_holdout)
    for (let i = Math.floor(o.hot_holdout / 2); i < hot.length; i += o.hot_holdout)
      test.add(hot[i].name);
  const names = (l) => l.map((im) => im.name);
  return {
    chosen,
    test,
    hot: new Set(names(hot)),
    hot_c,
    limit,
    skipped: Object.fromEntries(Object.entries(skipped).map(([k, v]) => [k, names(v)])),
  };
}

/** train.canopy_ceiling over near points (Float64Array xyz [n*3]) */
function canopyCeiling(near, centre, radius, cell, margin, deflt) {
  const n = Math.ceil((2 * radius) / cell);
  const origin = [centre[0] - radius, centre[1] - radius];
  const count = near.length / 3;
  const cells = new Map();
  const clip = (v) => (v < 0 ? 0 : v > n - 1 ? n - 1 : v);
  for (let p = 0; p < count; p++) {
    const i = clip(Math.trunc((near[3 * p] - origin[0]) / cell));
    const j = clip(Math.trunc((near[3 * p + 1] - origin[1]) / cell));
    const key = i * n + j;
    let l = cells.get(key);
    if (!l) cells.set(key, (l = []));
    l.push(near[3 * p + 2]);
  }
  const grid = new Float64Array(n * n).fill(-1e9); // nan_to_num(nan=-1e9)
  for (const [key, zs] of cells)
    if (zs.length >= 10) grid[key] = percentileF64(Float64Array.from(zs).sort(), 90);
  // scipy maximum_filter size 3, mode reflect (= max over the in-grid neighbours)
  const out = new Float32Array(n * n);
  for (let i = 0; i < n; i++)
    for (let j = 0; j < n; j++) {
      let m = -Infinity;
      for (let a = Math.max(0, i - 1); a <= Math.min(n - 1, i + 1); a++)
        for (let b = Math.max(0, j - 1); b <= Math.min(n - 1, j + 1); b++)
          m = Math.max(m, grid[a * n + b]);
      out[i * n + j] = Math.min(m < -1e8 ? deflt : m + margin, deflt);
    }
  return { origin, cell, n, grid: out };
}

function ceilingAt(c, x, y) {
  const clip = (v) => (v < 0 ? 0 : v > c.n - 1 ? c.n - 1 : v);
  const i = clip(Math.trunc((x - c.origin[0]) / c.cell));
  const j = clip(Math.trunc((y - c.origin[1]) / c.cell));
  return c.grid[i * c.n + j];
}

/** cam_from_world 4x4 as train.py's float32 viewmats tensor (row-major, Math.fround values) */
function viewmat(im) {
  const R = im.R,
    t = im.t;
  return [...R[0], t[0], ...R[1], t[1], ...R[2], t[2], 0, 0, 0, 1].map(f);
}

/** world_from_cam of a float32 viewmat (np.linalg.inv in float32, here float64 then rounded) */
function camToWorld(vm) {
  const R = [
    [vm[0], vm[1], vm[2]],
    [vm[4], vm[5], vm[6]],
    [vm[8], vm[9], vm[10]],
  ];
  const t = [vm[3], vm[7], vm[11]];
  const Rt = [0, 1, 2].map((i) => [0, 1, 2].map((j) => f(R[j][i])));
  const C = [0, 1, 2].map((i) => f(-(R[0][i] * t[0] + R[1][i] * t[1] + R[2][i] * t[2])));
  return { Rt, C };
}

/** train.locate_hotspot from argmax pixels (flat index in each hot view) */
function locateHotspot(argmax, viewmats, K, W) {
  if (argmax.length < 5) return null;
  const [fx, cx, cy] = [K[0][0], K[0][2], K[1][2]];
  const fy = K[1][1];
  const centres = [],
    dirs = [];
  for (const [n, flat] of argmax) {
    const { Rt, C } = camToWorld(viewmats[n]);
    const px = flat % W,
      py = Math.floor(flat / W);
    const ray = [(px - cx) / fx, (py - cy) / fy, 1];
    const d = Rt.map((r) => r[0] * ray[0] + r[1] * ray[1] + r[2] * ray[2]);
    const len = Math.hypot(...d);
    centres.push(C);
    dirs.push(d.map((v) => v / len));
  }
  const [X, err] = triangulateRays(centres, dirs, fx);
  return { enu: X, median_ray_error_px: err, frames: argmax.length };
}

/** train.py's settings from the scene: learning rates (means relative to the camera extent),
 * the means schedule and gsplat DefaultStrategy densification/pruning. */
export function training(o, extent, v_hot) {
  const lr = {
    means: 1.6e-4 * extent,
    scales: 5e-3,
    quats: 1e-3,
    opacities: 5e-2,
    sh0: 2.5e-3,
    shN: 2.5e-3 / 20,
  };
  if (o.lobe) Object.assign(lr, { lobe_amp: 1e-2, lobe_dir: 2e-3, lobe_kappa: 1e-2 });
  return {
    options: Object.fromEntries(Object.keys(TRAIN_DEFAULTS).map((k) => [k, o[k]])),
    v_hot,
    lr,
    adam_eps: 1e-15,
    means_lr_gamma: 0.01 ** (1 / o.iterations),
    strategy: {
      ...GSPLAT_DEFAULT_STRATEGY,
      refine_stop_iter: Math.floor(o.iterations / 2),
      reset_every: 3000,
      absgrad: true,
      grow_grad2d: o.grow_grad2d,
      refine_every: o.refine_every,
      verbose: false,
    },
    scene_scale: o.max_scale * 10,
    // fixed in train.py's code (documented, not derived)
    init: { opacity: 0.1, knn: 4, min_scale: 1e-3, quats: 'torch.rand', seed: 0 },
    sh_degree_schedule: 'min(step // 1000, sh_degree)',
    near_plane: 1.0,
    prune_outside_bounds_every: o.refine_every,
  };
}

/**
 * io: {info (frames.json), model: {cameras, images, points3D} (ArrayBuffer/views),
 *      readThermal(pos) -> Uint16Array [H*W] of frames list position pos,
 *      writeTarget(n, Uint16Array f16 bits [H*W]) (chosen image n, in order),
 *      progress(done, total), checkStop(), log(text), onCelsius(n, Float32Array) (optional:
 *      the undistorted Celsius image train.py maps, for tests)}
 * options: train.py option values (SCENE_DEFAULTS, TRAIN_DEFAULTS names)
 * returns {dataset (dataset.json without file names filled by the caller), points Float32Array
 *          [count*4], ceiling Float32Array [n*n], timings}
 */
export async function prepareScene(io, options = {}) {
  const o = { ...SCENE_DEFAULTS, ...TRAIN_DEFAULTS, ...options };
  const log = io.log ?? (() => {});
  const checkStop = io.checkStop ?? (() => {});
  const T = { start: performance.now() };
  const info = io.info;
  const frames = new Map(info.frames.map((fr, pos) => [fr.name, { ...fr, pos }]));
  const target = info.target_enu;
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
  const maps = k1 != null ? undistortMaps(fx, fy, cx, cy, k1, W, H) : null;
  const { images: allImages, xy } = readImages(io.model.images);
  for (const im of allImages)
    if (!frames.has(im.name)) throw new Error(`model image ${im.name} is not in frames.json`);
  allImages.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
  const sel = chooseImages(allImages, frames, o, target, log);
  const images = sel.chosen;
  const nImg = images.length;
  log(
    `${nImg} of ${allImages.length} registered images (${sel.hot.size} hot above ` +
      `${sel.hot_c.toFixed(1)} C), ${sel.test.size} held out`,
  );
  if (!nImg) throw new Error('no usable images');
  const imageById = new Map(allImages.map((im) => [im.id, im]));
  const index = new Map(images.map((im, n) => [im.id, n]));
  const viewmats = images.map(viewmat);

  // celsius (float32, splat_common.raw_to_celsius) undistorted with the refined camera
  const celsius = async (n) => {
    const fr = frames.get(images[n].name);
    const raw = await io.readThermal(fr.pos);
    if (raw.length !== W * H) throw new Error(`thermal frame ${fr.name}: ${raw.length} pixels`);
    const s = f(fr.temperature_scale_k),
      off = f(fr.temperature_offset_k),
      k273 = f(273.15);
    const c = new Float32Array(W * H);
    for (let i = 0; i < W * H; i++) c[i] = f(f(raw[i] * s) + off) - k273;
    return maps ? remapLinearReflect(c, W, H, maps.mapx, maps.mapy) : c;
  };
  const P1 = 0.3; // progress share of the first pass
  T.load = performance.now();

  // pass 1: the scale's sample and maximum
  const rows = Math.ceil(H / 8),
    cols = Math.ceil(W / 8);
  const sample = new Float32Array(nImg * rows * cols);
  let t1max = -Infinity;
  for (let n = 0; n < nImg; n++) {
    checkStop();
    const c = await celsius(n);
    let s = n * rows * cols;
    for (let y = 0; y < H; y += 8) for (let x = 0; x < W; x += 8) sample[s++] = c[y * W + x];
    for (let i = 0; i < c.length; i++) if (c[i] > t1max) t1max = c[i];
    io.progress?.(P1 * (n + 1), nImg);
  }
  sample.sort();
  const t0 = o.t0 ?? percentileF32(sample, 0.5) - 2;
  const t1 = o.t1 ?? t1max;
  const scale = tempScale(t0, t1, o.k);
  log(`temperature scale: ${t0.toFixed(1)} .. ${t1.toFixed(1)} C, knee ${o.k.toFixed(1)} C`);
  T.pass1 = performance.now();

  // scene bounds from the model points near the target
  const pts3 = readPoints3D(io.model.points3D);
  const P = pts3.xyz;
  const dist2 = (p) => Math.sqrt((P[3 * p] - target[0]) ** 2 + (P[3 * p + 1] - target[1]) ** 2);
  const nearIdx = [];
  for (let p = 0; p < pts3.count; p++) if (dist2(p) <= o.radius * 1.2) nearIdx.push(p);
  if (!nearIdx.length) throw new Error(`no sparse points within ${o.radius * 1.2} m of the target`);
  const near = new Float64Array(nearIdx.length * 3);
  nearIdx.forEach((p, k) => near.set(P.subarray(3 * p, 3 * p + 3), 3 * k));
  const zs = Float64Array.from(nearIdx, (p) => P[3 * p + 2]).sort();
  const [zLo, zHi] = [percentileF64(zs, 1), percentileF64(zs, 99)];
  const bounds = {
    centre: [target[0], target[1]],
    radius: o.radius * 1.1,
    z_min: zLo - o.height_margin,
    z_max: zHi + o.height_margin,
  };
  const ceiling = canopyCeiling(
    near,
    target,
    o.radius * 1.2,
    o.ceiling_cell,
    o.ceiling_margin,
    bounds.z_max,
  );
  const sortedGrid = Float64Array.from(ceiling.grid).sort();
  log(
    `scene bounds: radius ${bounds.radius.toFixed(0)} m, z ${bounds.z_min.toFixed(1)} .. ` +
      `${bounds.z_max.toFixed(1)} m, canopy ceiling median ${median(sortedGrid).toFixed(1)} m`,
  );

  // initial points inside the bounds and their observations in the chosen images
  const keepPts = [];
  const obsByImage = images.map(() => []); // [point slot, x, y]
  const unseen = [];
  for (let p = 0; p < pts3.count; p++) {
    const x = P[3 * p],
      y = P[3 * p + 1],
      z = P[3 * p + 2];
    if (dist2(p) > o.radius || !(bounds.z_min <= z && z <= ceilingAt(ceiling, x, y))) continue;
    const slot = keepPts.length;
    keepPts.push(p);
    let seen = 0;
    for (let e = pts3.trackStart[p]; e < pts3.trackStart[p + 1] && seen < 4; e++) {
      const n = index.get(pts3.trackImage[e]);
      if (n == null) continue;
      seen++;
      let [u, v] = xy(imageById.get(pts3.trackImage[e]), pts3.trackPoint2D[e]);
      if (k1 != null) [u, v] = undistortPoint(u, v, fx, fy, cx, cy, k1);
      obsByImage[n].push(slot, u, v);
    }
    if (!seen) unseen.push(slot);
  }
  if (keepPts.length < 100)
    throw new Error(`only ${keepPts.length} sparse points within ${o.radius} m of the target`);
  if (unseen.length) {
    // points no chosen frame observed: project into the (undistorted) chosen frames
    const hits = new Uint8Array(unseen.length);
    viewmats.forEach((vm, n) => {
      unseen.forEach((slot, i) => {
        if (hits[i] >= 4) return;
        const p = keepPts[slot];
        const X = P[3 * p],
          Y = P[3 * p + 1],
          Z = P[3 * p + 2];
        const xc = X * vm[0] + Y * vm[1] + Z * vm[2] + vm[3];
        const yc = X * vm[4] + Y * vm[5] + Z * vm[6] + vm[7];
        const zc = X * vm[8] + Y * vm[9] + Z * vm[10] + vm[11];
        const u = (fx * xc) / Math.max(zc, 1e-6) + cx;
        const v = (fy * yc) / Math.max(zc, 1e-6) + cy;
        if (zc > 1 && u >= 0 && u < W && v >= 0 && v < H) {
          hits[i]++;
          obsByImage[n].push(slot, u, v);
        }
      });
    });
    log(`${unseen.length} points without observations valued by projection`);
  }
  T.points = performance.now();

  // pass 2: targets (float16 of the scaled temperature), point values, hot spot pixels
  const sums = new Float64Array(keepPts.length);
  const counts = new Uint32Array(keepPts.length);
  const argmax = [];
  const bits = new Uint16Array(W * H);
  for (let n = 0; n < nImg; n++) {
    checkStop();
    const c = await celsius(n);
    io.onCelsius?.(n, c);
    let best = -1,
      bestAt = 0;
    for (let i = 0; i < c.length; i++) {
      const h = (bits[i] = f16bits(scale.forward32(c[i])));
      if (h > best) ((best = h), (bestAt = i)); // non-negative halves order as their bits
    }
    const ob = obsByImage[n];
    for (let k = 0; k < ob.length; k += 3) {
      // np.round: half to even
      const xi = roundHalfEven(ob[k + 1]),
        yi = roundHalfEven(ob[k + 2]);
      if (xi >= 0 && xi < W && yi >= 0 && yi < H) {
        sums[ob[k]] += F16[bits[yi * W + xi]];
        counts[ob[k]]++;
      }
    }
    if (sel.hot.has(images[n].name)) argmax.push([n, bestAt]);
    await io.writeTarget(n, bits);
    io.progress?.(P1 * nImg + (1 - P1) * (n + 1), nImg);
  }
  T.pass2 = performance.now();
  const kept = [];
  for (let s = 0; s < keepPts.length; s++) if (counts[s] > 0) kept.push(s);
  const points = new Float32Array(kept.length * 4);
  kept.forEach((s, k) => {
    const p = keepPts[s];
    points[4 * k] = P[3 * p];
    points[4 * k + 1] = P[3 * p + 1];
    points[4 * k + 2] = P[3 * p + 2];
    points[4 * k + 3] = sums[s] / counts[s];
  });
  log(`${nImg} images ${W}x${H}, ${kept.length} initial points`);
  const hotspot = locateHotspot(argmax, viewmats, K, W);
  if (hotspot)
    log(
      `hot spot from SfM poses at ENU ${hotspot.enu.map((v) => v.toFixed(1)).join(' ')} m ` +
        `(${hotspot.frames} frames, median ray error ${hotspot.median_ray_error_px.toFixed(1)} px)`,
    );

  const centres = images.map((im) => im.centre);
  const mean = [0, 1, 2].map((i) => centres.reduce((s, c) => s + c[i], 0) / nImg);
  const extent = Math.max(
    ...centres.map((c) => norm3(c[0] - mean[0], c[1] - mean[1], c[2] - mean[2])),
  );
  const v_hot = scale.forward64(sel.hot_c);
  const train = training(o, extent, v_hot);
  const dataset = {
    format: 'websplat.dataset',
    version: 1,
    width: W,
    height: H,
    K,
    undistortion:
      k1 != null
        ? {
            from: { model: cam.model, params: cam.params },
            method:
              'cv2.initUndistortRectifyMap(K, [k1,0,0,0], newK=K) + remap INTER_LINEAR, BORDER_REFLECT, on celsius float32',
          }
        : null,
    scale: scale.todict(),
    hot_c: sel.hot_c,
    v_hot,
    images: images.map((im, n) => ({
      name: im.name,
      frame: frames.get(im.name).pos,
      image_id: im.id,
      split: sel.test.has(im.name) ? 'test' : 'train',
      hot: sel.hot.has(im.name),
      viewmat: viewmats[n],
    })),
    skipped: { ...sel.skipped, pose_offset_limit_m: sel.limit },
    bounds: {
      ...bounds,
      ceiling: { origin: ceiling.origin, cell: ceiling.cell, n: ceiling.n, file: 'ceiling.f32' },
    },
    hotspot,
    extent_m: extent,
    lr: train.lr,
    training: train,
    targets: { file: 'targets.f16', dtype: 'f16', shape: [nImg, H, W] },
    points: { count: kept.length, file: 'points.f32', dtype: 'f32', shape: [kept.length, 4] },
    options: Object.fromEntries(Object.keys(SCENE_DEFAULTS).map((k) => [k, o[k]])),
    stats: {
      registered: allImages.length,
      hot: sel.hot.size,
      test: sel.test.size,
      points_in_bounds: keepPts.length,
      points_unseen: unseen.length,
    },
  };
  const ms = (a, b) => Math.round(T[b] - T[a]);
  return {
    dataset,
    points,
    ceiling: ceiling.grid,
    timings: {
      loadMs: ms('start', 'load'),
      pass1Ms: ms('load', 'pass1'),
      pointsMs: ms('pass1', 'points'),
      pass2Ms: ms('points', 'pass2'),
      totalMs: Math.round(performance.now() - T.start),
    },
  };
}

// np.linalg.norm: sqrt of the sum of squares (not hypot)
const norm3 = (a, b, c) => Math.sqrt(a * a + b * b + c * c);

function roundHalfEven(x) {
  const r = Math.round(x);
  return r - x === 0.5 && r % 2 !== 0 ? r - 1 : r;
}
