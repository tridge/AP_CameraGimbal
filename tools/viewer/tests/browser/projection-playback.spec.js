import { test, expect, fixture, start, openFile, waitIndexed, seekNumber } from './helpers.js';

test('slow projection skips intermediate playback frames, catches up on pause and cancels when disabled', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'));
  await waitIndexed(page);
  await page.evaluate(() => {
    const { state, elevation } = window.fireViewer;
    elevation.terrain = { at: () => 0, description: 'Playback test terrain' };
    state.frames.forEach((f, i) => {
      f.telemetry.position.lat_e7 += i * 1000;
    });
    state.dirtyPose = true;
  });
  await page.locator('#map-project-video').check();
  await page.waitForFunction(() => window.fireViewer.map.videoProjection.primitive?.ready);
  await page.evaluate(() => {
    const p = window.fireViewer.map.videoProjection,
      original = p.update;
    window.projectionFrames = [];
    p.update = function (image, record, elevation) {
      const { state } = window.fireViewer;
      if (state.playing) {
        window.projectionFrames.push({
          frame: state.number,
          imageMatches: image === state.image,
          poseMatches: record?.position?.lat_e7 === state.frame.telemetry.position.lat_e7,
        });
        // Simulate an expensive terrain/raster pass; the adaptive cadence must
        // leave playback room rather than doing this on every decoded frame.
        const until = performance.now() + 80;
        while (performance.now() < until) {}
      }
      return original.call(this, image, record, elevation);
    };
  });
  await page.getByRole('button', { name: 'Play video', exact: true }).click();
  await page.waitForFunction(
    () => !window.fireViewer.state.playing && window.fireViewer.state.number === 11,
    null,
    { timeout: 15000 },
  );
  await page.waitForFunction(
    () =>
      window.fireViewer.map.videoProjection.image === window.fireViewer.state.image &&
      window.fireViewer.map.videoProjection.frameId === window.fireViewer.state.frame.id,
  );
  const samples = await page.evaluate(() => window.projectionFrames);
  expect(samples.length).toBeGreaterThan(1);
  expect(samples.length).toBeLessThan(9);
  expect(samples.every((s) => s.imageMatches && s.poseMatches)).toBe(true);
  expect(samples.some((s, i) => i && s.frame - samples[i - 1].frame > 1)).toBe(true);
  await seekNumber(page, 0);
  await page.getByRole('button', { name: 'Play video', exact: true }).click();
  await page.waitForFunction(() => window.fireViewer.state.number >= 2);
  await page.locator('#offset').fill('99999');
  expect(await page.evaluate(() => !window.fireViewer.map.videoProjection.primitive)).toBe(true);
  await expect(page.locator('#map-projection-note')).toContainText('pose');
  await page.locator('#offset').fill('0');
  await page.locator('#map-project-video').uncheck();
  const count = await page.evaluate(() => window.projectionFrames.length);
  await page.waitForTimeout(500);
  expect(await page.evaluate(() => window.projectionFrames.length)).toBe(count);
  expect(
    await page.evaluate(
      () =>
        !window.fireViewer.map.videoProjection.primitive &&
        !window.fireViewer.map.videoProjection.texture,
    ),
  ).toBe(true);
  await page.getByRole('button', { name: 'Pause video', exact: true }).click();
});

test('Phoenix playback keeps projection current while skipping work over real terrain', async ({
  page,
}, testInfo) => {
  const file = process.env.VIEWER_PHOENIX_VIDEO;
  test.skip(!file, 'Set VIEWER_PHOENIX_VIDEO to qualify real projection playback.');
  test.setTimeout(180000);
  await page.route('**/viewer-config.js', (route) =>
    route.fulfill({
      contentType: 'text/javascript',
      body: "window.VIEWER_CONFIG={imagery:'none',terrainUrl:'https://plot.ardupilot.org/quantized/',terrainDatum:'amsl',amslToEllipsoidMetres:0};",
    }),
  );
  await start(page);
  await openFile(page, file);
  await waitIndexed(page);
  await seekNumber(page, 2184);
  await page.locator('#map-follow').uncheck();
  await page.locator('#map-3d').click();
  await page.locator('#map-project-video').check();
  await page.waitForFunction(() => window.fireViewer.map.videoProjection.primitive?.ready, null, {
    timeout: 120000,
  });
  await page.evaluate(() => {
    const { map, pose, elevation } = window.fireViewer;
    const centre = elevation.ground(pose, 960, 700, 1920, 1080);
    map.viewer.camera.lookAt(map.position(centre.lat, centre.lon, centre.altMsl), {
      heading: 0,
      pitch: -0.7,
      range: 5000,
    });
    const p = map.videoProjection,
      original = p.update;
    window.realProjectionFrames = [];
    p.update = function (image, record, elevation) {
      if (window.fireViewer.state.playing)
        window.realProjectionFrames.push({
          frame: window.fireViewer.state.number,
          matches: image === window.fireViewer.state.image,
        });
      return original.call(this, image, record, elevation);
    };
  });
  await page.getByRole('button', { name: 'Play video', exact: true }).click();
  await page.waitForTimeout(4000);
  await page.getByRole('button', { name: 'Pause video', exact: true }).click();
  await page.waitForFunction(
    () =>
      !window.fireViewer.state.decoding &&
      window.fireViewer.map.videoProjection.image === window.fireViewer.state.image &&
      window.fireViewer.map.videoProjection.frameId === window.fireViewer.state.frame.id,
  );
  const report = await page.evaluate(() => ({
    decodedFrames: window.fireViewer.state.number - 2184,
    projectionFrames: window.realProjectionFrames,
  }));
  expect(report.decodedFrames).toBeGreaterThan(3);
  expect(report.projectionFrames.length).toBeGreaterThan(0);
  expect(report.projectionFrames.length).toBeLessThan(report.decodedFrames);
  expect(report.projectionFrames.every((f) => f.matches)).toBe(true);
  await testInfo.attach('real-projection-cadence', {
    body: JSON.stringify(report, null, 2),
    contentType: 'application/json',
  });
});
