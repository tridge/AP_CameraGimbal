// Splat metadata as the native exporters write it: the PLY header's `comment apcg_splat {json}`
// (train.py save_ply, export_web.py write_ply) and the site meta.json (export_web.py).
import { pyJson } from './pyjson.js';

// fields that are Python floats natively (written 8.0, not 8)
export const FLOAT_FIELDS = [
  'target',
  'camera.position',
  'camera.target',
  'camera.fov_deg',
  'origin.lat',
  'origin.lon',
  'origin.alt_amsl',
  'temperature_scale.t0_c',
  'temperature_scale.t1_c',
  'temperature_scale.k_c',
  'hotspot.enu',
  'hotspot.median_ray_error_px',
  'trim_centre',
  'trim_radius',
];

// splat_common.trim_info: the viewer's trim circle from the training bounds {centre, radius}
// (Gaussians beyond radius were pruned; the fringe just inside it is poorly constrained)
export const TRIM_FRACTION = 0.85;
export function trimInfo(bounds) {
  if (!bounds?.radius) return {};
  return {
    trim_centre: [bounds.centre[0], bounds.centre[1]],
    trim_radius: TRIM_FRACTION * bounds.radius,
  };
}

export function apcgComment(info) {
  return `apcg_splat ${pyJson(info, { floats: FLOAT_FIELDS })}`;
}

export function metaJson(info) {
  return pyJson(info, { floats: FLOAT_FIELDS, indent: 1 });
}

export function toCelsius(scale) {
  if ((scale.kind ?? 'log1p') !== 'log1p') throw new Error(`temperature scale ${scale.kind}`);
  const norm = Math.log1p((scale.t1_c - scale.t0_c) / scale.k_c);
  return (v) => scale.t0_c + scale.k_c * Math.expm1(v * norm);
}

export function fromCelsius(scale) {
  const norm = Math.log1p((scale.t1_c - scale.t0_c) / scale.k_c);
  return (t) =>
    Math.min(1, Math.max(0, Math.log1p(Math.max(t - scale.t0_c, 0) / scale.k_c) / norm));
}

const sub = (a, b) => a.map((x, i) => x - b[i]);
const dot = (a, b) => a.reduce((s, x, i) => s + x * b[i], 0);
const norm = (a) => Math.sqrt(dot(a, a));

// export_web.py start_camera: the middle hot view (else the middle view) from cam_from_world
// viewmats [4x4 row-major]; looking nearly straight down, an oblique view from the same side
// view: the index of the view to start from (default export_web.py's choice)
export function startCamera({ viewmats, names, hot = [], K, height }, target, view = null) {
  const hotSet = new Set(hot);
  const hotIds = names.map((n, i) => (hotSet.has(n) ? i : -1)).filter((i) => i >= 0);
  const i = view ?? (hotIds.length ? hotIds[hotIds.length >> 1] : viewmats.length >> 1);
  const m = viewmats[i].flat();
  // c2w = inverse(viewmat): R^T, -R^T t
  const R = [0, 1, 2].map((r) => [m[4 * r], m[4 * r + 1], m[4 * r + 2]]);
  const t = [m[3], m[7], m[11]];
  const col = (c) => [R[c][0], R[c][1], R[c][2]]; // column c of R^T = row c of R
  const centre = [0, 1, 2].map((a) => -(R[0][a] * t[0] + R[1][a] * t[1] + R[2][a] * t[2]));
  const fov = (2 * Math.atan(height / 2 / K[1][1]) * 180) / Math.PI;
  const axis = col(2);
  if (axis[2] > -0.94) {
    const depth = Math.max(1, dot(sub(target, centre), axis));
    return { position: centre, target: centre.map((c, a) => c + axis[a] * depth), fov_deg: fov };
  }
  const dist = norm(sub(centre, target));
  let side = col(1)
    .slice(0, 2)
    .map((x) => -x);
  const sn = Math.max(norm(side), 1e-6);
  side = side.map((x) => x / sn);
  const el = (50 * Math.PI) / 180;
  const pos = [
    target[0] + dist * -side[0] * Math.cos(el),
    target[1] + dist * -side[1] * Math.cos(el),
    target[2] + dist * Math.sin(el),
  ];
  return { position: pos, target: [...target], fov_deg: fov };
}

// how the splat was trained: 'classic' (gsplat default, train.py; Brush's export renders as gsplat
// classic) adds the 0.3 px^2 blur without opacity compensation; the viewer renders it that way
export const RASTERIZE_MODE = 'classic';

// the result view's start (stage 7): the hot view whose recorded frame maximum is highest, the
// one where the hot source was seen best (frameMax: name -> maximum_c from frames.json); null
// (export_web.py's middle hot view) without hot views or frame maxima
export function hottestView({ names, hot = [] }, frameMax) {
  if (!frameMax) return null;
  let best = null;
  for (const n of hot) {
    const i = names.indexOf(n);
    if (
      i >= 0 &&
      Number.isFinite(frameMax[n]) &&
      (best === null || frameMax[n] > frameMax[names[best]])
    )
      best = i;
  }
  return best;
}

// train.py save_ply header info
export function trainInfo({ target, scale, origin, rasterizeMode = RASTERIZE_MODE, trim = {} }) {
  return {
    kind: 'thermal',
    up: [0, 0, 1],
    target,
    temperature_scale: scale,
    origin,
    rasterize_mode: rasterizeMode,
    ...trim,
  };
}

// export_web.py info for an RGB splat (train_rgb.py): colours as trained, no temperature scale
export function rgbWebInfo({
  count,
  shDegree,
  title,
  description = '',
  camera,
  target,
  origin,
  rasterizeMode = RASTERIZE_MODE,
  trim = {},
}) {
  return {
    kind: 'rgb',
    gaussians: count,
    sh_degree: shDegree,
    title,
    description,
    camera,
    up: [0, 0, 1],
    target,
    origin: origin ?? null,
    rasterize_mode: rasterizeMode,
    ...trim,
  };
}

// export_web.py info (PLY header and, with downloads, meta.json), thermal
export function webInfo({
  count,
  shDegree,
  title,
  description = '',
  camera,
  target,
  origin,
  scale,
  hotspot = null,
  colormap = 'ironbow',
  rasterizeMode = RASTERIZE_MODE,
  trim = {},
}) {
  if ((scale.kind ?? 'log1p') !== 'log1p') throw new Error(`temperature scale ${scale.kind}`);
  return {
    kind: 'thermal',
    gaussians: count,
    sh_degree: shDegree,
    title,
    description,
    camera,
    up: [0, 0, 1],
    target,
    origin: origin ?? null,
    rasterize_mode: rasterizeMode,
    ...trim,
    temperature_scale: scale,
    hotspot,
    colormap,
    display_range_c: [Math.floor(scale.t0_c), Math.ceil(scale.t1_c)],
  };
}
