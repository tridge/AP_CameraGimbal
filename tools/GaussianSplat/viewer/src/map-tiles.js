// The satellite map's tiles (no DOM): a Web Mercator quadtree about the splat's origin, fine over
// the splat and one zoom coarser per doubling of distance out to the horizon, and each tile's
// mesh (a grid in ENU metres, terrain heights, earth curvature, skirts against cracks).
import { EARTH_RADIUS, enuToLatLon } from './view-math.js';
import { latLonToEnu } from './terrain.js';

const DEG = Math.PI / 180;

// a tile's corners in degrees
export function mercatorBounds(z, x, y) {
  const n = 2 ** z;
  const lat = (t) => Math.atan(Math.sinh(Math.PI * (1 - (2 * t) / n))) / DEG;
  return {
    west: (x / n) * 360 - 180,
    east: ((x + 1) / n) * 360 - 180,
    north: lat(y),
    south: lat(y + 1),
  };
}

// metres across a zoom z tile at latitude lat
export const tileSize = (z, lat) => (2 * Math.PI * EARTH_RADIUS * Math.cos(lat * DEG)) / 2 ** z;

// horizontal distance (m) from the origin to the nearest point of a tile
export function tileDistance(origin, z, x, y) {
  const b = mercatorBounds(z, x, y);
  const [w, s] = latLonToEnu(origin, b.south, b.west);
  const [e, n] = latLonToEnu(origin, b.north, b.east);
  const dx = Math.max(w, 0, -e);
  const dy = Math.max(s, 0, -n);
  return Math.hypot(dx, dy);
}

function tilesWithin(origin, z, radius) {
  const n = 2 ** z;
  const [s, w] = enuToLatLon(origin, -radius, -radius);
  const [nn, e] = enuToLatLon(origin, radius, radius);
  const tx = (lon) => Math.floor(((lon + 180) / 360) * n);
  const ty = (lat) =>
    Math.floor(((1 - Math.log(Math.tan(lat * DEG) + 1 / Math.cos(lat * DEG)) / Math.PI) / 2) * n);
  const out = [];
  for (let y = Math.max(0, ty(nn)); y <= Math.min(n - 1, ty(s)); y++)
    for (let x = tx(w); x <= tx(e); x++) {
      const d = tileDistance(origin, z, x, y);
      if (d <= radius) out.push({ z, x, y, dist: d }); // x unwrapped: wrap it for URLs
    }
  return out;
}

// quadtree leaves: tiles meeting the near circle at zNear (the finest zoom up to maxZoom with at
// most maxNear of them), others split while nearer than ring x their size, out to farRadius.
// Sorted nearest first.
export function planTiles(
  origin,
  { nearRadius, farRadius = 25000, maxNear = 40, maxZoom = 18, ring = 0.75 } = {},
) {
  let zNear = maxZoom;
  while (zNear > 2 && tilesWithin(origin, zNear, nearRadius).length > maxNear) zNear--;
  const zRoot = Math.max(
    1,
    Math.min(zNear, Math.floor(Math.log2(tileSize(0, origin.lat) / farRadius))),
  );
  const leaves = [];
  const visit = (t) => {
    const split = t.z < zNear && (t.dist < nearRadius || t.dist < ring * tileSize(t.z, origin.lat));
    if (!split) {
      leaves.push(t);
      return;
    }
    for (const [dx, dy] of [
      [0, 0],
      [1, 0],
      [0, 1],
      [1, 1],
    ]) {
      const c = { z: t.z + 1, x: 2 * t.x + dx, y: 2 * t.y + dy };
      c.dist = tileDistance(origin, c.z, c.x, c.y);
      if (c.dist <= farRadius) visit(c);
    }
  };
  for (const t of tilesWithin(origin, zRoot, farRadius)) visit(t);
  leaves.sort((a, b) => a.dist - b.dist);
  return { zNear, zRoot, leaves };
}

// drop of the earth's surface below the local horizontal plane at horizontal distance d
export const curvatureDrop = (d) => (d * d) / (2 * EARTH_RADIUS);

// a tile's mesh: (n + 1)^2 grid vertices in ENU with z = height(e, n) - curvature, uv over the
// tile image (v up), and a skirt hanging `skirt` m below each edge
export function tileMesh(origin, tile, height, { n = 16, skirt = 10 } = {}) {
  const b = mercatorBounds(tile.z, tile.x, tile.y);
  const yTop = Math.log(Math.tan(Math.PI / 4 + (b.north * DEG) / 2));
  const yBottom = Math.log(Math.tan(Math.PI / 4 + (b.south * DEG) / 2));
  const m = n + 1;
  const count = m * m + 4 * m;
  const positions = new Float32Array(3 * count);
  const uvs = new Float32Array(2 * count);
  const vertex = (k, i, j, drop) => {
    const lon = b.west + ((b.east - b.west) * i) / n;
    const lat = (2 * Math.atan(Math.exp(yBottom + ((yTop - yBottom) * j) / n)) - Math.PI / 2) / DEG;
    const [e, nn] = latLonToEnu(origin, lat, lon);
    positions.set([e, nn, height(e, nn) - curvatureDrop(Math.hypot(e, nn)) - drop], 3 * k);
    uvs.set([i / n, j / n], 2 * k);
  };
  for (let j = 0; j < m; j++) for (let i = 0; i < m; i++) vertex(j * m + i, i, j, 0);
  const indices = [];
  for (let j = 0; j < n; j++)
    for (let i = 0; i < n; i++) {
      const a = j * m + i;
      indices.push(a, a + 1, a + m + 1, a, a + m + 1, a + m);
    }
  // skirts: the edge rings, south, east, north, west
  const edges = [(t) => [t, 0], (t) => [n, t], (t) => [n - t, n], (t) => [0, n - t]];
  let k = m * m;
  for (const edge of edges) {
    const start = k;
    for (let t = 0; t < m; t++, k++) vertex(k, ...edge(t), skirt);
    for (let t = 0; t < n; t++) {
      const [i0, j0] = edge(t);
      const [i1, j1] = edge(t + 1);
      const top0 = j0 * m + i0;
      const top1 = j1 * m + i1;
      indices.push(top0, start + t, top1, top1, start + t, start + t + 1);
    }
  }
  return { positions, uvs, indices: Uint32Array.from(indices) };
}
