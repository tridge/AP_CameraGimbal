import fs from 'node:fs';
import { test, expect, fixture, start, openFile, waitIndexed, seekNumber } from './helpers.js';

// Stands in for WebSplat's side of the handover; the test drives it through window.stub.
const STUB = `<!doctype html><meta charset="utf-8"><title>WebSplat stub</title><script>
const nonce = new URLSearchParams(location.search).get('request');
window.stub = { nonce, received: [], file: null,
  ready: (n = nonce) => opener.postMessage({ type: 'websplat.ready', nonce: n }, '*'),
  accept: () => opener.postMessage({ type: 'websplat.accepted', nonce, job: 'job-1' }, '*'),
  reject: (reason) => opener.postMessage({ type: 'websplat.rejected', nonce, reason }, '*') };
addEventListener('message', (e) => {
  const d = e.data || {};
  if (d.file) window.stub.file = d.file;
  window.stub.received.push({ type: d.type, nonce: d.nonce, origin: e.origin, request: d.request,
    file: d.file ? { name: d.file.name, size: d.file.size, isFile: d.file instanceof File } : null });
});
</script>`;
const LOCK = 'apcg-url-download:';

async function config(context, extra = {}) {
  await context.route('**/viewer-config.js', (route) =>
    route.fulfill({
      contentType: 'text/javascript',
      body: `window.VIEWER_CONFIG=${JSON.stringify({ imagery: 'none', terrainUrl: '', amslToEllipsoidMetres: 0, ...extra })};`,
    }),
  );
  await context.route('**/websplat/**', (route) =>
    route.fulfill({ contentType: 'text/html', body: STUB }),
  );
}

async function openThermal(page, name = 'thermal.mkv') {
  await openFile(page, fixture(name), { kind: 'gray16' });
  await waitIndexed(page);
  // flat test terrain at 1360 m AMSL
  await page.evaluate(() => {
    window.fireViewer.elevation.terrain = { at: () => 1360, description: 'Test terrain' };
  });
}

async function openPanel(page) {
  await expect(page.locator('#splat-create')).toBeEnabled();
  await page.locator('#splat-create').click();
  await expect(page.locator('#splat-panel')).toBeVisible();
  await expect(page.locator('#splat-alt')).toHaveValue('1360.0');
}

async function go(page) {
  const popup = page.waitForEvent('popup');
  await page.locator('#splat-go').click();
  const tab = await popup;
  await tab.waitForFunction(() => window.stub);
  return tab;
}

const held = (page) =>
  page.evaluate(async () =>
    (await navigator.locks.query()).held
      .map((l) => l.name)
      .filter((n) => n.startsWith('apcg-url-download:')),
  );

test('Create splat is only enabled for an indexed thermal MKV or RGB TS/MP4 with pose', async ({
  page,
  context,
}) => {
  await config(context);
  await start(page);
  await expect(page.locator('#splat-create')).toBeDisabled();
  await openFile(page, fixture('rgb.mkv'));
  await waitIndexed(page);
  await expect(page.locator('#splat-create')).toBeDisabled();
  await expect(page.locator('#splat-create')).toHaveAttribute('title', /FFV1 gray16/);
  await openThermal(page);
  await expect(page.locator('#splat-create')).toBeEnabled();
});

test('range, target, view and Go hand the request and file to WebSplat', async ({
  page,
  context,
}) => {
  await config(context);
  await start(page);
  await openThermal(page);
  await openPanel(page);
  await page.waitForFunction(() => window.fireViewer.map.viewer.scene.mode === 3);
  await expect(page.locator('#splat-range-summary')).toContainText('12 frames');
  await page.waitForFunction(() => window.fireViewer.map.lookPoints?.length === 12);

  await seekNumber(page, 3);
  await page.locator('#splat-set-start').click();
  await seekNumber(page, 8);
  await page.locator('#splat-set-end').click();
  await expect(page.locator('#splat-range-summary')).toContainText('6 frames');
  await expect(page.locator('#splat-band')).toBeVisible();
  await page.waitForFunction(() => {
    const { map, state } = window.fireViewer;
    const pts = map.videoTrack.map((p) => p.pts);
    return (
      pts.length &&
      Math.min(...pts) === state.frames[3].pts &&
      Math.max(...pts) === state.frames[8].pts
    );
  });
  await page.waitForFunction(() => window.fireViewer.map.lookPoints?.length === 6);

  // the viewer keeps working: stepping frames does not leave splat mode
  await page.locator('#next').click();
  await page.waitForFunction(() => window.fireViewer.state.number === 9);
  await expect(page.locator('#splat-panel')).toBeVisible();

  // pick on the map: the click sets the target and does not seek
  await page.locator('#map-follow').uncheck();
  await page.locator('#splat-pick').click();
  await expect(page.locator('#splat-pick')).toHaveAttribute('aria-pressed', 'true');
  const box = await page.locator('#map canvas').boundingBox();
  await page.mouse.click(box.x + box.width / 2 + 40, box.y + box.height / 2 + 30);
  await expect(page.locator('#splat-pick')).toHaveAttribute('aria-pressed', 'false');
  const picked = await page.evaluate(() => window.fireViewer.splatPanel.target);
  expect(picked.alt_amsl).toBe(1360);
  expect(Math.abs(picked.lat - -35.3219444)).toBeLessThan(0.05);
  expect(await page.evaluate(() => window.fireViewer.state.number)).toBe(9);

  // nudge the height: 1 m, shift 10 m
  await page.locator('#splat-alt-up').click();
  await page.locator('#splat-alt-up').click({ modifiers: ['Shift'] });
  await page.locator('#splat-alt-down').click();
  await expect(page.locator('#splat-alt')).toHaveValue('1370.0');
  expect(await page.evaluate(() => window.fireViewer.map.splatEntities.length)).toBe(2);

  await page.locator('#splat-use-view').click();
  await expect(page.locator('#splat-view')).toContainText('Bearing');
  const camera = await page.evaluate(() => {
    const m = window.fireViewer.map,
      c = m.viewer.camera.positionCartographic;
    return {
      lat: (c.latitude * 180) / Math.PI,
      lon: (c.longitude * 180) / Math.PI,
      alt: c.height - m.offset,
      fov: (m.viewer.camera.frustum.fovy * 180) / Math.PI,
    };
  });
  await page.locator('#splat-title').fill('orbit test');
  await page.locator('#splat-refine').uncheck();

  const tab = await go(page);
  await expect(page.locator('#splat-status')).toContainText('Waiting for WebSplat');
  const nonce = await tab.evaluate(() => window.stub.nonce);
  expect(nonce).toMatch(/^[0-9a-f]{32}$/);
  // wrong nonce, wrong source and wrong origin are ignored
  await tab.evaluate(() => window.stub.ready('0'.repeat(32)));
  await page.evaluate((nonce) => window.postMessage({ type: 'websplat.ready', nonce }, '*'), nonce);
  const stubUrl = tab.url();
  await tab.evaluate((url) => (location.href = url), stubUrl.replace('127.0.0.1', 'localhost'));
  await tab.waitForURL(/localhost/);
  await tab.waitForFunction(() => window.stub);
  await tab.evaluate(() => window.stub.ready());
  await tab.waitForTimeout(500);
  expect(await tab.evaluate(() => window.stub.received)).toEqual([]);
  await expect(page.locator('#splat-status')).toContainText('Waiting for WebSplat');
  await tab.evaluate((url) => (location.href = url), stubUrl);
  await tab.waitForURL(/127\.0\.0\.1/);
  await tab.waitForFunction(() => window.stub);

  await tab.evaluate(() => window.stub.ready());
  await tab.waitForFunction(() => window.stub.received.length === 1);
  // answered once only
  await tab.evaluate(() => window.stub.ready());
  await tab.waitForTimeout(300);
  const received = await tab.evaluate(() => window.stub.received);
  expect(received).toHaveLength(1);
  const expected = await page.evaluate(() => {
    const { state } = window.fireViewer,
      f = state.frames;
    const id = (i) => ({
      pts: f[i].timestampTicks,
      offset: f[i].offset,
      size: f[i].size,
      index: i,
    });
    return {
      format: 'websplat.request',
      version: 1,
      source: {
        name: state.file.name,
        size: state.file.size,
        lastModified: state.file.lastModified,
      },
      range: { timescale: 1000, start: id(3), end: id(8) },
      origin: location.origin,
    };
  });
  const message = received[0];
  expect(message.type).toBe('websplat.request');
  expect(message.nonce).toBe(nonce);
  expect(message.origin).toBe(expected.origin);
  expect(message.file).toEqual({ name: 'thermal.mkv', size: expected.source.size, isFile: true });
  const r = message.request;
  expect(Object.keys(r).sort()).toEqual(
    ['format', 'hfov', 'kind', 'range', 'source', 'target', 'title', 'version', 'view'].sort(),
  );
  expect(r.kind).toBe('thermal');
  expect(r.format).toBe(expected.format);
  expect(r.version).toBe(1);
  expect(r.source).toEqual(expected.source);
  expect(r.range).toEqual(expected.range);
  expect(r.range.start.pts).toBe(600);
  expect(r.range.end.pts).toBe(1600);
  expect(r.target.lat).toBeCloseTo(picked.lat, 6);
  expect(r.target.lon).toBeCloseTo(picked.lon, 6);
  expect(r.target).toMatchObject({ alt_amsl: 1370, refine: false });
  expect(r.hfov).toBeNull();
  expect(r.view.camera.lat).toBeCloseTo(camera.lat, 6);
  expect(r.view.camera.lon).toBeCloseTo(camera.lon, 6);
  expect(r.view.camera.alt_amsl).toBeCloseTo(camera.alt, 1);
  expect(r.view.fov_deg).toBeCloseTo(camera.fov, 2);
  expect(r.title).toBe('orbit test');
  // the File's bytes are readable in the WebSplat tab
  expect(
    await tab.evaluate(async () =>
      [...new Uint8Array(await window.stub.file.slice(0, 4).arrayBuffer())]
        .map((b) => b.toString(16))
        .join(''),
    ),
  ).toBe('1a45dfa3');
  await expect(page.locator('#splat-status')).toContainText('copying');
  await tab.evaluate(() => window.stub.accept());
  await expect(page.locator('#splat-status')).toContainText('WebSplat accepted the job job-1.');

  // Cancel restores the whole track; a new video resets splat mode
  await page.locator('#splat-cancel').click();
  await expect(page.locator('#splat-panel')).toBeHidden();
  await page.waitForFunction(() => window.fireViewer.map.videoTrack.length >= 12);
  await page.locator('#splat-create').click();
  await expect(page.locator('#splat-range-summary')).toContainText('6 frames');
  await openThermal(page, 'uncalibrated.mkv');
  await expect(page.locator('#splat-panel')).toBeHidden();
  await expect(page.locator('#splat-band')).toBeHidden();
  await page.locator('#splat-create').click();
  await expect(page.locator('#splat-range-summary')).toContainText('12 frames');
  await expect(page.locator('#splat-title')).toHaveValue('uncalibrated');
});

test('blocked popup, closed popup, rejection and the timing warning are reported', async ({
  page,
  context,
}) => {
  await config(context);
  await start(page);
  await openThermal(page);
  await openPanel(page);
  await expect(page.locator('#splat-warnings')).not.toContainText('Telemetry offset');
  await page.evaluate(() => {
    const input = document.getElementById('offset');
    input.value = '1.5';
    input.dispatchEvent(new Event('input'));
  });
  await expect(page.locator('#splat-warnings')).toContainText('Telemetry offset');

  const tab = await go(page);
  await tab.evaluate(() => window.stub.reject('no WebGPU adapter'));
  await expect(page.locator('#splat-status')).toContainText(
    'WebSplat rejected the request: no WebGPU adapter',
  );
  await tab.close();

  const second = await go(page);
  await second.evaluate(() => window.stub.ready());
  await expect(page.locator('#splat-status')).toContainText('copying');
  await second.close();
  await expect(page.locator('#splat-status')).toContainText('closed before it accepted');

  await page.evaluate(() => {
    window.open = () => null;
  });
  await page.locator('#splat-go').click();
  await expect(page.locator('#splat-status')).toContainText('blocked the WebSplat tab');
});

test('a pending handover keeps the downloaded recording until WebSplat accepts it', async ({
  page,
  context,
}) => {
  await config(context);
  const bytes = fs.readFileSync(fixture('thermal.mkv'));
  await context.route('**/recordings/thermal.mkv', (route) =>
    route.fulfill({
      body: bytes,
      headers: {
        'content-type': 'application/octet-stream',
        'content-length': String(bytes.length),
      },
    }),
  );
  await start(page);
  await page.evaluate(() => window.fireViewer.openUrl('video', '../recordings/thermal.mkv'));
  await page.waitForFunction(() => window.fireViewer.state.decoded?.kind === 'gray16');
  await waitIndexed(page);
  await page.evaluate(() => {
    window.fireViewer.elevation.terrain = { at: () => 1360, description: 'Test terrain' };
  });
  await openPanel(page);
  expect(await held(page)).toHaveLength(1);
  // two overlapping handovers of the same download
  const tab = await go(page);
  await tab.evaluate(() => window.stub.ready());
  await tab.waitForFunction(() => window.stub.file);
  const second = await go(page);
  await second.evaluate(() => window.stub.ready());
  await second.waitForFunction(() => window.stub.file);

  // another recording opened meanwhile must not release the handed-over download
  await openThermal(page, 'uncalibrated.mkv');
  expect(await held(page)).toHaveLength(1);
  expect(await tab.evaluate(async () => (await window.stub.file.arrayBuffer()).byteLength)).toBe(
    bytes.length,
  );
  await tab.evaluate(() => window.stub.accept());
  await expect(page.locator('#status')).toContainText('WebSplat accepted the job job-1.');
  // the second handover is still copying
  await page.waitForTimeout(300);
  expect(await held(page)).toHaveLength(1);
  expect(await second.evaluate(async () => (await window.stub.file.arrayBuffer()).byteLength)).toBe(
    bytes.length,
  );
  await second.close();
  await expect(page.locator('#status')).toContainText('closed before it accepted');
  await page.waitForFunction(
    async (prefix) => !(await navigator.locks.query()).held.some((l) => l.name.startsWith(prefix)),
    LOCK,
  );
});

test('the captured camera height is AMSL with a geoid offset on ellipsoid terrain', async ({
  page,
  context,
}) => {
  await config(context, { amslToEllipsoidMetres: 30 });
  await start(page);
  await openThermal(page);
  await page.locator('#map-follow').uncheck();
  await openPanel(page);
  await page.waitForFunction(() => window.fireViewer.map.viewer.scene.mode === 3);
  const pose = await page.evaluate(() => {
    const m = window.fireViewer.map;
    m.viewer.camera.setView({ destination: m.position(-35.33, 148.84, 2700) });
    return { offset: m.offset, pose: m.cameraPose() };
  });
  expect(pose.offset).toBe(30);
  expect(pose.pose.alt_amsl).toBeCloseTo(2700, 3);
  expect(pose.pose.lat).toBeCloseTo(-35.33, 6);
});

test('a short range inside a decimated long track still draws', async ({ page, context }) => {
  await config(context);
  await start(page);
  const result = await page.evaluate(async () => {
    const map = window.fireViewer.map,
      frames = Array.from({ length: 30000 }, (_, i) => ({
        id: i,
        pts: i * 0.1,
        telemetry: {
          utc_us: 1e15 + i * 1e5,
          position: {
            lat_e7: -353219444 + i * 10,
            lon_e7: 1488380556,
            alt_amsl_m: 1400,
            age_ms: 0,
          },
        },
      })),
      record = (f) => f.telemetry;
    await map.setVideoTrack(frames, record);
    const whole = map.videoTrack.filter((p) => p.pts >= 1000 && p.pts <= 1004).length;
    await map.setTrackRange(frames.slice(10000, 10041), record, {
      dimOthers: true,
      allFrames: frames,
    });
    return { whole, range: map.videoTrack.length, dimmed: map.dimEntities.length };
  });
  expect(result.whole).toBeLessThan(10);
  expect(result.range).toBe(41);
  expect(result.dimmed).toBeGreaterThan(0);
});

test('targets need a resolved height and follow range and offset changes', async ({
  page,
  context,
}) => {
  await config(context);
  await start(page);
  await openThermal(page);
  await openPanel(page);
  await expect(page.locator('#splat-go')).toBeEnabled();
  const noTerrain = () =>
    page.evaluate(() => {
      window.fireViewer.elevation.terrain = {
        at: () => null,
        lastIssue: { state: 'unavailable', reason: 'No test terrain' },
        description: 'none',
      };
    });

  // offset edits recompute the ground points
  await page.evaluate(() => {
    const map = window.fireViewer.map,
      original = map.setLookPoints.bind(map);
    window.lookCalls = 0;
    map.setLookPoints = (points) => (window.lookCalls++, original(points));
    const input = document.getElementById('offset');
    input.value = '0.4';
    input.dispatchEvent(new Event('input'));
  });
  await page.waitForFunction(() => window.lookCalls > 0);
  await expect(page.locator('#splat-alt')).toHaveValue('1360.0');

  // a range without ground points drops the automatic target
  await noTerrain();
  await seekNumber(page, 2);
  await page.locator('#splat-set-start').click();
  await expect(page.locator('#splat-warnings')).toContainText('No ground points');
  await expect(page.locator('#splat-alt')).toHaveValue('');
  await expect(page.locator('#splat-go')).toBeDisabled();

  // a pick without terrain needs an entered height
  await page.locator('#map-follow').uncheck();
  await page.locator('#splat-pick').click();
  const box = await page.locator('#map canvas').boundingBox();
  await page.mouse.click(box.x + box.width / 2, box.y + box.height / 2);
  await expect(page.locator('#splat-status')).toContainText('enter the target height');
  await expect(page.locator('#splat-lat')).not.toHaveValue('');
  await expect(page.locator('#splat-alt')).toHaveValue('');
  await expect(page.locator('#splat-go')).toBeDisabled();
  await page.locator('#splat-alt').fill('1500');
  await page.locator('#splat-alt').press('Enter');
  await expect(page.locator('#splat-go')).toBeEnabled();

  // or terrain arriving later
  await page.locator('#splat-pick').click();
  await page.mouse.click(box.x + box.width / 2 + 20, box.y + box.height / 2);
  await expect(page.locator('#splat-go')).toBeDisabled();
  await page.evaluate(() => {
    window.fireViewer.elevation.terrain = { at: () => 1420, description: 'Test terrain' };
    window.fireViewer.splatPanel.terrainChanged();
  });
  await expect(page.locator('#splat-alt')).toHaveValue('1420.0');
  await expect(page.locator('#splat-go')).toBeEnabled();
});

test('an RGB MPEG-TS with KLV pose hands over an rgb request with its width', async ({
  page,
  context,
}) => {
  await config(context);
  await start(page);
  await openFile(page, fixture('misb.ts'));
  await waitIndexed(page);
  await page.evaluate(() => {
    window.fireViewer.elevation.terrain = { at: () => 500, description: 'Test terrain' };
  });
  await expect(page.locator('#splat-create')).toBeEnabled();
  await page.locator('#splat-create').click();
  await expect(page.locator('#splat-alt')).toHaveValue('500.0');
  await expect(page.locator('#splat-rgb-row')).toBeVisible();
  await expect(page.locator('#splat-refine-label')).toBeHidden();
  await expect(page.locator('#splat-rgb-width option')).toHaveText(['Full (320)']);
  await expect(page.locator('#splat-rgb-estimate')).toContainText('× 320×240');
  await expect(page.locator('#splat-view')).toContainText('middle training view');

  const tab = await go(page);
  await tab.evaluate(() => window.stub.ready());
  await tab.waitForFunction(() => window.stub.received.length === 1);
  const [message] = await tab.evaluate(() => window.stub.received);
  const expected = await page.evaluate(() => {
    const f = window.fireViewer.state.frames,
      id = (i) => ({ pts: f[i].timestampTicks, offset: f[i].offset, size: f[i].size, index: i });
    return { timescale: 90000, start: id(0), end: id(f.length - 1) };
  });
  const r = message.request;
  expect(message.file).toMatchObject({ name: 'misb.ts', isFile: true });
  expect(r.kind).toBe('rgb');
  expect(r.rgb).toEqual({ width: 320 });
  expect(r.target.refine).toBe(false);
  expect(r.target.alt_amsl).toBe(500);
  expect(r.range).toEqual(expected);
  expect(r.title).toBe('misb');
  await tab.evaluate(() => window.stub.accept());
  await expect(page.locator('#splat-status')).toContainText('accepted');

  // a 2560 px source offers Full and the standard widths, 1280 by default
  await page.evaluate(() =>
    window.fireViewer.splatPanel.setSource({ kind: 'rgb', width: 2560, height: 1440 }),
  );
  await expect(page.locator('#splat-rgb-width option')).toHaveText([
    'Full (2560)',
    '1920',
    '1280',
    '960',
    '640',
  ]);
  await expect(page.locator('#splat-rgb-width')).toHaveValue('1280');
  await page.locator('#splat-rgb-width').selectOption('1920');
  await expect(page.locator('#splat-warnings')).toContainText('may exceed GPU memory');
  await expect(page.locator('#splat-rgb-estimate')).toContainText('1920×1080');
});
