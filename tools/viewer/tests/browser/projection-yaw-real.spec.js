import { test, expect, start, openFile, waitIndexed, seekNumber, screenshot } from './helpers.js';

test('Phoenix frames 2185–2345 stabilise projected yaw consistently across seeks', async ({
  page,
}, testInfo) => {
  const file = process.env.VIEWER_PHOENIX_VIDEO;
  test.skip(!file, 'Set VIEWER_PHOENIX_VIDEO to the Phoenix RGB recording.');
  test.setTimeout(180000);
  await start(page);
  await openFile(page, file);
  await waitIndexed(page);
  await page.evaluate(() => {
    window.fireViewer.elevation.terrain = { at: () => 0, description: 'Yaw test terrain' };
  });
  await page.locator('#map-project-video').check();
  const samples = [];
  for (let i = 2184; i <= 2344; i += 5) {
    await seekNumber(page, i);
    samples.push(
      await page.evaluate(() => {
        const { state, map } = window.fireViewer,
          r = state.frame.telemetry;
        return {
          frame: state.number,
          raw: r.vehicle_attitude.yaw_rad + r.gimbal_attitude.yaw_rad,
          displayed: JSON.parse(map.videoProjection.key)[3].yaw,
        };
      }),
    );
  }
  const peak = (values) =>
    Math.max(
      ...values
        .slice(1)
        .map((v, i) => Math.abs(Math.atan2(Math.sin(v - values[i]), Math.cos(v - values[i])))),
    );
  expect(peak(samples.map((s) => s.displayed))).toBeLessThan(peak(samples.map((s) => s.raw)) * 0.6);
  for (const sample of samples.filter((_, i) => i % 8 === 0).reverse()) {
    await seekNumber(page, sample.frame);
    const yaw = await page.evaluate(
      () => JSON.parse(window.fireViewer.map.videoProjection.key)[3].yaw,
    );
    expect(yaw).toBeCloseTo(sample.displayed, 10);
  }
  await expect(page.locator('#map-projection-note')).toContainText('Yaw stabilisation: 2 s');
  await screenshot(page, testInfo, 'phoenix-projection-stabilised.png');
});
