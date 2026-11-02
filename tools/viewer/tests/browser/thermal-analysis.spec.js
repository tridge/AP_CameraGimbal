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
import fs from 'node:fs/promises';

async function openAnalysis(page, file = fixture('thermal.mkv')) {
  await start(page);
  await openFile(page, file, { kind: 'gray16' });
  await waitIndexed(page);
  await page.locator('#thermal-analysis > summary').click();
  await expect(page.locator('#analysis-statistics')).toContainText('327,680 samples');
  expect(
    await page.locator('#thermal-analysis').evaluate((el) => el.getBoundingClientRect().height),
  ).toBeGreaterThan(400);
}
async function apply(page, method) {
  await page.locator('#analysis-method').selectOption(method);
  await page.locator('#analysis-apply').click();
  await page.waitForFunction(
    (method) => {
      const a = window.fireViewer.thermalAnalysis;
      return !a.busy && a.result?.settings.method === method && (method === 'raw' || a.plane);
    },
    method,
    { timeout: 45_000 },
  );
}

test('histograms and every method preserve raw radiometry, coordinates and reproducible findings', async ({
  page,
}, testInfo) => {
  test.setTimeout(180_000);
  await openAnalysis(page);
  await page.evaluate(() => {
    window.fireViewer.elevation.terrain = { at: () => 800, description: 'Test terrain' };
  });
  const original = await page.evaluate(() => Array.from(window.fireViewer.state.decoded.data));
  for (const method of [
    'background',
    'dog',
    'tophat',
    'cfar',
    'matched',
    'multiscale',
    'clahe',
    'rl',
    'wiener',
  ]) {
    await apply(page, method);
    expect(
      await page.evaluate(
        (original) => window.fireViewer.state.decoded.data.every((v, i) => v === original[i]),
        original,
      ),
    ).toBe(true);
    await expect(page.locator('#stretch')).toBeDisabled();
    await pointAt(page, 20, 30);
    await expect(page.locator('#hover-reading')).toContainText('Raw pixel 20, 30');
    await expect(page.locator('#hover-reading')).toContainText('1.94 °C');
    await page.locator('#analysis-histogram').selectOption('processed');
    await expect(page.locator('#analysis-statistics')).not.toContainText('°C');
    await page.locator('#analysis-view').selectOption('raw');
    await expect(page.locator('#stretch')).toBeEnabled();
    await page.locator('#analysis-view').selectOption('processed');
  }
  await page.locator('#analysis-peaks').click();
  await expect(page.locator('#peak-list li')).not.toHaveCount(0);
  expect(
    await page.evaluate(() =>
      window.fireViewer.state.peaks.some((p) => Math.hypot(p.x - 320, p.y - 256) < 3),
    ),
  ).toBe(true);
  await pointAt(page, 20, 30);
  await page.locator('#mark-finding').click();
  await expect(page.locator('#finding-count')).toHaveText('1');
  await page.locator('#export-findings').click();
  const json = await downloadJSON(page, '#export-json', testInfo, 'analysis-findings.json');
  const f = json.findings[0];
  expect(f.rawValue).toBe(17606);
  expect(f.temperatureC).toBeCloseTo(1.94375, 5);
  expect(f.rawPixel).toEqual({ x: 20, y: 30 });
  expect(f.ground).not.toBeNull();
  expect(f.analysis.method).toBe('wiener');
  expect(f.analysis.candidateMethod).toBe('matched');
  expect(f.analysis.inputSha256).toBe(f.calibration.sourceSha256);
  expect(f.analysis.temperatureFromOriginalOnly).toBe(true);
  await page.locator('#analysis-histogram-canvas').scrollIntoViewIfNeeded();
  await screenshot(page, testInfo, 'analysis-wiener-histogram.png');
});

test('raw-coordinate ROI, rotation, histogram CSV and raw/result comparison', async ({
  page,
}, testInfo) => {
  await openAnalysis(page);
  await page.locator('#rotation').selectOption('180');
  await pointAt(page, 319, 255); // Raw320,256 after rotation.
  await page.locator('.analysis-region > summary').click();
  await page.locator('#analysis-cursor').click();
  await expect(page.locator('#analysis-statistics')).toContainText('9,216 samples');
  expect(await page.evaluate(() => window.fireViewer.thermalAnalysis.roi)).toEqual({
    x: 272,
    y: 208,
    width: 96,
    height: 96,
  });
  await apply(page, 'matched');
  const canvasPixels = () => page.locator('#video-canvas').evaluate((c) => c.toDataURL());
  const withOutline = await canvasPixels();
  await page.locator('#thermal-analysis > summary').click();
  await expect.poll(canvasPixels).not.toBe(withOutline);
  await page.locator('#thermal-analysis > summary').click();
  await expect.poll(canvasPixels).toBe(withOutline);
  const download = page.waitForEvent('download');
  await page.locator('#analysis-export').click();
  const path = testInfo.outputPath('histogram.csv');
  await (await download).saveAs(path);
  const lines = (await fs.readFile(path, 'utf8')).trim().split('\n'),
    meta = JSON.parse(lines[0].slice(2));
  expect(meta.source).toBe('raw');
  expect(meta.roi.width).toBe(96);
  expect(lines.slice(2).reduce((s, line) => s + Number(line.split(',')[2]), 0)).toBe(96 * 96);
  await page.locator('#analysis-log').check();
  await page.locator('#analysis-histogram').selectOption('processed');
  await expect(page.locator('#analysis-statistics')).not.toContainText('°C');
  await screenshot(page, testInfo, 'analysis-roi-log-histogram.png');
});

test('seeking, cancellation, playback and RGB source replacement discard processed results', async ({
  page,
}) => {
  await openAnalysis(page);
  await page.locator('#analysis-method').selectOption('rl');
  await page.locator('#analysis-iterations').fill('25');
  await page.locator('#analysis-apply').click();
  await seekNumber(page, 3);
  await page.waitForFunction(() => !window.fireViewer.thermalAnalysis.busy);
  expect(await page.evaluate(() => window.fireViewer.thermalAnalysis.plane)).toBeNull();
  expect(await page.evaluate(() => window.fireViewer.thermalAnalysis.result?.inputSha256)).toBe(
    await page.evaluate(() => window.fireViewer.state.frame.thermalMetadata.source_sha256),
  );
  await page.locator('#analysis-apply').click();
  await page.locator('#analysis-cancel').click();
  expect(await page.evaluate(() => window.fireViewer.thermalAnalysis.plane)).toBeNull();
  await apply(page, 'matched');
  await page.locator('#play').click();
  await page.waitForFunction(() => window.fireViewer.state.number > 3);
  expect(await page.evaluate(() => window.fireViewer.thermalAnalysis.plane)).toBeNull();
  await page.locator('#play').click();
  await openFile(page, fixture('rgb.mp4'));
  await expect(page.locator('#analysis-apply')).toBeDisabled();
  await expect(page.locator('#analysis-availability')).toContainText('no detector samples');
  expect(await page.evaluate(() => window.fireViewer.thermalAnalysis.result)).toBeNull();
});

test('uncalibrated raw images retain sample-only analysis and a narrow viewport fits the controls', async ({
  page,
}, testInfo) => {
  await openAnalysis(page, fixture('uncalibrated.mkv'));
  await expect(page.locator('#analysis-statistics')).not.toContainText('°C');
  await apply(page, 'cfar');
  await expect(page.locator('#stretch')).toBeDisabled();
  await page.setViewportSize({ width: 390, height: 844 });
  await page.locator('#thermal-analysis').scrollIntoViewIfNeeded();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await screenshot(page, testInfo, 'analysis-mobile.png');
});

test('real flight thermal frame supports both restorations with unchanged raw samples', async ({
  page,
}, testInfo) => {
  test.skip(!process.env.VIEWER_REAL_THERMAL, 'Provide a local real thermal recording.');
  test.setTimeout(180_000);
  await start(page);
  await openFile(page, process.env.VIEWER_REAL_THERMAL, { kind: 'gray16', timeout: 120_000 });
  await page.locator('#thermal-analysis > summary').click();
  await page.waitForFunction(() => window.fireViewer.thermalAnalysis.result?.rawStats);
  const bytes = await page.evaluate(() => Array.from(window.fireViewer.state.decoded.data));
  for (const method of ['matched', 'multiscale', 'rl', 'wiener']) {
    await apply(page, method);
    expect(
      await page.evaluate(
        (before) => window.fireViewer.state.decoded.data.every((v, i) => v === before[i]),
        bytes,
      ),
    ).toBe(true);
    await screenshot(page, testInfo, `real-thermal-${method}.png`);
  }
  await waitIndexed(page);
  const reference = JSON.parse(await fs.readFile(fixture('real-reference.json'), 'utf8')).frames[1];
  await seekNumber(page, reference.number);
  await apply(page, 'matched');
  expect(await page.evaluate(() => window.fireViewer.thermalAnalysis.result.inputSha256)).toBe(
    reference.raw_sha256,
  );
  await screenshot(page, testInfo, 'real-thermal-midflight-matched.png');
});

test('analysis adds no duplicate image renders during ordinary playback and waits for decode', async ({
  page,
}) => {
  await start(page);
  await openFile(page, fixture('thermal.mkv'), { kind: 'gray16' });
  await waitIndexed(page);
  await page.evaluate(() => {
    const a = window.fireViewer.thermalAnalysis,
      change = a.onChange;
    window.analysisRepaints = 0;
    a.onChange = () => {
      window.analysisRepaints++;
      change();
    };
  });
  await page.locator('#play').click();
  await expect(page.locator('#analysis-apply')).toBeDisabled();
  await page.waitForFunction(() => window.fireViewer.state.number > 2);
  expect(await page.evaluate(() => window.analysisRepaints)).toBe(0);
  await page.locator('#play').click();
  await page.evaluate(() => {
    const d = window.fireViewer.state.decoder,
      original = d.decode.bind(d);
    window.restoreDecode = () => {
      d.decode = original;
    };
    d.decode = async (...args) => {
      await new Promise((resolve) => {
        window.releaseDecode = resolve;
      });
      return original(...args);
    };
    window.pendingAnalysisSeek = window.fireViewer.showFrame(7);
  });
  await expect(page.locator('#analysis-apply')).toBeDisabled();
  await page.evaluate(async () => {
    window.restoreDecode();
    window.releaseDecode();
    await window.pendingAnalysisSeek;
  });
  await expect(page.locator('#analysis-apply')).toBeEnabled();
  expect(await page.evaluate(() => window.fireViewer.state.number)).toBe(7);
});
