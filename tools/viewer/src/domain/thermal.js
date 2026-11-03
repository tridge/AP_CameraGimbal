import { finite } from './time.js';
export const PALETTES = {
  'White hot': [
    [0, 0, 0],
    [255, 255, 255],
  ],
  'Black hot': [
    [255, 255, 255],
    [0, 0, 0],
  ],
  Inferno: [
    [0, 0, 4],
    [66, 10, 104],
    [147, 38, 103],
    [221, 81, 58],
    [252, 165, 10],
    [252, 255, 164],
  ],
  Iron: [
    [0, 0, 0],
    [45, 0, 70],
    [130, 0, 70],
    [220, 45, 0],
    [255, 160, 20],
    [255, 255, 220],
  ],
  Viridis: [
    [68, 1, 84],
    [59, 82, 139],
    [33, 145, 140],
    [94, 201, 98],
    [253, 231, 37],
  ],
  GloryHot: [
    [20, 0, 200],
    [0, 150, 255],
    [0, 220, 120],
    [255, 235, 0],
    [255, 110, 0],
    [255, 0, 0],
  ],
};
export function calibration(metadata) {
  const scale = metadata?.temperature_scale_k,
    offset = metadata?.temperature_offset_k;
  return finite(scale) && scale > 0 && finite(offset) ? { scale, offset } : null;
}
export function celsius(value, metadata) {
  const c = calibration(metadata);
  return c && finite(value) ? value * c.scale + c.offset - 273.15 : null;
}
export function paletteTable(name) {
  const stops = PALETTES[name];
  if (!stops) throw new Error('Unknown thermal palette.');
  const table = new Uint8Array(256 * 3);
  for (let i = 0; i < 256; i++) {
    const t = (i / 255) * (stops.length - 1),
      j = Math.min(stops.length - 2, Math.floor(t)),
      f = t - j;
    for (let c = 0; c < 3; c++)
      table[i * 3 + c] = Math.round(stops[j][c] * (1 - f) + stops[j + 1][c] * f);
  }
  return table;
}
export function sampleRange(pixels) {
  let low = Infinity,
    high = -Infinity;
  for (const value of pixels) {
    low = Math.min(low, value);
    high = Math.max(high, value);
  }
  if (!finite(low) || !finite(high)) throw new Error('No finite display samples.');
  return [low, Math.max(low + 1, high)];
}
function validateLimits(low, high) {
  if (!finite(low) || !finite(high) || high <= low) throw new Error('High must exceed Low.');
}
function validateGamma(gamma) {
  if (!finite(gamma) || gamma < 0.1 || gamma > 5)
    throw new Error('Gamma must be between 0.1 and 5.');
}
export function stretch(pixels, fixedRange, metadata, { raw = false } = {}) {
  if (fixedRange) {
    const [low, high] = fixedRange;
    validateLimits(low, high);
    if (raw) return [low, high];
    const cal = calibration(metadata);
    if (!cal) throw new Error('Fixed Celsius limits require calibrated raw thermal pixels.');
    const converted = [low, high].map((value) => (value + 273.15 - cal.offset) / cal.scale);
    // Celsius arithmetic can put an exact detector count a few ULPs below
    // its integer value. Restore only those boundaries, keeping genuinely
    // fractional limits and windows narrower than one detector count intact.
    const snapped = converted.map((value, i) => {
      const integer = Math.round(value);
      const tolerance =
        (8 * Number.EPSILON * Math.max(1, Math.abs(fixedRange[i]), 273.15, Math.abs(cal.offset))) /
        cal.scale;
      return Math.abs(value - integer) <= tolerance ? integer : value;
    });
    return snapped[1] > snapped[0] ? snapped : converted;
  }
  const hist = new Uint32Array(65536);
  for (const value of pixels) hist[value]++;
  let low = 0,
    high = 65535,
    total = 0;
  for (let i = 0; i < hist.length; i++) {
    total += hist[i];
    if (total >= pixels.length * 0.01) {
      low = i;
      break;
    }
  }
  total = 0;
  for (let i = 0; i < hist.length; i++) {
    total += hist[i];
    if (total >= pixels.length * 0.99) {
      high = i;
      break;
    }
  }
  return [low, Math.max(low + 1, high)];
}
function mapped(value, low, high, gamma) {
  const t = Math.max(0, Math.min(1, (value - low) / (high - low)));
  return Math.round(255 * (gamma === 1 ? t : Math.pow(t, gamma)));
}
export function colorize(pixels, name, limits, { gamma = 1 } = {}) {
  validateLimits(...limits);
  validateGamma(gamma);
  const colors = paletteTable(name),
    rgba = new Uint8ClampedArray(pixels.length * 4),
    [low, high] = limits;
  const background = name === 'GloryHot' ? sampleRange(pixels)[0] : low;
  for (let i = 0; i < pixels.length; i++) {
    if (name === 'GloryHot' && pixels[i] <= low) {
      // Keep cool scene structure visible. Low is a real threshold in the
      // source units, independent of gamma and of the upper colour limit.
      const gray = Math.round(
        (mapped(pixels[i], background, Math.max(background + 1, low), gamma) * 200) / 255,
      );
      rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = gray;
    } else {
      const p = mapped(pixels[i], low, high, gamma);
      rgba[i * 4] = colors[p * 3];
      rgba[i * 4 + 1] = colors[p * 3 + 1];
      rgba[i * 4 + 2] = colors[p * 3 + 2];
    }
    rgba[i * 4 + 3] = 255;
  }
  return rgba;
}
export function adjustRgba(pixels, limits, { gamma = 1 } = {}) {
  validateLimits(...limits);
  validateGamma(gamma);
  const rgba = new Uint8ClampedArray(pixels.length);
  // Encoded RGB has only 256 possible channel values. Preserve exactly the
  // same curve while avoiding millions of pow/spread calls per video frame.
  const bytes = pixels instanceof Uint8Array || pixels instanceof Uint8ClampedArray;
  const table = new Uint8ClampedArray(256);
  if (bytes) for (let i = 0; i < table.length; i++) table[i] = mapped(i, ...limits, gamma);
  for (let i = 0; i < pixels.length; i += 4) {
    for (let channel = 0; channel < 3; channel++)
      rgba[i + channel] = bytes
        ? table[pixels[i + channel]]
        : mapped(pixels[i + channel], ...limits, gamma);
    rgba[i + 3] = pixels[i + 3];
  }
  return rgba;
}
export function thermalPeaks(pixels, width, height, count = 6) {
  // A local contrast aid. Scores are detector samples, not restored temperatures.
  const candidates = [],
    step = 2;
  for (let y = 7; y < height - 7; y += step)
    for (let x = 7; x < width - 7; x += step) {
      let sum = 0,
        n = 0;
      for (let dy = -6; dy <= 6; dy += 3)
        for (let dx = -6; dx <= 6; dx += 3)
          if (Math.abs(dx) > 2 || Math.abs(dy) > 2) {
            sum += pixels[(y + dy) * width + x + dx];
            n++;
          }
      let best = pixels[y * width + x],
        bx = x,
        by = y;
      for (let dy = -1; dy <= 1; dy++)
        for (let dx = -1; dx <= 1; dx++) {
          const value = pixels[(y + dy) * width + x + dx];
          if (value > best) {
            best = value;
            bx = x + dx;
            by = y + dy;
          }
        }
      const contrast = best - sum / n;
      if (contrast > 0) candidates.push({ x: bx, y: by, value: best, contrast });
    }
  candidates.sort((a, b) => b.contrast - a.contrast);
  const selected = [];
  for (const c of candidates) {
    if (selected.every((p) => Math.hypot(p.x - c.x, p.y - c.y) > 12)) selected.push(c);
    if (selected.length === count) break;
  }
  return selected;
}
export function rawPixel(displayX, displayY, width, height, rotation) {
  const x = Math.floor(displayX),
    y = Math.floor(displayY);
  if (x < 0 || y < 0 || x >= width || y >= height) return null;
  return rotation === 180 ? { x: width - 1 - x, y: height - 1 - y } : { x, y };
}
