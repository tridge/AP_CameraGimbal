// The satellite map's imagery sources: Esri World Imagery, ACT Government aerial
// imagery (Esri outside its coverage), Google satellite through the Map Tiles API (needs a key
// from the runtime config) and a custom XYZ template. Unit tested in test/imagery.test.mjs.
import { enuToLatLon } from './view-math.js';
import { mercatorBounds, planTiles } from './map-tiles.js';

export const ESRI = {
  id: 'esri',
  label: 'Esri World Imagery',
  url: 'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}',
  credit: 'Esri, Maxar, Earthstar Geographics',
  maxZoom: 18,
};

// ACTmapi's current capture in Web Mercator (CC BY 4.0). Tiles outside the ACT are 404, those
// on its border are PNGs transparent outside it.
export const ACT = {
  id: 'act',
  label: 'ACT Government aerial (ACT only)',
  url: 'https://tiles.arcgis.com/tiles/E5n4f1VY84i0xSjy/arcgis/rest/services/ACT_Aerial_Imagery_Current_3857/MapServer/tile/{z}/{y}/{x}',
  credit:
    'ACT Aerial Imagery from ACTmapi © Australian Capital Territory and Aerometrex (CC BY 4.0)',
  minZoom: 8,
  maxZoom: 21,
  bounds: { south: -35.93, west: 148.74, north: -35.115, east: 149.421 },
  fallback: ESRI,
};

const GOOGLE_API = 'https://tile.googleapis.com';
const GOOGLE_MAX_ZOOM = 22;
const GOOGLE = { id: 'google', label: 'Google satellite' };
const CUSTOM = { id: 'custom', label: 'Custom tiles' };
const TILE_TIMEOUT_MS = 20000;

// the Google Map Tiles API key from the runtime config (window.VIEWER_CONFIG), else null
export function googleKey(config) {
  const key = config?.googleMapsApiKey;
  return typeof key === 'string' && key.trim() ? key.trim() : null;
}

// an http(s) XYZ template with {z}, {x} and {y}
export function validTemplate(url) {
  if (typeof url !== 'string' || !['{z}', '{x}', '{y}'].every((k) => url.includes(k))) return false;
  try {
    return ['https:', 'http:'].includes(new URL(url).protocol);
  } catch {
    return false;
  }
}

// the sources offered in the settings: Google only with a key
export function listSources(config) {
  return [ESRI, ...(googleKey(config) ? [GOOGLE] : []), ACT, CUSTOM].map(({ id, label }) => ({
    id,
    label,
  }));
}

const inside = (b, lat, lon) => lat >= b.south && lat <= b.north && lon >= b.west && lon <= b.east;

// the source to draw about origin for the stored settings: Esri unless the chosen one is usable
// here (outside: true when the choice does not cover the origin)
export function chooseSource(settings, config, origin) {
  const id = settings?.imagery;
  if (id === 'act') {
    return inside(ACT.bounds, origin.lat, origin.lon) ? ACT : { ...ESRI, outside: true };
  }
  if (id === 'google' && googleKey(config)) return { ...GOOGLE, key: googleKey(config) };
  if (id === 'custom' && validTemplate(settings.imageryUrl)) {
    return {
      ...CUSTOM,
      url: settings.imageryUrl,
      credit: settings.imageryCredit || 'custom tiles',
      maxZoom: settings.imageryMaxZoom ?? ESRI.maxZoom,
    };
  }
  return ESRI;
}

export const sourceCredit = (source) =>
  [source.credit, source.fallback?.credit].filter(Boolean).join(', ');

export function tileUrl(template, { z, x, y }) {
  const n = 2 ** z;
  return template
    .replace('{z}', z)
    .replace('{y}', y)
    .replace('{x}', ((x % n) + n) % n);
}

// the tile holding `tile` at no finer than maxZoom, and tile's square in it (fractions, y down)
export function ancestorTile(tile, maxZoom) {
  const d = Math.max(0, tile.z - maxZoom);
  const s = 2 ** d;
  const n = 2 ** tile.z;
  const x = ((tile.x % n) + n) % n;
  return {
    z: tile.z - d,
    x: Math.floor(x / s),
    y: Math.floor(tile.y / s),
    crop: d ? { x: (x % s) / s, y: (tile.y % s) / s, size: 1 / s } : null,
  };
}

function covers(source, tile) {
  if (tile.z < (source.minZoom ?? 0)) return false;
  if (!source.bounds) return true;
  const b = mercatorBounds(tile.z, tile.x, tile.y);
  const s = source.bounds;
  return b.north > s.south && b.south < s.north && b.east > s.west && b.west < s.east;
}

async function fetchLayer(source, tile, fetch) {
  const t = ancestorTile(tile, source.maxZoom);
  const url = typeof source.url === 'function' ? await source.url(t) : tileUrl(source.url, t);
  const r = await fetch(url, { signal: AbortSignal.timeout(TILE_TIMEOUT_MS) });
  if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
  return { blob: await r.blob(), crop: t.crop };
}

async function isPng(blob) {
  const b = new Uint8Array(await blob.slice(0, 4).arrayBuffer());
  return b[0] === 0x89 && b[1] === 0x50 && b[2] === 0x4e && b[3] === 0x47;
}

// a tile's images, bottom first: [{blob, crop}], crop the part of the image to draw (null: all).
// A source with a fallback draws that where it has no tile, and under its partly transparent
// (PNG) tiles.
export async function loadTileLayers(source, tile, fetch = globalThis.fetch) {
  const under = source.fallback;
  if (!under) return [await fetchLayer(source, tile, fetch)];
  if (covers(source, tile)) {
    const top = await fetchLayer(source, tile, fetch).catch(() => null);
    if (top && !(await isPng(top.blob))) return [top];
    if (top) {
      const below = await fetchLayer(under, tile, fetch).catch(() => null);
      return below ? [below, top] : [top];
    }
  }
  return [await fetchLayer(under, tile, fetch)];
}

function boxAround(origin, radius) {
  const [south, west] = enuToLatLon(origin, -radius, -radius);
  const [north, east] = enuToLatLon(origin, radius, radius);
  return { south, west, north, east };
}

// Google satellite 2D tiles through the Map Tiles API: one session, renewed before it expires
export function googleImagery(key, { fetch = globalThis.fetch, now = Date.now } = {}) {
  let session = null; // {token, expiry (ms)}
  let pending = null;
  const getJson = async (url, init) => {
    const r = await fetch(url, init);
    if (!r.ok) throw new Error(`Google Map Tiles API: HTTP ${r.status}`);
    return r.json();
  };
  async function token() {
    if (session && now() < session.expiry - 600e3) return session.token;
    pending ??= getJson(`${GOOGLE_API}/v1/createSession?key=${encodeURIComponent(key)}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ mapType: 'satellite', language: 'en-US', region: 'US' }),
    })
      .then((j) => {
        if (!j.session) throw new Error('Google Map Tiles API: no session');
        const expiry = Number(j.expiry) * 1000;
        session = { token: j.session, expiry: Number.isFinite(expiry) ? expiry : now() + 3600e3 };
        return session.token;
      })
      .finally(() => {
        pending = null;
      });
    return pending;
  }
  const query = async () =>
    `session=${encodeURIComponent(await token())}&key=${encodeURIComponent(key)}`;
  // the attribution and the zoom limits over a box
  async function viewport(box, zoom) {
    const q = ['north', 'south', 'east', 'west'].map((k) => `${k}=${box[k].toFixed(6)}`).join('&');
    return getJson(`${GOOGLE_API}/tile/v1/viewport?${await query()}&zoom=${zoom}&${q}`);
  }
  // the tile plan about origin at the zooms Google has there, its source and its attribution
  async function open(origin, { nearRadius, farRadius }) {
    const near = boxAround(origin, nearRadius);
    const limits = await viewport(near, GOOGLE_MAX_ZOOM);
    const here = (limits.maxZoomRects ?? [])
      .filter((r) => inside(r, origin.lat, origin.lon) && Number.isInteger(r.maxZoom))
      .map((r) => r.maxZoom);
    const maxZoom = Math.min(GOOGLE_MAX_ZOOM, here.length ? Math.max(...here) : ESRI.maxZoom);
    const plan = planTiles(origin, { nearRadius, farRadius, maxZoom });
    const views = await Promise.all([
      viewport(near, plan.zNear),
      viewport(boxAround(origin, farRadius), plan.zRoot),
    ]);
    const copyright = [...new Set(views.map((v) => v.copyright).filter(Boolean))];
    if (!copyright.length) throw new Error('Google Map Tiles API: no attribution');
    return {
      plan,
      source: {
        ...GOOGLE,
        maxZoom,
        credit: `Google Maps, ${copyright.join(', ')}`,
        url: async ({ z, x, y }) => `${GOOGLE_API}/v1/2dtiles/${z}/${x}/${y}?${await query()}`,
      },
    };
  }
  return { token, viewport, open };
}

// window.VIEWER_CONFIG from an optional viewer-config.js at url (the video viewer's runtime
// config); null when there is none
export function loadRuntimeConfig(url, win = globalThis.window) {
  if (win.VIEWER_CONFIG) return Promise.resolve(win.VIEWER_CONFIG);
  return new Promise((resolve) => {
    const s = win.document.createElement('script');
    s.src = url;
    s.onload = () => resolve(win.VIEWER_CONFIG ?? null);
    s.onerror = () => resolve(null);
    win.document.head.append(s);
  });
}
