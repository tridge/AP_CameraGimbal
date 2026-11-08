// Telemetry-seeded mapping with the fork (ws_m_*): sfm.py prior_mapping(). Every image starts
// at its telemetry pose (registered); each round triangulates from scratch, runs pose-prior
// bundle adjustment and filters; intrinsics are refined from round 1 on.
import { imageKeypoints } from './features.js';

export const PRIOR_MAP_DEFAULTS = {
  thresholds: [40, 16, 8, 4], // px, per round
  priorSigma: 5, // m, telemetry position std dev (covariance sigma^2 I, CARTESIAN)
  maxIterations: 50, // Ceres iterations per BA
  cameraModel: 'SIMPLE_RADIAL',
};

const g = (x, digits) => Number(x.toPrecision(digits)); // printf %.<digits>g

// sfm.py's database camera: [K00, K02, K12] (+ k1 0) printed '%.6f' into the reader options
export function cameraFromFrames(info, model = 'SIMPLE_RADIAL') {
  const K = info.K;
  const f6 = (x) => Number(x.toFixed(6));
  const params =
    model === 'PINHOLE'
      ? [K[0][0], K[1][1], K[0][2], K[1][2]]
      : [K[0][0], K[0][2], K[1][2], ...({ SIMPLE_RADIAL: [0], RADIAL: [0, 0] }[model] ?? [])];
  return { model, width: info.width, height: info.height, params: params.map(f6) };
}

// scipy Rotation.from_matrix(R).as_quat() as w, x, y, z
export function quatWxyz(R) {
  const d = [R[0][0], R[1][1], R[2][2], R[0][0] + R[1][1] + R[2][2]];
  let c = 0;
  for (let k = 1; k < 4; k++) if (d[k] > d[c]) c = k;
  const q = [0, 0, 0, 0]; // x y z w
  if (c !== 3) {
    const i = c,
      j = (i + 1) % 3,
      k = (j + 1) % 3;
    q[i] = 1 - d[3] + 2 * R[i][i];
    q[j] = R[j][i] + R[i][j];
    q[k] = R[k][i] + R[i][k];
    q[3] = R[k][j] - R[j][k];
  } else {
    q[0] = R[2][1] - R[1][2];
    q[1] = R[0][2] - R[2][0];
    q[2] = R[1][0] - R[0][1];
    q[3] = 1 + d[3];
  }
  const n = Math.hypot(...q);
  return [q[3] / n, q[0] / n, q[1] / n, q[2] / n];
}

// sfm.py's telemetry model (sparse/telemetry images.txt): cam_from_world with the quaternion
// printed '%.12g' and the translation '%.9g'
export function telemetryPose(frame) {
  const Rwc = frame.rotation,
    C = frame.position;
  const R = [0, 1, 2].map((r) => [0, 1, 2].map((c) => Rwc[c][r])); // R_cw = R_wc^T
  const q = quatWxyz(R).map((v) => g(v, 12));
  const t = R.map((row) => -(row[0] * C[0] + row[1] * C[1] + row[2] * C[2])).map((v) => g(v, 9));
  return [...q, ...t];
}

// camera: cameraFromFrames(); frames: frames.json frames; features: decodeFeatures();
// geometry: decodeGeometry() entries (frame indices). opts.onProgress(phase, done, total,
// round) may return truthy to stop (throws AbortError); opts.onRound(stats) after each round.
// Returns {rounds, files: {'cameras.bin', 'images.bin', 'points3D.bin', 'rigs.bin',
// 'frames.bin'}} (COLMAP binary model, image ids 1..N in frames order).
export function priorMap(cw, camera, frames, features, geometry, opts = {}) {
  const o = { ...PRIOR_MAP_DEFAULTS, ...opts };
  const M = cw.M;
  // the telemetry model's camera is printed '%.9g' (sparse/telemetry/cameras.txt); the database
  // keeps the reader's values
  const params = new Float64Array(camera.params);
  M._ws_reset_stop();
  let round = -1;
  cw.onProgress = o.onProgress
    ? (phase, done, total) => o.onProgress(phase, done, total, round)
    : null;
  const stopped = () => Object.assign(new Error('mapping stopped'), { name: 'AbortError', round });
  try {
    cw.check(
      cw.with([camera.model, params], (m, p) =>
        M._ws_m_begin(m, camera.width, camera.height, p, params.length),
      ),
      'ws_m_begin',
    );
    const s2 = o.priorSigma * o.priorSigma;
    const cov = new Float64Array([s2, 0, 0, 0, s2, 0, 0, 0, s2]);
    frames.forEach((f, k) => {
      const pose = new Float64Array(telemetryPose(f));
      const pos = new Float64Array(f.position);
      const kp = imageKeypoints(features, k);
      cw.check(
        cw.with([f.name, kp, pose, pos, cov], (name, pk, pp, ppos, pc) =>
          M._ws_m_image(name, pk, features.counts[k], 6, pp, ppos, pc),
        ),
        'ws_m_image',
      );
    });
    for (const e of geometry) {
      if (!e.matches.length) continue;
      cw.check(
        cw.with([e.matches], (p) => M._ws_m_geometry(e.i, e.j, e.config, p, e.matches.length / 2)),
        'ws_m_geometry',
      );
    }
    cw.check(M._ws_m_prepare(), 'ws_m_prepare');
    const rounds = [];
    for (let n = 0; n < o.thresholds.length; n++) {
      round = n;
      const rc = cw.check(M._ws_m_round(o.thresholds[n], n, o.maxIterations), 'ws_m_round');
      if (rc === 1) throw stopped();
      const s = cw.f64(M._ws_m_round_stats(), 12);
      const stats = {
        round: n,
        threshold_px: s[0],
        triangulated: s[1],
        filtered: s[2],
        observations: s[3],
        ba_iterations: s[4],
        ba_termination: s[5],
        mean_reproj_px: s[6],
        seconds: s[7],
        focal: s[8],
        k1: s[9],
        ba_initial_cost: s[10],
        ba_final_cost: s[11],
      };
      rounds.push(stats);
      if (o.onRound) o.onRound(stats);
    }
    cw.check(M._ws_m_export(), 'ws_m_export');
    const names = ['cameras.bin', 'images.bin', 'points3D.bin', 'rigs.bin', 'frames.bin'];
    const files = {};
    names.forEach(
      (name, k) => (files[name] = cw.bytes(M._ws_m_export_data(k), M._ws_m_export_size(k))),
    );
    return { rounds, files };
  } finally {
    cw.onProgress = null;
    M._ws_m_end();
  }
}

// model/model.json (CONTRACTS.md 5)
export function modelJson(camera, rounds, opts = {}) {
  const o = { ...PRIOR_MAP_DEFAULTS, ...opts };
  return {
    format: 'websplat.model',
    version: 1,
    camera_model: camera.model,
    rounds: rounds.map((r) => ({
      threshold_px: r.threshold_px,
      triangulated: r.triangulated,
      filtered: r.filtered,
      mean_reproj_px: r.mean_reproj_px,
      ba_iterations: r.ba_iterations,
      focal: r.focal,
      k1: r.k1,
    })),
    prior_sigma_m: o.priorSigma,
    max_iterations: o.maxIterations,
    image_order: 'frames',
  };
}
