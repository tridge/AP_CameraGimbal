// COLMAP SIFT through the fork (ws_sift): VLFeat on the CPU, FeatureExtractionOptions()
// defaults as sfm.py's extraction (max_num_features 8192), on stage 2's CLAHE 8-bit images.
export const SIFT_DEFAULTS = { maxNumFeatures: 8192 };
// RGB jobs' config.features default, as sfm.py; mapping thins matches to fit its heap
export const RGB_MAX_FEATURES = 8192;
// RGB jobs' config.map.camera default, as the native RGB recipe (sfm.py --camera-model RADIAL)
export const RGB_CAMERA = 'RADIAL';

// grey: Uint8Array(width * height), top row first. Returns {keypoints: Float32Array(6 n)
// (x, y, a11, a12, a21, a22), descriptors: Uint8Array(128 n)}
export function siftImage(cw, grey, width, height, opts = {}) {
  const o = { ...SIFT_DEFAULTS, ...opts };
  const M = cw.M;
  const n = cw.check(
    cw.with([grey], (p) => M._ws_sift(p, width, height, o.maxNumFeatures)),
    'ws_sift',
  );
  return {
    keypoints: new Float32Array(M.HEAPU8.buffer, M._ws_sift_keypoints(), 6 * n).slice(),
    descriptors: cw.bytes(M._ws_sift_descriptors(), 128 * n),
  };
}
