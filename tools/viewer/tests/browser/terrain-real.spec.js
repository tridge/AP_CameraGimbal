import {
  test,
  expect,
  start,
  openFile,
  waitIndexed,
  seekNumber,
  pointAt,
  screenshot,
} from './helpers.js';
const thermal = process.env.VIEWER_REAL_THERMAL,
  bin = process.env.VIEWER_REAL_BIN;
const terrainUrl = process.env.VIEWER_REAL_TERRAIN_URL;
test('real quantized terrain locates hovered and marked thermal pixels in both map modes', async ({
  page,
}, testInfo) => {
  test.skip(
    !thermal || !bin || !terrainUrl,
    'Set VIEWER_REAL_THERMAL/BIN/TERRAIN_URL for the real terrain qualification.',
  );
  test.setTimeout(240000);
  await page.route('**/viewer-config.js', (route) =>
    route.fulfill({
      contentType: 'text/javascript',
      body: `window.VIEWER_CONFIG=${JSON.stringify({ imagery: 'none', terrainUrl, terrainDatum: 'amsl', amslToEllipsoidMetres: 0 })};`,
    }),
  );
  await start(page);
  await page.waitForFunction(
    (url) => window.fireViewer.map.configuration.terrainUrl === url,
    terrainUrl,
  );
  if (new URL(terrainUrl, page.url()).origin !== new URL(page.url()).origin) {
    const probe = await page.evaluate(async (url) => {
      // Non-safelisted Content-Type forces a real browser CORS preflight.
      const response = await fetch(new URL('layer.json', url), {
        headers: { 'Content-Type': 'application/json' },
      });
      return { status: response.status, format: (await response.json()).format };
    }, terrainUrl);
    expect(probe).toEqual({ status: 200, format: 'quantized-mesh-1.0' });
  }
  await expect(page.locator('#open-dem,#dem-file,#ground-altitude,#clear-dem')).toHaveCount(0);
  await openFile(page, thermal, { kind: 'gray16' });
  await waitIndexed(page);
  await page.locator('#log-file').setInputFiles(bin);
  await page.waitForFunction(() => window.fireViewer.state.log?.mission?.items?.length === 81);
  await seekNumber(page, 8315);
  await pointAt(page, 337, 373);
  await page.waitForFunction(
    () => window.fireViewer.elevation.ground(window.fireViewer.pose, 337, 373, 640, 512) !== null,
  );
  await expect(page.locator('#hover-reading')).toContainText('(approx.)');
  const ground = await page.evaluate(() =>
    window.fireViewer.elevation.ground(window.fireViewer.pose, 337, 373, 640, 512),
  );
  expect(ground.terrain.terrainDatum).toBe('amsl');
  expect(ground.terrain.terrainLevel).toBe(12);
  expect(ground.altMsl).toBeGreaterThan(700);
  expect(ground.altMsl).toBeLessThan(1000);
  await page.locator('#map-3d').click();
  await page.evaluate(() => window.fireViewer.map.focus(-35.32, 148.847, 3500));
  const other = await page.evaluate(() =>
    window.fireViewer.elevation.ground(window.fireViewer.pose, 337, 373, 640, 512),
  );
  expect(other).toEqual(ground);
  await screenshot(page, testInfo, 'real-terrain-chrome-3d.png');
  await page.locator('#map-2d').click();
  await screenshot(page, testInfo, 'real-terrain-chrome-2d.png');
  // Hold new tiles long enough to move playback while the marked pose resolves.
  const marked = await page.evaluate(async () => {
    const { map, state, elevation, markFinding, showFrame } = window.fireViewer;
    const provider = map.viewer.terrainProvider;
    const request = provider.requestTileGeometry;
    // Delay the provider result, allowing ordinary HTTP cancellation to keep
    // working when the renderer or adapter abandons a prefetched tile.
    provider.requestTileGeometry = function (...args) {
      const result = request.apply(this, args);
      return result
        ? Promise.resolve(result).then(
            (data) => new Promise((resolve) => setTimeout(() => resolve(data), 400)),
          )
        : result;
    };
    try {
      map.terrain.configure(provider, map.configuration);
      const index = state.findings.length,
        pending = markFinding();
      await showFrame(8316);
      await pending;
      return state.findings[index];
    } finally {
      provider.requestTileGeometry = request;
    }
  });
  expect(marked.frameNumber).toBe(8315);
  expect(marked.rawPixel).toEqual({ x: 337, y: 373 });
  expect(marked.groundUnavailableReason).toBeNull();
  expect(marked.ground).not.toBeNull();
  expect(marked.ground.lat).toBeCloseTo(ground.lat, 10);
  expect(marked.ground.lon).toBeCloseTo(ground.lon, 10);
  console.log('Real terrain Chrome:', JSON.stringify({ ground, markedFrame: marked.frameNumber }));
});
