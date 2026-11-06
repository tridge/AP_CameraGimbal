import fs from 'node:fs/promises';
import {
  test,
  expect,
  fixture,
  start,
  openFile,
  waitIndexed,
  downloadJSON,
  screenshot,
} from './helpers.js';

const detail = (page, label) =>
  page
    .locator('#video-info-content dt')
    .filter({ hasText: new RegExp(`^${label.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}$`) })
    .locator('xpath=following-sibling::dd[1]');

test('project icon and video information popup expose thermal metadata and export without pixel data', async ({
  page,
}, testInfo) => {
  await start(page);
  const button = page.getByRole('button', { name: 'Video information', exact: true });
  await expect(button).toBeDisabled();
  const icon = page.getByAltText('AP_CameraGimbal project icon', { exact: true });
  await expect(icon).toBeVisible();
  const artwork = await icon.evaluate(async (image) => ({
    source: await (await fetch(image.src)).text(),
    width: image.naturalWidth,
    favicon: document.querySelector('link[rel="icon"]').href,
    url: image.src,
  }));
  expect(artwork.width).toBe(512);
  expect(artwork.source).toBe(
    await fs.readFile(new URL('../../../../assets/camera-gimbal.svg', import.meta.url), 'utf8'),
  );
  expect(artwork.favicon).toBe(artwork.url);
  await openFile(page, fixture('thermal.mkv'));
  await waitIndexed(page);
  expect(
    await button.evaluate((e) => e.parentElement.contains(document.getElementById('source-name'))),
  ).toBe(true);
  await button.click();
  await expect(page.getByRole('dialog', { name: 'Video information', exact: true })).toBeVisible();
  await expect(detail(page, 'Filename')).toHaveText('thermal.mkv');
  await expect(detail(page, 'Codec')).toHaveText('FFV1');
  await expect(detail(page, 'Frame rate (average)')).toHaveText('5.000 fps');
  await expect(detail(page, 'Playback duration')).toHaveText('2.400 s');
  await expect(detail(page, 'Encoded dimensions')).toHaveText('640 × 512');
  await expect(detail(page, 'Telemetry type')).toContainText('APCG raw thermal');
  await expect(detail(page, 'Calibrated raw thermal frames')).toHaveText('12 / 12 frames');
  await page.getByText('All available metadata (JSON)', { exact: true }).click();
  await expect(page.locator('#video-info-json')).toContainText('source_sha256');
  await screenshot(page, testInfo, 'video-information-thermal.png');
  const exported = await downloadJSON(page, '#download-video-info', testInfo, 'video-info.json');
  expect(exported.file.name).toBe('thermal.mkv');
  expect(exported.statistics.averageFrameRate).toBeCloseTo(5);
  expect(exported.currentFrame.thermalMetadata.pixel_format).toBe('gray16le');
  expect(exported.tracks[0].codecPrivate.byteLength).toBeGreaterThan(0);
  expect(exported.decoded).not.toHaveProperty('data');
  await page.keyboard.press('Escape');
  await expect(page.locator('#video-info-dialog')).not.toBeVisible();
  await expect(button).toBeFocused();
});

test('video information replaces old metadata and safely displays long filenames at mobile width', async ({
  page,
}, testInfo) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await start(page);
  const name = '<img src=x onerror=alert(1)>-a-very-long-local-recording-name.mp4';
  await page
    .locator('#video-file')
    .setInputFiles({ name, mimeType: 'video/mp4', buffer: await fs.readFile(fixture('rgb.mp4')) });
  await page.waitForFunction(
    () => window.fireViewer.state.decoded?.kind === 'rgba' && window.fireViewer.state.indexed,
  );
  await page.getByRole('button', { name: 'Video information', exact: true }).click();
  await expect(detail(page, 'Filename')).toHaveText(name);
  await expect(detail(page, 'Codec')).toHaveText('H.264 / AVC');
  await expect(detail(page, 'Frame rate (average)')).toHaveText('10.000 fps');
  await expect(detail(page, 'Telemetry type')).toContainText('APCG JSON SEI');
  await expect(page.locator('#video-info-content img')).toHaveCount(0);
  expect(
    await page.evaluate(
      () =>
        document.documentElement.scrollWidth <= innerWidth &&
        document.querySelector('#video-info-dialog').scrollWidth <=
          document.querySelector('#video-info-dialog').clientWidth,
    ),
  ).toBe(true);
  await screenshot(page, testInfo, 'video-information-mobile.png');
  await page.getByRole('button', { name: 'Close video information', exact: true }).click();
  await openFile(page, fixture('misb.ts'));
  await waitIndexed(page);
  await page.getByRole('button', { name: 'Video information', exact: true }).click();
  await expect(detail(page, 'Filename')).toHaveText('misb.ts');
  await expect(detail(page, 'Telemetry type')).toContainText('MISB ST0601 KLV');
  await expect(detail(page, 'Calibrated raw thermal frames')).toHaveText('0 / 20 frames');
});
