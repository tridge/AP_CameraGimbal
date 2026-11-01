import { finite } from './time.js';
const RAD = Math.PI / 180,
  DEG = 180 / Math.PI;
export function validPose(record) {
  const p = record?.position,
    g = record?.gimbal_attitude,
    a = record?.vehicle_attitude;
  if (!p || !g || !a || [p, g, a].some((x) => !finite(x.age_ms) || x.age_ms < 0 || x.age_ms > 2000))
    return null;
  if (![p.lat_e7, p.lon_e7].every(finite)) return null;
  const lat = p.lat_e7 / 1e7,
    lon = p.lon_e7 / 1e7,
    altitude = p.alt_amsl_m;
  if (
    ![lat, lon, altitude, g.roll_rad, g.pitch_rad, g.yaw_rad, a.yaw_rad].every(finite) ||
    Math.abs(lat) > 90 ||
    Math.abs(lon) > 180
  )
    return null;
  return { lat, lon, altitude, roll: g.roll_rad, pitch: g.pitch_rad, yaw: g.yaw_rad + a.yaw_rad };
}
export function cameraRay(record, x, y, width, height) {
  const pose = validPose(record),
    fov = record?.hfov_deg;
  if (!pose || !finite(fov) || fov <= 0 || fov >= 179 || width <= 0 || height <= 0) return null;
  const focal = width / (2 * Math.tan((fov * RAD) / 2));
  let right = (x - (width - 1) / 2) / focal,
    down = (y - (height - 1) / 2) / focal;
  const r = pose.roll,
    p = pose.pitch,
    yaw = pose.yaw;
  [right, down] = [
    right * Math.cos(r) - down * Math.sin(r),
    right * Math.sin(r) + down * Math.cos(r),
  ];
  const forward = Math.cos(p) + down * Math.sin(p);
  down = -Math.sin(p) + down * Math.cos(p);
  const north = forward * Math.cos(yaw) - right * Math.sin(yaw),
    east = forward * Math.sin(yaw) + right * Math.cos(yaw),
    length = Math.hypot(north, east, down);
  return { pose, north: north / length, east: east / length, down: down / length };
}
export function offsetLocation(lat, lon, north, east) {
  const a = 6378137,
    e2 = 0.00669437999014,
    s = Math.sin(lat * RAD),
    w = Math.sqrt(1 - e2 * s * s),
    m = (a * (1 - e2)) / (w * w * w),
    n = a / w;
  return { lat: lat + (north / m) * DEG, lon: lon + (east / (n * Math.cos(lat * RAD))) * DEG };
}
export class ElevationModel {
  constructor(terrain = null) {
    this.terrain = terrain;
  }
  get description() {
    return this.terrain?.description || 'Terrain not configured';
  }
  at(lat, lon) {
    return this.terrain?.at(lat, lon) ?? null;
  }
  async resolve(...args) {
    const terrain = this.terrain,
      generation = terrain?.generation,
      until = Date.now() + 21000;
    let result = this.project(...args);
    while (result.status === 'loading' && Date.now() < until) {
      await new Promise((resolve) => setTimeout(resolve, 50));
      if (this.terrain !== terrain || terrain?.generation !== generation)
        return {
          point: null,
          status: 'unavailable',
          reason: 'Terrain settings changed; mark this pixel again.',
        };
      result = this.project(...args);
    }
    return result.status === 'loading'
      ? {
          point: null,
          status: 'unavailable',
          reason: 'Terrain request timed out; try marking this pixel again.',
        }
      : result;
  }
  ground(...args) {
    return this.project(...args).point;
  }
  footprint(record, width, height, steps = 4) {
    const pixels = [];
    for (let i = 0; i < steps; i++) pixels.push([((width - 1) * i) / steps, 0]);
    for (let i = 0; i < steps; i++) pixels.push([width - 1, ((height - 1) * i) / steps]);
    for (let i = 0; i < steps; i++) pixels.push([(width - 1) * (1 - i / steps), height - 1]);
    for (let i = 0; i < steps; i++) pixels.push([0, (height - 1) * (1 - i / steps)]);
    const samples = pixels.map(([x, y]) => this.project(record, x, y, width, height));
    if (samples.every((p) => p.point)) {
      const points = samples.map((p) => p.point);
      return { complete: true, segments: [[...points, points[0]]], reason: null };
    }
    const gap = samples.findIndex((p) => !p.point),
      segments = [];
    let line = [];
    // Begin after a gap so the circular perimeter's seam cannot split a valid
    // run. Never close an outline across sky, terrain holes or unknown pose.
    for (let i = 1; i <= samples.length; i++) {
      const point = samples[(gap + i) % samples.length].point;
      if (point) line.push(point);
      else {
        if (line.length > 1) segments.push(line);
        line = [];
      }
    }
    const reasons = [...new Set(samples.filter((s) => !s.point).map((s) => s.reason))];
    return {
      complete: false,
      segments,
      reason: `${segments.length ? 'Partial footprint' : 'Footprint unavailable'}: ${reasons.join(' ')}`,
    };
  }
  project(record, x, y, width, height, maxRange = 10000) {
    const missing = (reason, status = 'unavailable') => ({ point: null, reason, status });
    if (!validPose(record))
      return missing('Camera pose is missing, stale or outside the time-alignment interval.');
    const ray = cameraRay(record, x, y, width, height);
    if (!ray) return missing('Camera field of view is unavailable or invalid.');
    if (ray.down <= 0) return missing('This pixel points above the horizon.');
    const terrain = this.terrain?.forRay?.(ray.pose.lat, ray.pose.lon) || this.terrain;
    const noHeight = () => {
      const issue = terrain?.lastIssue || {
        state: 'unavailable',
        reason: 'Terrain is not configured in Settings.',
      };
      if (issue.state === 'loading') terrain?.prefetchRay?.(ray, maxRange);
      return missing(issue.reason, issue.state);
    };
    const sample = (d) => {
      const p = offsetLocation(ray.pose.lat, ray.pose.lon, ray.north * d, ray.east * d),
        h = terrain?.at(p.lat, p.lon) ?? null;
      return h === null
        ? null
        : {
            ...p,
            altMsl: h,
            // Terrain falls below the camera's local tangent plane. A ray's
            // altitude above the curved surface therefore gains this term.
            difference:
              ray.pose.altitude -
              ray.down * d +
              ((ray.north ** 2 + ray.east ** 2) * d ** 2) / (2 * 6371008.8) -
              h,
          };
    };
    let first = sample(0);
    if (!first) return noHeight();
    if (first.difference <= 0)
      return missing('Camera altitude is at or below the terrain at the aircraft position.');
    let lo = 0;
    for (let hi = 20; hi <= maxRange; hi += 20) {
      const current = sample(hi);
      if (!current) return noHeight();
      if (current.difference <= 0) {
        let end = hi;
        for (let j = 0; j < 15; j++) {
          const mid = (lo + end) / 2,
            q = sample(mid);
          if (!q) return noHeight();
          if (q.difference > 0) lo = mid;
          else end = mid;
        }
        const out = sample((lo + end) / 2);
        if (!out) return noHeight();
        return {
          status: 'ready',
          reason: null,
          point: {
            lat: out.lat,
            lon: out.lon,
            altMsl: out.altMsl,
            range: (lo + end) / 2,
            elevationSource: this.description,
            terrain: terrain?.provenance?.() ?? null,
            approximate: true,
          },
        };
      }
      lo = hi;
    }
    return missing(`No ground intersection within ${maxRange / 1000} km.`);
  }
}
