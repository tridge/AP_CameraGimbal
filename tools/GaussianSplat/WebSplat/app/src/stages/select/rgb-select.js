/** Stage 2 for RGB recordings: which frames to extract and their pose priors, after
 * extract_ts_frames.py (frames at `fps`, pose from the frame's telemetry, KLV within 0.2 s), plus
 * the thermal selection's view test (target in view, within max_range) and a frame budget.
 * Pure: frames are ingest index records ({index, pts, telemetry}).
 */
import {
  LocalFrame,
  cameraRotation,
  dot,
  intrinsics,
  nearLon,
  normLon,
  project,
  sub,
  triangulateRays,
} from './geometry.js';
import { SelectError } from './select.js';
import { median } from './numeric.js';

export const RGB_SELECT_VERSION = 1;

export const RGB_DEFAULTS = Object.freeze({
  lat: null,
  lon: null,
  target_alt: 0, // m above the takeoff altitude
  hfov: null, // degrees; null: the recording's, else 70 (extract_ts_frames.py)
  width: 1280, // processing width (train_rgb.py --width)
  start: 0,
  end: null,
  fps: 3.0,
  min_height: 5.0, // m above the target (not takeoff: KLV recordings may start airborne)
  max_offset_ms: 200, // telemetry at most this far from the frame
  margin: 0.1,
  max_range: 2000,
  budget: null, // at most this many frames (every step-th); null: no limit
});

/** Processing size: width as given, height scaled with the aspect kept. */
export function processingSize(srcW, srcH, width) {
  if (!Number.isInteger(width) || width < 1) throw new SelectError(`invalid width ${width}`);
  if (width > srcW)
    throw new SelectError(`processing width ${width} is above the recording's ${srcW}`);
  return { width, height: Math.round((srcH * width) / srcW) };
}

const num = Number.isFinite;
const optional = (v) => v == null || num(v);

/** Whether a telemetry record gives a pose: finite position and platform attitude, and finite
 * sensor roll and pitch (sensor yaw may be absent: 0, as cameraRotation). */
export function finitePose(t) {
  const p = t?.position,
    v = t?.vehicle_attitude,
    g = t?.gimbal_attitude;
  return (
    !!p &&
    !!v &&
    !!g &&
    num(p.lat_e7) &&
    num(p.lon_e7) &&
    num(p.alt_amsl_m) &&
    Math.abs(p.lat_e7) <= 90e7 &&
    num(v.roll_rad) &&
    num(v.pitch_rad) &&
    num(v.yaw_rad) &&
    num(g.roll_rad) &&
    num(g.pitch_rad) &&
    optional(g.yaw_rad)
  );
}

/** [ENU camera centre, world_from_cam] in `local`, longitude taken near the frame's origin. */
export function rgbPose(t, local) {
  const p = t.position;
  const C = local.toEnu(p.lat_e7 / 1e7, nearLon(p.lon_e7 / 1e7, local.lon), p.alt_amsl_m);
  return [C, cameraRotation(t)];
}

/** frames: ingest records; home: takeoff altitude (m AMSL); src: {width, height, timescale}.
 * Returns {selected: [index], entries: [frames.json frame], width, height, hfov, K, origin,
 * target_enu, stats, log}. */
export function selectRgb(frames, home, src, params) {
  const p = { ...RGB_DEFAULTS, ...params };
  if (p.lat == null || p.lon == null)
    throw new SelectError('target latitude and longitude are required');
  const { width, height } = processingSize(src.width, src.height, p.width);
  const end = p.end == null ? frames.length - 1 : Math.min(p.end, frames.length - 1);
  const local = new LocalFrame(p.lat, p.lon, home);
  const point = [0, 0, p.target_alt ?? 0];
  const hfov =
    p.hfov ||
    frames.slice(p.start, end + 1).find((f) => f.telemetry?.hfov_deg)?.telemetry.hfov_deg ||
    70.0;
  const K = intrinsics(width, height, hfov);
  const log = [];
  const t0 = frames[0].pts;
  const counts = {
    range: end - p.start + 1,
    sampled: 0,
    noPose: 0,
    stale: 0,
    low: 0,
    outOfView: 0,
  };
  let nextT = -Infinity;
  let candidates = [];
  for (let i = p.start; i <= end; i++) {
    const f = frames[i];
    const ts = (f.pts - t0) / src.timescale;
    if (ts < nextT) continue;
    const t = f.telemetry;
    if (!finitePose(t)) {
      counts.noPose++;
      continue;
    }
    if (Math.abs(t.offset_ms ?? 0) > p.max_offset_ms) {
      counts.stale++;
      continue;
    }
    const [C, R] = rgbPose(t, local);
    if (C[2] - point[2] < p.min_height) {
      counts.low++;
      continue;
    }
    nextT = ts + 1 / p.fps;
    counts.sampled++;
    const pr = project(K, C, R, point);
    const mx = p.margin * width,
      my = p.margin * height;
    if (
      !pr ||
      pr[1] >= p.max_range ||
      pr[0][0] < mx ||
      pr[0][0] >= width - mx ||
      pr[0][1] < my ||
      pr[0][1] >= height - my
    ) {
      counts.outOfView++;
      continue;
    }
    candidates.push({
      index: i,
      name: `${String(i).padStart(5, '0')}.png`,
      utc_us: t.utc_us ?? null,
      media_s: ts,
      position: C,
      rotation: R,
    });
  }
  const step = p.budget ? Math.max(1, Math.ceil(candidates.length / p.budget)) : 1;
  const entries = candidates.filter((_, k) => k % step === 0);
  if (entries.length < 3)
    throw new SelectError(
      `only ${entries.length} frames see the target (${counts.sampled} sampled at ${p.fps} fps, ` +
        `${counts.outOfView} not looking at it, ${counts.low} lower than ${p.min_height} m above it)`,
    );
  log.push(
    `${entries.length} frames: ${counts.sampled} sampled at ${p.fps} fps from ${counts.range}, ` +
      `${candidates.length} see the target` +
      (step > 1 ? `, every ${step}th kept (budget ${p.budget})` : ''),
  );
  if (counts.noPose) log.push(`${counts.noPose} frames without a pose skipped`);
  if (counts.stale)
    log.push(`${counts.stale} frames with telemetry more than ${p.max_offset_ms} ms away skipped`);
  return {
    selected: entries.map((e) => e.index),
    entries,
    width,
    height,
    hfov,
    K,
    origin: { lat: p.lat, lon: p.lon, alt_amsl: home, frame: 'ENU, up from takeoff altitude' },
    target_enu: point,
    step,
    counts,
    log,
    params: p,
  };
}

/** A target for the standalone form: where the optical axes converge (an orbit), else where
 * those looking down more than ~11 degrees meet the takeoff-height plane, the median
 * (extract_ts_frames.py --no-terrain). KLV recordings that start airborne have no usable
 * takeoff plane, hence the convergence first. */
export function proposeRgbTarget(frames, home, start = 0, end = frames.length - 1) {
  const first = frames.slice(start, end + 1).find((f) => finitePose(f.telemetry));
  if (!first) throw new SelectError('no frames in the range carry a pose');
  const p0 = first.telemetry.position;
  const local = new LocalFrame(p0.lat_e7 / 1e7, p0.lon_e7 / 1e7, home);
  const centres = [],
    dirs = [],
    xs = [],
    ys = [];
  const every = Math.max(1, Math.floor((end - start + 1) / 300));
  for (let i = start; i <= end; i += every) {
    const t = frames[i].telemetry;
    if (!finitePose(t)) continue;
    const [C, R] = rgbPose(t, local);
    const a = [R[0][2], R[1][2], R[2][2]];
    centres.push(C);
    dirs.push(a);
    if (a[2] >= -0.2 || C[2] <= 0) continue;
    const s = -C[2] / a[2];
    xs.push(C[0] + a[0] * s);
    ys.push(C[1] + a[1] * s);
  }
  if (centres.length >= 5) {
    try {
      // errors in pixels of a nominal 1000 px focal length (the robust weights assume pixels)
      const [X, err] = triangulateRays(centres, dirs, 1000, 5, { rcond: 1e-9 });
      // converging (an orbit): median ray error under ~17 degrees, in front of most cameras
      const front = centres.filter((c, k) => dot(sub(X, c), dirs[k]) > 0).length;
      if (err < 300 && front >= 0.9 * centres.length) {
        const [lat, lon] = local.toGeodetic(X);
        return {
          lat,
          lon: normLon(lon),
          target_alt: X[2],
          rule: 'optical axes converge',
          views: centres.length,
        };
      }
    } catch {
      // parallel or ill-conditioned axes
    }
  }
  if (!xs.length)
    throw new SelectError(
      'the camera views neither converge nor meet the ground at takeoff height; enter the target',
    );
  const [lat, lon] = local.toGeodetic([median(xs), median(ys), 0]);
  return {
    lat,
    lon: normLon(lon),
    target_alt: 0,
    rule: 'views meet the takeoff-height ground',
    views: xs.length,
  };
}
