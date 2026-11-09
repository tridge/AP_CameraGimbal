// Stage 6: the Brush fork's input directory from stage 5b's dataset (CONTRACTS.md 5b), plus the
// Brush options that carry train.py's training settings. Used by the train stage worker and by
// the Node tests (test/stage6/); file writing and compression are injected.
//
//   sparse/0/cameras.txt, images.txt, points3D.txt   PINHOLE K, each view's cam_from_world
//   images/NAME.png    16-bit grayscale round(65535 * v) of the float16 target v (no gamma)
//   init.ply           train.py init_params: points, SH DC (v - 0.5) / SH_C0, kNN log scales,
//                      opacity logit(0.1), uniform [0, 1) quaternions (seeded; not torch's draws)
//   eval_views.txt     the 'test' split (frozen holdouts)
//   bounds.json        train.py's spatial pruning region and gsplat's too-big rule
export const SH_C0 = 0.28209479177387814;

// float16 bits -> float32 (exact)
export function halfToFloat(h) {
  const s = h & 0x8000 ? -1 : 1;
  const e = (h >> 10) & 0x1f;
  const m = h & 0x3ff;
  if (e === 0) return s * m * 2 ** -24;
  if (e === 31) return m ? NaN : s * Infinity;
  return s * (1 + m / 1024) * 2 ** (e - 15);
}
const HALF = new Float32Array(65536);
for (let i = 0; i < 65536; i++) HALF[i] = halfToFloat(i);

// 16-bit transport of a float16 target image: round(65535 * v), v in [0, 1]
export function targetToU16(f16) {
  const out = new Uint16Array(f16.length);
  for (let i = 0; i < f16.length; i++) {
    const v = HALF[f16[i]];
    out[i] = Math.round(65535 * Math.min(1, Math.max(0, v)));
  }
  return out;
}

const CRC = new Uint32Array(256);
for (let n = 0; n < 256; n++) {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  CRC[n] = c >>> 0;
}
function crc32(bytes, crc = 0xffffffff) {
  for (let i = 0; i < bytes.length; i++) crc = CRC[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
  return crc;
}
function chunk(type, data) {
  const out = new Uint8Array(12 + data.length);
  const dv = new DataView(out.buffer);
  dv.setUint32(0, data.length);
  for (let i = 0; i < 4; i++) out[4 + i] = type.charCodeAt(i);
  out.set(data, 8);
  dv.setUint32(8 + data.length, (crc32(out.subarray(4, 8 + data.length)) ^ 0xffffffff) >>> 0);
  return out;
}

// 16-bit grayscale PNG; deflate(bytes) -> zlib stream (CompressionStream('deflate') or zlib)
export async function encodePng16(u16, width, height, deflate) {
  const raw = new Uint8Array(height * (1 + 2 * width));
  for (let y = 0; y < height; y++) {
    const o = y * (1 + 2 * width);
    raw[o] = 0; // filter: none
    for (let x = 0; x < width; x++) {
      const v = u16[y * width + x];
      raw[o + 1 + 2 * x] = v >> 8; // big-endian samples
      raw[o + 2 + 2 * x] = v & 0xff;
    }
  }
  const ihdr = new Uint8Array(13);
  const dv = new DataView(ihdr.buffer);
  dv.setUint32(0, width);
  dv.setUint32(4, height);
  ihdr.set([16, 0, 0, 0, 0], 8); // bit depth 16, grayscale, deflate, no filter, no interlace
  const idat = await deflate(raw);
  const parts = [
    new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk('IHDR', ihdr),
    chunk('IDAT', idat),
    chunk('IEND', new Uint8Array(0)),
  ];
  const out = new Uint8Array(parts.reduce((a, p) => a + p.length, 0));
  let o = 0;
  for (const p of parts) {
    out.set(p, o);
    o += p.length;
  }
  return out;
}

// train.py knn_scale: mean distance to the 3 nearest other points (cKDTree k=4), min 1e-3
export function knnScales(xyz, n) {
  const idx = Int32Array.from({ length: n }, (_, i) => i);
  const nodes = [];
  function build(lo, hi, depth) {
    if (lo >= hi) return -1;
    const axis = depth % 3;
    const sub = Array.from(idx.subarray(lo, hi)).sort(
      (a, b) => xyz[a * 3 + axis] - xyz[b * 3 + axis],
    );
    idx.set(sub, lo);
    const mid = (lo + hi) >> 1;
    const node = nodes.length;
    nodes.push({ p: idx[mid], axis, left: -1, right: -1 });
    nodes[node].left = build(lo, mid, depth + 1);
    nodes[node].right = build(mid + 1, hi, depth + 1);
    return node;
  }
  const root = build(0, n, 0);
  const out = new Float32Array(n);
  const best = new Float64Array(4); // squared distances, ascending
  for (let q = 0; q < n; q++) {
    best.fill(Infinity);
    const qx = xyz[q * 3],
      qy = xyz[q * 3 + 1],
      qz = xyz[q * 3 + 2];
    const stack = [root];
    while (stack.length) {
      const ni = stack.pop();
      if (ni < 0) continue;
      const nd = nodes[ni];
      const p = nd.p;
      const dx = xyz[p * 3] - qx,
        dy = xyz[p * 3 + 1] - qy,
        dz = xyz[p * 3 + 2] - qz;
      const d2 = dx * dx + dy * dy + dz * dz;
      if (d2 < best[3]) {
        let k = 3;
        while (k > 0 && best[k - 1] > d2) {
          best[k] = best[k - 1];
          k--;
        }
        best[k] = d2;
      }
      const diff = [qx, qy, qz][nd.axis] - xyz[p * 3 + nd.axis];
      const [near, far] = diff < 0 ? [nd.left, nd.right] : [nd.right, nd.left];
      if (diff * diff < best[3]) stack.push(far);
      stack.push(near);
    }
    // best[0] is the query itself (or a duplicate at distance 0), as cKDTree's first result
    const mean = (Math.sqrt(best[1]) + Math.sqrt(best[2]) + Math.sqrt(best[3])) / 3;
    out[q] = Math.max(mean, 1e-3);
  }
  return out;
}

// mulberry32: a seeded uniform [0, 1) source for the initial quaternions
function rng(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// init.ply (binary little-endian 3DGS PLY) from points.f32 [count, 4] (x, y, z, v)
export function initPly(points, count, shDegree, seed = 0) {
  const xyz = new Float32Array(count * 3);
  for (let i = 0; i < count; i++) xyz.set(points.subarray(i * 4, i * 4 + 3), i * 3);
  const scales = knnScales(xyz, count);
  const rand = rng(seed);
  const nRest = 3 * ((shDegree + 1) ** 2 - 1);
  const props = [
    'x',
    'y',
    'z',
    'f_dc_0',
    'f_dc_1',
    'f_dc_2',
    ...Array.from({ length: nRest }, (_, i) => `f_rest_${i}`),
    'opacity',
    'scale_0',
    'scale_1',
    'scale_2',
    'rot_0',
    'rot_1',
    'rot_2',
    'rot_3',
  ];
  const head =
    `ply\nformat binary_little_endian 1.0\ncomment WebSplat stage 6 initial Gaussians\nelement vertex ${count}\n` +
    props.map((p) => `property float ${p}\n`).join('') +
    'end_header\n';
  const headBytes = new TextEncoder().encode(head);
  const body = new Float32Array(count * props.length);
  const logit = Math.log(0.1 / 0.9);
  for (let i = 0; i < count; i++) {
    const r = body.subarray(i * props.length, (i + 1) * props.length);
    r[0] = xyz[i * 3];
    r[1] = xyz[i * 3 + 1];
    r[2] = xyz[i * 3 + 2];
    const dc = (points[i * 4 + 3] - 0.5) / SH_C0;
    r[3] = dc;
    r[4] = dc;
    r[5] = dc;
    const o = 6 + nRest;
    r[o] = logit;
    const ls = Math.log(scales[i]);
    r[o + 1] = ls;
    r[o + 2] = ls;
    r[o + 3] = ls;
    for (let k = 0; k < 4; k++) r[o + 4 + k] = rand();
  }
  const out = new Uint8Array(headBytes.length + body.byteLength);
  out.set(headBytes);
  out.set(new Uint8Array(body.buffer), headBytes.length);
  return out;
}

// what train.py does that the fork does not (or not yet): refuse rather than train differently
export function unsupported(dataset) {
  const o = dataset.training.options;
  const out = [];
  if (o.hot_tolerance) out.push('hot_tolerance');
  if (o.lobe) out.push('lobe');
  if (o.source_gain) out.push('source_gain');
  if (o.pose_opt) out.push('pose_opt');
  if (o.appearance) out.push('appearance');
  if (o.focus_radius > 0) out.push('focus_radius');
  return out;
}

// Brush-only controls with no train.py equivalent (notes/stage6.md)
export const GROWTH_GRAD_THRESHOLD = 0.0004;
export const MEAN_NOISE_WEIGHT = 50;
// Brush's refine period (its own densification; also when bounds pruning runs): train.py's 100
// with Brush's growth loses up to 2.7 dB on a hot view (notes/stage6.md)
export const REFINE_EVERY = 200;

// Brush options (kebab-case TrainStreamConfig keys) for the dataset's training settings. The
// growth threshold is Brush's own densification control (no gsplat equivalent): G1's value.
// train.py's SH schedule `min(step // N, sh_degree)` as Brush's --sh-degree-interval N
export function shDegreeInterval(dataset) {
  const s = dataset.training.sh_degree_schedule;
  const m = /^min\(step \/\/ (\d+), sh_degree\)$/.exec(s ?? '');
  if (!m) throw new Error(`train.py SH degree schedule not supported by the Brush trainer: ${s}`);
  return Number(m[1]);
}

export function brushConfig(
  dataset,
  {
    growthGradThreshold = GROWTH_GRAD_THRESHOLD,
    meanNoiseWeight = MEAN_NOISE_WEIGHT,
    refineEvery = REFINE_EVERY,
    startIter = 0,
  } = {},
) {
  const t = dataset.training;
  const o = t.options;
  return {
    'total-train-iters': o.iterations,
    'start-iter': startIter,
    'sh-degree': o.sh_degree,
    'sh-degree-interval': shDegreeInterval(dataset),
    'ssim-weight': o.ssim_weight,
    'hot-weight': o.hot_weight,
    'hot-threshold': t.v_hot,
    'lr-mean': t.lr.means,
    'lr-mean-end': t.lr.means * t.means_lr_gamma ** o.iterations,
    'lr-mean-absolute': true,
    'lr-scale': t.lr.scales,
    'lr-rotation': t.lr.quats,
    'lr-opac': t.lr.opacities,
    'lr-coeffs-dc': t.lr.sh0,
    'lr-coeffs-sh-scale': t.lr.sh0 / t.lr.shN,
    'refine-every': refineEvery,
    // gsplat refines while refine_start_iter < step < refine_stop_iter: growth (gradient and
    // screen-size splits), pruning and Brush's opacity decay all stop there
    'growth-start-iter': t.strategy.refine_start_iter + 1,
    'growth-stop-iter': t.strategy.refine_stop_iter,
    'refine-start-iter': t.strategy.refine_start_iter,
    'refine-stop-iter': t.strategy.refine_stop_iter,
    // train.py's bounds pruning, every N steps to the end, independent of refinement
    'prune-bounds-every': t.prune_outside_bounds_every,
    'growth-grad-threshold': growthGradThreshold,
    'max-splats': o.cap,
    'mean-noise-weight': meanNoiseWeight,
    'background-noise-strength': 0,
    'max-resolution': Math.max(dataset.width, dataset.height),
    'eval-every': o.iterations,
  };
}

export function boundsJson(dataset, ceiling) {
  const b = dataset.bounds;
  const s = dataset.training.strategy;
  return {
    centre: b.centre,
    radius: b.radius,
    z_min: b.z_min,
    ceiling_origin: b.ceiling.origin,
    ceiling_cell: b.ceiling.cell,
    ceiling_n: b.ceiling.n,
    ceiling: Array.from(ceiling),
    // gsplat DefaultStrategy: too big = max scale > prune_scale3d * scene_scale, for
    // reset_every < step < refine_stop_iter
    max_scale: s.prune_scale3d * dataset.training.scene_scale,
    scale_prune_from: s.reset_every + 1,
    scale_prune_until: s.refine_stop_iter,
  };
}

function fmt(x) {
  return Number.isInteger(x) ? String(x) : x.toPrecision(17).replace(/\.?0+$/, '');
}

// quaternion w, x, y, z of a row-major 3x3 rotation (float64)
function quat(m) {
  const [a, b, c, d, e, f, g, h, i] = m;
  const tr = a + e + i;
  let w, x, y, z;
  if (tr > 0) {
    const s = Math.sqrt(tr + 1) * 2;
    w = s / 4;
    x = (h - f) / s;
    y = (c - g) / s;
    z = (d - b) / s;
  } else if (a > e && a > i) {
    const s = Math.sqrt(1 + a - e - i) * 2;
    w = (h - f) / s;
    x = s / 4;
    y = (b + d) / s;
    z = (c + g) / s;
  } else if (e > i) {
    const s = Math.sqrt(1 + e - a - i) * 2;
    w = (c - g) / s;
    x = (b + d) / s;
    y = s / 4;
    z = (f + h) / s;
  } else {
    const s = Math.sqrt(1 + i - a - e) * 2;
    w = (d - b) / s;
    x = (c + g) / s;
    y = (f + h) / s;
    z = s / 4;
  }
  return [w, x, y, z];
}

export function colmapText(dataset) {
  const K = dataset.K;
  const cameras =
    `# CAMERA_ID MODEL WIDTH HEIGHT PARAMS[]\n1 PINHOLE ${dataset.width} ${dataset.height} ` +
    `${fmt(K[0][0])} ${fmt(K[1][1])} ${fmt(K[0][2])} ${fmt(K[1][2])}\n`;
  const lines = ['# IMAGE_ID QW QX QY QZ TX TY TZ CAMERA_ID NAME', '# (no points lines)'];
  dataset.images.forEach((im, n) => {
    const v = im.viewmat;
    const q = quat([v[0], v[1], v[2], v[4], v[5], v[6], v[8], v[9], v[10]]);
    lines.push(
      `${n + 1} ${q.map(fmt).join(' ')} ${fmt(v[3])} ${fmt(v[7])} ${fmt(v[11])} 1 ${im.name}`,
      '',
    );
  });
  return {
    'sparse/0/cameras.txt': cameras,
    'sparse/0/images.txt': lines.join('\n') + '\n',
    'sparse/0/points3D.txt': '# POINT3D_ID X Y Z R G B ERROR TRACK[]\n',
    'eval_views.txt':
      dataset.images
        .filter((im) => im.split === 'test')
        .map((im) => im.name)
        .join('\n') + '\n',
  };
}

// Write the directory. io: {readTargets(i) -> Uint16Array f16 bits of image i, points, ceiling,
// write(path, bytes|string), deflate(bytes), progress(i, n), signal}
export async function writeBrushDataset(dataset, io) {
  const bad = unsupported(dataset);
  if (bad.length)
    throw new Error(`train.py options not supported by the Brush trainer: ${bad.join(', ')}`);
  for (const [path, text] of Object.entries(colmapText(dataset))) await io.write(path, text);
  await io.write('bounds.json', JSON.stringify(boundsJson(dataset, io.ceiling)));
  await io.write(
    'init.ply',
    initPly(io.points, dataset.points.count, dataset.training.options.sh_degree),
  );
  const n = dataset.images.length;
  for (let i = 0; i < n; i++) {
    io.signal?.throwIfAborted?.();
    const png = await encodePng16(
      targetToU16(await io.readTargets(i)),
      dataset.width,
      dataset.height,
      io.deflate,
    );
    await io.write(`images/${dataset.images[i].name}`, png);
    io.progress?.(i + 1, n);
  }
}
