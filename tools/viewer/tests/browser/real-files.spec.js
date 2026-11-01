import fs from 'node:fs/promises';
import path from 'node:path';
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

const thermal = process.env.VIEWER_REAL_THERMAL;
const rgb = process.env.VIEWER_REAL_RGB;
const bin = process.env.VIEWER_REAL_BIN;

test('real large thermal archive remains local and preserves first, middle and last raw frames', async ({
  page,
}, testInfo) => {
  test.skip(!thermal, 'Set VIEWER_REAL_THERMAL to opt into the original local recording.');
  test.setTimeout(240_000);
  const reference = JSON.parse(await fs.readFile(fixture('real-reference.json'), 'utf8'));
  await start(page);
  const began = Date.now();
  await openFile(page, thermal, { chooser: true, kind: 'gray16', timeout: 90_000 });
  const firstVisibleMs = Date.now() - began;
  console.log('Real thermal first visible:', firstVisibleMs, 'ms');
  await waitIndexed(page, 120_000);
  const measurements = await page.evaluate(() => ({
    frames: window.fireViewer.state.frames.length,
    sourceBytes: window.fireViewer.state.file.size,
    usedJSHeapSize: performance.memory?.usedJSHeapSize ?? null,
    totalJSHeapSize: performance.memory?.totalJSHeapSize ?? null,
    jsHeapSizeLimit: performance.memory?.jsHeapSizeLimit ?? null,
    lastPacketOffset: window.fireViewer.state.frames.at(-1).offset,
    userAgent: navigator.userAgent,
  }));
  measurements.firstVisibleMs = firstVisibleMs;
  measurements.indexCompleteMs = Date.now() - began;
  console.log('Real thermal indexed:', JSON.stringify(measurements));
  measurements.scope =
    'Empirical Chrome main-page JS heap, excludes workers, decoder WASM, GPU and browser process memory. Not a cross-browser memory guarantee.';
  expect(measurements.frames).toBe(reference.count);
  expect(measurements.sourceBytes).toBe(reference.source.size);
  // This specific recording proves offsets beyond the signed 32-bit boundary.
  if (reference.source.size > 2 ** 31)
    expect(measurements.lastPacketOffset).toBeGreaterThan(2 ** 31);
  const measurementsPath = testInfo.outputPath('large-archive-measurements.json');
  await fs.mkdir(path.dirname(measurementsPath), { recursive: true });
  await fs.writeFile(measurementsPath, JSON.stringify(measurements, null, 2) + '\n');
  await testInfo.attach('large-archive-measurements', {
    path: measurementsPath,
    contentType: 'application/json',
  });
  for (const expected of reference.frames) {
    await seekNumber(page, expected.number);
    const actual = await page.evaluate(async () => {
      const s = window.fireViewer.state,
        d = s.decoded;
      const digest = await crypto.subtle.digest(
        'SHA-256',
        new Uint8Array(d.data.buffer, d.data.byteOffset, d.data.byteLength),
      );
      return {
        hash: Array.from(new Uint8Array(digest), (n) => n.toString(16).padStart(2, '0')).join(''),
        sample: d.data[30 * d.width + 20],
        width: d.width,
        height: d.height,
        recordedHash: s.frame.thermalMetadata.source_sha256,
        scale: s.frame.thermalMetadata.temperature_scale_k,
        offset: s.frame.thermalMetadata.temperature_offset_k,
        utc: s.frame.telemetry?.utc_us,
        pts: s.frame.pts,
      };
    });
    console.log('Real thermal raw frame verified:', expected.number);
    expect(actual.hash).toBe(expected.raw_sha256);
    expect(actual.recordedHash).toBe(expected.raw_sha256);
    expect(actual.sample).toBe(expected.sample.value);
    expect(actual.pts).toBeCloseTo(expected.pts, 6);
    await pointAt(page, 20, 30);
    await expect(page.locator('#hover-reading')).toContainText('Raw pixel 20, 30');
    await expect(page.locator('#hover-reading')).toContainText(
      `${(actual.sample * actual.scale + actual.offset - 273.15).toFixed(2)} °C`,
    );
    await expect(page.locator('#telemetry-fields')).toContainText('UTC');
    await screenshot(page, testInfo, `real-thermal-frame-${expected.number}.png`);
  }
  // Seek with the actual range control as well as checking exact frame hashes.
  const midpoint = reference.frames[1];
  await page.locator('#seek').evaluate((el, pts) => {
    el.value = String(pts);
    el.dispatchEvent(new Event('input', { bubbles: true }));
  }, midpoint.pts);
  await page.waitForFunction(
    (number) => Math.abs(window.fireViewer.state.number - number) < 2,
    midpoint.number,
  );
  await page.locator('#play').click();
  await expect(page.locator('#play')).toHaveText('Pause');
  const middle = await page.evaluate(() => window.fireViewer.state.number);
  await page.waitForFunction((number) => window.fireViewer.state.number > number, middle);
  await page.locator('#play').click();
  await pointAt(page, 20, 30);
  await page.locator('#scan-peaks').click();
  await expect(page.locator('#peak-list li')).not.toHaveCount(0);
  await page.evaluate(() => {
    window.fireViewer.elevation.terrain = {
      at: () => 820,
      description: 'Deterministic test terrain',
    };
  });
  await pointAt(page, 20, 30);
  await page.locator('#mark-finding').click();
  await page.locator('#export-findings').click();
  const marked = await downloadJSON(page, '#export-json', testInfo, 'real-findings.json');
  expect(marked.findings).toHaveLength(1);
  expect(marked.findings[0].rawValue).toBeGreaterThan(0);
  expect(marked.findings[0].recordedUtcUs).toBeGreaterThan(0);
  await page.locator('#export-findings').click();
  const geo = await downloadJSON(page, '#export-geojson', testInfo, 'real-findings.geojson');
  expect(geo.type).toBe('FeatureCollection');
});

test('real RGB and flight BIN show local telemetry, complete mission and fences in 2D and 3D', async ({
  page,
}, testInfo) => {
  test.skip(!rgb || !bin, 'Set VIEWER_REAL_RGB and VIEWER_REAL_BIN to opt into flight files.');
  test.setTimeout(180_000);
  await start(page);
  await openFile(page, rgb, { chooser: true, timeout: 90_000 });
  await waitIndexed(page);
  const count = await page.evaluate(() => window.fireViewer.state.frames.length);
  expect(count).toBeGreaterThan(100);
  for (const number of [0, Math.floor(count / 2), count - 1]) {
    await seekNumber(page, number);
    expect(await page.evaluate(() => window.fireViewer.state.decoded.kind)).toBe('rgba');
    await expect(page.locator('#telemetry-fields')).toContainText('UTC');
  }
  await seekNumber(page, Math.floor(count / 2));
  const recorded = await page.evaluate(() =>
    JSON.stringify(window.fireViewer.state.frame.telemetry),
  );
  await page.locator('#log-file').setInputFiles(bin);
  await expect(page.locator('#log-summary')).toContainText('81 mission items · 1 fences', {
    timeout: 90_000,
  });
  expect(await page.evaluate(() => JSON.stringify(window.fireViewer.state.frame.telemetry))).toBe(
    recorded,
  );
  const counts = await page.evaluate(() => ({
    mission: window.fireViewer.state.log.mission.items.length,
    fences: window.fireViewer.state.log.fences.length,
    fenceVertices: window.fireViewer.state.log.fences[0].points.length,
    mapEntities: window.fireViewer.map.logEntities.length,
  }));
  expect(counts.mission).toBe(81);
  expect(counts.fences).toBe(1);
  expect(counts.fenceVertices).toBe(26);
  expect(counts.mapEntities).toBeGreaterThan(1);
  await page.locator('#map-3d').click();
  expect(await page.evaluate(() => window.fireViewer.map.viewer.scene.mode)).toBe(3);
  await screenshot(page, testInfo, 'real-rgb-mission-fences-3d.png');
  await page.locator('#map-2d').click();
  expect(await page.evaluate(() => window.fireViewer.map.viewer.scene.mode)).toBe(2);
  await screenshot(page, testInfo, 'real-rgb-mission-fences-2d.png');
});
