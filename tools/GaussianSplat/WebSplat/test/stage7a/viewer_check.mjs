// Stage 7a in Chrome: the JS exports of the tiny model in the GaussianSplat viewer (WebGL, its
// built dist/ as deployed), by every route a user or the app takes:
//   file     the open-a-file page with the JS splat.ply (metadata from the PLY header)
//   url_spz  ?url=splat.spz&meta=meta.json
//   site     an export_web.py site page (window.SPLAT) holding the JS splat.spz and meta.json
//   embed    the test page: export in a worker, result in the embedded viewer (postMessage), from
//            the start camera and the opposite side, PLY and SPZ, and a meta.json without
//            rasterize_mode (Spark's antialiased default)
// Each case: screenshot (panel hidden, whitehot, fixed range = display_range_c) and the viewer's
// own temperature readout at every Gaussian centre. compare_views.py checks them.
//   node viewer_check.mjs WEB JS OUT [--port 8797]
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { args, env } from '../harness/lib/env.mjs';
import { capture, launchLocal } from '../harness/lib/browser.mjs';
import { serve } from '../harness/lib/serve.mjs';

const a = args();
const [web, js, out] = a._;
if (!out) throw new Error('usage: node viewer_check.mjs WEB JS OUT');
const port = +(a.port ?? 8797);
const base = `http://127.0.0.1:${port}`;
mkdirSync(out, { recursive: true });
const expected = JSON.parse(readFileSync(`${js}/expected.json`, 'utf8'));
const meta = JSON.parse(readFileSync(`${js}/meta.json`, 'utf8'));
const W = expected.width;
const H = expected.height;

const server = await serve(web, port);
const profile = `${env('WEBSPLAT_DATA')}/chrome-profiles/stage7a`;
const browser = await launchLocal({ profile });
const log = [];
const results = { cases: {} };
let failures = 0;
const fail = (msg) => {
  console.log(`FAIL ${msg}`);
  failures++;
};

async function viewerFrame(page, embedded) {
  if (!embedded) return page.mainFrame();
  for (let i = 0; i < 100; i++) {
    const f = page.frames().find((x) => x.url().includes('/viewer/index.html'));
    if (f) return f;
    await page.waitForTimeout(100);
  }
  throw new Error('no viewer frame');
}

// wait until the viewer shows a thermal splat, then fix the display for the comparison
async function prepare(frame, range) {
  await frame.waitForFunction(
    () =>
      !document.getElementById('thermal-controls').hidden &&
      document.getElementById('status').hidden,
    null,
    { timeout: 30000 },
  );
  await frame.addStyleTag({
    content: '#panel, .credits, #status, .project, #open-panel { display: none !important; }',
  });
  await frame.evaluate(([lo, hi]) => {
    const cmap = document.getElementById('cmap');
    cmap.value = 'whitehot';
    cmap.dispatchEvent(new Event('change'));
    const auto = document.getElementById('auto');
    auto.checked = false;
    auto.dispatchEvent(new Event('change'));
    for (const [id, v] of [
      ['lo', lo],
      ['hi', hi],
    ]) {
      const el = document.getElementById(id);
      el.value = v;
      el.dispatchEvent(new Event('input'));
    }
  }, range);
  await frame.waitForTimeout(1500); // the full-resolution render after the start view settles
}

async function readouts(page, frame, cam) {
  const rows = [];
  for (const g of expected.cameras[cam].gaussians) {
    if (g.u < 0 || g.v < 0 || g.u >= W || g.v >= H) {
      rows.push(null);
      continue;
    }
    await page.mouse.move(g.u, g.v);
    await page.waitForTimeout(60);
    const text = await frame.evaluate(() => document.getElementById('hover').textContent);
    const m = text.match(/(-?[\d.]+) °C/);
    rows.push(m ? +m[1] : null);
  }
  await page.mouse.move(W - 1, H - 1);
  return rows;
}

async function shoot(page, frame, name, cam, info = {}) {
  const sizes = await frame.evaluate(() => {
    const c = document.getElementById('canvas');
    return { css: [c.clientWidth, c.clientHeight], buffer: [c.width, c.height] };
  });
  if (sizes.buffer[0] !== W || sizes.buffer[1] !== H) {
    fail(`${name}: canvas ${sizes.buffer} is not ${W}x${H}`);
  }
  // the title comes from the header / meta.json and contains 'end_header' (document.title
  // collapses the newline)
  const title = await frame.evaluate(() => document.title);
  const want = meta.title.replace(/\s+/g, ' ');
  if (title !== want) fail(`${name}: title ${JSON.stringify(title)}, not ${JSON.stringify(want)}`);
  await page.screenshot({ path: `${out}/${name}.png`, clip: { x: 0, y: 0, width: W, height: H } });
  const temps = await readouts(page, frame, cam);
  results.cases[name] = { camera: cam, temps, sizes, ...info };
  console.log(`${name}: ${temps.map((t) => (t === null ? '-' : t.toFixed(1))).join(' ')}`);
}

async function newPage(tag) {
  const page = await browser.context.newPage();
  await page.setViewportSize({ width: W, height: H });
  capture(page, log, tag);
  page.on('pageerror', (e) => console.log(`  ${tag} page error: ${e.stack}`));
  return page;
}

const range = meta.display_range_c;
try {
  // file: the open-a-file page, as at plot.ardupilot.org/splat/
  {
    const page = await newPage('file');
    await page.goto(`${base}/viewer/index.html`);
    await page.setInputFiles('#file', `${js}/splat.ply`);
    const frame = page.mainFrame();
    await prepare(frame, range);
    await shoot(page, frame, 'file_start', 'start');
    await page.close();
  }
  // url_spz and site: SPZ + meta.json
  {
    const page = await newPage('url_spz');
    await page.goto(`${base}/viewer/index.html?url=../files/splat.spz&meta=../files/meta.json`);
    await prepare(page.mainFrame(), range);
    await shoot(page, page.mainFrame(), 'spz_start', 'start', { spz: true });
    await page.close();
  }
  {
    const page = await newPage('site');
    await page.goto(`${base}/site/2026-11-08/tiny/run1/`);
    await prepare(page.mainFrame(), range);
    await shoot(page, page.mainFrame(), 'site_start', 'start', { spz: true });
    await page.close();
  }
  // embed: worker export, then the embedded viewer
  {
    const page = await newPage('embed');
    await page.goto(`${base}/embed/index.html`);
    const exp = await page.evaluate((t) => window.runExport('../files', t), meta.title);
    results.workerExport = exp;
    console.log(`worker export: ${exp.ms.toFixed(0)} ms, ${JSON.stringify(exp.sizes)}`);
    for (const name of ['splat.ply', 'splat.spz', 'meta.json']) {
      const b = Buffer.from(await page.evaluate((n) => window.fileBase64(n), name), 'base64');
      const ref = readFileSync(`${js}/${name}`);
      if (!b.equals(ref)) fail(`worker ${name} differs from the Node export`);
      else console.log(`PASS worker ${name} = Node export (${b.length} bytes)`);
    }
    await page.evaluate(() => window.openResult('splat.ply'));
    let frame = await viewerFrame(page, true);
    await prepare(frame, range);
    await shoot(page, frame, 'embed_start', 'start');
    const m = await page.evaluate(() => window.resultMeta());
    const back = { ...m, camera: { ...expected.cameras.back } };
    delete back.camera.viewmat;
    delete back.camera.f;
    delete back.camera.gaussians;
    await page.evaluate((mm) => window.openResult('splat.ply', mm), back);
    await prepare(frame, range);
    await shoot(page, frame, 'embed_back', 'back');
    // a splat that does not say how it was trained keeps Spark's antialiased rendering
    const legacy = { ...back };
    delete legacy.rasterize_mode;
    await page.evaluate((mm) => window.openResult('splat.ply', mm), legacy);
    await prepare(frame, range);
    await shoot(page, frame, 'embed_legacy_back', 'back', { mode: 'antialiased' });
    await page.evaluate((mm) => window.openResult('splat.spz', mm), m);
    await prepare(frame, range);
    await shoot(page, frame, 'embed_spz_start', 'start', { spz: true });
    await page.close();
  }
} catch (e) {
  fail(`driver: ${e.stack ?? e}`);
} finally {
  await browser.close();
  server.close();
}
const errors = log.filter(
  (l) => ['error', 'pageerror', 'crash'].includes(l.type) || l.type === 'http',
);
results.console = log;
writeFileSync(`${out}/viewer.json`, JSON.stringify(results, null, 1));
if (errors.length) {
  for (const e of errors) console.log(`  console ${e.tag} ${e.type}: ${e.text}`);
  fail(`${errors.length} console errors`);
}
console.log(failures ? `${failures} FAILED` : 'viewer driver: done');
process.exit(failures ? 1 : 0);
