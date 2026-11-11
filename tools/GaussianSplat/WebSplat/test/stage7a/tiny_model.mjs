// The stage 7a tiny known model: ten Gaussians with chosen positions, anisotropic scales,
// rotations (unnormalised quaternions), opacities, SH up to degree 3 and temperatures, plus the
// dataset.json / frames.json fields the export reads (a 12-view orbit, 4 hot views).
//   node tiny_model.mjs OUTDIR  ->  OUTDIR/{params.bin, dataset.json, frames.json, model.json}
import { mkdirSync, writeFileSync } from 'node:fs';
import { emptySplat, SH_C0, writeParams } from '../../app/src/stages/export/ply.js';

export const SCALE = { kind: 'log1p', t0_c: 10.906773567199707, t1_c: 126.42314147949219, k_c: 8 };
export const TARGET = [0, 0, 10];
const f32 = Math.fround;
const logit = (p) => Math.log(p / (1 - p));
const norm = (v) => Math.hypot(...v);

function quatMul([aw, ax, ay, az], [bw, bx, by, bz]) {
  return [
    aw * bw - ax * bx - ay * by - az * bz,
    aw * bx + ax * bw + ay * bz - az * by,
    aw * by - ax * bz + ay * bw + az * bx,
    aw * bz + ax * by - ay * bx + az * bw,
  ];
}
const axisAngle = (axis, deg) => {
  const h = (deg * Math.PI) / 360;
  const n = norm(axis);
  return [Math.cos(h), ...axis.map((a) => (Math.sin(h) * a) / n)];
};

// name, position, scales (m), quaternion w,x,y,z (unnormalised on purpose), opacity, value per
// channel (scaled temperature), shN coefficients (same in every channel), what it checks
export const GAUSSIANS = [
  { name: 'hot', pos: [0, 0, 10], s: [0.8, 0.8, 0.8], q: [1, 0, 0, 0], a: 0.995, v: 0.9 },
  {
    name: 'east',
    pos: [10.123, 0.0371, 9.987],
    s: [0.8, 0.8, 0.8],
    q: [2, 0, 0, 0],
    a: 0.99,
    v: 0.3,
  },
  { name: 'north', pos: [0, 10, 10], s: [0.8, 0.8, 0.8], q: [1, 0, 0, 0], a: 0.99, v: 0.55 },
  {
    name: 'needle',
    pos: [-10, 0, 10],
    s: [4, 0.4, 0.4],
    q: quatMul(axisAngle([0, 0, 1], 50), axisAngle([1, 0, 0], 30)).map((x) => 1.7 * x),
    a: 0.99,
    v: 0.7,
  },
  { name: 'half', pos: [0, -10, 10], s: [1, 1, 1], q: [1, 0, 0, 0], a: 0.7, v: 0.8 },
  {
    name: 'sh1',
    pos: [-10.25, -9.8125, 10.0004],
    s: [0.8, 0.8, 0.8],
    q: [1, 0, 0, 0],
    a: 0.99,
    v: 0.5,
    shN: [0.3, -0.2, 0.25],
  },
  {
    name: 'sh23',
    pos: [10, 10, 10],
    s: [0.8, 0.8, 0.8],
    q: [1, 0, 0, 0],
    a: 0.99,
    v: 0.45,
    shN: [0, 0, 0, 0.2, -0.15, 0.1, 0.25, -0.2, 0.1, -0.1, 0.15, 0.05, -0.12, 0.08, 0.1],
  },
  { name: 'high', pos: [0.31, -0.07, 22.5], s: [0.8, 0.8, 0.8], q: [1, 0, 0, 0], a: 0.99, v: 0.4 },
  {
    name: 'rgb',
    pos: [10, -10, 10],
    s: [0.8, 0.8, 0.8],
    q: [1, 0, 0, 0],
    a: 0.99,
    v: [0.2, 0.5, 0.8],
  },
  {
    name: 'disc',
    pos: [-10, 10, 10],
    s: [2.5, 0.6, 0.05],
    q: axisAngle([0.3, -0.8, 0.5], 70).map((x) => 0.6 * x),
    a: 0.99,
    v: 0.65,
  },
];

export function tinySplat() {
  const s = emptySplat(GAUSSIANS.length, 3);
  GAUSSIANS.forEach((g, i) => {
    s.means.set(g.pos, 3 * i);
    s.scales.set(g.s.map(Math.log), 3 * i);
    s.quats.set(g.q, 4 * i);
    s.opacities[i] = logit(g.a);
    const v = Array.isArray(g.v) ? g.v : [g.v, g.v, g.v];
    s.sh0.set(
      v.map((x) => (x - 0.5) / SH_C0),
      3 * i,
    );
    (g.shN ?? []).forEach((c, j) => s.shN.set([c, c, c], (i * 15 + j) * 3));
  });
  return s;
}

// cam_from_world looking from C at T (OpenCV axes, z up world), float32-representable
export function lookAt(C, T) {
  const f = T.map((t, i) => t - C[i]);
  const fn = norm(f);
  const fw = f.map((x) => x / fn);
  let r = [fw[1], -fw[0], 0]; // fw x up
  const rn = norm(r);
  r = r.map((x) => x / rn);
  const u = [r[1] * fw[2] - r[2] * fw[1], r[2] * fw[0] - r[0] * fw[2], r[0] * fw[1] - r[1] * fw[0]];
  const R = [r, u.map((x) => -x), fw].map((row) => row.map(f32));
  const t = R.map((row) => f32(-(row[0] * C[0] + row[1] * C[1] + row[2] * C[2])));
  return [...R[0], t[0], ...R[1], t[1], ...R[2], t[2], 0, 0, 0, 1];
}

export const WIDTH = 640;
export const HEIGHT = 512;
export const FOCAL = 1493.0;

export function orbit({ radius = 90, height = 60, n = 12 } = {}) {
  return Array.from({ length: n }, (_, i) => {
    const az = (i * 2 * Math.PI) / n;
    const C = [radius * Math.cos(az), radius * Math.sin(az), TARGET[2] + height];
    return { name: `${String(i).padStart(5, '0')}.png`, C, viewmat: lookAt(C, TARGET) };
  });
}

export function tinyDataset() {
  const views = orbit();
  return {
    format: 'websplat.dataset',
    version: 1,
    width: WIDTH,
    height: HEIGHT,
    K: [
      [FOCAL, 0, WIDTH / 2],
      [0, FOCAL, HEIGHT / 2],
      [0, 0, 1],
    ],
    scale: SCALE,
    images: views.map((v, i) => ({
      name: v.name,
      frame: i,
      split: 'train',
      hot: i >= 3 && i <= 6,
      viewmat: v.viewmat,
    })),
    hotspot: { enu: [...TARGET], frames: 4, median_ray_error_px: 0.5 },
    // prepare's bounds as the export reads them (the trim circle), radius 1.1 x 150 m
    bounds: { centre: [TARGET[0], TARGET[1]], radius: 165 },
  };
}

export const FRAMES = {
  format: 'websplat.frames',
  version: 1,
  origin: {
    lat: -35.57991111814981,
    lon: 148.9066523807906,
    alt_amsl: 978.445,
    frame: 'ENU, up from takeoff altitude',
  },
  target_enu: [...TARGET],
};

if (import.meta.url === `file://${process.argv[1]}`) {
  const out = process.argv[2];
  if (!out) throw new Error('usage: node tiny_model.mjs OUTDIR');
  mkdirSync(out, { recursive: true });
  writeFileSync(`${out}/params.bin`, writeParams(tinySplat()));
  writeFileSync(`${out}/dataset.json`, JSON.stringify(tinyDataset()));
  writeFileSync(`${out}/frames.json`, JSON.stringify(FRAMES));
  writeFileSync(
    `${out}/model.json`,
    JSON.stringify({ scale: SCALE, gaussians: GAUSSIANS }, null, 1),
  );
  console.log(`wrote the tiny model (${GAUSSIANS.length} Gaussians) to ${out}`);
}
