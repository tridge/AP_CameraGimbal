import { test, expect, start, openFile, waitIndexed, seekNumber, screenshot } from './helpers.js';

test('SCAR IR legacy KLV supplies camera pose at the first frame and 40 seconds', async ({
  page,
}, testInfo) => {
  const recording = process.env.VIEWER_SCAR_VIDEO;
  test.skip(!recording, 'Set VIEWER_SCAR_VIDEO to the original SCAR-2016-11-16-153822_IR.ts.');
  test.setTimeout(180000);
  await start(page);
  await openFile(page, recording, { kind: 'rgba', timeout: 90000 });
  await waitIndexed(page);
  const result = await page.evaluate(() => {
    const s = window.fireViewer.state,
      first = s.frames[0].telemetry;
    return {
      frames: s.frames.length,
      positions: s.frames.filter((f) => f.telemetry?.position).length,
      first,
      at40: s.frames.findIndex((f) => f.pts - s.origin >= 40),
    };
  });
  expect(result.first.sourceSchema).toBe('misb.st0601');
  expect(result.first.position.lat_e7).toBe(477023478);
  expect(result.first.position.age_ms).toBeCloseTo(7.133333333, 5);
  expect(result.first.provenance.klv_au_cell_compatibility).toBe('unmarked-complete-cell');
  expect(result.positions / result.frames).toBeGreaterThan(0.99);
  await seekNumber(page, result.at40);
  await page.getByRole('button', { name: 'Video information', exact: true }).click();
  await expect(page.locator('#video-info-content')).toContainText('H.264 / AVC');
  await expect(page.locator('#video-info-content')).toContainText('50.000 fps');
  await expect(page.locator('#video-info-content')).toContainText('MISB ST0601 KLV');
  await expect(page.locator('#video-info-json')).toContainText('unmarked-complete-cell');
  await screenshot(page, testInfo, 'scar-video-information.png');
  await page.getByRole('button', { name: 'Close video information', exact: true }).click();
  expect(await page.evaluate(() => !!window.fireViewer.pose?.gimbal_attitude)).toBe(true);
  await expect(page.locator('#timing-note')).toContainText('approximate');
  // A deterministic DEM isolates pose association from external tile coverage.
  await page.evaluate(() => {
    const { map, elevation, state, pose } = window.fireViewer;
    elevation.terrain = { at: () => 0, description: 'Test terrain' };
    map.setFollow(false);
    window.projectionTestPose = pose;
  });
  await page.locator('#map-project-video').check();
  await page.waitForFunction(
    () =>
      window.fireViewer.map.videoProjection.mesh?.indices.length > 0 &&
      window.fireViewer.map.videoProjection.primitive?.ready,
  );
  const view = await page.evaluate(() => {
    const vertices = window.fireViewer.map.videoProjection.mesh.vertices;
    return {
      lat: vertices.reduce((sum, p) => sum + p.lat, 0) / vertices.length,
      lon: vertices.reduce((sum, p) => sum + p.lon, 0) / vertices.length,
    };
  });
  for (const mode of ['2d', '3d']) {
    await page.locator('#map-' + mode).click();
    await page.evaluate((view) => window.fireViewer.map.focus(view.lat, view.lon, 6000), view);
    await screenshot(page, testInfo, `scar-pose-projection-${mode}.png`);
  }
});
