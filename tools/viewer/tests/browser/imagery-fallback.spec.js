import { test, expect, start, screenshot } from './helpers.js';

// Exercise the real imagery provider and renderer against a deterministic ArcGIS
// service: green parent imagery, gray HTTP-200 missing tiles, blue detailed imagery.
test('satellite zoom retains available imagery in 2D/3D without limiting detailed coverage', async ({
  page,
}, testInfo) => {
  const images = await page.evaluate(() => {
    const canvas = document.createElement('canvas');
    canvas.width = canvas.height = 256;
    const context = canvas.getContext('2d');
    return ['#20b040', '#cccccc', '#2040b0'].map((color) => {
      context.fillStyle = color;
      context.fillRect(0, 0, 256, 256);
      return canvas.toDataURL().split(',')[1];
    });
  });
  const requests = [];
  await page.route('**/viewer-config.js', (route) =>
    route.fulfill({
      contentType: 'text/javascript',
      body: "window.VIEWER_CONFIG={imagery:'satellite',terrainUrl:'',amslToEllipsoidMetres:42};",
    }),
  );
  await page.route('https://server.arcgisonline.com/**/World_Imagery/MapServer/**', (route) => {
    const match = new URL(route.request().url()).pathname.match(/\/tile\/(\d+)\/(\d+)\/(\d+)$/);
    if (!match) return route.fulfill({ status: 500, body: 'Metadata unavailable' });
    const [level, y, x] = match.slice(1).map(Number);
    requests.push({ level, x, y });
    // Western hemisphere has fine coverage; the thermal flight in Australia
    // has no tiles beyond level 18. (23/0/0 is always the missing reference.)
    const missing = level > 18 && (x >= 2 ** (level - 1) || (x === 0 && y === 0));
    return route.fulfill({
      contentType: 'image/png',
      body: Buffer.from(images[missing ? 1 : level > 18 ? 2 : 0], 'base64'),
    });
  });
  await start(page);
  await page.waitForFunction(() => {
    const provider = window.fireViewer.map.viewer.imageryLayers.get(0)?.imageryProvider;
    return provider?.tileDiscardPolicy?.isReady();
  });
  expect(
    await page.evaluate(() => {
      const p = window.fireViewer.map.viewer.imageryLayers.get(0).imageryProvider;
      return {
        maximumLevel: p.maximumLevel,
        pickFeatures: p.enablePickFeatures,
        offset: window.fireViewer.map.offset,
      };
    }),
  ).toEqual({ maximumLevel: 23, pickFeatures: false, offset: 42 });

  for (const mode of ['2d', '3d']) {
    await page.locator('#map-' + mode).click();
    for (const [lon, dominant] of [
      [148.907, 'green'],
      [-100, 'blue'],
    ]) {
      await page.evaluate((lon) => window.fireViewer.map.focus(-35.579, lon, 80), lon);
      await expect
        .poll(() =>
          requests.some(
            (p) =>
              p.level > 18 &&
              p.x > 0 &&
              (lon > 0 ? p.x > 2 ** (p.level - 1) : p.x < 2 ** (p.level - 1)),
          ),
        )
        .toBe(true);
      await page.waitForFunction(() => window.fireViewer.map.viewer.scene.globe.tilesLoaded);
      let result;
      await expect
        .poll(
          async () => {
            result = await page.evaluate(
              () =>
                new Promise((resolve) => {
                  const scene = window.fireViewer.map.viewer.scene;
                  const remove = scene.postRender.addEventListener(() => {
                    const gl = scene.context._gl;
                    const size = 64,
                      pixels = new Uint8Array(size * size * 4);
                    gl.readPixels(
                      Math.floor((gl.drawingBufferWidth - size) / 2),
                      Math.floor((gl.drawingBufferHeight - size) / 2),
                      size,
                      size,
                      gl.RGBA,
                      gl.UNSIGNED_BYTE,
                      pixels,
                    );
                    let green = 0,
                      blue = 0;
                    for (let i = 0; i < pixels.length; i += 4) {
                      if (pixels[i + 1] > pixels[i] * 2 && pixels[i + 1] > pixels[i + 2] * 2)
                        green++;
                      if (pixels[i + 2] > pixels[i] * 2 && pixels[i + 2] > pixels[i + 1] * 2)
                        blue++;
                    }
                    remove();
                    resolve({ green, blue, height: scene.camera.positionCartographic.height });
                  });
                  scene.requestRender();
                }),
            );
            return result[dominant];
          },
          { message: `${mode}, ${lon}: rendered imagery` },
        )
        .toBeGreaterThan(4000);
      expect(result.height, 'close zoom is preserved').toBeLessThan(200);
      await screenshot(page, testInfo, `${mode}-${dominant}-imagery.png`);
    }
  }
  expect(requests.some((p) => p.level > 18 && p.x > 2 ** (p.level - 1))).toBe(true);
  expect(requests.some((p) => p.level > 18 && p.x > 0 && p.x < 2 ** (p.level - 1))).toBe(true);
});

test('an unavailable imagery reference does not block terrain or datum settings', async ({
  page,
}) => {
  await start(page);
  await page.route('https://server.arcgisonline.com/**', (route) =>
    route.fulfill({ status: 500, body: 'Imagery unavailable' }),
  );
  await page.route('**/test-terrain/layer.json', (route) =>
    route.fulfill({
      json: {
        tilejson: '2.1.0',
        format: 'quantized-mesh-1.0',
        version: '1.0.0',
        scheme: 'tms',
        projection: 'EPSG:4326',
        tiles: ['{z}/{x}/{y}.terrain'],
        maxzoom: 0,
        available: [[{ startX: 0, startY: 0, endX: 1, endY: 0 }]],
      },
    }),
  );
  await page.route('**/test-terrain/**/*.terrain*', (route) => route.fulfill({ status: 404 }));
  expect(
    await page.evaluate(() =>
      window.fireViewer.map.configure({
        imagery: 'satellite',
        terrainUrl: new URL('test-terrain/', location.href).href,
        terrainDatum: 'ellipsoid',
        amslToEllipsoidMetres: 37,
      }),
    ),
  ).toBe(true);
  await page.waitForFunction(() =>
    window.fireViewer.map.viewer.imageryLayers.get(0).imageryProvider.tileDiscardPolicy.isReady(),
  );
  expect(
    await page.evaluate(() => {
      const { map } = window.fireViewer;
      return {
        offset: map.offset,
        terrainOffset: map.terrain.offset,
        providerApplied: map.terrain.provider === map.viewer.terrainProvider,
        available: !!map.terrain.provider.availability,
      };
    }),
  ).toEqual({ offset: 37, terrainOffset: 37, providerApplied: true, available: true });
});
