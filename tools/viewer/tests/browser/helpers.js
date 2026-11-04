import { test as base, expect } from '@playwright/test';
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';

export { expect };
export const fixture = (name) => fileURLToPath(new URL(`./fixtures/${name}`, import.meta.url));
export const test = base.extend({
  networkAudit: [
    async ({ page, context }, use, testInfo) => {
      const requests = [],
        errors = [];
      context.on('request', (request) =>
        requests.push({ method: request.method(), url: request.url() }),
      );
      page.on('pageerror', (error) => errors.push(error.message));
      context.on('page', (popup) => popup.on('pageerror', (error) => errors.push(error.message)));
      // Same-origin runtime config remains loaded through the production URL.
      // Offline imagery makes the tests repeatable without credentials or tiles.
      await page.route('**/viewer-config.js', (route) =>
        route.fulfill({
          contentType: 'text/javascript',
          body: "window.VIEWER_CONFIG={imagery:'none',googleMapsApiKey:'',terrainUrl:'',amslToEllipsoidMetres:0};",
        }),
      );
      await use({ requests, errors });
      const auditPath = testInfo.outputPath('network-requests.json');
      await fs.mkdir(path.dirname(auditPath), { recursive: true });
      await fs.writeFile(auditPath, JSON.stringify(requests, null, 2) + '\n');
      await testInfo.attach('network-requests', {
        path: auditPath,
        contentType: 'application/json',
      });
      expect(
        requests.filter((r) => !['GET', 'HEAD', 'OPTIONS'].includes(r.method)),
        'local recordings and logs must never become POST/PUT/PATCH uploads',
      ).toEqual([]);
      expect(errors, 'uncaught browser/worker errors').toEqual([]);
    },
    { auto: true },
  ],
});

export async function start(page) {
  await page.goto('./');
  await expect(
    page.getByRole('heading', { name: 'Video flight review', exact: true }),
  ).toBeVisible();
  await page.waitForFunction(() => window.fireViewer?.map?.viewer?.scene?.globe);
  await expect(page.locator('#map canvas')).toBeVisible();
}

export async function openFile(
  page,
  file,
  { chooser = false, kind = null, timeout = 60_000 } = {},
) {
  if (chooser) {
    const choosing = page.waitForEvent('filechooser');
    await page.getByRole('button', { name: 'Open video', exact: true }).click();
    await (await choosing).setFiles(file);
  } else await page.locator('#video-file').setInputFiles(file);
  await page.waitForFunction(
    ({ name, kind }) => {
      const s = window.fireViewer?.state;
      return s?.file?.name === name && s.frame && s.decoded && (!kind || s.decoded.kind === kind);
    },
    { name: path.basename(file), kind },
    { timeout },
  );
  await expect(page.locator('#video-canvas')).toBeVisible();
  await expect(page.locator('#play')).toBeEnabled();
}

export async function waitIndexed(page, timeout = 120_000) {
  await page.waitForFunction(() => window.fireViewer?.state.indexed, null, { timeout });
}

export async function seekNumber(page, number) {
  await page.evaluate((number) => window.fireViewer.showFrame(number), number);
  await page.waitForFunction(
    (number) => window.fireViewer.state.number === number && !window.fireViewer.state.decoding,
    number,
    { timeout: 60_000 },
  );
}

export async function pointAt(page, x, y) {
  // Analysis controls can scroll the video out of the review column's viewport.
  // Bring it back before calculating a physical mouse position over the bitmap.
  await page.locator('#video-stage').scrollIntoViewIfNeeded();
  const point = await page.locator('#video-canvas').evaluate(
    (canvas, { x, y }) => {
      const s = window.fireViewer.state,
        r = canvas.getBoundingClientRect(),
        d = s.decoded;
      const scale = Math.min(r.width / d.width, r.height / d.height);
      return {
        x: r.left + (r.width - d.width * scale) / 2 + (x + 0.2) * scale,
        y: r.top + (r.height - d.height * scale) / 2 + (y + 0.2) * scale,
      };
    },
    { x, y },
  );
  await page.mouse.move(point.x, point.y);
  return point;
}

export async function downloadJSON(page, button, testInfo, name) {
  const pending = page.waitForEvent('download');
  await page.locator(button).click();
  const download = await pending,
    dest = testInfo.outputPath(name);
  await download.saveAs(dest);
  return JSON.parse(await fs.readFile(dest, 'utf8'));
}

export async function screenshot(page, testInfo, name) {
  const dest = testInfo.outputPath(name);
  await page.bringToFront();
  await page.screenshot({ path: dest, fullPage: true });
  await testInfo.attach(name, { path: dest, contentType: 'image/png' });
  const physical = testInfo.outputPath(name.replace(/\.png$/, '-xephyr.png'));
  execFileSync('import', [
    '-display',
    process.env.VIEWER_TEST_DISPLAY || ':99',
    '-window',
    'root',
    physical,
  ]);
  await testInfo.attach(name + ' (physical Xephyr)', { path: physical, contentType: 'image/png' });
}
