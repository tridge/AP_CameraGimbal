/** OpenCV CLAHE (imgproc/src/clahe.cpp) for 8-bit single-channel images, reproducing its integer
 * clipping and float32 interpolation so the output is bit-identical to cv2.createCLAHE().apply().
 */
const f = Math.fround;

// cvRound: nearest, ties to even
function roundEven(x) {
  const r = Math.round(x);
  return r - x === 0.5 && r % 2 !== 0 ? r - 1 : r;
}

/** Border pixel index for cv::BORDER_REFLECT_101. */
function reflect101(p, n) {
  if (n === 1) return 0;
  while (p < 0 || p >= n) p = p < 0 ? -p : 2 * n - p - 2;
  return p;
}

/** src: Uint8Array w*h. Returns a new Uint8Array. */
export function clahe(src, width, height, { clipLimit = 3.0, tilesX = 8, tilesY = 8 } = {}) {
  const histSize = 256;
  let lutSrc = src,
    lutW = width,
    lutH = height;
  if (width % tilesX || height % tilesY) {
    // copyMakeBorder(bottom/right, BORDER_REFLECT_101), as OpenCV does for the histograms only
    lutW = width + tilesX - (width % tilesX);
    lutH = height + tilesY - (height % tilesY);
    lutSrc = new Uint8Array(lutW * lutH);
    for (let y = 0; y < lutH; y++) {
      const sy = reflect101(y, height) * width;
      for (let x = 0; x < lutW; x++) lutSrc[y * lutW + x] = src[sy + reflect101(x, width)];
    }
  }
  const tw = lutW / tilesX,
    th = lutH / tilesY;
  const tileSizeTotal = tw * th;
  const lutScale = f((histSize - 1) / tileSizeTotal);
  let limit = 0;
  if (clipLimit > 0) limit = Math.max(Math.trunc((clipLimit * tileSizeTotal) / histSize), 1);

  const lut = new Uint8Array(tilesX * tilesY * histSize);
  const hist = new Int32Array(histSize);
  for (let k = 0; k < tilesX * tilesY; k++) {
    const ty = Math.floor(k / tilesX),
      tx = k % tilesX;
    hist.fill(0);
    for (let y = ty * th; y < (ty + 1) * th; y++) {
      const row = y * lutW;
      for (let x = tx * tw; x < (tx + 1) * tw; x++) hist[lutSrc[row + x]]++;
    }
    if (limit > 0) {
      let clipped = 0;
      for (let i = 0; i < histSize; i++)
        if (hist[i] > limit) {
          clipped += hist[i] - limit;
          hist[i] = limit;
        }
      const batch = Math.trunc(clipped / histSize);
      let residual = clipped - batch * histSize;
      for (let i = 0; i < histSize; i++) hist[i] += batch;
      if (residual !== 0) {
        const step = Math.max(Math.trunc(histSize / residual), 1);
        for (let i = 0; i < histSize && residual > 0; i += step, residual--) hist[i]++;
      }
    }
    let sum = 0;
    const base = k * histSize;
    for (let i = 0; i < histSize; i++) {
      sum += hist[i];
      lut[base + i] = Math.min(255, Math.max(0, roundEven(f(sum * lutScale))));
    }
  }

  // bilinear interpolation between the four neighbouring tiles' LUTs, in float32
  const invTw = f(1 / tw),
    invTh = f(1 / th);
  const ind1 = new Int32Array(width),
    ind2 = new Int32Array(width),
    xa = new Float32Array(width),
    xa1 = new Float32Array(width);
  for (let x = 0; x < width; x++) {
    const txf = f(f(x * invTw) - 0.5);
    let tx1 = Math.floor(txf);
    let tx2 = tx1 + 1;
    xa[x] = f(txf - tx1);
    xa1[x] = f(1 - xa[x]);
    tx1 = Math.max(tx1, 0);
    tx2 = Math.min(tx2, tilesX - 1);
    ind1[x] = tx1 * histSize;
    ind2[x] = tx2 * histSize;
  }
  const dst = new Uint8Array(width * height);
  for (let y = 0; y < height; y++) {
    const tyf = f(f(y * invTh) - 0.5);
    let ty1 = Math.floor(tyf);
    let ty2 = ty1 + 1;
    const ya = f(tyf - ty1),
      ya1 = f(1 - ya);
    ty1 = Math.max(ty1, 0);
    ty2 = Math.min(ty2, tilesY - 1);
    const p1 = ty1 * tilesX * histSize,
      p2 = ty2 * tilesX * histSize;
    const row = y * width;
    for (let x = 0; x < width; x++) {
      const v = src[row + x];
      const a = ind1[x] + v,
        b = ind2[x] + v;
      const top = f(f(f(lut[p1 + a] * xa1[x]) + f(lut[p1 + b] * xa[x])) * ya1);
      const bottom = f(f(f(lut[p2 + a] * xa1[x]) + f(lut[p2 + b] * xa[x])) * ya);
      dst[row + x] = Math.min(255, Math.max(0, roundEven(f(top + bottom))));
    }
  }
  return dst;
}
