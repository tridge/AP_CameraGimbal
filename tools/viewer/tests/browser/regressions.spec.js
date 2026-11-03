import {
  test,
  expect,
  fixture,
  start,
  openFile,
  waitIndexed,
  seekNumber,
  pointAt,
  screenshot,
} from './helpers.js';

const rawCelsius = (x, y, frame = 0) => (17536 + 2 * x + y + frame * 64) / 64 - 273.15;

async function hoverRaw(page, x, y, rotation = 0, frame = 0) {
  // Move elsewhere first: a clipped canvas must not pass with stale hover text.
  await pointAt(page, 300, 200);
  const display = rotation === 180 ? [639 - x, 511 - y] : [x, y];
  const client = await pointAt(page, ...display);
  expect(
    await page.evaluate(({ x, y }) => document.elementFromPoint(x, y)?.id, client),
    `Raw corner ${x},${y} must be visible and receive pointer events`,
  ).toBe('video-canvas');
  await expect(page.locator('#hover-reading')).toContainText(`Raw pixel ${x}, ${y}`);
  await expect(page.locator('#hover-reading')).toContainText(
    `${rawCelsius(x, y, frame).toFixed(2)} °C`,
  );
}

test('entire thermal image fits stage and all four calibrated raw corners remain reachable after rotation', async ({
  page,
}, testInfo) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'), { kind: 'gray16' });
  const canvas = await page.locator('#video-canvas').boundingBox();
  const stage = await page.locator('#video-stage').boundingBox();
  // Fit preserves the detector aspect ratio and may leave unused stage space.
  expect(canvas.width / canvas.height).toBeCloseTo(640 / 512, 3);
  expect(canvas.x).toBeGreaterThanOrEqual(stage.x - 1);
  expect(canvas.y).toBeGreaterThanOrEqual(stage.y - 1);
  expect(canvas.x + canvas.width).toBeLessThanOrEqual(stage.x + stage.width + 1);
  expect(canvas.y + canvas.height).toBeLessThanOrEqual(stage.y + stage.height + 1);
  // Corners probe the contained bitmap, including the previously clipped bottom.
  for (const rotation of [0, 180]) {
    await page.locator('#rotation').selectOption(String(rotation));
    for (const [x, y] of [
      [0, 0],
      [639, 0],
      [639, 511],
      [0, 511],
    ])
      await hoverRaw(page, x, y, rotation);
  }
  await page.getByRole('spinbutton', { name: 'Raw pixel X', exact: true }).fill('20');
  await page.getByRole('spinbutton', { name: 'Raw pixel Y', exact: true }).fill('30');
  await expect(page.locator('#hover-reading')).toContainText('Raw pixel 20, 30');
  await expect(page.locator('#hover-reading')).toContainText('1.94 °C');
  await screenshot(page, testInfo, 'complete-thermal-corners-rotated.png');
});

test('rapid next requests advance while invalid Celsius edits retain the last valid display without stale image or pose', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'), { kind: 'gray16' });
  await waitIndexed(page);
  await page.locator('#stretch').selectOption('fixed');
  await page.locator('#stretch-high').fill('-10');
  await page.locator('#stretch-high').press('Tab');
  await expect(page.locator('#display-note')).toContainText('last valid display settings');
  await expect(page.locator('#stretch-high')).toHaveAttribute('aria-invalid', 'true');
  await expect(page.locator('#stretch')).toHaveValue('fixed');
  // Synchronous real control events guarantee these requests overlap a decode.
  await page.locator('#next').evaluate((button) => {
    button.click();
    button.click();
    button.click();
  });
  await expect(page.locator('#frame-number')).toHaveText('Frame 4 / 12');
  await hoverRaw(page, 20, 30, 0, 3);
  await expect(page.locator('#telemetry-fields')).toContainText('2026-08-28 02:08:32.967 UTC');
  const displayed = await page.locator('#video-canvas').evaluate((canvas) => {
    const c = canvas.getContext('2d');
    return {
      current: [...c.getImageData(326, 256, 1, 1).data],
      old: [...c.getImageData(320, 256, 1, 1).data],
    };
  });
  expect(displayed.current.slice(0, 3)).toEqual([255, 255, 255]);
  expect(displayed.old[0]).toBeLessThan(255);
  expect(await page.evaluate(() => window.fireViewer.state.decoded.data[30 * 640 + 20])).toBe(
    17606 + 3 * 64,
  );
  await expect(page.locator('#stretch-high')).toHaveValue('-10');
  await page.locator('#stretch-high').fill('50');
  await expect(page.locator('#stretch-high')).toHaveAttribute('aria-invalid', 'false');
});

test('unknown video UTC cannot silently select latest mission; latest remains an explicit choice', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('unknown-time.mkv'), { kind: 'gray16' });
  await page.locator('#log-file').setInputFiles(fixture('mission.bin'));
  await expect(page.locator('#log-time-mode')).toBeEnabled();
  await expect(page.locator('#log-time-mode')).toHaveValue('frame');
  await expect(page.locator('#log-summary')).toContainText('0 mission items · 0 fences');
  await expect(page.locator('#log-summary')).toContainText(/uncertain|unavailable|unknown/i);
  expect(await page.evaluate(() => window.fireViewer.state.visibleLog.mission.items.length)).toBe(
    0,
  );
  await page.locator('#log-time-mode').selectOption('latest');
  await expect(page.locator('#log-summary')).toContainText('4 mission items · 4 fences');
  await page.locator('#log-time-mode').selectOption('frame');
  await expect(page.locator('#log-summary')).toContainText('0 mission items · 0 fences');
});

test('a truncated archive retains playable prefix and stops at its last indexed frame', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('partial.mkv'), { kind: 'gray16' });
  await expect(page.locator('#error-banner')).toBeVisible();
  await expect(page.locator('#status')).toContainText(/indexing stopped|prefix|incomplete/i);
  const count = await page.evaluate(() => window.fireViewer.state.frames.length);
  expect(count).toBe(12);
  await seekNumber(page, count - 1);
  await page.locator('#play').click();
  await expect(page.locator('#play')).toHaveText('Play');
  expect(await page.evaluate(() => window.fireViewer.state.playing)).toBe(false);
  await page.locator('#previous').click();
  await expect(page.locator('#frame-number')).toContainText('Frame 11 / 12');
});

test('missing calibration never invents Celsius from raw pixels', async ({ page }) => {
  await start(page);
  await openFile(page, fixture('uncalibrated.mkv'), { kind: 'gray16' });
  await pointAt(page, 20, 30);
  await expect(page.locator('#hover-reading')).toContainText('17606');
  await expect(page.locator('#hover-reading')).not.toContainText('°C');
  await expect(page.locator('#stretch')).toBeEnabled();
  await expect(page.locator('#stretch-low')).toBeEnabled();
  await expect(page.locator('#stretch-high')).toBeEnabled();
  await expect(page.locator('#stretch-units')).toHaveText('raw counts');
  await expect(page.locator('#stretch')).toHaveValue('auto');
  await page.locator('#next').click();
  await expect(page.locator('#frame-number')).toContainText('Frame 2 / 12');
  await pointAt(page, 20, 30);
  await expect(page.locator('#hover-reading')).toContainText('17670');
  await expect(page.locator('#hover-reading')).not.toContainText('°C');
});

test('fixed Celsius rendering recovers when the next frame loses calibration and keeps image, raw values and UTC together', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('mixed-calibration.mkv'), { kind: 'gray16' });
  await waitIndexed(page);
  await page.locator('#stretch').selectOption('fixed');
  await page.locator('#next').click();
  await expect(page.locator('#frame-number')).toHaveText('Frame 2 / 12');
  await expect(page.locator('#stretch')).toHaveValue('auto');
  await expect(page.locator('#stretch')).toBeEnabled();
  await expect(page.locator('#stretch-units')).toHaveText('raw counts');
  await pointAt(page, 20, 30);
  await expect(page.locator('#hover-reading')).toContainText('17670 raw');
  await expect(page.locator('#hover-reading')).not.toContainText('°C');
  await expect(page.locator('#telemetry-fields')).toContainText('2026-08-28 02:08:32.567 UTC');
  const pixels = await page.locator('#video-canvas').evaluate((canvas) => {
    const c = canvas.getContext('2d');
    return {
      current: [...c.getImageData(322, 256, 1, 1).data],
      old: [...c.getImageData(319, 256, 1, 1).data],
    };
  });
  expect(pixels.current.slice(0, 3)).toEqual([255, 255, 255]);
  expect(pixels.old[0]).toBeLessThan(255);
  expect(await page.evaluate(() => window.fireViewer.state.decoded.data[30 * 640 + 20])).toBe(
    17670,
  );
});

test('failed terrain settings preserve active datum and time anchors disclose the additional offset', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'), { kind: 'gray16' });
  // Cesium intentionally treats layer.json404 as legacy terrain;500 is a real failure.
  await page.route('**/no-such-terrain/**', (route) =>
    route.fulfill({ status: 500, body: 'Test terrain is unavailable' }),
  );
  const prior = await page.evaluate(() => ({
    state: window.fireViewer.state.config,
    map: window.fireViewer.map.offset,
    configuration: window.fireViewer.map.configuration,
  }));
  await page.locator('#settings-button').click();
  await page.locator('#geoid-offset').fill('40');
  await page.locator('#terrain-url').fill('/viewer/no-such-terrain/');
  await page.locator('#apply-settings').click();
  await expect(page.locator('#error-banner')).toBeVisible();
  await expect(page.locator('#apply-settings')).toBeEnabled();
  const after = await page.evaluate(() => ({
    state: window.fireViewer.state.config,
    map: window.fireViewer.map.offset,
    configuration: window.fireViewer.map.configuration,
  }));
  expect(after).toEqual(prior);
  await page.getByRole('button', { name: 'Close settings', exact: true }).click();
  await page.locator('#dismiss-error').click();
  await page.locator('#anchors-button').click();
  await page.locator('#anchors-json').fill(
    JSON.stringify([
      { videoSeconds: 0, utc: '2026-08-28T02:08:32.367Z' },
      { videoSeconds: 2, utc: '2026-08-28T02:08:34.367Z' },
    ]),
  );
  await page.locator('#apply-anchors').click();
  await page.locator('#offset').fill('0.5');
  await expect(page.locator('#timing-note')).toContainText(/anchor/i);
  await expect(page.locator('#timing-note')).toContainText(/0\.5|0\.500/);
  const fields = await page.locator('#telemetry-fields').textContent();
  expect(fields).toContain('2026-08-28 02:08:32.367 UTC');
  expect(fields).toContain('2026-08-28 02:08:32.867 UTC');
});
