import {
  test,
  expect,
  fixture,
  start,
  openFile,
  waitIndexed,
  pointAt,
  screenshot,
} from './helpers.js';

const pixel = (page, x, y) =>
  page
    .locator('#video-canvas')
    .evaluate(
      (canvas, point) => [...canvas.getContext('2d').getImageData(point.x, point.y, 1, 1).data],
      { x, y },
    );

test('gamma and GloryHot use editable windows and retain measured thermal pixels', async ({
  page,
}, testInfo) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'), { kind: 'gray16' });
  await waitIndexed(page);
  await expect(page.locator('#stretch-low')).toBeEnabled();
  await expect(page.locator('#stretch-high')).toBeEnabled();
  await expect(page.locator('#stretch-units')).toHaveText('°C');
  await page.locator('#stretch').selectOption('gamma');
  expect(Number(await page.locator('#stretch-high').inputValue())).toBeGreaterThan(100);
  await page.locator('#stretch-low').fill('5');
  await page.locator('#stretch-high').fill('25');
  await page.locator('#display-gamma').fill('1');
  const linear = await pixel(page, 300, 200);
  await page.locator('#display-gamma').fill('2');
  const gamma = await pixel(page, 300, 200);
  expect(gamma[0]).toBeLessThan(linear[0]);
  await pointAt(page, 20, 30);
  await expect(page.locator('#hover-reading')).toContainText('1.94 °C');
  await page.locator('#palette').selectOption('GloryHot');
  // Sample away from the hover crosshair, which is deliberately coloured.
  expect(new Set((await pixel(page, 50, 40)).slice(0, 3)).size).toBe(1);
  expect(new Set((await pixel(page, 400, 300)).slice(0, 3)).size).toBeGreaterThan(1);
  const beforeHigh = await pixel(page, 400, 300);
  await page.locator('#stretch-high').fill('40');
  expect(await pixel(page, 400, 300)).not.toEqual(beforeHigh);
  await screenshot(page, testInfo, 'gloryhot-gamma.png');
  await page.locator('#stretch').selectOption('auto');
  await page.locator('#stretch-low').fill('5');
  await expect(page.locator('#stretch')).toHaveValue('fixed');
  await page.locator('#stretch-high').fill('30');
  await page.locator('#next').click();
  await expect(page.locator('#frame-number')).toContainText('Frame 2 / 12');
  await expect(page.locator('#stretch-low')).toHaveValue('5');
  await expect(page.locator('#stretch-high')).toHaveValue('30');
  await page.locator('#stretch-low').fill('');
  await expect(page.locator('#stretch-low')).toHaveAttribute('aria-invalid', 'true');
  await page.locator('#next').click();
  await expect(page.locator('#frame-number')).toContainText('Frame 3 / 12');
  await expect(page.locator('#stretch-low')).toHaveValue('');
  await expect(page.locator('#palette')).toHaveValue('GloryHot');
  await page.locator('#stretch-low').fill('5');
  await expect(page.locator('#stretch-low')).toHaveAttribute('aria-invalid', 'false');
  expect(await page.evaluate(() => window.fireViewer.state.decoded.data[30 * 640 + 20])).toBe(
    17734,
  );
});

test('uncalibrated raw and Original RGB accept levels and gamma using honest units', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('uncalibrated.mkv'), { kind: 'gray16' });
  await expect(page.locator('#stretch-units')).toHaveText('raw counts');
  await page.locator('#stretch').selectOption('gamma');
  await page.locator('#stretch-low').fill('17500');
  await page.locator('#stretch-high').fill('19000');
  await pointAt(page, 20, 30);
  await expect(page.locator('#hover-reading')).not.toContainText('°C');
  await expect(page.locator('#hover-reading')).toContainText('17606');
  await openFile(page, fixture('misb.ts'), { kind: 'rgba' });
  await expect(page.locator('#stretch-units')).toHaveText('intensity (0–255)');
  await expect(page.locator('#palette')).toHaveValue('Original');
  const original = await pixel(page, 50, 40);
  await page.locator('#stretch').selectOption('gamma');
  await page.locator('#display-gamma').fill('2');
  const gamma = await pixel(page, 50, 40);
  expect(gamma).not.toEqual(original);
  await page.locator('#stretch-high').fill('128');
  expect(await pixel(page, 50, 40)).not.toEqual(gamma);
  await expect(page.locator('#hover-reading')).not.toContainText('°C');
});

test('committing a numeric display edit does not render the frame a second time', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'), { kind: 'gray16' });
  await waitIndexed(page);
  await page.locator('#stretch').selectOption('gamma');
  for (const [id, value] of [
    ['stretch-low', '5'],
    ['stretch-high', '30'],
    ['display-gamma', '1.5'],
  ]) {
    await page.evaluate(() => {
      window.imageBeforeEdit = window.fireViewer.state.image;
    });
    await page.locator(`#${id}`).fill(value);
    expect(
      await page.evaluate(() => window.imageBeforeEdit !== window.fireViewer.state.image),
    ).toBe(true);
    await page.evaluate(() => {
      window.imageAfterEdit = window.fireViewer.state.image;
    });
    await page.locator(`#${id}`).press('Tab');
    expect(await page.evaluate(() => window.imageAfterEdit === window.fireViewer.state.image)).toBe(
      true,
    );
  }
});

test('GloryHot preserves the Auto Low boundary when switching to a manual window', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'), { kind: 'gray16' });
  await waitIndexed(page);
  await page.evaluate(() => {
    const state = window.fireViewer.state;
    state.decoded.data.fill(22002);
    state.frame.thermalMetadata.temperature_scale_k = Math.fround(0.0123);
    state.frame.thermalMetadata.temperature_offset_k = 0;
  });
  await page.locator('#palette').selectOption('GloryHot');
  const temperature = 22002 * Math.fround(0.0123) - 273.15;
  expect(Number(await page.locator('#stretch-low').inputValue())).toBe(temperature);
  const automatic = await pixel(page, 300, 200);
  expect(new Set(automatic.slice(0, 3)).size).toBe(1);
  await page.locator('#stretch').selectOption('fixed');
  expect(await pixel(page, 300, 200)).toEqual(automatic);
  await page.locator('#stretch').selectOption('gamma');
  expect(new Set((await pixel(page, 300, 200)).slice(0, 3)).size).toBe(1);
});
