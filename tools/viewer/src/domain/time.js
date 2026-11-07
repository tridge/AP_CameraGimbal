/** Playback, recorded UTC and pose-selection clocks remain distinct. */
export function finite(value) {
  return typeof value === 'number' && Number.isFinite(value);
}
export function utcText(value) {
  if (!finite(value) || value <= 0) return 'Unavailable';
  const date = new Date(value / 1000);
  return finite(date.getTime())
    ? date.toISOString().replace('T', ' ').replace('Z', ' UTC')
    : 'Unavailable';
}
export function validateTiePoints(points) {
  if (!Array.isArray(points)) throw new Error('Time anchors must be a JSON array.');
  return points
    .map((p, i) => {
      const seconds = p.videoSeconds;
      const utcUs = finite(p.utcUs)
        ? p.utcUs
        : typeof p.utc === 'string' && /(?:Z|[+-]\d\d:\d\d)$/.test(p.utc)
          ? Date.parse(p.utc) * 1000
          : NaN;
      if (!finite(seconds) || !finite(utcUs) || utcUs <= 0)
        throw new Error(
          `Invalid time anchor ${i + 1}; use videoSeconds and timezone-qualified utc.`,
        );
      if (i && seconds <= points[i - 1].videoSeconds)
        throw new Error('Time anchors must increase in videoSeconds.');
      return { videoSeconds: seconds, utcUs };
    })
    .map((p, i, a) => {
      if (i && p.utcUs <= a[i - 1].utcUs) throw new Error('Time anchors must increase in UTC.');
      return p;
    });
}
export function correctedUtc(seconds, recordedUtc, offsetSeconds, points = []) {
  if (!finite(offsetSeconds)) return null;
  let value = recordedUtc;
  if (points.length) {
    if (points.length === 1) value = points[0].utcUs + (seconds - points[0].videoSeconds) * 1e6;
    else {
      // No unannounced extrapolation beyond a calibrated interval.
      if (seconds < points[0].videoSeconds || seconds > points.at(-1).videoSeconds) return null;
      let i = 1;
      while (i < points.length - 1 && seconds > points[i].videoSeconds) i++;
      const a = points[i - 1],
        b = points[i];
      value =
        a.utcUs +
        ((seconds - a.videoSeconds) / (b.videoSeconds - a.videoSeconds)) * (b.utcUs - a.utcUs);
    }
  }
  return finite(value) ? Math.round(value + offsetSeconds * 1e6) : null;
}
export function frameAt(frames, seconds, origin = frames[0]?.pts || 0) {
  let lo = 0,
    hi = frames.length;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    if (frames[mid].pts - origin <= seconds) lo = mid + 1;
    else hi = mid;
  }
  return Math.max(0, lo - 1);
}
/** Rebase saved review seconds without changing the source frame or UTC fit.
 * Negative anchors retain calibration before an edited clip. Findings outside
 * its presentation interval remain exportable but cannot seek a hidden frame.
 */
export function rebaseReviewTime(
  findings,
  anchors,
  previousOrigin,
  origin,
  presentationEnd = Infinity,
  presentationStart = 0,
) {
  const shift = previousOrigin - origin;
  return {
    findings: findings.map((finding) => {
      const videoSeconds = finite(finding.originalPts)
        ? finding.originalPts - origin
        : finding.videoSeconds + shift;
      return {
        ...finding,
        videoSeconds,
        presentationUnavailable:
          videoSeconds < presentationStart - origin || videoSeconds >= presentationEnd - origin,
      };
    }),
    anchors: anchors.map((point) => ({ ...point, videoSeconds: point.videoSeconds + shift })),
  };
}
export const wrapLongitude = (x) => ((((x + 180) % 360) + 360) % 360) - 180;
const wrap = (x) => Math.atan2(Math.sin(x), Math.cos(x));
export class PoseTimeline {
  constructor() {
    this.samples = [];
  }
  rebuild(frames) {
    this.samples = frames
      .filter((f) => finite(f.telemetry?.utc_us))
      .map((f) => ({ utc: f.telemetry.utc_us, record: f.telemetry }))
      .sort((a, b) => a.utc - b.utc);
    // Repeated labels are not invented subsecond exposure dates. Keep one pose
    // per label, retaining the first sample rather than blending unrelated data.
    this.samples = this.samples.filter((s, i, a) => !i || s.utc !== a[i - 1].utc);
  }
  at(utc, original, unshifted = false) {
    if (unshifted) return original;
    if (
      !finite(utc) ||
      !this.samples.length ||
      utc < this.samples[0].utc ||
      utc > this.samples.at(-1).utc
    )
      return null;
    let lo = 0,
      hi = this.samples.length;
    while (lo < hi) {
      const m = (lo + hi) >>> 1;
      if (this.samples[m].utc < utc) lo = m + 1;
      else hi = m;
    }
    const b = this.samples[lo];
    if (b.utc === utc) return b.record;
    const a = this.samples[lo - 1];
    if (!a || b.utc - a.utc > 2e6) return null;
    const f = (utc - a.utc) / (b.utc - a.utc),
      out = { ...a.record, pose_utc_us: utc };
    for (const name of ['position', 'vehicle_attitude', 'gimbal_attitude', 'velocity']) {
      const left = a.record[name],
        right = b.record[name];
      const required = {
        position: ['lat_e7', 'lon_e7', 'alt_amsl_m'],
        vehicle_attitude: ['yaw_rad'],
        gimbal_attitude: ['roll_rad', 'pitch_rad', 'yaw_rad'],
        velocity: ['vn_m_s', 've_m_s', 'vd_m_s'],
      }[name];
      if (
        !left ||
        !right ||
        [left, right].some(
          (v) =>
            !finite(v.age_ms) ||
            v.age_ms < 0 ||
            v.age_ms > 2000 ||
            required.some((key) => !finite(v[key])),
        )
      ) {
        out[name] = null;
        continue;
      }
      out[name] = { ...left };
      for (const key of new Set([...Object.keys(left), ...Object.keys(right)])) {
        if (key === 'age_ms') continue;
        const value = left[key],
          other = right[key];
        if (finite(value) && finite(other))
          out[name][key] = key.endsWith('_rad')
            ? value + wrap(other - value) * f
            : key === 'lon_e7'
              ? wrapLongitude(value / 1e7 + wrapLongitude((other - value) / 1e7) * f) * 1e7
              : value + (other - value) * f;
        else if (
          typeof value === 'number' ||
          typeof other === 'number' ||
          value == null ||
          other == null
        )
          out[name][key] = null;
      }
      out[name].age_ms = Math.max(left.age_ms, right.age_ms);
    }
    return out;
  }
}
