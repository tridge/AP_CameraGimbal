/** Stage 2: port of tools/GaussianSplat/extract_frames.py (frame selection and pose priors).
 *
 * The flow and its decisions are the native tool's; its command-line inputs are `params`
 * (DEFAULTS: its argparse defaults, named as in CONTRACTS.md's selection options). Pixels come
 * from a decoder with decode(indices) -> async iterable of {index, meta, data: Uint16Array},
 * ascending. Outputs (CONTRACTS.md websplat.frames v1) go to a store (see opfs-store.js):
 *   frames.json    the native frames.json schema plus format, version and selection
 *   features.u8    [N, H, W] CLAHE 8-bit feature images (native images/NNNNN.png)
 *   thermal.u16    [N, H, W] rotated raw radiometric frames (native thermal/NNNNN.png)
 */
import { clahe } from './clahe.js';
import {
  RawStats,
  celsiusFn,
  percentileArray,
  percentileScalarF32,
  medianF32,
  median,
} from './numeric.js';
import {
  LocalFrame,
  cameraRotation,
  intrinsics,
  invK,
  matmul,
  matvec,
  norm,
  project,
  triangulateRays,
} from './geometry.js';

export const SELECT_VERSION = 1;

export const DEFAULTS = Object.freeze({
  lat: null, // target latitude (required; propose() suggests the hot spot)
  lon: null,
  target_alt: null, // target height above takeoff (m); null: hot spot, else 0
  locate_hotspot: true,
  hotspot_min_c: 60.0,
  hotspot_max_error: 60.0,
  rotation: null, // degrees; null: the mkv's rotation_deg
  hfov: null, // degrees; null: metadata, else 24.2
  margin: 0.1,
  radius: null,
  start: 0,
  end: null,
  step: 1,
  max_range: 2000,
  max_median_dev: 15.0,
});

export class SelectError extends Error {}

const truthy = (x) =>
  !(
    x == null ||
    x === false ||
    x === 0 ||
    x === '' ||
    (Array.isArray(x) && !x.length) ||
    (typeof x === 'object' && !Object.keys(x).length)
  );
const pyMod = (a, n) => ((a % n) + n) % n;

/** np.rot90(image, (degrees // 90) % 4) */
export function rotate(data, width, height, degrees) {
  const k = pyMod(Math.floor(degrees / 90), 4);
  if (!k) return { data, width, height };
  const W = width,
    H = height;
  const out = new data.constructor(W * H);
  if (k === 2) {
    for (let i = 0, n = W * H; i < n; i++) out[i] = data[n - 1 - i];
    return { data: out, width: W, height: H };
  }
  // k=1: out[i][j] = m[j][W-1-i]; k=3: out[i][j] = m[H-1-j][i]; out is W rows of H
  for (let i = 0; i < W; i++)
    for (let j = 0; j < H; j++)
      out[i * H + j] = k === 1 ? data[j * W + (W - 1 - i)] : data[(H - 1 - j) * W + i];
  return { data: out, width: H, height: W };
}

export function isBlank(stats, meta) {
  const [p1, p99] = percentileArray(stats, [1, 99]);
  return (p99 - p1) * (meta?.temperature_scale_k ?? 1 / 64) < 0.5;
}

const isNum = (x) => typeof x === 'number' && Number.isFinite(x);

/** Fields native extract_frames.py would fail on (TypeError on None/strings): [{index, field}].
 * Only where native reads them before decoding: takeoff altitude over all frames, the first
 * frame's size, poses over start..end, and frame maxima over start..end when locating the hot
 * spot. Radiometry is checked per decoded frame (checkDecoded). Absent optional keys keep
 * native's defaults. */
export function telemetryProblems(metas, start, end, { locate = true } = {}) {
  const problems = [];
  const bad = (index, field) => problems.push({ index, field });
  metas.forEach((m, i) => {
    if (!m) return bad(i, 'metadata');
    const t = m.telemetry,
      p = t?.position;
    if (truthy(t) && truthy(p) && p.alt_relative_m != null) {
      if (!isNum(p.alt_relative_m)) bad(i, 'position.alt_relative_m');
      if (!isNum(p.alt_amsl_m)) bad(i, 'position.alt_amsl_m');
    }
    if (i === 0 && (!isNum(m.width) || !isNum(m.height))) bad(i, 'width/height');
    if (i < start || i > end) return;
    if (locate && 'maximum_c' in m && !isNum(m.maximum_c)) bad(i, 'maximum_c');
    if (!truthy(t) || !truthy(p) || !truthy(t.vehicle_attitude) || !truthy(t.gimbal_attitude))
      return;
    if (p.alt_amsl_m == null) return;
    for (const k of ['lat_e7', 'lon_e7', 'alt_amsl_m']) if (!isNum(p[k])) bad(i, `position.${k}`);
    if (!isNum(t.vehicle_attitude.yaw_rad)) bad(i, 'vehicle_attitude.yaw_rad');
    for (const k of ['yaw_rad', 'pitch_rad', 'roll_rad'])
      if (k in t.gimbal_attitude && !isNum(t.gimbal_attitude[k])) bad(i, `gimbal_attitude.${k}`);
  });
  return problems;
}

/** Radiometry of a frame native decodes (is_blank, raw_to_celsius) */
export function checkDecoded(index, meta) {
  for (const k of ['temperature_scale_k', 'temperature_offset_k'])
    if (k in meta && !isNum(meta[k]))
      throw new SelectError(
        `incomplete metadata (extract_frames.py fails on it too): ${k} in frame ${index}`,
      );
}

/** Reject incomplete telemetry explicitly, naming the fields and frames. */
export function checkTelemetry(metas, start, end, options) {
  if (!metas.length) throw new SelectError('no frames');
  const problems = telemetryProblems(metas, start, end, options);
  if (!problems.length) return;
  const byField = new Map();
  for (const { index, field } of problems)
    byField.set(field, [...(byField.get(field) ?? []), index]);
  const text = [...byField]
    .map(
      ([f, ix]) =>
        `${f} in frame${ix.length > 1 ? 's' : ''} ${ix.slice(0, 20).join(', ')}${ix.length > 20 ? ', ...' : ''}`,
    )
    .join('; ');
  const e = new SelectError(`incomplete telemetry (extract_frames.py fails on it too): ${text}`);
  e.problems = problems;
  throw e;
}

export function framePose(meta, local) {
  const t = meta?.telemetry;
  if (
    !truthy(t) ||
    !truthy(t.position) ||
    !truthy(t.vehicle_attitude) ||
    !truthy(t.gimbal_attitude)
  )
    return null;
  const p = t.position;
  if (p.alt_amsl_m == null) return null;
  return [local.toEnu(p.lat_e7 / 1e7, p.lon_e7 / 1e7, p.alt_amsl_m), cameraRotation(t)];
}

/** feature_image(): 8-bit contrast-stretched input to CLAHE, via a LUT over raw values */
export function featureInput(raw, stats, meta) {
  const c = celsiusFn(meta);
  const [lo, hi] = percentileArray(stats, [1, 99], c);
  const scale = Math.max(hi - lo, 0.5);
  const lut = new Uint8Array(stats.max + 1);
  for (let r = stats.min; r <= stats.max; r++) {
    const v = Math.min(Math.max((c(r) - lo) / scale, 0), 1);
    lut[r] = Math.trunc(v * 255);
  }
  const out = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i++) out[i] = lut[raw[i]];
  return out;
}

/** np.argmax of the frame rotated by k*90 degrees for k = 0..3, as [x, y] in the rotated frame,
 * without rotating it: the first maximum in each rotation's scan order. */
export function hottestAll(raw, width, height) {
  let max = 0;
  for (let i = 0; i < raw.length; i++) if (raw[i] > max) max = raw[i];
  const W = width,
    H = height;
  const best = [Infinity, Infinity, Infinity, Infinity];
  for (let r = 0, i = 0; r < H; r++)
    for (let c = 0; c < W; c++, i++) {
      if (raw[i] !== max) continue;
      // linear index in the rotated image (np.rot90, counter-clockwise)
      const keys = [
        r * W + c,
        (W - 1 - c) * H + r,
        (H - 1 - r) * W + (W - 1 - c),
        c * H + (H - 1 - r),
      ];
      for (let k = 0; k < 4; k++) if (keys[k] < best[k]) best[k] = keys[k];
    }
  return best.map((key, k) => {
    const w = k % 2 ? H : W;
    return [key % w, Math.floor(key / w)];
  });
}

/** Camera geometry implied by the metadata and parameters. */
export function cameraModel(metas, params) {
  const first = metas[0];
  let width = first.width,
    height = first.height;
  const rotation = params.rotation == null ? (first.rotation_deg ?? 0) : params.rotation;
  if (pyMod(rotation, 180)) [width, height] = [height, width];
  const hfov = params.hfov || first.hfov_deg || 24.2;
  return { width, height, rotation, hfov, K: intrinsics(width, height, hfov) };
}

export function homeAltitude(metas) {
  const homes = [];
  for (const m of metas) {
    const p = m?.telemetry?.position;
    if (truthy(m?.telemetry) && truthy(p) && p.alt_relative_m != null)
      homes.push(p.alt_amsl_m - p.alt_relative_m);
  }
  if (!homes.length) throw new SelectError('no frames carry position telemetry');
  return median(homes);
}

export function hotFrames(metas, minC, start, end) {
  const hot = [];
  metas.forEach((m, i) => {
    if (start <= i && i <= end && (m?.maximum_c ?? -300) >= minC) hot.push(i);
  });
  return hot;
}

/** Decode `indices` once, keeping only what locate_hotspot needs (blank flag and hottest pixel
 * per rotation), so proposals can try several rotations without decoding again. */
export async function hotSamples(decoder, indices, { signal, width, height } = {}) {
  const samples = new Map();
  for await (const { index, meta, data } of decoder.decode(indices, { signal })) {
    checkDecoded(index, meta);
    const blank = isBlank(new RawStats(data), meta);
    samples.set(index, { meta, blank, hottest: hottestAll(data, width, height) });
  }
  return samples;
}

/** locate_hotspot() over decoded samples: [X, n, err]. rcond > 0 rejects ill-conditioned
 * geometry (proposals); 0 fails only where native's np.linalg.solve does. */
export function locateHotspot(samples, hot, local, K, rotation, { rcond = 0 } = {}) {
  const centres = [],
    dirs = [];
  const Ki = invK(K);
  for (const i of hot) {
    const s = samples.get(i);
    if (!s) continue;
    const pose = framePose(s.meta, local);
    if (pose === null || s.blank) continue;
    const [x, y] = s.hottest[pyMod(Math.floor(rotation / 90), 4)];
    const d = matvec(matmul(pose[1], Ki), [x, y, 1.0]);
    centres.push(pose[0]);
    const n = norm(d);
    dirs.push([d[0] / n, d[1] / n, d[2] / n]);
  }
  if (centres.length < 5) return [null, centres.length, null];
  const [X, err] = triangulateRays(centres, dirs, K[0][0], 5, { rcond });
  return [X, centres.length, err];
}

const fmt = (v, d = 1) => v.toFixed(d);

/** close every handle, returning the errors instead of stopping at the first */
function closeAll(handles) {
  const errors = [];
  for (const h of handles) {
    if (!h) continue;
    try {
      h.close();
    } catch (e) {
      errors.push(e);
    }
  }
  return errors;
}

const RCOND = 1e-9;

/** Settings propose() reads; a stage that caches its output must key on them. */
export const PROPOSAL_KEYS = ['start', 'end', 'hotspot_min_c', 'hotspot_max_error', 'hfov'];

/** Automatic proposals for the parameters native extraction takes on the command line.
 * Optional estimates (each rotation trial, the optical-axis point) fail independently. */
export async function propose(ingest, decoder, { params = {}, signal, cache } = {}) {
  const p = { ...DEFAULTS, ...params };
  const metas = ingest.frames.map((f) => f.meta);
  const notes = [];
  const end = p.end == null ? metas.length - 1 : Math.min(p.end, metas.length - 1);
  checkTelemetry(metas, p.start, end);
  const home = homeAltitude(metas);
  // the provisional origin: the first validated pose in start..end
  const posed = metas.slice(p.start, end + 1).filter((m) => framePose(m, new LocalFrame(0, 0, 0)));
  if (!posed.length)
    throw new SelectError('no frames in the range carry position and attitude telemetry');
  const p0 = posed[0].telemetry.position;
  const provisional = new LocalFrame(p0.lat_e7 / 1e7, p0.lon_e7 / 1e7, home);
  const metaRotation = metas[0].rotation_deg ?? 0;
  const hot = hotFrames(metas, p.hotspot_min_c, p.start, end);
  const samples = cache ?? (await hotSamples(decoder, hot, { signal, ...ingest.track }));
  const finite = (X) => !!X && X.every(Number.isFinite);

  // the rotation whose hottest pixels triangulate most consistently
  const trials = [];
  for (const rotation of [0, 90, 180, 270]) {
    const cam = cameraModel(metas, { ...p, rotation });
    try {
      const [X, n, err] = locateHotspot(samples, hot, provisional, cam.K, rotation, {
        rcond: RCOND,
      });
      trials.push(
        finite(X) && Number.isFinite(err)
          ? { rotation, X, n, err }
          : { rotation, X: null, n, err: null },
      );
    } catch (e) {
      trials.push({ rotation, X: null, n: null, err: null, failure: e.message });
    }
  }
  const usable = trials.filter((t) => t.X && t.err <= p.hotspot_max_error);
  usable.sort((a, b) => a.err - b.err);
  const fromMeta = trials.find((t) => t.rotation === pyMod(metaRotation, 360));
  let chosen = usable[0] ?? null;
  // keep the recorded rotation unless another is clearly better
  if (
    chosen &&
    fromMeta?.X &&
    fromMeta.err <= p.hotspot_max_error &&
    fromMeta.err <= 1.5 * chosen.err
  )
    chosen = fromMeta;
  const rotation = chosen ? chosen.rotation : metaRotation;
  const cam = cameraModel(metas, { ...p, rotation });
  // hfov: the effective value (override, else metadata, else 24.2)
  const proposal = { rotation, hfov: cam.hfov, target_alt: null, lat: null, lon: null };
  if (chosen && chosen.rotation !== metaRotation)
    notes.push(
      `hot spot rays agree best with rotation ${chosen.rotation} (median ${fmt(chosen.err, 0)} px) ` +
        `rather than the recorded ${metaRotation} (${fromMeta?.X ? fmt(fromMeta.err, 0) + ' px' : 'no fix'})`,
    );
  if (!p.hfov && !metas[0].hfov_deg) notes.push('no HFOV in the metadata; assuming 24.2 degrees');

  // where the optical axes converge (the orbit centre), as a check and fallback
  let lookAt = null;
  const centres = [],
    dirs = [];
  for (let i = p.start; i <= end; i++) {
    const pose = framePose(metas[i], provisional);
    if (!pose) continue;
    centres.push(pose[0]);
    dirs.push([pose[1][0][2], pose[1][1][2], pose[1][2][2]]);
  }
  if (centres.length >= 5) {
    try {
      const r = triangulateRays(centres, dirs, cam.K[0][0], 5, { rcond: RCOND });
      if (finite(r[0]) && Number.isFinite(r[1])) lookAt = r;
    } catch {
      /* parallel or ill-conditioned axes: no estimate */
    }
  }
  if (!lookAt) notes.push('the optical axes do not converge on a point');
  let target;
  if (chosen) {
    target = chosen.X;
    notes.push(
      `hot spot from ${chosen.n} frames above ${p.hotspot_min_c} C, ${fmt(target[2])} m above takeoff ` +
        `(median ray error ${fmt(chosen.err, 0)} px)`,
    );
    if (lookAt) {
      const d = Math.hypot(target[0] - lookAt[0][0], target[1] - lookAt[0][1]);
      if (d > 300)
        notes.push(`the hot spot is ${fmt(d, 0)} m from where the camera points; check it`);
    }
  } else if (lookAt) {
    target = lookAt[0];
    // selection would otherwise put the target at takeoff height
    proposal.target_alt = target[2];
    notes.push(
      (hot.length
        ? `the ${hot.length} hot frames do not give a consistent hot spot`
        : `no frames above ${p.hotspot_min_c} C`) +
        `; using where the camera points, ${fmt(target[2])} m above takeoff`,
    );
  } else
    throw new SelectError(
      'cannot propose a target: no hot spot and the optical axes do not converge',
    );
  const [lat, lon] = provisional.toGeodetic(target);
  Object.assign(proposal, { lat, lon });
  return {
    proposal,
    hotspot: chosen ? { lat, lon, up: chosen.X[2], frames: chosen.n, errorPx: chosen.err } : null,
    lookAt: lookAt ? { enu: lookAt[0], errorPx: lookAt[1] } : null,
    rotations: trials.map((t) => ({
      rotation: t.rotation,
      frames: t.n,
      errorPx: t.err,
      failure: t.failure ?? null,
    })),
    homeAltitude: home,
    options: Object.fromEntries(PROPOSAL_KEYS.map((k) => [k, p[k]])),
    notes,
    samples,
  };
}

/** The native extraction flow. mode: 'single' decodes the selected frames once and drops the
 * drifted frames' provisional outputs; 'redecode' decodes the kept frames again for the outputs,
 * as native does; 'cache' keeps the candidates' raw frames in the store (cache.u16) and re-reads
 * them. The outputs are identical. */
export async function selectFrames(
  ingest,
  decoder,
  store,
  params,
  { mode = 'single', signal, onProgress = () => {}, samples = null, sourceName = null } = {},
) {
  const p = { ...DEFAULTS, ...params };
  if (p.lat == null || p.lon == null)
    throw new SelectError('target latitude and longitude are required');
  const log = [];
  const timings = {};
  let t0 = performance.now();
  const metas = ingest.frames.map((f) => f.meta);
  const end = p.end == null ? metas.length - 1 : Math.min(p.end, metas.length - 1);
  checkTelemetry(metas, p.start, end, { locate: p.locate_hotspot });
  const { width, height, rotation, hfov, K } = cameraModel(metas, p);
  const homeAlt = homeAltitude(metas);
  let local = new LocalFrame(p.lat, p.lon, homeAlt);
  let target = [0.0, 0.0, p.target_alt || 0.0];
  let hotspot = null;
  if (!p.locate_hotspot) {
    // as native: no hot spot search
  } else {
    const hot = hotFrames(metas, p.hotspot_min_c, p.start, end);
    const have =
      samples && hot.every((i) => samples.has(i))
        ? samples
        : await hotSamples(decoder, hot, { signal, ...ingest.track });
    let X, n, err;
    try {
      [X, n, err] = locateHotspot(have, hot, local, K, rotation);
    } catch (e) {
      // native: numpy.linalg.LinAlgError
      throw new SelectError(`hot spot rays are degenerate: ${e.message}`);
    }
    hotspot = { enu: X && X.slice(), frames: n, errorPx: err, used: false };
    if (X !== null && err > p.hotspot_max_error) {
      log.push(
        `hotspot rays disagree (median ${fmt(err, 0)} px from ${n} frames): check rotation and the ` +
          'gimbal yaw convention; keeping the given position',
      );
    } else if (X !== null) {
      log.push(
        `hotspot from ${n} frames at ENU [${X.map((v) => fmt(v)).join(' ')}] m (median ray error ${fmt(err, 0)} px)`,
      );
      if (p.target_alt != null) X[2] = p.target_alt;
      if (Math.hypot(X[0], X[1]) < 300) {
        target = X;
        hotspot.used = true;
      } else log.push('hotspot is far from the given position, ignoring it');
    }
  }
  timings.hotspotMs = performance.now() - t0;
  // re-centre the local frame on the target, keeping takeoff altitude as the vertical datum
  const [tLat, tLon] = local.toGeodetic(target);
  local = new LocalFrame(tLat, tLon, homeAlt);
  const targetUp = target[2];
  log.push(
    `target ${fmt(tLat, 7)} ${fmt(tLon, 7)}, ${fmt(targetUp)} m above takeoff (${fmt(homeAlt + targetUp)} m AMSL)`,
  );

  let selected = [];
  const mx = p.margin * width,
    my = p.margin * height;
  const point = [0.0, 0.0, targetUp];
  for (let i = p.start; i <= end; i++) {
    const pose = framePose(metas[i], local);
    if (pose === null) continue;
    if (p.radius != null) {
      const c = pose[0],
        axis = [pose[1][0][2], pose[1][1][2], pose[1][2][2]];
      if (axis[2] < -0.1) {
        const s = (targetUp - c[2]) / axis[2];
        if (Math.hypot(c[0] + axis[0] * s - point[0], c[1] + axis[1] * s - point[1]) < p.radius)
          selected.push(i);
      }
      continue;
    }
    const pr = project(K, pose[0], pose[1], point);
    if (pr === null) continue;
    const [[u, v], depth] = pr;
    if (mx <= u && u < width - mx && my <= v && v < height - my && depth < p.max_range)
      selected.push(i);
  }
  selected = selected.filter((_, k) => k % Math.max(1, p.step) === 0);
  if (selected.length < 3) throw new SelectError(`only ${selected.length} frames see the target`);
  log.push(
    `${selected.length} of ${metas.length} frames ${p.radius != null ? `look within ${fmt(p.radius, 0)} m of the target` : 'see the target'} ` +
      `(frames ${selected[0]}..${selected.at(-1)})`,
  );

  // pass over the selected frames: blank and drift statistics (plus outputs in 'single' mode)
  t0 = performance.now();
  const blank = [],
    medians = new Map(),
    entries = new Map();
  const total = selected.length;
  let done = 0;
  const pixels = width * height;
  let features = null,
    thermal = null,
    cache = null;
  const slots = []; // frame index of each [H, W] slot written
  timings.computeMs = timings.writeMs = 0;
  const output = (index, meta, raw) => {
    let t = performance.now();
    const stats = new RawStats(raw);
    const rot = rotate(raw, ingest.track.width, ingest.track.height, rotation);
    if (rot.width !== width || rot.height !== height)
      throw new SelectError(`frame ${index} has a different size`);
    const c = celsiusFn(meta);
    const image = clahe(featureInput(rot.data, stats, meta), width, height);
    timings.computeMs += performance.now() - t;
    t = performance.now();
    features.write(image, slots.length * pixels);
    thermal.write(
      new Uint8Array(rot.data.buffer, rot.data.byteOffset, rot.data.byteLength),
      slots.length * pixels * 2,
    );
    slots.push(index);
    timings.writeMs += performance.now() - t;
    const [centre, R] = framePose(meta, local);
    entries.set(index, {
      tmin: percentileScalarF32(stats, 1, c),
      tmax: c(stats.max),
      frame: {
        index,
        name: `${String(index).padStart(5, '0')}.png`,
        utc_us: meta.telemetry.utc_us ?? null,
        position: centre,
        rotation: R,
        minimum_c: c(stats.min),
        maximum_c: c(stats.max),
        temperature_scale_k: meta.temperature_scale_k ?? 1 / 64,
        temperature_offset_k: meta.temperature_offset_k ?? 0,
      },
    });
  };
  const rawBytes = ingest.track.width * ingest.track.height * 2;
  const cached = new Map();
  let drifted, keep, typicalMedian;
  const files = ['features.u8', 'thermal.u16', 'frames.json'];
  // completion = frames.json written; until then any failure removes every output
  let complete = false,
    kept,
    info;
  try {
    try {
      // acquired inside the protected scope, released independently
      features = await store.open('features.u8');
      thermal = await store.open('thermal.u16');
      if (mode === 'cache') cache = await store.open('cache.u16');
      for await (const { index, meta, data } of decoder.decode(selected, { signal })) {
        checkDecoded(index, meta);
        const stats = new RawStats(data);
        if (isBlank(stats, meta)) blank.push(index);
        else {
          medians.set(index, medianF32(stats, celsiusFn(meta)));
          if (mode === 'single') output(index, meta, data);
          else if (cache) {
            cache.write(
              new Uint8Array(data.buffer, data.byteOffset, data.byteLength),
              cached.size * rawBytes,
            );
            cached.set(index, cached.size);
          }
        }
        onProgress({ phase: 'select', done: ++done, total });
      }
      timings.statsMs = performance.now() - t0;
      typicalMedian = median([...medians.values()]);
      drifted = [...medians.entries()]
        .filter(([, m]) => Math.abs(m - typicalMedian) > p.max_median_dev)
        .map(([i]) => i)
        .sort((a, b) => a - b);
      keep = [...medians.keys()].filter((i) => !drifted.includes(i)).sort((a, b) => a - b);
      t0 = performance.now();
      if (mode === 'single') {
        // drop the drifted frames' provisional slots
        let to = 0;
        slots.forEach((index, from) => {
          if (drifted.includes(index)) return;
          if (from !== to) {
            features.write(features.read(pixels, from * pixels), to * pixels);
            thermal.write(thermal.read(pixels * 2, from * pixels * 2), to * pixels * 2);
          }
          to++;
        });
        slots.splice(0, slots.length, ...slots.filter((i) => !drifted.includes(i)));
        for (const i of drifted) entries.delete(i);
      } else if (cache) {
        done = 0;
        for (const i of keep) {
          const bytes = cache.read(rawBytes, cached.get(i) * rawBytes);
          output(i, metas[i], new Uint16Array(bytes.buffer));
          onProgress({ phase: 'output', done: ++done, total: keep.length });
        }
      } else {
        done = 0;
        for await (const { index, meta, data } of decoder.decode(keep, { signal })) {
          output(index, meta, data);
          onProgress({ phase: 'output', done: ++done, total: keep.length });
        }
      }
      features.truncate(slots.length * pixels);
      thermal.truncate(slots.length * pixels * 2);
      const errors = closeAll([features, thermal, cache]);
      features = thermal = cache = null;
      if (errors.length) throw errors[0];
    } finally {
      closeAll([features, thermal, cache]);
      if (mode === 'cache') await store.remove('cache.u16').catch(() => {});
    }
    timings.outputMs = performance.now() - t0;
    kept = keep.map((i) => entries.get(i));
    if (slots.join() !== keep.join() || !kept.length) {
      if (!kept.length)
        // native fails here too (np.max of an empty list)
        throw new SelectError('no usable frames: all selected frames are blank or drifted');
      throw new Error('internal error: output slots out of order');
    }
    info = {
      format: 'websplat.frames',
      version: SELECT_VERSION,
      source: sourceName,
      width,
      height,
      hfov_deg: hfov,
      K,
      rotation_deg: rotation,
      origin: { lat: tLat, lon: tLon, alt_amsl: homeAlt, frame: 'ENU, up from takeoff altitude' },
      target_enu: [0.0, 0.0, targetUp],
      temperature_c: {
        p1: median(kept.map((e) => e.tmin)),
        max: Math.max(...kept.map((e) => e.tmax)),
      },
      frames: kept.map((e) => e.frame),
      selection: {
        options: p,
        hotspot: hotspot && {
          enu: hotspot.enu,
          frames: hotspot.frames,
          median_ray_error_px: hotspot.errorPx,
          used: hotspot.used,
        },
        considered: end - p.start + 1,
        selected: selected.length,
        blank,
        drifted,
        typical_median_c: Number.isFinite(typicalMedian) ? typicalMedian : null,
        mode,
      },
    };
    await store.write('frames.json', JSON.stringify(info, null, 1));
    complete = true;
  } finally {
    // a failed run leaves no partial outputs
    if (!complete) for (const f of files) await store.remove(f).catch(() => {});
  }
  if (blank.length) log.push(`skipped ${blank.length} blank frames: ${blank.join(', ')}`);
  if (drifted.length)
    log.push(
      `skipped ${drifted.length} frames with median temperature more than ${fmt(p.max_median_dev, 0)} C from ` +
        `the typical ${fmt(typicalMedian)} C: ${drifted.join(', ')}`,
    );
  log.push(`wrote ${kept.length} frames`);
  return {
    info,
    log,
    selected,
    blank,
    drifted,
    hotspot,
    typical: typicalMedian,
    timings,
    params: p,
  };
}
