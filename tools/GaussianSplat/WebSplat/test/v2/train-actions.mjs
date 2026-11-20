// Training actions through the page, with short training: Preview while training (twice; training
// must go on), Stop and accept, a reload after it (training must not run again), and the result
// viewer's ceiling slider (screenshots before and after, the clipped PLY download).
//   thermal (default): the orbit MKV chosen on the page; accepted while training runs.
//   rgb: a request handed over by the stub viewer (WEBSPLAT_RGB_SMALL, 60 s at --width 640);
//     Stop after the first checkpoint, reload, Preview from the checkpoint, accept from it.
// usage: node train-actions.mjs --out DIR [--kind thermal|rgb] [--iterations 4000]
//        [--checkpoint-every 1000] [--accept-at 0.6] [--adapter low-power] [--port 8861]
// Writes DIR/{report.json, *.png}; the Chrome profile is DIR/profile. Exit 1 if a check fails.
import { mkdirSync, openAsBlob, writeFileSync } from 'node:fs';
import { basename } from 'node:path';
import { args, env } from '../harness/lib/env.mjs';
import { capture, launchLocal } from '../harness/lib/browser.mjs';
import { indexVideo } from '../../app/src/stages/ingest/rgb.js';
import { serveV2 } from './serve.mjs';

const a = args();
if (!a.out) throw new Error('--out DIR is required');
const kind = a.kind ?? 'thermal';
const iterations = +(a.iterations ?? 4000);
const ckptEvery = +(a['checkpoint-every'] ?? 1000);
const acceptAt = +(a['accept-at'] ?? 0.6);
const port = +(a.port ?? 8861);
const adapter = a.adapter ?? null;
const out = a.out;
mkdirSync(out, { recursive: true });
const site = `http://127.0.0.1:${port}`;
const t0 = Date.now();
const say = (s) =>
  console.log(`${new Date().toISOString().slice(11, 19)} +${((Date.now() - t0) / 1000) | 0}s ${s}`);
const report = { kind, iterations, checks: [] };
const check = (name, ok, detail = '') => {
  report.checks.push({ name, ok: !!ok, detail });
  say(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? ': ' + String(detail).slice(0, 400) : ''}`);
};
const save = () => writeFileSync(`${out}/report.json`, JSON.stringify(report, null, 1));

const server = await serveV2({
  port,
  extra: { '/test/': new URL('./stub/', import.meta.url).pathname },
});
const browser = await launchLocal({
  profile: `${out}/profile`,
  adapter: adapter ?? undefined,
});
const ctx = browser.context;
const log = [];
const q = adapter ? `adapter=${adapter}&` : '';

const snap = (page) => page.evaluate(() => window.websplat.snapshot());
const train = async (page) => (await snap(page)).stages.find((s) => s.id === 'train');
async function waitFor(page, what, fn, timeoutMs = 3600000) {
  const end = Date.now() + timeoutMs;
  let last = '';
  for (;;) {
    const s = await snap(page);
    const line = s.stages
      .map(
        (x) =>
          `${x.id}:${x.state}${x.progress?.total && x.state === 'running' ? ` ${x.progress.done}/${x.progress.total}` : ''}`,
      )
      .join(' ');
    if (line.replace(/ [\d.]+\/\d+/g, '') !== last.replace(/ [\d.]+\/\d+/g, '')) say(line);
    last = line;
    const failed = s.stages.find((x) => x.state === 'failed');
    if (failed) throw new Error(`${failed.id} failed: ${failed.error?.message}`);
    if (fn(s)) return s;
    if (Date.now() > end) throw new Error(`timed out waiting for ${what}`);
    await page.waitForTimeout(1000);
  }
}
const trainOf = (s) => s.stages.find((x) => x.id === 'train');

async function openPage(url) {
  const page = await ctx.newPage();
  await page.setViewportSize({ width: 1280, height: 1100 });
  capture(page, log, kind);
  page.on('console', (m) => m.type() === 'error' && say(`console: ${m.text().slice(0, 300)}`));
  page.on('dialog', (d) => {
    say(`dialog: ${d.message().split('\n')[0]}`);
    d.accept();
  });
  if (url) await page.goto(url);
  return page;
}

async function preview(page, name) {
  const seq = await page.evaluate(() => window.websplat.preview?.seq ?? 0);
  const before = (await train(page)).progress?.done;
  const tp = Date.now();
  // Preview, or Refresh once the preview is open
  await page.click(
    (await page.locator('#preview').isVisible()) ? '#preview-refresh' : '#train-preview',
  );
  await page.waitForFunction((n) => window.websplat.preview?.seq > n, seq, {
    timeout: 600000,
  });
  const p = await page.evaluate(() => ({ ...window.websplat.preview }));
  await page.waitForTimeout(6000); // the viewer's first full-size frame
  await (await page.$('#preview-viewer')).screenshot({ path: `${out}/${name}.png` });
  const status = await page.textContent('#train-preview-status');
  say(`preview: ${status}`);
  return { ...p, before, ms: Date.now() - tp, status };
}

async function ceiling(page, name) {
  const viewer = page.frames().find((f) => f.url().includes('splat-viewer'));
  await viewer.waitForFunction(() => window.splatViewer?.ceiling?.range, null, {
    timeout: 60000,
  });
  await page.waitForTimeout(4000);
  await (await page.$('#result-viewer')).screenshot({ path: `${out}/${name}-ceiling-off.png` });
  const off = await viewer.evaluate(() => window.splatViewer.ceiling);
  // a quarter of the way from the target to the highest Gaussian
  await viewer.evaluate(() => {
    const s = document.getElementById('ceiling');
    s.value = 250;
    s.dispatchEvent(new Event('input'));
  });
  await page.waitForTimeout(5000);
  await (await page.$('#result-viewer')).screenshot({ path: `${out}/${name}-ceiling-on.png` });
  const on = await viewer.evaluate(() => ({
    ...window.splatViewer.ceiling,
    text: document.getElementById('ceiling-value').textContent,
  }));
  check(
    'ceiling: off by default, then hides the Gaussians above it',
    off.height === null && on.height > off.range[0] && on.hidden > 0,
    JSON.stringify({ off, on }),
  );
  const dl = page.waitForEvent('download', { timeout: 120000 });
  await viewer.click('#ceiling-download');
  const d = await dl;
  const path = `${out}/${name}-clipped.ply`;
  await d.saveAs(path);
  const bytes = new Uint8Array(await (await openAsBlob(path)).arrayBuffer());
  const head = new TextDecoder('latin1').decode(bytes.subarray(0, 8192));
  const n = +head.match(/element vertex (\d+)/)[1];
  // every kept Gaussian is at or below the ceiling (exact float32 positions; up is +z)
  const props = [...head.matchAll(/^property float (\S+)$/gm)].map((m) => m[1]);
  const start = head.indexOf('end_header\n') + 11;
  const dv = new DataView(bytes.buffer, start);
  let maxZ = -Infinity;
  for (let i = 0; i < n; i++)
    maxZ = Math.max(maxZ, dv.getFloat32(4 * (i * props.length + props.indexOf('z')), true));
  const gaussians = await page.evaluate(() => window.websplat.job().stages.export.result.gaussians);
  check(
    'clipped PLY download: only Gaussians at or below the ceiling',
    maxZ <= on.height && n < gaussians,
    `${n} of ${gaussians} kept, highest ${maxZ.toFixed(2)} m, ceiling ${on.height.toFixed(2)} m`,
  );
  // the viewer counts Spark's centres (float16 for colour splats): a few differ at the ceiling
  check(
    'clipped PLY download matches the view',
    Math.abs(n - (gaussians - on.hidden)) <= Math.max(2, 0.01 * gaussians),
    `${n} kept, view shows ${gaussians - on.hidden}`,
  );
  report[`${name}Ceiling`] = { off, on, downloaded: n };
}

async function resultChecks(page, planned) {
  await page.waitForFunction(() => window.websplat.result?.opened, null, {
    timeout: 600000,
  });
  const m = await page.evaluate(() => window.websplat.job());
  const tr = m.stages.train.result;
  const ex = m.stages.export.result;
  const meta = await page.evaluate(() => window.websplat.result.meta);
  report.train = tr;
  report.exportStoppedEarly = ex.stoppedEarly;
  report.metaStoppedEarly = meta.stopped_early;
  check(
    'train result: stopped early, iterations done of planned',
    tr.stopped_early && tr.iterations < planned && tr.iterations_planned === planned,
    JSON.stringify(tr),
  );
  check(
    'meta.json and export result say so',
    meta.stopped_early?.iterations === tr.iterations &&
      meta.stopped_early?.iterations_planned === planned &&
      ex.stoppedEarly?.iterations === tr.iterations,
    JSON.stringify(meta.stopped_early),
  );
  const text = await page.textContent('#result-stopped').catch(() => null);
  check('result summary says so', text?.includes(`of ${planned.toLocaleString('en')}`), text);
  return m;
}

// a reload after the accept: nothing runs again
async function reloadCheck(url, before) {
  const page = await openPage(url);
  await page.waitForFunction(() => window.websplat?.ready, null, {
    timeout: 120000,
  });
  await page.waitForTimeout(8000);
  const s = await snap(page);
  const m = await page.evaluate(() => window.websplat.job());
  check(
    'reload: training not restarted, still done with the same result',
    !s.running &&
      s.stages.every((x) => x.state === 'done') &&
      m.stages.train.runs === before.stages.train.runs &&
      m.stages.train.result.iterations === before.stages.train.result.iterations,
    JSON.stringify({ running: s.running, runs: m.stages.train.runs }),
  );
  await page.waitForFunction(() => window.websplat.result?.opened, null, {
    timeout: 300000,
  });
  await (await page.$('#result')).screenshot({ path: `${out}/${kind}-reloaded-result.png` });
  return page;
}

async function thermal() {
  const page = await openPage(`${site}/websplat/index.html?${q}pipeline=thermal`);
  await page.waitForSelector('#file', { state: 'visible' });
  await page.setInputFiles('#file', `${env('WEBSPLAT_DATA')}/input/orbit.mkv`);
  await page.waitForFunction(() => window.websplat?.snapshot, null, { timeout: 120000 });
  await page.click('#start');
  say('started');
  await page.waitForSelector('#proposal', {
    state: 'visible',
    timeout: 600000,
  });
  await page.evaluate(
    ([n, ck]) => {
      const c = window.websplat.testing.job().manifest.config;
      c.prepare = { ...c.prepare, iterations: n };
      c.train = { ...c.train, checkpointEvery: ck };
    },
    [iterations, ckptEvery],
  );
  await page.click('#proposal-form button[type=submit]');
  await waitFor(
    page,
    'training',
    (s) => (trainOf(s).progress?.done ?? 0) >= 0.3 * iterations && trainOf(s).phase === 'train',
  );
  const p1 = await preview(page, 'thermal-preview-1');
  await page.waitForTimeout(10000);
  const after = (await train(page)).progress.done;
  check(
    'preview opened mid-training',
    p1.opened && p1.iteration > 0 && p1.gaussians > 0,
    p1.status,
  );
  check(
    'training continues after the preview',
    after > p1.iteration,
    `${p1.iteration} -> ${after}`,
  );
  const p2 = await preview(page, 'thermal-preview-2');
  check(
    'refreshed preview is newer',
    p2.iteration > p1.iteration,
    `${p1.iteration} -> ${p2.iteration}`,
  );
  report.previews = [p1, p2];
  await page.click('#preview-close');
  check('preview closed', await page.locator('#preview').isHidden());
  await waitFor(
    page,
    'the accept point',
    (s) => (trainOf(s).progress?.done ?? 0) >= acceptAt * iterations,
  );
  await page.click('#train-accept');
  say('accepted');
  await waitFor(page, 'the result', (s) => s.stages.every((x) => x.state === 'done'));
  const m = await resultChecks(page, iterations);
  await (await page.$('#result')).screenshot({ path: `${out}/thermal-result.png` });
  const url = page.url();
  await page.close();
  const p = await reloadCheck(url, m);
  await ceiling(p, 'thermal');
}

async function rgb() {
  const file = env('WEBSPLAT_RGB_SMALL');
  const media = await indexVideo(await openAsBlob(file));
  media.source.close();
  const fps = media.track.timescale / (media.frames[1].pts - media.frames[0].pts);
  const mid = media.frames.length >> 1;
  const half = Math.round((60 * fps) / 2);
  const ep = (i) => ({
    pts: media.frames[i].pts,
    offset: media.frames[i].offset,
    size: media.frames[i].size,
    index: i,
  });
  const request = {
    format: 'websplat.request',
    version: 1,
    kind: 'rgb',
    rgb: { width: +(a.width ?? 640) },
    source: { name: basename(file), size: (await openAsBlob(file)).size },
    range: {
      timescale: media.track.timescale,
      start: ep(mid - half),
      end: ep(mid + half),
    },
    target: { lat: -35.32207, lon: 148.8349, alt_amsl: 790, refine: false },
    hfov: null,
    view: null,
    title: 'train actions rgb',
  };
  const nonce = [...crypto.getRandomValues(new Uint8Array(16))]
    .map((b) => b.toString(16).padStart(2, '0'))
    .join('');
  const opener = await openPage(`${site}/test/opener.html`);
  await opener.setInputFiles('#file', file);
  const popup = ctx.waitForEvent('page');
  await opener.evaluate((o) => window.openSplat(o), {
    url: `${site}/websplat/index.html?${q}autostart=0`,
    nonce,
    request,
  });
  let page = await popup;
  capture(page, log, kind);
  page.on('dialog', (d) => d.accept());
  await page.setViewportSize({ width: 1280, height: 1100 });
  await page.waitForFunction(() => window.websplat?.request?.state === 'accepted', null, {
    timeout: 600000,
  });
  await page.waitForFunction(() => typeof window.websplat?.start === 'function', null, {
    timeout: 60000,
  });
  await page.evaluate(
    ([n, ck]) => {
      const c = window.websplat.testing.job().manifest.config;
      c.prepare = { ...c.prepare, iterations: n };
      c.train = { ...c.train, checkpointEvery: ck };
      window.websplat.start();
    },
    [iterations, ckptEvery],
  );
  await waitFor(
    page,
    'training',
    (s) => (trainOf(s).progress?.done ?? 0) >= 0.2 * iterations && trainOf(s).phase === 'train',
  );
  const p1 = await preview(page, 'rgb-preview-live');
  await page.waitForTimeout(10000);
  const after = (await train(page)).progress.done;
  check('RGB preview opened mid-training', p1.opened && p1.gaussians > 0, p1.status);
  check(
    'training continues after the preview',
    after > p1.iteration,
    `${p1.iteration} -> ${after}`,
  );
  await page.click('#preview-close');
  await waitFor(page, 'a checkpoint', (s) => trainOf(s).checkpoint?.done >= ckptEvery);
  await page.click('#stop');
  await waitFor(page, 'stopped', (s) => !s.running);
  const ck = (await train(page)).checkpoint.done;
  const url = page.url();
  await page.close();
  await opener.close();
  // reload, preview the checkpoint, accept it
  page = await openPage(url);
  await page.waitForFunction(() => window.websplat?.ready, null, {
    timeout: 120000,
  });
  await page.waitForTimeout(3000);
  check('reload while stopped: nothing runs', !(await snap(page)).running);
  const p2 = await preview(page, 'rgb-preview-checkpoint');
  check('checkpoint preview', p2.opened && p2.iteration === ck, p2.status);
  report.previews = [p1, p2];
  await page.click('#train-accept');
  say(`accepted the checkpoint at ${ck}`);
  await waitFor(page, 'the result', (s) => s.stages.every((x) => x.state === 'done'));
  const m = await resultChecks(page, iterations);
  check(
    // Brush reports steps in small groups: the first report after the checkpoint
    'accepted at the checkpoint (the first steps after it)',
    m.stages.train.result.iterations > ck && m.stages.train.result.iterations <= ck + 50,
    m.stages.train.result.iterations,
  );
  await (await page.$('#result')).screenshot({ path: `${out}/rgb-result.png` });
  await page.close();
  const p = await reloadCheck(url, m);
  await ceiling(p, 'rgb');
}

try {
  await (kind === 'rgb' ? rgb() : thermal());
} catch (e) {
  report.error = String(e?.stack ?? e);
  say(`FAILED ${e.message}`);
} finally {
  report.pageErrors = log.filter((x) => x.type === 'pageerror').map((x) => `${x.tag}: ${x.text}`);
  report.seconds = (Date.now() - t0) / 1000;
  save();
  await browser.close();
  server.close();
}
const failed = report.error || report.checks.some((c) => !c.ok);
say(failed ? 'FAILED' : 'all checks passed');
process.exit(failed ? 1 : 0);
