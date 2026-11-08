/** Port of splat_common.py's pose helpers (float64, 3-vectors and row-major 3x3 arrays). */
export const EARTH_RADIUS = 6378137.0;
const DEG2RAD = Math.PI / 180.0; // CPython math.radians
const RAD2DEG = 180.0 / Math.PI; // CPython math.degrees
const radians = (x) => x * DEG2RAD;
const degrees = (x) => x * RAD2DEG;

// OpenCV camera axes (x right, y down, z forward) in body FRD axes
export const R_BODY_CAM = [
  [0, 0, 1],
  [1, 0, 0],
  [0, 1, 0],
];
export const R_ENU_NED = [
  [0, 1, 0],
  [1, 0, 0],
  [0, 0, -1],
];

export function matmul(a, b) {
  return a.map((row) =>
    [0, 1, 2].map((j) => row[0] * b[0][j] + row[1] * b[1][j] + row[2] * b[2][j]),
  );
}
export const matvec = (m, v) => m.map((row) => row[0] * v[0] + row[1] * v[1] + row[2] * v[2]);
export const transpose = (m) => [0, 1, 2].map((i) => [m[0][i], m[1][i], m[2][i]]);
export const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
export const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
export const norm = (a) => Math.sqrt(dot(a, a));

export function rotX(a) {
  const c = Math.cos(a),
    s = Math.sin(a);
  return [
    [1, 0, 0],
    [0, c, -s],
    [0, s, c],
  ];
}
export function rotY(a) {
  const c = Math.cos(a),
    s = Math.sin(a);
  return [
    [c, 0, s],
    [0, 1, 0],
    [-s, 0, c],
  ];
}
export function rotZ(a) {
  const c = Math.cos(a),
    s = Math.sin(a);
  return [
    [c, -s, 0],
    [s, c, 0],
    [0, 0, 1],
  ];
}

/** Flat-earth ENU frame about an origin; adequate over a few km. */
export class LocalFrame {
  constructor(lat, lon, alt) {
    this.lat = lat;
    this.lon = lon;
    this.alt = alt;
    this.coslat = Math.cos(radians(lat));
  }
  toEnu(lat, lon, alt) {
    const n = radians(lat - this.lat) * EARTH_RADIUS;
    const e = radians(lon - this.lon) * EARTH_RADIUS * this.coslat;
    return [e, n, alt - this.alt];
  }
  toGeodetic(enu) {
    const lat = this.lat + degrees(enu[1] / EARTH_RADIUS);
    const lon = this.lon + degrees(enu[0] / (EARTH_RADIUS * this.coslat));
    return [lat, lon, this.alt + enu[2]];
  }
}

/** World(ENU)-from-camera rotation (gimbal roll/pitch level, yaw vehicle-relative). */
export function cameraRotation(t) {
  const g = t.gimbal_attitude ?? {};
  const yaw = t.vehicle_attitude.yaw_rad + (g.yaw_rad ?? 0.0);
  const rNedBody = matmul(matmul(rotZ(yaw), rotY(g.pitch_rad ?? 0.0)), rotX(g.roll_rad ?? 0.0));
  return matmul(matmul(R_ENU_NED, rNedBody), R_BODY_CAM);
}

export function intrinsics(width, height, hfovDeg) {
  const f = width / 2 / Math.tan(radians(hfovDeg) / 2);
  return [
    [f, 0, width / 2],
    [0, f, height / 2],
    [0, 0, 1.0],
  ];
}

/** inverse of an upper-triangular pinhole K */
export function invK(K) {
  const [[fx, s, cx], [, fy, cy]] = K;
  return [
    [1 / fx, -s / (fx * fy), (s * cy - cx * fy) / (fx * fy)],
    [0, 1 / fy, -cy / fy],
    [0, 0, 1],
  ];
}

/** 3x3 solve by LU with partial pivoting (LAPACK dgesv's algorithm) */
// rcond 0 fails on an exactly zero pivot, as LAPACK does; rcond > 0 also rejects pivots below
// rcond * max |A| (ill-conditioned systems)
export function solve3(A, b, rcond = 0) {
  const a = A.map((r) => r.slice());
  const x = b.slice();
  const scale = Math.max(...A.flat().map(Math.abs));
  if (!Number.isFinite(scale) || !b.every(Number.isFinite)) throw new Error('non-finite system');
  for (let k = 0; k < 3; k++) {
    let p = k;
    for (let i = k + 1; i < 3; i++) if (Math.abs(a[i][k]) > Math.abs(a[p][k])) p = i;
    if (a[p][k] === 0 || Math.abs(a[p][k]) <= rcond * scale) throw new Error('singular matrix');
    [a[k], a[p]] = [a[p], a[k]];
    [x[k], x[p]] = [x[p], x[k]];
    for (let i = k + 1; i < 3; i++) {
      const m = a[i][k] / a[k][k];
      for (let j = k; j < 3; j++) a[i][j] -= m * a[k][j];
      x[i] -= m * x[k];
    }
  }
  for (let k = 2; k >= 0; k--) {
    let s = x[k];
    for (let j = k + 1; j < 3; j++) s -= a[k][j] * x[j];
    x[k] = s / a[k][k];
  }
  return x;
}

function medianOf(values) {
  const s = Float64Array.from(values).sort();
  const h = s.length >> 1;
  return s.length % 2 ? s[h] : (s[h - 1] + s[h]) / 2;
}

/** Robust least-squares intersection of rays; returns [point, median error in pixels].
 * rcond: see solve3. */
export function triangulateRays(centres, dirs, focal, iterations = 5, { rcond = 0 } = {}) {
  let weights = centres.map(() => 1);
  let X, err;
  const proj = (d) => [0, 1, 2].map((i) => [0, 1, 2].map((j) => (i === j ? 1 : 0) - d[i] * d[j]));
  for (let it = 0; it < iterations; it++) {
    const A = [
      [0, 0, 0],
      [0, 0, 0],
      [0, 0, 0],
    ];
    const b = [0, 0, 0];
    centres.forEach((c, k) => {
      const P = proj(dirs[k]);
      const Pc = matvec(P, c);
      for (let i = 0; i < 3; i++) {
        for (let j = 0; j < 3; j++) A[i][j] += weights[k] * P[i][j];
        b[i] += weights[k] * Pc[i];
      }
    });
    X = solve3(A, b, rcond);
    err = centres.map((c, k) => (norm(matvec(proj(dirs[k]), sub(X, c))) / norm(sub(X, c))) * focal);
    const scale = Math.max(medianOf(err), 5.0);
    weights = err.map((e) => 1 / (1 + (e / (2 * scale)) ** 2));
  }
  const e = medianOf(err);
  if (rcond > 0 && !(X.every(Number.isFinite) && Number.isFinite(e)))
    throw new Error('rays give no finite intersection');
  return [X, e];
}

/** extract_frames.project: pixel and depth of a world point, or null behind the camera */
export function project(K, centre, R, point) {
  const x = matvec(transpose(R), sub(point, centre));
  if (x[2] <= 0) return null;
  const uv = matvec(K, x);
  return [[uv[0] / uv[2], uv[1] / uv[2]], x[2]];
}
