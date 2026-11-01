import {
  test,
  expect,
  fixture,
  start,
  openFile,
  waitIndexed,
  seekNumber,
  pointAt,
  downloadJSON,
  screenshot,
} from './helpers.js';

test('thermal review keeps raw values through palettes, rotation, playback and exports', async ({
  page,
}, testInfo) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'), { chooser: true, kind: 'gray16' });
  await waitIndexed(page);
  expect(await page.evaluate(() => window.fireViewer.state.frames.length)).toBe(12);
  await expect(page.locator('#rotation')).toHaveValue('0'); // metadata says180; raw display is independent
  await expect(page.locator('#telemetry-fields')).toContainText('2026-08-28 02:08:32.367 UTC');
  await expect(page.locator('#telemetry-fields')).toContainText('110.85');
  await pointAt(page, 20, 30);
  await expect(page.locator('#hover-reading')).toContainText('Raw pixel 20, 30');
  await expect(page.locator('#hover-reading')).toContainText('1.94 °C');
  // All palettes alter display only, never the temperature source.
  const colors = [];
  for (const name of ['White hot', 'Black hot', 'Inferno', 'Iron', 'Viridis']) {
    await page.locator('#palette').selectOption(name);
    await pointAt(page, 20, 30);
    await expect(page.locator('#hover-reading')).toContainText('1.94 °C');
    colors.push(
      await page
        .locator('#video-canvas')
        .evaluate((canvas) =>
          [...canvas.getContext('2d').getImageData(300, 200, 1, 1).data].join(','),
        ),
    );
  }
  expect(new Set(colors).size).toBe(5);
  await page.locator('#rotation').selectOption('180');
  await pointAt(page, 619, 481);
  await expect(page.locator('#hover-reading')).toContainText('Raw pixel 20, 30');
  await expect(page.locator('#hover-reading')).toContainText('1.94 °C');
  await page.locator('#rotation').selectOption('0');
  await page.locator('#video-stage').focus();
  await page.keyboard.press('ArrowRight');
  await expect(page.locator('#frame-number')).toHaveText('Frame 2 / 12');
  await pointAt(page, 20, 30);
  await expect(page.locator('#hover-reading')).toContainText('2.94 °C');
  await page.keyboard.press('ArrowLeft');
  await expect(page.locator('#frame-number')).toHaveText('Frame 1 / 12');
  await page.locator('#play').click();
  await expect(page.locator('#play')).toHaveText('Pause');
  await page.waitForFunction(() => window.fireViewer.state.number > 0);
  await page.locator('#play').click();
  await expect(page.locator('#play')).toHaveText('Play');
  await seekNumber(page, 0);
  await page.evaluate(() => {
    window.fireViewer.elevation.terrain = {
      at: () => 800,
      description: 'Deterministic test terrain',
    };
  });
  await pointAt(page, 20, 30);
  await page.locator('#mark-finding').click();
  await expect(page.locator('#finding-count')).toHaveText('1');
  await page.locator('#export-findings').click();
  const json = await downloadJSON(page, '#export-json', testInfo, 'findings.json');
  expect(json.findings).toHaveLength(1);
  expect(json.findings[0].rawPixel).toEqual({ x: 20, y: 30 });
  expect(json.findings[0].rawValue).toBe(17606);
  expect(json.findings[0].temperatureC).toBeCloseTo(1.94375, 5);
  expect(json.findings[0].recordedUtcUs).toBe(1787882912367000);
  expect(json.findings[0].calibration.sourceSha256).toMatch(/^[0-9a-f]{64}$/);
  expect(json.findings[0].ground).not.toBeNull();
  await page.locator('#export-findings').click();
  const geo = await downloadJSON(page, '#export-geojson', testInfo, 'findings.geojson');
  expect(geo.type).toBe('FeatureCollection');
  expect(geo.features).toHaveLength(1);
  expect(geo.features[0].geometry.coordinates).toEqual([
    json.findings[0].ground.lon,
    json.findings[0].ground.lat,
  ]);
  await page.locator('#scan-peaks').click();
  await expect(page.locator('#peak-list li')).not.toHaveCount(0);
  expect(
    await page.evaluate(() =>
      window.fireViewer.state.peaks.some((p) => Math.abs(p.x - 320) < 3 && Math.abs(p.y - 256) < 3),
    ),
  ).toBeTruthy();
  await screenshot(page, testInfo, 'thermal-controls-findings.png');
});

test('both video telemetry formats open locally; repeated file replacement keeps current frame', async ({
  page,
}, testInfo) => {
  await start(page);
  await openFile(page, fixture('rgb.mp4'));
  await waitIndexed(page);
  expect(await page.evaluate(() => window.fireViewer.state.frame.telemetry.schema)).toBe(
    'apcg.telemetry.v1',
  );
  await expect(page.locator('#hover-reading')).not.toContainText('150.00 °C');
  await openFile(page, fixture('misb.ts'));
  await waitIndexed(page);
  const fields = await page.evaluate(() => ({
    record: window.fireViewer.state.frame.telemetry,
    count: window.fireViewer.state.frames.length,
    kind: window.fireViewer.state.decoded.kind,
  }));
  expect(fields.count).toBeGreaterThan(1);
  expect(fields.record.position.lat_e7).toBeLessThan(0);
  expect(fields.record.hfov_deg).toBeGreaterThan(0);
  expect(fields.record.provenance.format).toBe('misb-klv');
  await page.locator('#video-file').setInputFiles(fixture('thermal.mkv'));
  await page.locator('#video-file').setInputFiles(fixture('rgb.mp4'));
  await page.locator('#video-file').setInputFiles(fixture('thermal.mkv'));
  await page.waitForFunction(
    () =>
      window.fireViewer.state.file?.name === 'thermal.mkv' &&
      window.fireViewer.state.decoded?.kind === 'gray16',
  );
  await waitIndexed(page);
  await page.evaluate(() => {
    window.fireViewer.showFrame(10);
    window.fireViewer.showFrame(3);
    return window.fireViewer.showFrame(7);
  });
  await page.waitForFunction(() => window.fireViewer.state.number === 7);
  expect(await page.evaluate(() => window.fireViewer.state.decoded.data[30 * 640 + 20])).toBe(
    17606 + 7 * 64,
  );
  await expect(page.locator('#source-name')).toHaveText('thermal.mkv');
  await screenshot(page, testInfo, 'replacement-misb-thermal.png');
});

test('BIN mission and typed fences appear in both map modes without replacing video pose', async ({
  page,
}, testInfo) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'));
  const embedded = await page.evaluate(() =>
    JSON.stringify(window.fireViewer.state.frame.telemetry),
  );
  await page.locator('#log-file').setInputFiles(fixture('mission.bin'));
  await expect(page.locator('#log-summary')).toContainText('4 mission items · 4 fences');
  const log = await page.evaluate(() => window.fireViewer.state.log);
  expect(log.mission.items.map((i) => i.frame)).toEqual([0, 3, 6, 10]);
  expect(log.fences.map((f) => [f.kind, f.exclusion])).toEqual([
    ['polygon', false],
    ['polygon', true],
    ['circle', false],
    ['circle', true],
  ]);
  expect(await page.evaluate(() => JSON.stringify(window.fireViewer.state.frame.telemetry))).toBe(
    embedded,
  );
  expect(await page.evaluate(() => window.fireViewer.map.logEntities.length)).toBeGreaterThan(6);
  await page.locator('#map-3d').click();
  await expect(page.locator('#map-3d')).toHaveAttribute('aria-pressed', 'true');
  expect(await page.evaluate(() => window.fireViewer.map.viewer.scene.mode)).toBe(3);
  await screenshot(page, testInfo, 'mission-fences-3d.png');
  await page.locator('#map-2d').click();
  expect(await page.evaluate(() => window.fireViewer.map.viewer.scene.mode)).toBe(2);
  await screenshot(page, testInfo, 'mission-fences-2d.png');
});

test('malformed file has an actionable error and a valid replacement recovers', async ({
  page,
}) => {
  await start(page);
  await page.locator('#video-file').setInputFiles(fixture('bad.mkv'));
  await expect(page.locator('#error-banner')).toBeVisible();
  await expect(page.locator('#error-text')).not.toHaveText('');
  await openFile(page, fixture('thermal.mkv'));
  await expect(page.locator('#error-banner')).toBeHidden();
});
