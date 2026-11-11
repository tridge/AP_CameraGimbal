// Quantized-mesh terrain (plot.ardupilot.org/quantized/, the video viewer's terrain: SRTM heights,
// AMSL) for the satellite map: tile choice from layer.json availability, decoding, and heights and
// geometry in the splat's ENU frame (z = AMSL - origin.alt_amsl, the takeoff altitude). No DOM.
// Heights are sampled with the video viewer's mesh sampler, so both read a tile the same way.
import { meshSampler } from '../../../viewer/src/map/mesh-sampler.js';
import { EARTH_RADIUS, enuToLatLon, median } from './view-math.js';

export const TERRAIN_URL = 'https://plot.ardupilot.org/quantized/';
export const TERRAIN_CREDIT = 'SRTM';
const DEG = Math.PI / 180;

export function latLonToEnu(origin, lat, lon) {
  return [
    (lon - origin.lon) * DEG * EARTH_RADIUS * Math.cos(origin.lat * DEG),
    (lat - origin.lat) * DEG * EARTH_RADIUS,
  ];
}

// layer.json (TMS, EPSG:4326): {maxzoom, available(z, x, y)}
export function parseLayer(layer) {
  if (layer?.format !== 'quantized-mesh-1.0' || layer.projection !== 'EPSG:4326')
    throw new Error('not a geographic quantized-mesh layer');
  const ranges = Array.isArray(layer.available) ? layer.available : null;
  if (!ranges) throw new Error('the terrain has no tile availability');
  const maxzoom = Math.min(layer.maxzoom ?? ranges.length - 1, ranges.length - 1);
  return {
    maxzoom,
    tiles: layer.tiles?.[0] ?? '{z}/{x}/{y}.terrain',
    available: (z, x, y) =>
      !!ranges[z]?.some((r) => x >= r.startX && x <= r.endX && y >= r.startY && y <= r.endY),
  };
}

// geographic tiling: 2^(z+1) x 2^z tiles, y from the south
export function tileRect(z, x, y) {
  const d = 180 / 2 ** z;
  return {
    west: -180 + x * d,
    south: -90 + y * d,
    east: -180 + (x + 1) * d,
    north: -90 + (y + 1) * d,
  };
}
export function tileXY(z, lat, lon) {
  const d = 180 / 2 ** z;
  const n = 2 ** z;
  return [
    Math.min(2 * n - 1, Math.max(0, Math.floor((lon + 180) / d))),
    Math.min(n - 1, Math.max(0, Math.floor((lat + 90) / d))),
  ];
}

// the level for a ground resolution: a terrain tile (65 x 65 vertices on the plot terrain) about
// `span` metres across, coarser where layer.json has no tile there; null if none
export function terrainLevel(layer, lat, lon, span) {
  let z = Math.round(Math.log2((180 * DEG * EARTH_RADIUS) / Math.max(span, 1)));
  z = Math.max(0, Math.min(layer.maxzoom, z));
  for (; z >= 0; z--) if (layer.available(z, ...tileXY(z, lat, lon))) return z;
  return null;
}

// quantized-mesh-1.0 (header, zigzag-delta u/v/height, high-water-mark indices; edges and
// extensions ignored) in Cesium's QuantizedMeshTerrainData field layout, which meshSampler reads
export function decodeQuantizedMesh(buffer) {
  const dv = new DataView(buffer);
  let at = 88; // centre, min/max height, bounding sphere, horizon occlusion point
  const minimumHeight = dv.getFloat32(24, true);
  const maximumHeight = dv.getFloat32(28, true);
  const n = dv.getUint32(at, true);
  at += 4;
  if (!(n > 0) || at + 6 * n > buffer.byteLength) throw new Error('bad terrain tile');
  const arrays = [0, 1, 2].map(() => new Uint16Array(n));
  for (const a of arrays) {
    let value = 0;
    for (let i = 0; i < n; i++, at += 2) {
      const z = dv.getUint16(at, true);
      value += (z >> 1) ^ -(z & 1);
      a[i] = value;
    }
  }
  const wide = n > 65536;
  if (wide && at % 4) at += 4 - (at % 4);
  const triangles = dv.getUint32(at, true);
  at += 4;
  const size = wide ? 4 : 2;
  if (at + 3 * triangles * size > buffer.byteLength) throw new Error('bad terrain tile');
  const indices = wide ? new Uint32Array(3 * triangles) : new Uint16Array(3 * triangles);
  let highest = 0;
  for (let i = 0; i < indices.length; i++, at += size) {
    const code = wide ? dv.getUint32(at, true) : dv.getUint16(at, true);
    indices[i] = highest - code;
    if (code === 0) highest++;
  }
  if (highest > n) throw new Error('bad terrain tile');
  return {
    _uValues: arrays[0],
    _vValues: arrays[1],
    _heightValues: arrays[2],
    _indices: indices,
    _minimumHeight: minimumHeight,
    _maximumHeight: maximumHeight,
  };
}

const memo = new Map(); // url -> Promise, for the page's lifetime

function cached(url, load) {
  if (!memo.has(url)) {
    const p = load(url);
    memo.set(url, p);
    p.catch(() => memo.delete(url)); // retried next time
  }
  return memo.get(url);
}

async function get(fetchFn, u) {
  const r = await fetchFn(u);
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return r;
}

// layer.json, parsed; fetchFn(url) -> Response
export function openLayer(url = TERRAIN_URL, fetchFn = fetch) {
  return cached(new URL('layer.json', url).href, async (u) =>
    parseLayer(await (await get(fetchFn, u)).json()),
  );
}

// one tile: {key, level, x, y, rect (degrees), data, sampler}
export function loadTile(layer, z, x, y, url = TERRAIN_URL, fetchFn = fetch) {
  const path = layer.tiles.replace('{z}', z).replace('{x}', x).replace('{y}', y);
  return cached(new URL(path, url).href, async (u) => {
    const data = decodeQuantizedMesh(await (await get(fetchFn, u)).arrayBuffer());
    const rect = tileRect(z, x, y);
    return {
      key: `${z}/${x}/${y}`,
      level: z,
      x,
      y,
      rect,
      data,
      sampler: meshSampler(data),
      radians: {
        west: rect.west * DEG,
        south: rect.south * DEG,
        width: (rect.east - rect.west) * DEG,
        height: (rect.north - rect.south) * DEG,
      },
    };
  });
}

// the loaded tiles about origin, of mixed levels: heights from the finest tile holding a point
export class Terrain {
  constructor(origin, tiles = []) {
    this.origin = origin;
    this.tiles = [];
    for (const t of tiles) this.add(t);
  }

  add(tile) {
    if (this.tiles.some((t) => t.key === tile.key)) return;
    this.tiles.push(tile);
    this.tiles.sort((a, b) => b.level - a.level);
  }

  // AMSL height at a point, NaN outside the tiles; minLevel: only tiles at least this fine
  amsl(lat, lon, minLevel = 0) {
    for (const t of this.tiles) {
      if (t.level < minLevel) break;
      const r = t.rect;
      if (lat < r.south || lat > r.north || lon < r.west || lon > r.east) continue;
      const h = t.sampler.sample(t.radians, lon * DEG, lat * DEG);
      if (Number.isFinite(h)) return h;
    }
    return NaN;
  }

  // splat-frame height (ENU z, above the takeoff altitude) at ENU (e, n), no curvature
  heightAt(e, n, minLevel = 0) {
    return this.amsl(...enuToLatLon(this.origin, e, n), minLevel) - this.origin.alt_amsl;
  }
}

// splat ground minus terrain over the ground grid's cells within radius of centre: {median, count}
// (positive: the splat's ground is above the terrain)
export function terrainOffset(grid, heightAt, centre, radius) {
  const d = [];
  for (let k = 0; k < grid.h.length; k++) {
    if (!Number.isFinite(grid.h[k])) continue;
    const x = grid.x0 + ((k % grid.cells) + 0.5) * grid.cell;
    const y = grid.y0 + (Math.floor(k / grid.cells) + 0.5) * grid.cell;
    if (Math.hypot(x - centre[0], y - centre[1]) > radius) continue;
    const t = heightAt(x, y);
    if (Number.isFinite(t)) d.push(grid.h[k] - t);
  }
  return d.length ? { median: median(d), count: d.length } : null;
}
