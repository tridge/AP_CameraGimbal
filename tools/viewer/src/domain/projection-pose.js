import { validPose } from './geometry.js';
import { finite } from './time.js';

const wrap = (angle) => Math.atan2(Math.sin(angle), Math.cos(angle));
const median = (values) => {
  const sorted = values.slice().sort((a, b) => a - b),
    middle = sorted.length >> 1;
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
};

/** A deterministic, centred robust fit of earth-frame camera yaw. This is a
 * display aid only: never rewrite telemetry, alignment or pixel-finding poses.
 * Unlike a playback-time low-pass, seeks and reverse playback give the same
 * result and a steady turn incurs no phase lag. Gaps/stale samples stop the fit.
 */
function fitProjectionPose(record, timeline, seconds) {
  const pose = validPose(record),
    utc = record?.pose_utc_us ?? record?.utc_us;
  if (!pose || !finite(utc) || !finite(seconds) || seconds <= 0) return record;
  const samples = timeline.samples;
  let lo = 0,
    hi = samples.length;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    if (samples[mid].utc < utc) lo = mid + 1;
    else hi = mid;
  }
  const nearest =
    lo && (lo === samples.length || utc - samples[lo - 1].utc < samples[lo].utc - utc)
      ? lo - 1
      : lo;
  const centre = samples[nearest];
  if (!centre || Math.abs(centre.utc - utc) > 250000 || !validPose(centre.record)) return record;
  const first = [],
    last = [],
    radius = Math.min(2, seconds) * 500000;
  for (const [direction, output] of [
    [-1, first],
    [1, last],
  ]) {
    let previous = centre.utc;
    for (let i = nearest + direction; i >= 0 && i < samples.length; i += direction) {
      const s = samples[i];
      if (
        Math.abs(s.utc - utc) > radius ||
        Math.abs(s.utc - previous) > 250000 ||
        !validPose(s.record)
      )
        break;
      output.push(s);
      previous = s.utc;
    }
  }
  const nearby = [...first.reverse(), centre, ...last];
  if (nearby.length < 5 || nearby.at(-1).utc - nearby[0].utc < 200000) return record;
  // Bound fitting cost for unusually high-rate metadata.
  const stride = Math.max(1, Math.ceil(nearby.length / 61));
  const points = nearby
    .filter((_, i) => i % stride === 0)
    .map((s) => ({
      t: (s.utc - utc) / 1e6,
      y: pose.yaw + wrap(validPose(s.record).yaw - pose.yaw),
    }));
  const slopes = [];
  for (let i = 0; i < points.length; i++)
    for (let j = i + 1; j < points.length; j++)
      if (points[j].t - points[i].t > 0.05)
        slopes.push((points[j].y - points[i].y) / (points[j].t - points[i].t));
  if (!slopes.length) return record;
  let slope = median(slopes),
    yaw = median(points.map((p) => p.y - slope * p.t));
  for (let iteration = 0; iteration < 3; iteration++) {
    const threshold = Math.max(
      (0.02 * Math.PI) / 180,
      3 * 1.4826 * median(points.map((p) => Math.abs(p.y - yaw - slope * p.t))),
    );
    let w = 0,
      wt = 0,
      wy = 0,
      wtt = 0,
      wty = 0;
    for (const p of points) {
      const residual = Math.abs(p.y - yaw - slope * p.t) / threshold;
      if (residual >= 1) continue;
      const weight = (1 - residual ** 2) ** 2 * Math.exp(-0.5 * (p.t / (seconds / 4)) ** 2);
      w += weight;
      wt += weight * p.t;
      wy += weight * p.y;
      wtt += weight * p.t * p.t;
      wty += weight * p.t * p.y;
    }
    const determinant = w * wtt - wt * wt;
    if (determinant < 1e-12) break;
    yaw = (wy * wtt - wty * wt) / determinant;
    slope = (w * wty - wt * wy) / determinant;
  }
  return {
    ...record,
    gimbal_attitude: {
      ...record.gimbal_attitude,
      yaw_rad: wrap(yaw - record.vehicle_attitude.yaw_rad),
    },
    projection_yaw_smoothing: {
      seconds,
      samples: points.length,
      first_utc_us: nearby[0].utc,
      last_utc_us: nearby.at(-1).utc,
      raw_yaw_rad: wrap(pose.yaw),
      yaw_rad: wrap(yaw),
    },
  };
}

const caches = new WeakMap();
export function projectionPose(record, timeline, seconds = 2) {
  if (!validPose(record) || !finite(seconds) || seconds <= 0) return record;
  let cache = caches.get(timeline);
  if (!cache || cache.samples !== timeline.samples) {
    cache = { samples: timeline.samples, windows: new Map() };
    caches.set(timeline, cache);
  }
  if (!cache.windows.has(seconds)) cache.windows.set(seconds, new WeakMap());
  const fits = cache.windows.get(seconds);
  const fit = (r) => {
    if (!fits.has(r))
      fits.set(r, fitProjectionPose(r, timeline, seconds * 0.7).projection_yaw_smoothing || null);
    return fits.get(r);
  };
  const centre = fit(record);
  if (!centre) return record;
  const utc = record.pose_utc_us ?? record.utc_us,
    radius = seconds * 150000;
  // A short centred blend prevents the robust estimator's changing inlier set
  // from introducing new frame-to-frame steps. Cache fits for adjacent frames.
  let lo = 0,
    hi = timeline.samples.length;
  while (lo < hi) {
    const m = (lo + hi) >>> 1;
    if (timeline.samples[m].utc < utc - radius) lo = m + 1;
    else hi = m;
  }
  let w = 0,
    wt = 0,
    wy = 0,
    wtt = 0,
    wty = 0;
  for (let i = lo; i < timeline.samples.length; i++) {
    const s = timeline.samples[i];
    if (s.utc > utc + radius) break;
    if (s.utc < centre.first_utc_us || s.utc > centre.last_utc_us) continue;
    const f = fit(s.record);
    if (!f) continue;
    const t = (s.utc - utc) / 1e6,
      weight = Math.exp(-0.5 * (t / (seconds * 0.075)) ** 2);
    const y = centre.yaw_rad + wrap(f.yaw_rad - centre.yaw_rad);
    w += weight;
    wt += weight * t;
    wy += weight * y;
    wtt += weight * t * t;
    wty += weight * t * y;
  }
  const det = w * wtt - wt * wt;
  const yaw = det > 1e-12 ? (wy * wtt - wty * wt) / det : centre.yaw_rad;
  return {
    ...record,
    gimbal_attitude: {
      ...record.gimbal_attitude,
      yaw_rad: wrap(yaw - record.vehicle_attitude.yaw_rad),
    },
    projection_yaw_smoothing: { ...centre, seconds, yaw_rad: wrap(yaw) },
  };
}
