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

// the finest level whose tiles over a square of half size `half` m about origin are all available
// and at most maxTiles; null if none at minZoom or finer
export function chooseTiles(origin, half, layer, { maxTiles = 16, minZoom = 6 } = {}) {
  const [south, west] = enuToLatLon(origin, -half, -half);
  const [north, east] = enuToLatLon(origin, half, half);
  for (let z = layer.maxzoom; z >= minZoom; z--) {
    const [x0, y0] = tileXY(z, south, west);
    const [x1, y1] = tileXY(z, north, east);
    const tiles = [];
    for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) tiles.push([x, y]);
    if (tiles.length <= maxTiles && tiles.every(([x, y]) => layer.available(z, x, y)))
      return { z, tiles };
  }
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

// terrain around origin, or a rejection with the reason; fetchFn(url) -> Response
export async function loadTerrain(origin, half, { url = TERRAIN_URL, fetchFn = fetch } = {}) {
  const get = async (u) => {
    const r = await fetchFn(u);
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    return r;
  };
  const layer = await cached(new URL('layer.json', url).href, async (u) =>
    parseLayer(await (await get(u)).json()),
  );
  const choice = chooseTiles(origin, half, layer);
  if (!choice) throw new Error('no terrain coverage here');
  const tiles = await Promise.all(
    choice.tiles.map(async ([x, y]) => {
      const path = layer.tiles.replace('{z}', choice.z).replace('{x}', x).replace('{y}', y);
      const data = await cached(new URL(path, url).href, async (u) =>
        decodeQuantizedMesh(await (await get(u)).arrayBuffer()),
      );
      const r = tileRect(choice.z, x, y);
      return {
        x,
        y,
        rect: r,
        data,
        sampler: meshSampler(data),
        radians: {
          west: r.west * DEG,
          south: r.south * DEG,
          width: (r.east - r.west) * DEG,
          height: (r.north - r.south) * DEG,
        },
      };
    }),
  );
  return new Terrain(origin, choice.z, tiles);
}

export class Terrain {
  constructor(origin, level, tiles) {
    this.origin = origin;
    this.level = level;
    this.tiles = tiles;
  }

  // AMSL height at a point, NaN outside the tiles
  amsl(lat, lon) {
    for (const t of this.tiles) {
      const r = t.rect;
      if (lat < r.south || lat > r.north || lon < r.west || lon > r.east) continue;
      const h = t.sampler.sample(t.radians, lon * DEG, lat * DEG);
      if (Number.isFinite(h)) return h;
    }
    return NaN;
  }

  // splat-frame height (ENU z, above the takeoff altitude) at ENU (e, n)
  heightAt(e, n) {
    return this.amsl(...enuToLatLon(this.origin, e, n)) - this.origin.alt_amsl;
  }

  // the tiles' triangles with a vertex within radius of the origin, in ENU:
  // {positions: Float32Array xyz, indices: Uint32Array}
  geometry(radius) {
    const pos = [];
    const idx = [];
    const S = 32767;
    for (const t of this.tiles) {
      const d = t.data;
      const { west, south, east, north } = t.rect;
      const base = pos.length / 3;
      const inside = new Uint8Array(d._uValues.length);
      for (let i = 0; i < d._uValues.length; i++) {
        const lon = west + ((east - west) * d._uValues[i]) / S;
        const lat = south + ((north - south) * d._vValues[i]) / S;
        const [e, n] = latLonToEnu(this.origin, lat, lon);
        const h =
          d._minimumHeight + ((d._maximumHeight - d._minimumHeight) * d._heightValues[i]) / S;
        pos.push(e, n, h - this.origin.alt_amsl);
        inside[i] = Math.hypot(e, n) <= radius ? 1 : 0;
      }
      const ix = d._indices;
      for (let i = 0; i < ix.length; i += 3)
        if (inside[ix[i]] || inside[ix[i + 1]] || inside[ix[i + 2]])
          idx.push(base + ix[i], base + ix[i + 1], base + ix[i + 2]);
    }
    return { positions: Float32Array.from(pos), indices: Uint32Array.from(idx) };
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
