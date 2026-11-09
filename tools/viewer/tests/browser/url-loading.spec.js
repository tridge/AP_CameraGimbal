import http from 'node:http';
import fs from 'node:fs';
import { test, expect, fixture } from './helpers.js';

// A second origin serving fixtures; /nocors/ omits Access-Control-Allow-Origin, /stall/
// sends part of the body and then waits, /slow/ takes about two seconds.
let server, origin;
const stalled = new Set();
test.beforeAll(async () => {
  server = http.createServer((req, res) => {
    const [, mode, name] = req.url.split('/');
    const cors = mode === 'nocors' ? {} : { 'access-control-allow-origin': '*' };
    let path;
    try {
      path = fixture(name);
      fs.accessSync(path);
    } catch {
      res.writeHead(404, cors).end('not found');
      return;
    }
    const size = fs.statSync(path).size;
    res.writeHead(200, {
      ...cors,
      'content-type': 'application/octet-stream',
      'content-length': String(size),
    });
    if (mode === 'stall') {
      res.write(fs.readFileSync(path).subarray(0, 1024));
      stalled.add(res);
    } else if (mode === 'slow') {
      const data = fs.readFileSync(path),
        step = Math.ceil(data.length / 10);
      let at = 0;
      const timer = setInterval(() => {
        if (res.destroyed) return clearInterval(timer);
        res.write(data.subarray(at, (at += step)));
        if (at >= data.length) (clearInterval(timer), res.end());
      }, 200);
    } else fs.createReadStream(path).pipe(res);
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  origin = `http://127.0.0.1:${server.address().port}`;
});
test.afterAll(async () => {
  for (const res of stalled) res.destroy();
  server.closeAllConnections();
  await new Promise((resolve) => server.close(resolve));
});

// The shipped CSP connect-src does not list this test origin; the CSP report is checked last.
test.use({ bypassCSP: true });

const downloads = (page) =>
  page.evaluate(async () => {
    const root = await navigator.storage.getDirectory();
    const dir = await root.getDirectoryHandle('url-downloads', { create: true });
    const names = [];
    for await (const name of dir.keys()) names.push(name);
    return names.sort();
  });
const open = (page, query) =>
  page.goto(`./?${new URLSearchParams(query).toString()}`, { waitUntil: 'domcontentloaded' });
const opened = (page, name) =>
  page.waitForFunction(
    (name) => {
      const s = window.fireViewer?.state;
      return s?.file?.name === name && s.frame && s.decoded && !s.decoding;
    },
    name,
    { timeout: 60_000 },
  );

test('?url= and ?log= download from a CORS origin into OPFS and open as local files', async ({
  page,
}) => {
  await open(page, { url: `${origin}/cors/thermal.mkv`, log: `${origin}/cors/mission.bin` });
  await page.waitForFunction(
    () => {
      const s = window.fireViewer?.state;
      return s?.file?.name === 'thermal.mkv' && s.frame && s.decoded?.kind === 'gray16';
    },
    null,
    { timeout: 60_000 },
  );
  await page.waitForFunction(() => window.fireViewer.state.indexed, null, { timeout: 60_000 });
  expect(await page.evaluate(() => window.fireViewer.state.frames.length)).toBe(12);
  await expect(page.locator('#source-name')).toHaveText('thermal.mkv');
  await expect(page.locator('#telemetry-fields')).toContainText('2026-08-28 02:08:32.367 UTC');
  await expect(page.locator('#log-summary')).toContainText('4 mission items · 4 fences');
  await expect(page.locator('#downloads .download')).toHaveCount(0);
  await expect(page.locator('#error-banner')).toBeHidden();
  expect(await downloads(page)).toHaveLength(2);
  // a reload releases the old copies; the next download prunes them
  await open(page, { url: `${origin}/cors/rgb.mp4` });
  await page.waitForFunction(() => window.fireViewer?.state?.decoded, null, { timeout: 60_000 });
  expect(await downloads(page)).toHaveLength(1);
});

test('?url= errors explain HTTP status, missing CORS and unsupported types', async ({ page }) => {
  for (const [query, message] of [
    [`${origin}/cors/missing.mkv`, 'HTTP 404'],
    [`${origin}/nocors/thermal.mkv`, 'Access-Control-Allow-Origin'],
    [`${origin}/cors/thermal.avi`, 'not a supported video'],
    ['file:///etc/passwd', 'Only http and https'],
  ]) {
    await open(page, { url: query });
    await expect(page.locator('#error-banner')).toBeVisible();
    await expect(page.locator('#error-text')).toContainText(message);
    await expect(page.locator('#downloads .download')).toHaveCount(0);
    await expect(page.locator('#empty-video')).toBeVisible();
  }
});

test('a ?url= download shows progress and can be cancelled', async ({ page }) => {
  await open(page, { url: `${origin}/stall/thermal.mkv` });
  const row = page.locator('#downloads .download');
  await expect(row).toContainText('Downloading thermal.mkv');
  await expect(row).toContainText(/\d+% · 0\.0 \/ \d+\.\d MB/);
  await row.getByRole('button', { name: 'Cancel' }).click();
  await expect(row).toHaveCount(0);
  await expect(page.locator('#status')).toHaveText('Download of thermal.mkv cancelled');
  await expect(page.locator('#error-banner')).toBeHidden();
  expect(await downloads(page)).toHaveLength(0);
});

test('a log failure stays visible after a slower video opens, and vice versa', async ({ page }) => {
  await open(page, { url: `${origin}/slow/thermal.mkv`, log: `${origin}/cors/missing.bin` });
  await expect(page.locator('#error-text')).toContainText('missing.bin: HTTP 404');
  await opened(page, 'thermal.mkv');
  await expect(page.locator('#error-banner')).toBeVisible();
  await expect(page.locator('#error-text')).toContainText('missing.bin: HTTP 404');
  await open(page, { url: `${origin}/cors/missing.mkv`, log: `${origin}/slow/mission.bin` });
  await expect(page.locator('#error-text')).toContainText('missing.mkv: HTTP 404');
  await expect(page.locator('#log-summary')).toContainText('mission.bin ·');
  await expect(page.locator('#error-text')).toContainText('missing.mkv: HTTP 404');
});

test('a superseded download never reports over its replacement', async ({ page }) => {
  await open(page, { url: `${origin}/stall/thermal.mkv` });
  await expect(page.locator('#downloads .download')).toContainText(/\d+%/);
  // replaced by a local file
  await page.locator('#video-file').setInputFiles(fixture('rgb.mp4'));
  await opened(page, 'rgb.mp4');
  await expect(page.locator('#downloads .download')).toHaveCount(0);
  await expect(page.locator('#status')).not.toContainText('cancelled');
  await expect(page.locator('#error-banner')).toBeHidden();
  // replaced by another URL
  await page.evaluate(
    (u) => void window.fireViewer.openUrl('video', u),
    `${origin}/stall/thermal.mkv`,
  );
  await expect(page.locator('#downloads .download')).toContainText(/\d+%/);
  await page.evaluate(
    (u) => void window.fireViewer.openUrl('video', u),
    `${origin}/slow/thermal.mkv`,
  );
  await opened(page, 'thermal.mkv');
  await expect(page.locator('#downloads .download')).toHaveCount(0);
  await expect(page.locator('#status')).not.toContainText('cancelled');
  await expect(page.locator('#error-banner')).toBeHidden();
  expect(await downloads(page)).toHaveLength(1);
});

test('reloading during a download leaves nothing behind after the next download', async ({
  page,
}) => {
  await open(page, { url: `${origin}/stall/thermal.mkv` });
  await expect(page.locator('#downloads .download')).toContainText(/\d+%/);
  const [partial] = await downloads(page);
  await open(page, { url: `${origin}/cors/rgb.mp4` });
  await opened(page, 'rgb.mp4');
  const left = await downloads(page);
  expect(left).toHaveLength(1);
  expect(left).not.toContain(partial);
});

test('another tab cannot delete a file this tab is reading', async ({ page, context }) => {
  await open(page, { url: `${origin}/cors/thermal.mkv` });
  await opened(page, 'thermal.mkv');
  const [mine] = await downloads(page);
  const other = await context.newPage();
  await open(other, { url: `${origin}/cors/rgb.mp4` });
  await opened(other, 'rgb.mp4');
  expect(await downloads(other)).toContain(mine);
  await page.evaluate(() => window.fireViewer.showFrame(7));
  await page.waitForFunction(() => {
    const s = window.fireViewer.state;
    return s.number === 7 && s.decoded && !s.decoding;
  });
  await expect(page.locator('#error-banner')).toBeHidden();
  await page.close();
  await expect
    .poll(() =>
      other.evaluate(async (mine) => {
        const { held } = await navigator.locks.query();
        return held.some((lock) => lock.name.endsWith(mine));
      }, mine),
    )
    .toBe(false);
  await other.evaluate(
    (u) => void window.fireViewer.openUrl('log', u),
    `${origin}/cors/mission.bin`,
  );
  await expect(other.locator('#log-summary')).toContainText('4 mission items');
  const left = await downloads(other);
  expect(left).toHaveLength(2);
  expect(left).not.toContain(mine);
});

test('a failed handover reports the error and keeps the previous copy owned', async ({ page }) => {
  // window.failWorker makes matching workers throw, so openVideo/loadLog fail mid-handover
  await page.addInitScript(() => {
    const Native = window.Worker;
    window.Worker = class extends Native {
      constructor(url, options) {
        if (window.failWorker && String(url).includes(window.failWorker))
          throw new Error(`test blocked ${window.failWorker}`);
        super(url, options);
      }
    };
  });
  const held = () =>
    page.evaluate(async () => (await navigator.locks.query()).held.map((lock) => lock.name).sort());
  await open(page, { url: `${origin}/cors/thermal.mkv`, log: `${origin}/cors/mission.bin` });
  await opened(page, 'thermal.mkv');
  await expect(page.locator('#log-summary')).toContainText('4 mission items');
  await expect.poll(held).toHaveLength(2);
  const before = await held();
  for (const [kind, file, worker] of [
    ['video', 'rgb.mp4', 'index.worker'],
    ['log', 'mission.bin', 'log.worker'],
  ]) {
    await page.evaluate(
      ([kind, url, worker]) => {
        window.failWorker = worker;
        void window.fireViewer.openUrl(kind, url);
      },
      [kind, `${origin}/cors/${file}`, worker],
    );
    await expect(page.locator('#error-text')).toContainText(`test blocked ${worker}`);
    await expect(page.locator('#downloads .download')).toHaveCount(0);
    expect(await held()).toEqual(before);
  }
  // the failed copies were released, so the next download prunes them
  await page.evaluate((url) => {
    window.failWorker = null;
    void window.fireViewer.openUrl('log', url);
  }, `${origin}/cors/mission.bin`);
  await expect.poll(held).not.toEqual(before);
  await expect.poll(async () => (await downloads(page)).length).toBe(3);
});

test.describe('with the shipped CSP', () => {
  test.use({ bypassCSP: false });
  test('a connect-src block is reported as such', async ({ page }) => {
    await open(page, { url: `${origin}/cors/thermal.mkv` });
    await expect(page.locator('#error-text')).toContainText(
      'Content-Security-Policy (connect-src)',
    );
  });
});
