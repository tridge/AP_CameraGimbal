import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  ACT,
  ESRI,
  ancestorTile,
  chooseSource,
  failureReason,
  googleImagery,
  googleKey,
  listSources,
  loadRuntimeConfig,
  loadTileLayers,
  sourceCredit,
  tileUrl,
  validTemplate,
} from '../src/imagery.js';

const CANBERRA = { lat: -35.2789, lon: 149.0055 };
const SYDNEY = { lat: -33.86, lon: 151.21 };
const KEY = 'fake-key';
const CONFIG = { googleMapsApiKey: KEY };

const JPEG = new Blob([Uint8Array.of(0xff, 0xd8, 0xff, 0xe0, 1)]);
const PNG = new Blob([Uint8Array.of(0x89, 0x50, 0x4e, 0x47, 2)]);
// the Web Mercator tile at a zoom
function tileAt({ lat, lon }, z) {
  const n = 2 ** z;
  const r = (lat * Math.PI) / 180;
  return {
    z,
    x: Math.floor(((lon + 180) / 360) * n),
    y: Math.floor(((1 - Math.asinh(Math.tan(r)) / Math.PI) / 2) * n),
  };
}
// fetch answering each URL from reply(url): a Blob, an object (JSON) or an HTTP status
function mockFetch(reply) {
  const calls = [];
  const fetch = async (url, init) => {
    calls.push({ url: String(url), init });
    const body = await reply(String(url), init);
    if (typeof body === 'number') return { ok: false, status: body };
    return { ok: true, status: 200, blob: async () => body, json: async () => body };
  };
  return { fetch, calls };
}

test('listSources and googleKey: Google only with a key', () => {
  const ids = (c) => listSources(c).map((s) => s.id);
  assert.deepEqual(ids(null), ['esri', 'act', 'custom']);
  assert.deepEqual(ids({}), ['esri', 'act', 'custom']);
  assert.deepEqual(ids({ googleMapsApiKey: '  ' }), ['esri', 'act', 'custom']);
  assert.deepEqual(ids({ googleMapsApiKey: 7 }), ['esri', 'act', 'custom']);
  assert.deepEqual(ids(CONFIG), ['esri', 'google', 'act', 'custom']);
  assert.equal(googleKey({ googleMapsApiKey: ` ${KEY} ` }), KEY);
  assert.equal(googleKey(undefined), null);
});

test('chooseSource: Esri unless the choice is usable here', () => {
  assert.equal(chooseSource({}, null, SYDNEY), ESRI);
  assert.equal(chooseSource({ imagery: 'esri' }, CONFIG, SYDNEY), ESRI);
  assert.equal(chooseSource({ imagery: 'nonsense' }, CONFIG, SYDNEY), ESRI);
  // ACT only inside its coverage
  assert.equal(chooseSource({ imagery: 'act' }, null, CANBERRA), ACT);
  const out = chooseSource({ imagery: 'act' }, null, SYDNEY);
  assert.equal(out.id, 'esri');
  assert.equal(out.outside, true);
  assert.equal(out.maxZoom, ESRI.maxZoom);
  // Google needs the key
  assert.equal(chooseSource({ imagery: 'google' }, null, SYDNEY), ESRI);
  assert.equal(chooseSource({ imagery: 'google' }, { googleMapsApiKey: '' }, SYDNEY), ESRI);
  assert.deepEqual(chooseSource({ imagery: 'google' }, CONFIG, SYDNEY), {
    id: 'google',
    label: 'Google satellite',
    key: KEY,
  });
  // custom needs a template
  assert.equal(chooseSource({ imagery: 'custom' }, null, SYDNEY), ESRI);
  assert.equal(
    chooseSource({ imagery: 'custom', imageryUrl: 'https://x/{z}' }, null, SYDNEY),
    ESRI,
  );
  const url = 'https://tiles.example/{z}/{x}/{y}.jpg';
  const c = chooseSource({ imagery: 'custom', imageryUrl: url }, null, SYDNEY);
  assert.deepEqual([c.id, c.url, c.maxZoom, c.credit], ['custom', url, 18, 'custom tiles']);
  const d = chooseSource(
    { imagery: 'custom', imageryUrl: url, imageryCredit: 'Me', imageryMaxZoom: 20 },
    null,
    SYDNEY,
  );
  assert.deepEqual([d.maxZoom, d.credit], [20, 'Me']);
});

test('validTemplate: http(s) with {z}, {x} and {y}', () => {
  assert.ok(validTemplate('https://tiles.example/{z}/{x}/{y}.png'));
  assert.ok(validTemplate('http://localhost:8080/t?x={x}&y={y}&z={z}'));
  assert.ok(!validTemplate('https://tiles.example/{z}/{x}.png'));
  assert.ok(!validTemplate('javascript:{z}{x}{y}'));
  assert.ok(!validTemplate('tiles/{z}/{x}/{y}.png'));
  assert.ok(!validTemplate(undefined));
});

test('credits and zoom limits per source', () => {
  assert.equal(sourceCredit(ESRI), 'Esri, Maxar, Earthstar Geographics');
  assert.match(sourceCredit(ACT), /ACTmapi © Australian Capital Territory.*CC BY 4\.0.*Esri/);
  assert.equal(ESRI.maxZoom, 18);
  assert.equal(ACT.maxZoom, 21);
  assert.equal(ACT.minZoom, 8);
});

test('tileUrl and ancestorTile', () => {
  assert.equal(
    tileUrl(ESRI.url, { z: 3, x: 9, y: 2 }), // x wraps
    'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/3/2/1',
  );
  assert.equal(tileUrl('https://t/{z}/{x}/{y}.png', { z: 2, x: -1, y: 1 }), 'https://t/2/3/1.png');
  assert.match(tileUrl(ACT.url, { z: 21, x: 1916595, y: 1268461 }), /tile\/21\/1268461\/1916595$/);
  assert.deepEqual(ancestorTile({ z: 18, x: 5, y: 6 }, 18), { z: 18, x: 5, y: 6, crop: null });
  assert.deepEqual(ancestorTile({ z: 20, x: 4 * 5 + 3, y: 4 * 6 + 1 }, 18), {
    z: 18,
    x: 5,
    y: 6,
    crop: { x: 0.75, y: 0.25, size: 0.25 },
  });
});

test('loadTileLayers: a plain source is one request, a failure throws', async () => {
  const tile = tileAt(SYDNEY, 18);
  const ok = mockFetch(() => JPEG);
  assert.deepEqual(await loadTileLayers(ESRI, tile, ok.fetch), [{ blob: JPEG, crop: null }]);
  assert.deepEqual(
    ok.calls.map((c) => c.url),
    [tileUrl(ESRI.url, tile)],
  );
  assert.ok(ok.calls[0].init.signal instanceof AbortSignal); // the timeout
  await assert.rejects(loadTileLayers(ESRI, tile, mockFetch(() => 404).fetch), /HTTP 404/);
});

test('loadTileLayers: ACT tiles, with Esri outside coverage and under border tiles', async () => {
  const isAct = (url) => url.startsWith(ACT.url.split('{')[0]);
  // inside: the ACT tile alone
  const tile = tileAt(CANBERRA, 21);
  let m = mockFetch((url) => (isAct(url) ? JPEG : 500));
  assert.deepEqual(await loadTileLayers(ACT, tile, m.fetch), [{ blob: JPEG, crop: null }]);
  assert.deepEqual(
    m.calls.map((c) => c.url),
    [tileUrl(ACT.url, tile)],
  );
  // no ACT tile (404): the Esri tile above it, at Esri's finest zoom, cropped to this tile
  m = mockFetch((url) => (isAct(url) ? 404 : JPEG));
  const up = ancestorTile(tile, ESRI.maxZoom);
  assert.equal(up.z, 18);
  assert.equal(up.crop.size, 1 / 8);
  assert.deepEqual(await loadTileLayers(ACT, tile, m.fetch), [{ blob: JPEG, crop: up.crop }]);
  assert.deepEqual(
    m.calls.map((c) => c.url),
    [tileUrl(ACT.url, tile), tileUrl(ESRI.url, up)],
  );
  // a border tile (PNG, transparent outside the ACT): Esri below it
  m = mockFetch((url) => (isAct(url) ? PNG : JPEG));
  const low = tileAt(CANBERRA, 12);
  assert.deepEqual(await loadTileLayers(ACT, low, m.fetch), [
    { blob: JPEG, crop: null },
    { blob: PNG, crop: null },
  ]);
  // ... and alone when Esri fails
  m = mockFetch((url) => (isAct(url) ? PNG : 503));
  assert.deepEqual(await loadTileLayers(ACT, low, m.fetch), [{ blob: PNG, crop: null }]);
  // outside the coverage box, or coarser than the service: Esri only, ACT never asked
  for (const t of [tileAt(SYDNEY, 14), tileAt(CANBERRA, 7)]) {
    m = mockFetch((url) => (isAct(url) ? 500 : JPEG));
    assert.deepEqual(await loadTileLayers(ACT, t, m.fetch), [{ blob: JPEG, crop: null }]);
    assert.deepEqual(
      m.calls.map((c) => c.url),
      [tileUrl(ESRI.url, t)],
    );
  }
  // neither: the tile fails
  await assert.rejects(loadTileLayers(ACT, tile, mockFetch(() => 404).fetch), /HTTP 404/);
});

// the Map Tiles API: sessions numbered from 1, each lasting `life` seconds
function mockGoogle({ life = 14 * 86400, rects, copyright = 'Imagery ©2026 Example' } = {}) {
  const state = { now: 1.8e12, sessions: 0 };
  const m = mockFetch((url, init) => {
    const u = new URL(url);
    if (u.pathname === '/v1/createSession') {
      assert.equal(init.method, 'POST');
      assert.deepEqual(JSON.parse(init.body), {
        mapType: 'satellite',
        language: 'en-US',
        region: 'US',
      });
      state.sessions++;
      return { session: `s${state.sessions}`, expiry: String(state.now / 1000 + life) };
    }
    if (u.pathname === '/tile/v1/viewport') {
      const zoom = Number(u.searchParams.get('zoom'));
      return {
        copyright: typeof copyright === 'function' ? copyright(zoom) : copyright,
        maxZoomRects: rects ?? [{ maxZoom: 20, north: 90, south: -90, east: 180, west: -180 }],
      };
    }
    return 404;
  });
  return { ...m, state, now: () => state.now };
}

test('google: one session, reused until it nears expiry, then renewed', async () => {
  const m = mockGoogle({ life: 3600 });
  const g = googleImagery(KEY, m);
  assert.deepEqual(await Promise.all([g.token(), g.token()]), ['s1', 's1']); // one request
  assert.equal(await g.token(), 's1');
  assert.equal(m.calls.length, 1);
  assert.equal(m.calls[0].url, `https://tile.googleapis.com/v1/createSession?key=${KEY}`);
  m.state.now += 2999e3; // over 10 minutes left
  assert.equal(await g.token(), 's1');
  m.state.now += 1e3;
  assert.equal(await g.token(), 's2');
  assert.equal(m.calls.length, 2);
});

test('google: a refused key or a bad reply is an error, and is retried', async () => {
  let status = 403;
  const m = mockFetch(() => status);
  const g = googleImagery(KEY, { fetch: m.fetch });
  await assert.rejects(g.token(), /HTTP 403/);
  await assert.rejects(g.open(SYDNEY, { nearRadius: 150, farRadius: 25000 }), /HTTP 403/);
  status = 200;
  const bad = googleImagery(KEY, { fetch: mockFetch(() => ({})).fetch });
  await assert.rejects(bad.token(), /no session/);
});

test('google: a request with no reply times out, and the next one starts again', async () => {
  let calls = 0;
  let hang = true;
  const fetch = (url, init) => {
    calls++;
    if (!hang) return Promise.resolve({ ok: true, json: async () => ({ session: 's1' }) });
    return new Promise((resolve, reject) => {
      // the signal's own timer does not keep the process alive
      const waiting = setTimeout(() => reject(new Error('not aborted')), 5000);
      init.signal.addEventListener('abort', () => {
        clearTimeout(waiting);
        reject(init.signal.reason);
      });
    });
  };
  const g = googleImagery(KEY, { fetch, timeout: 20 });
  await assert.rejects(g.token(), /Google Map Tiles API: no reply/);
  hang = false;
  assert.equal(await g.token(), 's1');
  assert.equal(calls, 2);
});

test('google: open plans to the zoom limit here and collects the attribution', async () => {
  const m = mockGoogle({
    copyright: (zoom) => (zoom >= 15 ? 'Imagery ©2026 Near' : 'Imagery ©2026 Far'),
    rects: [
      { maxZoom: 22, north: 10, south: 0, east: 10, west: 0 }, // elsewhere
      { maxZoom: 19, north: -30, south: -40, east: 155, west: 145 },
      { maxZoom: 17, north: -30, south: -40, east: 155, west: 145 },
    ],
  });
  const g = googleImagery(KEY, m);
  const { plan, source } = await g.open(SYDNEY, { nearRadius: 150, farRadius: 25000 });
  assert.equal(source.maxZoom, 19);
  assert.equal(plan.zNear, 19);
  assert.equal(source.id, 'google');
  assert.equal(sourceCredit(source), 'Google Maps, Imagery ©2026 Near, Imagery ©2026 Far');
  const views = m.calls
    .filter((c) => c.url.includes('/tile/v1/viewport'))
    .map((c) => new URL(c.url));
  assert.deepEqual(
    views.map((u) => Number(u.searchParams.get('zoom'))),
    [22, plan.zNear, plan.zRoot],
  );
  for (const u of views) {
    assert.equal(u.searchParams.get('session'), 's1');
    assert.equal(u.searchParams.get('key'), KEY);
    const [n, s, e, w] = ['north', 'south', 'east', 'west'].map((k) =>
      Number(u.searchParams.get(k)),
    );
    assert.ok(n > SYDNEY.lat && s < SYDNEY.lat && e > SYDNEY.lon && w < SYDNEY.lon);
  }
  // tiles: the official 2D tiles endpoint with the session and key
  const tile = plan.leaves[0];
  assert.equal(
    await source.url(tile),
    `https://tile.googleapis.com/v1/2dtiles/${tile.z}/${tile.x}/${tile.y}?session=s1&key=${KEY}`,
  );
  const before = m.calls.length;
  const t = mockFetch(() => JPEG);
  assert.deepEqual(await loadTileLayers(source, tile, t.fetch), [{ blob: JPEG, crop: null }]);
  assert.equal(t.calls[0].url, await source.url(tile));
  // a tile fetched after the session has expired uses a new one
  m.state.now += 15 * 86400e3;
  assert.match(await source.url(tile), /\?session=s2&key=fake-key$/);
  assert.equal(m.calls.length, before + 1);
});

test('google: no zoom limit here plans as Esri, no attribution is an error', async () => {
  const g = googleImagery(KEY, mockGoogle({ rects: [] }));
  const { source } = await g.open(SYDNEY, { nearRadius: 150, farRadius: 25000 });
  assert.equal(source.maxZoom, 18);
  const none = googleImagery(KEY, mockGoogle({ copyright: '' }));
  await assert.rejects(none.open(SYDNEY, { nearRadius: 150, farRadius: 25000 }), /no attribution/);
});

// a window whose appended script loads (running `run`) or fails
function fakeWindow(run) {
  const win = {
    document: {
      head: { append: (s) => queueMicrotask(() => (run ? (run(win), s.onload()) : s.onerror())) },
    },
  };
  win.document.createElement = () => ({});
  return win;
}

test('loadRuntimeConfig: the key from viewer-config.js, null without the file', async () => {
  const set = (win) => (win.VIEWER_CONFIG = { imagery: 'satellite', googleMapsApiKey: KEY });
  const config = await loadRuntimeConfig('viewer-config.js', fakeWindow(set));
  assert.equal(googleKey(config), KEY);
  // missing file: no config, no Google
  const missing = await loadRuntimeConfig('viewer-config.js', fakeWindow(null));
  assert.equal(missing, null);
  assert.deepEqual(
    listSources(missing).map((s) => s.id),
    ['esri', 'act', 'custom'],
  );
  // a file that sets nothing
  assert.equal(
    await loadRuntimeConfig(
      'viewer-config.js',
      fakeWindow(() => {}),
    ),
    null,
  );
  // already set by the page
  const win = fakeWindow(null);
  win.VIEWER_CONFIG = { googleMapsApiKey: '' };
  assert.equal(googleKey(await loadRuntimeConfig('viewer-config.js', win)), null);
});

test('failureReason: says why without the tile address', async () => {
  const blob = new Blob([Uint8Array.of(0xff, 0xd8, 0xff, 0xe0)]);
  const source = { url: () => `https://tile.example/1/2/3?key=${KEY}`, maxZoom: 22 };
  const tile = { z: 3, x: 1, y: 2 };
  const reason = (fetch) => loadTileLayers(source, tile, fetch).then(() => null, failureReason);
  assert.equal(await reason(async () => ({ ok: true, blob: async () => blob })), null);
  assert.equal(await reason(async () => ({ ok: false, status: 403 })), 'HTTP 403');
  assert.equal(
    await reason(async () => {
      throw new TypeError('Failed to fetch');
    }),
    'offline, or blocked by the network',
  );
  assert.equal(
    await reason(async () => {
      throw new DOMException('timed out', 'TimeoutError');
    }),
    'no reply',
  );
  assert.equal(failureReason(new Error('bad image')), 'the tiles could not be read');
  assert.equal(failureReason(new SyntaxError('bad JSON'), 'invalid reply'), 'invalid reply');
  // from the session, renewed for a tile
  assert.equal(failureReason(new Error('Google Map Tiles API: no reply')), 'no reply');
  assert.equal(failureReason(new Error('Google Map Tiles API: HTTP 429')), 'HTTP 429');
});
