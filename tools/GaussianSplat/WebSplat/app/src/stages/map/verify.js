// Two-view verification of stage 4's raw matches with the fork (ws_v_*): COLMAP
// TwoViewGeometryOptions() defaults as pycolmap.verify_matches, including the clearing of
// pairs below min_num_inliers (15) to an empty config-0 geometry. Output entries are
// encodeGeometry()'s (match/format.js).
import { STATUS_SKIPPED, MODEL_F64 } from '../match/format.js';
import { imageKeypoints } from './features.js';

export const VERIFY_DEFAULTS = {
  seed: 0, // RANSAC PRNG seed, reset per estimate: results independent of order and worker
  legacy: true, // COLMAP 3.13 estimators (the native reference); false = this build's 4.2.1
};

// camera: {model, width, height, params}; features: decodeFeatures() (keypoints, counts,
// offsets suffice); images: the frame indices whose keypoints to load (default all)
export function createVerifier(cw, camera, features, opts = {}, images = null) {
  const o = { ...VERIFY_DEFAULTS, ...opts };
  const M = cw.M;
  const params = new Float64Array(camera.params);
  const n = features.counts.length;
  cw.check(
    cw.with([camera.model, params], (m, p) =>
      M._ws_v_begin(m, camera.width, camera.height, p, params.length, n),
    ),
    'ws_v_begin',
  );
  for (const k of images ?? features.counts.keys()) {
    const kp = imageKeypoints(features, k);
    cw.check(
      cw.with([kp], (p) => M._ws_v_image(k, p, features.counts[k], 6)),
      'ws_v_image',
    );
  }
  let buf = 0,
    cap = 0;
  return {
    // e: a decodeMatches() entry
    pair(e) {
      if (e.status === STATUS_SKIPPED)
        return {
          i: e.i,
          j: e.j,
          config: 0,
          matches: new Uint32Array(0),
          model: new Float64Array(MODEL_F64),
        };
      const m = e.matches;
      if (m.byteLength > cap) {
        if (buf) M._free(buf);
        cap = Math.max(m.byteLength, 2 * cap, 4096);
        buf = M._malloc(cap);
        if (!buf) throw new Error('out of wasm memory');
      }
      M.HEAPU32.set(m, buf / 4);
      const count = cw.check(
        M._ws_v_pair(e.i, e.j, buf, m.length / 2, o.seed, o.legacy ? 1 : 0),
        'ws_v_pair',
      );
      return {
        i: e.i,
        j: e.j,
        config: M._ws_v_config(),
        matches: cw.u32(M._ws_v_inliers(), 2 * count),
        model: cw.f64(M._ws_v_model(), MODEL_F64),
      };
    },
    end() {
      if (buf) M._free(buf);
      buf = cap = 0;
      M._ws_v_end();
    },
  };
}

// all pairs in one synchronous call; opts.onProgress(done, total) may return truthy to stop
export function verifyPairs(cw, camera, features, pairs, opts = {}) {
  const v = createVerifier(cw, camera, features, opts);
  const out = [];
  try {
    for (let n = 0; n < pairs.length; n++) {
      if (opts.onProgress && n % 16 === 0 && opts.onProgress(n, pairs.length))
        throw Object.assign(new Error('verification stopped'), { name: 'AbortError', done: n });
      out.push(v.pair(pairs[n]));
    }
    if (opts.onProgress) opts.onProgress(pairs.length, pairs.length);
  } finally {
    v.end();
  }
  return out;
}
