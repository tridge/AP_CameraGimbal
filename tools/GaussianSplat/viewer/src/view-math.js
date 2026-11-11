// Pure helpers for the viewer (no three.js or DOM): trim circle and ground estimates, camera
// limits, ENU to latitude and longitude and the stored settings. Unit tested in test/view-math.test.mjs.

export const EARTH_RADIUS = 6378137; // splat_common.LocalFrame
export const MIN_PITCH_DEG = 5; // horizon-level orbit: the camera stays this far above the target's horizon
export const SETTINGS_KEY = 'apcg-splat-viewer';
export const DEFAULT_SETTINGS = {
  level: true,
  trim: true,
  satellite: true,
  alignTerrain: true,
  panel: null,
};

const finite = (x) => typeof x === 'number' && Number.isFinite(x);

export function loadSettings(storage) {
  let s = {};
  try {
    s = JSON.parse(storage?.getItem(SETTINGS_KEY) || '{}') || {};
  } catch {
    s = {};
  }
  const out = { ...DEFAULT_SETTINGS };
  for (const k of ['level', 'trim', 'satellite', 'alignTerrain'])
    if (typeof s[k] === 'boolean') out[k] = s[k];
  if (s.panel === 'open' || s.panel === 'collapsed') out.panel = s.panel;
  return out;
}

export function saveSettings(storage, settings) {
  try {
    storage?.setItem(SETTINGS_KEY, JSON.stringify(settings));
  } catch {
    // private mode or storage disabled: settings last for this page only
  }
}

// the file's trim circle: trim_radius (m) about trim_centre [x, y], else about the target
export function trimFromMeta(meta) {
  const r = meta?.trim_radius;
  if (!finite(r) || r <= 0) return null;
  const c = meta.trim_centre ?? meta.target;
  const centre = Array.isArray(c) && finite(c[0]) && finite(c[1]) ? [c[0], c[1]] : null;
  return centre ? { centre, radius: r } : null;
}

function quantile(sorted, f) {
  return sorted[Math.min(sorted.length - 1, Math.max(0, Math.floor(f * (sorted.length - 1))))];
}

export function median(values) {
  const a = Float64Array.from(values).sort();
  return a.length ? quantile(a, 0.5) : NaN;
}

// trim radius for a splat without one: where the areal density of Gaussians (by horizontal
// distance from centre) falls below a fraction of its mid-radius level, or the 99th percentile
// distance; the trim is a little inside that edge, where the chaotic fringe starts
export const TRIM_FRACTION = 0.85;
export function estimateTrim(xs, ys, centre, { bins = 60, drop = 0.15 } = {}) {
  const n = xs.length;
  if (!n) return null;
  const r = new Float64Array(n);
  for (let i = 0; i < n; i++) r[i] = Math.hypot(xs[i] - centre[0], ys[i] - centre[1]);
  r.sort();
  const rMax = quantile(r, 0.99);
  if (!(rMax > 0)) return null;
  const counts = new Float64Array(bins);
  for (let i = 0; i < n && r[i] < rMax; i++) counts[Math.floor((r[i] / rMax) * bins)]++;
  // density per unit annulus area (common factors dropped)
  const dens = Array.from(counts, (c, i) => c / (2 * i + 1));
  const bin = (x) => Math.min(bins - 1, Math.floor((x / rMax) * bins));
  const [b25, b50, b75] = [0.25, 0.5, 0.75].map((f) => bin(quantile(r, f)));
  const ref = median(dens.slice(b25, b75 + 1));
  let edge = rMax;
  for (let i = b50 + 1; i < bins; i++) {
    if (dens[i] < drop * ref && (i + 1 >= bins || dens[i + 1] < drop * ref)) {
      edge = (i / bins) * rMax;
      break;
    }
  }
  return { edge, radius: TRIM_FRACTION * edge };
}

// coarse ground heights: per cell (radius / 8) the 10th percentile height of its Gaussians;
// ground = the median cell within half the radius (all cells if none)
export function groundGrid(xs, ys, zs, centre, radius, { cells = 16, minCount = 8 } = {}) {
  const cell = (2 * radius) / cells;
  const x0 = centre[0] - radius;
  const y0 = centre[1] - radius;
  const lists = Array.from({ length: cells * cells }, () => []);
  for (let i = 0; i < xs.length; i++) {
    const cx = Math.floor((xs[i] - x0) / cell);
    const cy = Math.floor((ys[i] - y0) / cell);
    if (cx >= 0 && cy >= 0 && cx < cells && cy < cells) lists[cy * cells + cx].push(zs[i]);
  }
  const h = new Float64Array(cells * cells).fill(NaN);
  const inner = [];
  const all = [];
  lists.forEach((l, k) => {
    if (l.length < minCount) return;
    l.sort((a, b) => a - b);
    h[k] = quantile(l, 0.1);
    all.push(h[k]);
    const dx = x0 + ((k % cells) + 0.5) * cell - centre[0];
    const dy = y0 + (Math.floor(k / cells) + 0.5) * cell - centre[1];
    if (Math.hypot(dx, dy) <= 0.5 * radius) inner.push(h[k]);
  });
  if (!all.length) return null;
  const ground = median(inner.length ? inner : all);
  return { x0, y0, cell, cells, h, ground };
}

// ground height under (x, y): its cell, else the overall ground (NaN without fallback)
export function groundAt(grid, x, y, fallback = true) {
  if (!grid) return -Infinity;
  const cx = Math.floor((x - grid.x0) / grid.cell);
  const cy = Math.floor((y - grid.y0) / grid.cell);
  const v =
    cx >= 0 && cy >= 0 && cx < grid.cells && cy < grid.cells ? grid.h[cy * grid.cells + cx] : NaN;
  return Number.isFinite(v) ? v : fallback ? grid.ground : NaN;
}

// horizon-level limits for a camera at pos looking at target ([x, y, z], z up): at least
// minPitchDeg above the target's horizon (the same distance from it), then at least clearance
// above ground(x, y). Returns a new position.
export function clampCamera(
  pos,
  target,
  { minPitchDeg = MIN_PITCH_DEG, ground = null, clearance = 1 } = {},
) {
  const d = [pos[0] - target[0], pos[1] - target[1], pos[2] - target[2]];
  const dist = Math.hypot(...d);
  let out = [...pos];
  const minPitch = (minPitchDeg * Math.PI) / 180;
  if (dist > 0 && d[2] < dist * Math.sin(minPitch)) {
    const h = Math.hypot(d[0], d[1]);
    const hNew = dist * Math.cos(minPitch);
    const [ux, uy] = h > 1e-9 ? [d[0] / h, d[1] / h] : [0, -1];
    out = [target[0] + ux * hNew, target[1] + uy * hNew, target[2] + dist * Math.sin(minPitch)];
  }
  if (ground) {
    const g = ground(out[0], out[1]) + clearance;
    if (out[2] < g) out[2] = g;
  }
  return out;
}

// the +/- buttons' dolly: 0.75 per step as the video viewer's map, within [min, max]
export function dollyDistance(dist, inward, min = 0, max = Infinity) {
  return Math.min(max, Math.max(min, dist * (inward ? 0.75 : 1 / 0.75)));
}

// ENU metres about origin {lat, lon} (flat earth, splat_common.LocalFrame) to degrees
export function enuToLatLon(origin, e, n) {
  const lat = origin.lat + ((n / EARTH_RADIUS) * 180) / Math.PI;
  const lon =
    origin.lon + ((e / (EARTH_RADIUS * Math.cos((origin.lat * Math.PI) / 180))) * 180) / Math.PI;
  return [lat, lon];
}
