import { test, expect, fixture, start, openFile, waitIndexed, screenshot } from './helpers.js';

async function trackPixels(page) {
  return page.evaluate(async () => {
    const scene = window.fireViewer.map.viewer.scene;
    await new Promise((r) => setTimeout(r, 300));
    return new Promise((resolve) => {
      const remove = scene.postRender.addEventListener(() => {
        remove();
        const gl = scene.context._gl,
          pixels = new Uint8Array(gl.drawingBufferWidth * gl.drawingBufferHeight * 4);
        gl.readPixels(
          0,
          0,
          gl.drawingBufferWidth,
          gl.drawingBufferHeight,
          gl.RGBA,
          gl.UNSIGNED_BYTE,
          pixels,
        );
        let blue = 0,
          red = 0;
        for (let i = 0; i < pixels.length; i += 4) {
          if (pixels[i] < 100 && pixels[i + 1] < 170 && pixels[i + 2] > 200) blue++;
          if (pixels[i] > 180 && pixels[i + 1] < 100 && pixels[i + 2] < 100) red++;
        }
        resolve({ blue, red });
      });
      scene.requestRender();
    });
  });
}

test('thermal track renders cool and hot colors in 2D/3D, with missing and RGB fallbacks', async ({
  page,
}, testInfo) => {
  await start(page);
  await expect(page.locator('#map-temperature-legend')).toBeHidden();
  await openFile(page, fixture('thermal.mkv'));
  await waitIndexed(page);
  await expect(page.locator('#map-temperature-low')).toHaveText('110.9 °C');
  await expect(page.locator('#map-temperature-high')).toHaveText('121.9 °C');
  await page.locator('#map-follow').uncheck();
  await page.evaluate(async () => {
    const { state, map } = window.fireViewer;
    state.frames.forEach((f, i) => {
      f.telemetry.position = {
        ...f.telemetry.position,
        lat_e7: -353200000 + i * 1500,
        lon_e7: 1488400000 + i * 3000,
      };
    });
    await map.setVideoTrack(state.frames);
  });
  for (const mode of ['2d', '3d']) {
    await page.locator('#map-' + mode).click();
    await page.evaluate(() => window.fireViewer.map.focus(-35.3192, 148.8416, 3500));
    const pixels = await trackPixels(page);
    expect(pixels.blue, `${mode} cool track pixels`).toBeGreaterThan(5);
    expect(pixels.red, `${mode} hot track pixels`).toBeGreaterThan(5);
    await screenshot(page, testInfo, `temperature-track-${mode}.png`);
  }
  // A missing maximum must not pick up the nested RGB snapshot's TMAX.
  await page.evaluate(() => {
    window.fireViewer.state.frames[5].thermalMetadata.maximum_c = null;
  });
  await page.locator('#offset').fill('0.1');
  await expect(page.locator('#map-temperature-missing')).toBeVisible();
  await page.locator('#offset').fill('0');
  // Encoded videos with no thermal telemetry keep their cyan entity track.
  await openFile(page, fixture('misb.ts'));
  await waitIndexed(page);
  await expect(page.locator('#map-temperature-legend')).toBeHidden();
  const rgb = await page.evaluate(() => {
    const map = window.fireViewer.map;
    return {
      range: map.trackTemperatureRange,
      primitive: !!map.temperatureTrack,
      points: !!map.temperaturePoints,
      colors: map.flightEntities.map((e) => {
        const c = e.polyline.material.color.getValue();
        return [c.red, c.green, c.blue];
      }),
    };
  });
  expect(rgb.range).toBeNull();
  expect(rgb.primitive).toBe(false);
  expect(rgb.points).toBe(false);
  expect(rgb.colors.length).toBeGreaterThan(0);
  expect(rgb.colors.every((c) => JSON.stringify(c) === '[0,1,1]')).toBe(true);
});

test('real thermal recording uses its full frame-maximum range', async ({ page }, testInfo) => {
  const recording = process.env.VIEWER_TEMPERATURE_VIDEO;
  test.skip(!recording, 'Set VIEWER_TEMPERATURE_VIDEO for local thermal track qualification.');
  test.setTimeout(180000);
  await start(page);
  await openFile(page, recording, { kind: 'gray16' });
  await waitIndexed(page);
  await expect(page.locator('#map-temperature-legend')).toBeVisible();
  await page.waitForFunction(() => window.fireViewer.map.videoTrack.length > 0);
  const range = await page.evaluate(() => {
    const { state, map } = window.fireViewer;
    const maxima = state.frames.map((f) => f.thermalMetadata?.maximum_c).filter(Number.isFinite);
    return {
      actual: map.trackTemperatureRange,
      expected: { min: Math.min(...maxima), max: Math.max(...maxima) },
    };
  });
  expect(range.actual).toEqual(range.expected);
  for (const mode of ['2d', '3d']) {
    await page.locator('#map-' + mode).click();
    const pixels = await trackPixels(page);
    expect(pixels.red, `${mode}: hot samples remain visible on overlapping passes`).toBeGreaterThan(
      0,
    );
    await screenshot(page, testInfo, `real-temperature-track-${mode}.png`);
  }
});
