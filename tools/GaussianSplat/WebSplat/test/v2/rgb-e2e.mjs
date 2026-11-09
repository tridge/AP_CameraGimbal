// End to end: an RGB recording opened in the built video viewer, Create splat (range, default
// target, width, optional captured view), Go, and the WebSplat job run to its result view.
// usage: node rgb-e2e.mjs --source small|full --width 1280 --start 3000 --end 6600 [--view]
//        [--iterations N] [--port 8820] [--name RUN] [--adapter low-power]
// The viewer build is served at /video/ (WEBSPLAT_VIEWER_DIST, else tools/viewer/dist; built with
// VIEWER_BASE_PATH=/video/) and WebSplat's at /websplat/ (WEBSPLAT_APP_DIST, else app/dist).
// Recordings: WEBSPLAT_RGB_SMALL / WEBSPLAT_RGB_FULL. Writes $WEBSPLAT_DATA/v2-rgb-e2e/RUN/
// {report.json, result.png, viewer.png}.
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { args, env } from '../harness/lib/env.mjs';
import { capture, launchLocal } from '../harness/lib/browser.mjs';
import { serveV2, VIEWER_DIST } from './serve.mjs';

const a = args();
const data = env('WEBSPLAT_DATA');
const source = a.source ?? 'small';
const file = env(source === 'full' ? 'WEBSPLAT_RGB_FULL' : 'WEBSPLAT_RGB_SMALL');
const width = +(a.width ?? 640);
const [start, end] = [+(a.start ?? 3000), +(a.end ?? 6600)];
const port = +(a.port ?? 8820);
const name = a.name ?? `${source}-${width}`;
const out = `${data}/v2-rgb-e2e/${name}`;
mkdirSync(out, { recursive: true });
const site = `http://127.0.0.1:${port}`;
const t0 = Date.now();
const say = (s) =>
  console.log(
    `${new Date().toISOString().slice(11, 19)} +${((Date.now() - t0) / 1000).toFixed(0)}s ${s}`,
  );
const report = { name, source, width, range: [start, end], view: !!a.view, events: [] };
const save = () => writeFileSync(`${out}/report.json`, JSON.stringify(report, null, 1));

const server = await serveV2({
  port,
  video: process.env.WEBSPLAT_VIEWER_DIST ? resolve(process.env.WEBSPLAT_VIEWER_DIST) : VIEWER_DIST,
});
const browser = await launchLocal({
  profile: `${data}/harness/profiles/v2-rgb-e2e-${name}`,
  adapter: a.adapter ?? 'low-power',
});
const ctx = browser.context;
const log = [];
// no imagery (tiles are not needed to pick the range); the shared terrain for the look points
await ctx.route('**/video/viewer-config.js', (route) =>
  route.fulfill({
    contentType: 'text/javascript',
    body: `window.VIEWER_CONFIG=${JSON.stringify({
      imagery: 'none',
      terrainUrl: 'https://plot.ardupilot.org/quantized/',
      terrainDatum: 'amsl',
      amslToEllipsoidMetres: 0,
      websplatUrl: '../websplat/',
    })};`,
  }),
);
try {
  const viewer = await ctx.newPage();
  await viewer.setViewportSize({ width: 1500, height: 1000 });
  capture(viewer, log, 'viewer');
  await viewer.goto(`${site}/video/`);
  await viewer.waitForFunction(() => window.fireViewer?.map?.viewer?.scene?.globe, null, {
    timeout: 60000,
  });
  await viewer.locator('#video-file').setInputFiles(file);
  await viewer.waitForFunction(() => window.fireViewer?.state.indexed, null, { timeout: 600000 });
  say('indexed in the viewer');
  await viewer.locator('#splat-create').click();
  await viewer.waitForSelector('#splat-panel', { state: 'visible' });
  const seek = async (n) => {
    await viewer.evaluate((n) => window.fireViewer.showFrame(n), n);
    await viewer.waitForFunction(
      (n) => window.fireViewer.state.number === n && !window.fireViewer.state.decoding,
      n,
      { timeout: 120000 },
    );
  };
  await seek(start);
  await viewer.locator('#splat-set-start').click();
  await seek(end);
  await viewer.locator('#splat-set-end').click();
  // the default target: the median of the in-range look points on the terrain
  await viewer.waitForFunction(() => window.fireViewer.splatPanel?.target?.alt_amsl != null, null, {
    timeout: 120000,
  });
  await viewer.waitForTimeout(3000); // look points settle once terrain tiles load
  await viewer.selectOption('#splat-rgb-width', String(width));
  const target = await viewer.evaluate(() => window.fireViewer.splatPanel.target);
  report.target = target;
  say(`target ${JSON.stringify(target)}`);
  if (a.view) {
    // an oblique view from the south-east, 540 m from the target
    await viewer.evaluate((t) => {
      const m = window.fireViewer.map;
      const cam = m.viewer.camera;
      const C3 = cam.position.constructor;
      const centre = C3.fromDegrees(t.lon, t.lat, t.alt_amsl + (m.offset ?? 0));
      cam.lookAt(centre, new C3(300, -300, 330));
      cam.lookAtTransform(cam.transform.constructor.IDENTITY);
    }, target);
    await viewer.locator('#splat-use-view').click();
    report.viewCaptured = await viewer.locator('#splat-view').textContent();
  }
  await viewer.locator('#splat-title').fill(`RGB orbit ${source} ${width}`);
  await viewer.screenshot({ path: `${out}/viewer.png` });
  const popup = ctx.waitForEvent('page');
  await viewer.locator('#splat-go').click();
  const page = await popup;
  capture(page, log, 'websplat');
  await page.setViewportSize({ width: 1280, height: 1000 });
  await page.waitForFunction(
    () => ['accepted', 'failed', 'rejected'].includes(window.websplat?.request?.state),
    null,
    { timeout: 900000 },
  );
  report.request = await page.evaluate(() => window.websplat.request);
  report.url = page.url().replace(site, '');
  say(`handover ${JSON.stringify(report.request)}`);
  if (report.request.state !== 'accepted') throw new Error('handover failed');
  await page.waitForFunction(() => typeof window.websplat?.snapshot === 'function' && !!window.websplat.testing?.job, null, {
    timeout: 120000,
  });
  if (a.iterations) {
    // before the stages start: autostart has begun ingest, which does not read config.prepare
    await page.evaluate((n) => {
      const c = window.websplat.testing.job().manifest.config;
      c.prepare = { ...c.prepare, iterations: n };
    }, +a.iterations);
  }
  let last = '';
  for (;;) {
    const s = await page.evaluate(() => window.websplat.snapshot());
    const line = s.stages
      .map(
        (x) =>
          `${x.id}:${x.state}${x.state === 'running' && x.progress?.total ? ` ${Math.round((100 * x.progress.done) / x.progress.total)}%` : ''}`,
      )
      .join(' ');
    if (line !== last) {
      say(line);
      report.events.push({ t: Date.now() - t0, line });
      save();
      last = line;
    }
    const failed = s.stages.find((x) => x.state === 'failed');
    if (failed) throw new Error(`${failed.id} failed: ${failed.error?.message}`);
    if (s.stages.every((x) => x.state === 'done')) break;
    await page.waitForTimeout(5000);
  }
  await page.waitForFunction(() => window.websplat.result?.opened, null, { timeout: 300000 });
  await page.waitForTimeout(8000);
  const m = await page.evaluate(() => window.websplat.job());
  const snap = await page.evaluate(() => window.websplat.snapshot());
  const r = (id) => m.stages[id]?.result;
  report.jobId = m.jobId;
  report.stages = Object.fromEntries(
    snap.stages.map((x) => [x.id, { ms: x.elapsedMs, workMs: x.timings?.workMs }]),
  );
  report.ingest = {
    range: r('ingest').range,
    homeAltitude: r('ingest').homeAltitude,
    rule: r('ingest').homeAltitudeRule,
  };
  report.select = { ...r('select'), log: undefined, decoderStats: undefined };
  report.map = { ...r('map') };
  report.prepare = r('prepare');
  report.train = r('train');
  report.export = { ...r('export'), spz: undefined };
  report.meta = await page.evaluate(() => window.websplat.result.meta);
  report.config = { select: m.config.select, export: m.config.export };
  await (await page.$('#result-viewer')).screenshot({ path: `${out}/result.png` });
  say(`done: ${report.export.gaussians} Gaussians`);
} catch (e) {
  report.error = String(e?.stack ?? e);
  say(`FAILED ${e.message}`);
} finally {
  report.pageErrors = log.filter((x) => x.type === 'pageerror').map((x) => `${x.tag}: ${x.text}`);
  save();
  await browser.close();
  server.close();
}
process.exit(report.error ? 1 : 0);
