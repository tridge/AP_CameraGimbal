// OpenCV 5.0 functions train.py uses, for a camera with only k1 distortion, reproduced exactly
// (checked against cv2 by test/stage5b): initUndistortRectifyMap (newK = K, R = I, CV_32FC1),
// remap INTER_LINEAR + BORDER_REFLECT on float32 images (OpenCV 5's SIMD path: direct
// fractional weights with FMA, not the 1/32 fixed-point tables of OpenCV 4), undistortPoints
// (5 iterations, P = K).
const f = Math.fround;
// float32 fused multiply-add through float64 (the product of two float32 values is exact)
const fma32 = (a, b, c) => f(a * b + c);

/** cv::invert(K, DECOMP_LU) row-major for K = [[fx,0,cx],[0,fy,cy],[0,0,1]] */
function invPinhole(fx, fy, cx, cy) {
  return [1 / fx, 0, -cx / fx, 0, 1 / fy, -cy / fy, 0, 0, 1];
}

/** cv2.initUndistortRectifyMap(K, [k1,0,0,0], None, K, (W, H), CV_32FC1) */
export function undistortMaps(fx, fy, cx, cy, k1, W, H) {
  const ir = invPinhole(fx, fy, cx, cy);
  const mapx = new Float32Array(W * H);
  const mapy = new Float32Array(W * H);
  for (let i = 0; i < H; i++) {
    let _x = i * ir[1] + ir[2],
      _y = i * ir[4] + ir[5],
      _w = i * ir[7] + ir[8];
    for (let j = 0; j < W; j++, _x += ir[0], _y += ir[3], _w += ir[6]) {
      const w = 1 / _w,
        x = _x * w,
        y = _y * w;
      const r2 = x * x + y * y;
      const kr = 1 + k1 * r2;
      mapx[i * W + j] = fx * (x * kr) + cx;
      mapy[i * W + j] = fy * (y * kr) + cy;
    }
  }
  return { mapx, mapy };
}

const reflect = (p, n) => {
  // BORDER_REFLECT: fedcba|abcdefgh|hgfedcb
  for (;;) {
    if (p < 0) p = -p - 1;
    else if (p >= n) p = 2 * n - p - 1;
    else return p;
  }
};

/** cv2.remap(src float32 [H, W], mapx, mapy, INTER_LINEAR, borderMode=BORDER_REFLECT) */
export function remapLinearReflect(src, W, H, mapx, mapy, out = new Float32Array(W * H)) {
  for (let i = 0; i < W * H; i++) {
    const sx = mapx[i],
      sy = mapy[i];
    const x0 = Math.floor(sx),
      y0 = Math.floor(sy);
    const ax = f(sx - x0),
      ay = f(sy - y0);
    let p00, p01, p10, p11;
    if (x0 >= 0 && y0 >= 0 && x0 + 1 < W && y0 + 1 < H) {
      const o = y0 * W + x0;
      p00 = src[o];
      p01 = src[o + 1];
      p10 = src[o + W];
      p11 = src[o + W + 1];
    } else {
      const xa = reflect(x0, W),
        xb = reflect(x0 + 1, W);
      const ya = reflect(y0, H) * W,
        yb = reflect(y0 + 1, H) * W;
      p00 = src[ya + xa];
      p01 = src[ya + xb];
      p10 = src[yb + xa];
      p11 = src[yb + xb];
    }
    const a = fma32(ax, f(p01 - p00), p00);
    const b = fma32(ax, f(p11 - p10), p10);
    out[i] = fma32(ay, f(b - a), a);
  }
  return out;
}

/** cv2.undistortPoints(xy, K, [k1,0,0,0], P=K) for one point (default criteria: 5 iterations) */
export function undistortPoint(u, v, fx, fy, cx, cy, k1) {
  const ifx = 1 / fx,
    ify = 1 / fy;
  const x0 = (u - cx) * ifx,
    y0 = (v - cy) * ify;
  let x = x0,
    y = y0;
  for (let j = 0; j < 5; j++) {
    const r2 = x * x + y * y;
    const icdist = 1 / (1 + k1 * r2);
    if (icdist < 0) {
      x = (u - cx) * ifx;
      y = (v - cy) * ify;
      break;
    }
    x = x0 * icdist;
    y = y0 * icdist;
  }
  return [fx * x + cx, fy * y + cy];
}
