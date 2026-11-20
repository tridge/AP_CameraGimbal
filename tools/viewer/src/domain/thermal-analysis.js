// Immutable single-frame analysis. All distances are in original detector pixels.
export const ANALYSIS_VERSION = 'apcg.thermal-analysis.v1';
export const METHODS = {
  raw: 'Original / histogram only',
  background: 'Gaussian background subtraction',
  dog: 'Difference of Gaussians',
  tophat: 'White top-hat',
  cfar: 'Guarded local contrast',
  matched: 'Gaussian matched filter',
  multiscale: 'Multi-scale LoG',
  clahe: 'CLAHE local contrast',
  rl: 'Noise-damped Richardson–Lucy',
  wiener: 'Regularized Wiener',
};
export const DEFAULT_ANALYSIS = Object.freeze({
  method: 'matched',
  sigma: 1,
  background: 6,
  iterations: 8,
  damping: 1,
  regularization: 1,
  clip: 2,
  destripe: false,
});
const MAX_PIXELS = 2097152;
function bound(value, fallback, low, high, integer = false) {
  const v = typeof value === 'number' && Number.isFinite(value) ? value : fallback;
  const n = Math.max(low, Math.min(high, v));
  return integer ? Math.round(n) : n;
}
export function analysisSettings(value = {}) {
  return {
    method: Object.hasOwn(METHODS, value?.method) ? value.method : DEFAULT_ANALYSIS.method,
    sigma: bound(value?.sigma, 1, 0.5, 4),
    background: bound(value?.background, 6, 2, 16),
    iterations: bound(value?.iterations, 8, 1, 200, true),
    damping: bound(value?.damping, 1, 0, 10),
    regularization: bound(value?.regularization, 1, 0.1, 10),
    clip: bound(value?.clip, 2, 1, 8),
    destripe: value?.destripe === true,
  };
}
export function regionOf(value, width, height) {
  const x = bound(value?.x, 0, 0, width - 1, true),
    y = bound(value?.y, 0, 0, height - 1, true);
  return {
    x,
    y,
    width: bound(value?.width, width, 1, width - x, true),
    height: bound(value?.height, height, 1, height - y, true),
  };
}
export function reflect(i, n) {
  const t = ((i % (2 * n)) + 2 * n) % (2 * n);
  return t < n ? t : 2 * n - 1 - t;
}
export function gaussianKernel(sigma) {
  const r = Math.ceil(4 * sigma),
    k = new Float64Array(2 * r + 1);
  let sum = 0;
  for (let i = -r; i <= r; i++) sum += k[i + r] = Math.exp((-i * i) / (2 * sigma * sigma));
  for (let i = 0; i < k.length; i++) k[i] /= sum;
  return k;
}
export function gaussian(data, width, height, sigma, out = new Float32Array(data.length)) {
  const k = gaussianKernel(sigma),
    r = (k.length - 1) / 2,
    temp = new Float32Array(data.length),
    row = new Float32Array(width + 2 * r);
  // Horizontal pass over a reflected, padded copy of each row.
  for (let y = 0, base = 0; y < height; y++, base += width) {
    for (let i = -r; i < width + r; i++) row[i + r] = data[base + reflect(i, width)];
    for (let x = 0; x < width; x++) {
      let sum = 0;
      for (let j = 0; j <= 2 * r; j++) sum += k[j] * row[x + j];
      temp[base + x] = sum;
    }
  }
  // Vertical pass accumulated row by row (double precision) for contiguous access.
  const acc = new Float64Array(width);
  for (let y = 0, base = 0; y < height; y++, base += width) {
    acc.fill(0);
    for (let j = -r; j <= r; j++) {
      const w = k[j + r],
        src = reflect(y + j, height) * width;
      for (let x = 0; x < width; x++) acc[x] += w * temp[src + x];
    }
    out.set(acc, base);
  }
  return out;
}
const subtract = (a, b) => Float32Array.from(a, (v, i) => v - b[i]);
function median(values) {
  values.sort((a, b) => a - b);
  const n = values.length;
  return n % 2 ? values[n >> 1] : (values[n / 2 - 1] + values[n / 2]) / 2;
}
export function madSigma(data, roi, width) {
  const values = [],
    step = Math.max(1, Math.ceil((roi.width * roi.height) / 65536));
  let i = 0;
  for (let y = roi.y; y < roi.y + roi.height; y++)
    for (let x = roi.x; x < roi.x + roi.width; x++) {
      const v = data[y * width + x];
      if ((i++ + y) % step === 0 && Number.isFinite(v)) values.push(v);
    }
  if (!values.length) return 0;
  const m = median(values);
  return 1.4826 * median(values.map((v) => Math.abs(v - m)));
}
export function histogram(data, width, height, region, raw = false) {
  const roi = regionOf(region, width, height);
  let min = Infinity,
    max = -Infinity,
    mean = 0,
    m2 = 0,
    count = 0,
    invalid = 0,
    endpoints = 0;
  const exact = raw ? new Uint32Array(65536) : null,
    values = raw ? null : new Float32Array(roi.width * roi.height);
  for (let y = roi.y; y < roi.y + roi.height; y++)
    for (let x = roi.x; x < roi.x + roi.width; x++) {
      const v = data[y * width + x];
      if (!Number.isFinite(v)) {
        invalid++;
        continue;
      }
      min = Math.min(min, v);
      max = Math.max(max, v);
      count++;
      const delta = v - mean;
      mean += delta / count;
      m2 += delta * (v - mean);
      if (raw) {
        exact[v]++;
        if (v === 0 || v === 65535) endpoints++;
      } else values[count - 1] = v;
    }
  if (!count) throw new Error('No finite samples in the analysis region.');
  const percentile = (fraction) => {
    const rank = Math.max(1, Math.ceil(fraction * count));
    if (exact) {
      let n = 0;
      for (let i = 0; i < exact.length; i++) if ((n += exact[i]) >= rank) return i;
    }
    return values[rank - 1];
  };
  if (values) values.subarray(0, count).sort();
  const bins = new Uint32Array(256),
    high = max === min ? min + 1 : max;
  const place = (v) => Math.min(255, Math.max(0, Math.floor(((v - min) / (high - min)) * 256)));
  if (exact) {
    for (let i = min; i <= max; i++) bins[place(i)] += exact[i];
  } else for (let i = 0; i < count; i++) bins[place(values[i])]++;
  return {
    min,
    max,
    mean,
    std: Math.sqrt(m2 / count),
    p01: percentile(0.01),
    median: percentile(0.5),
    p99: percentile(0.99),
    count,
    invalid,
    endpoints,
    low: min,
    high,
    bins,
    roi,
  };
}
function destripe(data, width, height) {
  const residual = subtract(data, gaussian(data, width, height, 3)),
    out = Float32Array.from(data);
  for (let y = 0; y < height; y++) {
    const m = median(Array.from(residual.subarray(y * width, (y + 1) * width)));
    for (let x = 0; x < width; x++) out[y * width + x] -= m;
  }
  for (let x = 0; x < width; x++) {
    const m = median(
      Array.from(
        { length: height },
        (_, y) => out[y * width + x] - data[y * width + x] + residual[y * width + x],
      ),
    );
    for (let y = 0; y < height; y++) out[y * width + x] -= m;
  }
  return out;
}
function extremum(data, width, height, radius, maximum) {
  const temp = new Float32Array(data.length),
    out = new Float32Array(data.length);
  for (let axis = 0; axis < 2; axis++) {
    const source = axis ? temp : data,
      dest = axis ? out : temp;
    for (let y = 0; y < height; y++)
      for (let x = 0; x < width; x++) {
        let best = maximum ? -Infinity : Infinity;
        for (let j = -radius; j <= radius; j++) {
          const v = axis
            ? source[reflect(y + j, height) * width + x]
            : source[y * width + reflect(x + j, width)];
          best = maximum ? Math.max(best, v) : Math.min(best, v);
        }
        dest[y * width + x] = best;
      }
  }
  return out;
}
function integral(data, width, height, squared) {
  const out = new Float64Array((width + 1) * (height + 1));
  for (let y = 0; y < height; y++) {
    let sum = 0;
    for (let x = 0; x < width; x++) {
      const v = data[y * width + x];
      sum += squared ? v * v : v;
      out[(y + 1) * (width + 1) + x + 1] = out[y * (width + 1) + x + 1] + sum;
    }
  }
  return out;
}
function localContrast(data, width, height, guard, radius, noise) {
  const sum = integral(data, width, height, false),
    square = integral(data, width, height, true);
  const out = new Float32Array(data.length);
  const area = (a, x, y, r) => {
    const x0 = Math.max(0, x - r),
      x1 = Math.min(width, x + r + 1),
      y0 = Math.max(0, y - r),
      y1 = Math.min(height, y + r + 1),
      stride = width + 1;
    return [
      a[y1 * stride + x1] - a[y0 * stride + x1] - a[y1 * stride + x0] + a[y0 * stride + x0],
      (x1 - x0) * (y1 - y0),
    ];
  };
  for (let y = 0; y < height; y++)
    for (let x = 0; x < width; x++) {
      const outer = area(sum, x, y, radius),
        inner = area(sum, x, y, guard),
        n = outer[1] - inner[1];
      if (!n) continue;
      const mean = (outer[0] - inner[0]) / n;
      const variance =
        (area(square, x, y, radius)[0] - area(square, x, y, guard)[0]) / n - mean * mean;
      out[y * width + x] =
        (data[y * width + x] - mean) / Math.sqrt(Math.max(noise * noise, variance));
    }
  return out;
}
function clahe(data, width, height, clip) {
  const summary = histogram(data, width, height),
    low = summary.min,
    range = Math.max(1, summary.max - low);
  const tilesX = Math.ceil(width / 32),
    tilesY = Math.ceil(height / 32),
    bins = 1024,
    maps = [];
  const bin = (v) => Math.max(0, Math.min(bins - 1, Math.floor(((v - low) / range) * (bins - 1))));
  for (let ty = 0; ty < tilesY; ty++)
    for (let tx = 0; tx < tilesX; tx++) {
      const hist = new Float64Array(bins);
      let count = 0;
      for (let y = ty * 32; y < Math.min(height, (ty + 1) * 32); y++)
        for (let x = tx * 32; x < Math.min(width, (tx + 1) * 32); x++) {
          hist[bin(data[y * width + x])]++;
          count++;
        }
      const cap = Math.max(1, (clip * count) / bins);
      let excess = 0;
      for (let i = 0; i < bins; i++) {
        excess += Math.max(0, hist[i] - cap);
        hist[i] = Math.min(hist[i], cap);
      }
      const cdf = new Float32Array(bins);
      let total = 0;
      for (let i = 0; i < bins; i++) cdf[i] = (total += hist[i] + excess / bins) / count;
      maps.push(cdf);
    }
  const out = new Float32Array(data.length);
  for (let y = 0; y < height; y++)
    for (let x = 0; x < width; x++) {
      const fx = Math.max(0, Math.min(tilesX - 1, (x + 0.5) / 32 - 0.5)),
        fy = Math.max(0, Math.min(tilesY - 1, (y + 0.5) / 32 - 0.5));
      const ix = Math.floor(fx),
        iy = Math.floor(fy),
        dx = fx - ix,
        dy = fy - iy,
        b = bin(data[y * width + x]);
      const at = (xx, yy) => maps[Math.min(tilesY - 1, yy) * tilesX + Math.min(tilesX - 1, xx)][b];
      out[y * width + x] =
        (1 - dy) * ((1 - dx) * at(ix, iy) + dx * at(ix + 1, iy)) +
        dy * ((1 - dx) * at(ix, iy + 1) + dx * at(ix + 1, iy + 1));
    }
  return out;
}
export function richardsonLucy(data, width, height, sigma, iterations, damping) {
  const floor = histogram(data, width, height).p01,
    n = data.length;
  const observed = Float32Array.from(data, (v) => Math.max(0, v - floor) + 0.001);
  const estimate = Float32Array.from(observed),
    blur = new Float32Array(n),
    ratio = new Float32Array(n),
    back = new Float32Array(n);
  for (let iteration = 0; iteration < iterations; iteration++) {
    gaussian(estimate, width, height, sigma, blur);
    for (let i = 0; i < n; i++) {
      const v = blur[i],
        residual = observed[i] - v;
      ratio[i] =
        Math.max(0.000001, v + Math.sign(residual) * Math.max(0, Math.abs(residual) - damping)) /
        Math.max(0.000001, v);
    }
    gaussian(ratio, width, height, sigma, back);
    for (let i = 0; i < n; i++) estimate[i] *= back[i];
  }
  return Float32Array.from(estimate, (v) => v + floor - 0.001);
}
function fft(real, imag, inverse) {
  const n = real.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      [real[i], real[j]] = [real[j], real[i]];
      [imag[i], imag[j]] = [imag[j], imag[i]];
    }
  }
  for (let len = 2; len <= n; len *= 2) {
    const angle = ((inverse ? 2 : -2) * Math.PI) / len,
      wr = Math.cos(angle),
      wi = Math.sin(angle);
    for (let start = 0; start < n; start += len) {
      let ar = 1,
        ai = 0;
      for (let j = 0; j < len / 2; j++) {
        const a = start + j,
          b = a + len / 2,
          br = ar * real[b] - ai * imag[b],
          bi = ar * imag[b] + ai * real[b];
        real[b] = real[a] - br;
        imag[b] = imag[a] - bi;
        real[a] += br;
        imag[a] += bi;
        const next = ar * wr - ai * wi;
        ai = ar * wi + ai * wr;
        ar = next;
      }
    }
  }
  if (inverse)
    for (let i = 0; i < n; i++) {
      real[i] /= n;
      imag[i] /= n;
    }
}
function fft2(real, imag, width, height, inverse) {
  for (let y = 0; y < height; y++)
    fft(
      real.subarray(y * width, (y + 1) * width),
      imag.subarray(y * width, (y + 1) * width),
      inverse,
    );
  const r = new Float64Array(height),
    im = new Float64Array(height);
  for (let x = 0; x < width; x++) {
    for (let y = 0; y < height; y++) {
      r[y] = real[y * width + x];
      im[y] = imag[y * width + x];
    }
    fft(r, im, inverse);
    for (let y = 0; y < height; y++) {
      real[y * width + x] = r[y];
      imag[y * width + x] = im[y];
    }
  }
}
export function wiener(data, width, height, sigma, k) {
  const padding = Math.ceil(4 * sigma),
    power = (n) => 2 ** Math.ceil(Math.log2(n));
  const w = power(width + 2 * padding),
    h = power(height + 2 * padding);
  if (w * h > 1048576)
    throw new Error('Wiener FFT exceeds the 1,048,576-cell memory budget. Choose another method.');
  let mean = 0;
  for (const v of data) mean += v / data.length;
  const real = new Float64Array(w * h),
    imag = new Float64Array(w * h);
  const taper = (i, start, length, total) =>
    i < start
      ? Math.sin(((Math.PI / 2) * i) / start) ** 2
      : i >= start + length
        ? Math.sin(((Math.PI / 2) * (total - 1 - i)) / (total - 1 - start - length)) ** 2
        : 1;
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++)
      real[y * w + x] =
        (data[reflect(y - padding, height) * width + reflect(x - padding, width)] - mean) *
        taper(x, padding, width, w) *
        taper(y, padding, height, h);
  fft2(real, imag, w, h, false);
  const kernel = gaussianKernel(sigma),
    r = (kernel.length - 1) / 2;
  const transfer = (n) =>
    Float64Array.from({ length: n }, (_, f) =>
      kernel.reduce((sum, v, i) => sum + v * Math.cos((2 * Math.PI * f * (i - r)) / n), 0),
    );
  const xh = transfer(w),
    yh = transfer(h);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      const t = xh[x] * yh[y],
        factor = Math.max(-8, Math.min(8, t / (t * t + k))),
        i = y * w + x;
      real[i] *= factor;
      imag[i] *= factor;
    }
  fft2(real, imag, w, h, true);
  return Float32Array.from(
    { length: data.length },
    (_, i) => real[(Math.floor(i / width) + padding) * w + (i % width) + padding] + mean,
  );
}
function candidates(score, raw, width, height, roi, border, sigma, scales, noise) {
  const found = [],
    separation = Math.max(2, Math.ceil(3 * sigma));
  for (let y = Math.max(border, roi.y); y < Math.min(height - border, roi.y + roi.height); y++)
    for (let x = Math.max(border, roi.x); x < Math.min(width - border, roi.x + roi.width); x++) {
      const i = y * width + x,
        v = score[i];
      if (!(v >= 3)) continue;
      let peak = true;
      for (let dy = -1; dy <= 1 && peak; dy++)
        for (let dx = -1; dx <= 1; dx++) {
          const j = (y + dy) * width + x + dx;
          if (score[j] > v || (j < i && score[j] === v)) {
            peak = false;
            break;
          }
        }
      if (peak) {
        found.push({
          x,
          y,
          value: raw[i],
          score: v,
          sigma: scales?.[i] ?? sigma,
          singlePixel:
            raw[i] > Math.max(raw[i - 1], raw[i + 1], raw[i - width], raw[i + width]) + 3 * noise,
        });
        // Bound the proposal list even for pathological high-contrast texture.
        if (found.length >= 512) {
          found.sort((a, b) => b.score - a.score || a.y - b.y || a.x - b.x);
          found.length = 256;
        }
      }
    }
  found.sort((a, b) => b.score - a.score || a.y - b.y || a.x - b.x);
  const selected = [];
  for (const p of found)
    if (selected.every((q) => Math.hypot(q.x - p.x, q.y - p.y) >= separation)) {
      selected.push(p);
      if (selected.length === 10) break;
    }
  return selected;
}
export function analyzeThermal(raw, width, height, options = {}, region = null) {
  if (
    !(raw instanceof Uint16Array) ||
    !Number.isInteger(width) ||
    !Number.isInteger(height) ||
    width < 4 ||
    height < 4 ||
    width * height !== raw.length ||
    raw.length > MAX_PIXELS
  )
    throw new Error('Analysis requires raw gray16 pixels, at least 4×4, at most 2,097,152 pixels.');
  const settings = analysisSettings(options),
    roi = regionOf(region, width, height),
    rawStats = histogram(raw, width, height, roi, true);
  if (settings.method === 'raw')
    return {
      settings,
      roi,
      rawStats,
      plane: null,
      unit: 'raw samples',
      candidates: [],
      version: ANALYSIS_VERSION,
    };
  const input = settings.destripe ? destripe(raw, width, height) : Float32Array.from(raw);
  const noiseResidual = subtract(input, gaussian(input, width, height, 1)),
    g = gaussianKernel(1);
  const norm = Math.sqrt(
    1 + g.reduce((s, v) => s + v * v, 0) ** 2 - 2 * g[(g.length - 1) / 2] ** 2,
  );
  const noiseRegion = regionOf(null, width, height),
    measuredNoise = madSigma(noiseResidual, noiseRegion, width) / norm;
  const noise = Math.max(1, measuredNoise);
  const backgroundScale = Math.max(settings.background, 2 * settings.sigma),
    background = gaussian(input, width, height, backgroundScale);
  const residual = subtract(input, background),
    matched = gaussian(residual, width, height, settings.sigma);
  const mk = gaussianKernel(settings.sigma),
    whiteFloor = noise * mk.reduce((s, v) => s + v * v, 0);
  const scoreNoise = Math.max(whiteFloor, madSigma(matched, noiseRegion, width), 0.000001);
  let score = Float32Array.from(matched, (v) => v / scoreNoise),
    plane,
    unit = 'sample residual',
    scales = null,
    k = null;
  switch (settings.method) {
    case 'background':
      plane = residual;
      break;
    case 'dog':
      plane = subtract(
        gaussian(input, width, height, settings.sigma),
        gaussian(input, width, height, 1.6 * settings.sigma),
      );
      break;
    case 'tophat': {
      const r = Math.ceil(backgroundScale);
      plane = subtract(
        input,
        extremum(extremum(input, width, height, r, false), width, height, r, true),
      );
      break;
    }
    case 'matched':
      plane = score;
      unit = 'empirical contrast σ';
      break;
    case 'cfar': {
      const guard = Math.ceil(3 * settings.sigma),
        outer = Math.max(guard + 2, Math.ceil(2 * backgroundScale));
      plane = score = localContrast(input, width, height, guard, outer, noise);
      unit = 'local contrast σ';
      break;
    }
    case 'multiscale': {
      plane = new Float32Array(raw.length);
      scales = new Float32Array(raw.length);
      plane.fill(-Infinity);
      for (const s of [0.7, 1, 1.4].map((v) => v * settings.sigma)) {
        const blur = gaussian(residual, width, height, s),
          response = new Float32Array(raw.length);
        for (let y = 0; y < height; y++)
          for (let x = 0; x < width; x++) {
            const i = y * width + x;
            response[i] =
              s *
              s *
              (4 * blur[i] -
                blur[y * width + reflect(x - 1, width)] -
                blur[y * width + reflect(x + 1, width)] -
                blur[reflect(y - 1, height) * width + x] -
                blur[reflect(y + 1, height) * width + x]);
          }
        const scaleNoise = Math.max(noise * 0.05, madSigma(response, noiseRegion, width));
        for (let i = 0; i < plane.length; i++)
          if (response[i] / scaleNoise > plane[i]) {
            plane[i] = response[i] / scaleNoise;
            scales[i] = s;
          }
      }
      score = plane;
      unit = 'multi-scale contrast σ';
      break;
    }
    case 'clahe':
      plane = clahe(input, width, height, settings.clip);
      unit = 'display intensity';
      break;
    case 'rl':
      plane = richardsonLucy(
        input,
        width,
        height,
        settings.sigma,
        settings.iterations,
        settings.damping * noise,
      );
      unit = 'restored samples (not temperature)';
      break;
    case 'wiener': {
      let mean = 0,
        m2 = 0,
        n = 0;
      for (const v of input) {
        const delta = v - mean;
        mean += delta / ++n;
        m2 += delta * (v - mean);
      }
      k = Math.max(
        0.001,
        Math.min(
          10,
          (settings.regularization * noise * noise) /
            Math.max(noise * noise, m2 / n - noise * noise),
        ),
      );
      plane = wiener(input, width, height, settings.sigma, k);
      unit = 'restored samples (not temperature)';
      break;
    }
  }
  for (const v of plane)
    if (!Number.isFinite(v))
      throw new Error('Non-finite analysis output; original frame retained.');
  const processedStats = histogram(plane, width, height, roi),
    displayStats =
      roi.x === 0 && roi.y === 0 && roi.width === width && roi.height === height
        ? processedStats
        : histogram(plane, width, height),
    border = Math.max(1, Math.ceil(4 * backgroundScale));
  return {
    settings,
    roi,
    rawStats,
    processedStats,
    displayStats,
    plane,
    score,
    noise,
    noiseFloorApplied: measuredNoise < 1,
    noiseScope: 'full frame',
    scoreNoise,
    k,
    wienerGainLimit: 8,
    unit,
    border,
    candidates: candidates(score, raw, width, height, roi, border, settings.sigma, scales, noise),
    candidateMethod:
      settings.method === 'cfar' || settings.method === 'multiscale' ? settings.method : 'matched',
    version: ANALYSIS_VERSION,
  };
}
