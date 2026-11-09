// RGB jobs through the page: a request handed over by the stub viewer (stub/opener.html), run to
// the end of selection (or further with --until), on the integrated GPU by default.
// usage: node rgb-page.mjs [--source 720p|full] [--width 640] [--seconds 60] [--until select|
//        prepare|export] [--iterations N] [--adapter low-power] [--port 8812] [--decoder auto|
//        software] [--dump DIR (the first selected frame, RGB and grey, as PPM/PGM)]
// Recordings: WEBSPLAT_RGB_SMALL (H.264 + KLV) and WEBSPLAT_RGB_FULL (HEVC + APCG SEI), from the
// environment or local.env. Exit 1 if a check fails.
import { mkdirSync, openAsBlob, writeFileSync } from 'node:fs';
import { basename } from 'node:path';
import { args, env } from '../harness/lib/env.mjs';
import { capture, launchLocal } from '../harness/lib/browser.mjs';
import { indexVideo } from '../../app/src/stages/ingest/rgb.js';
import { serveV2 } from './serve.mjs';

const a = args();
const data = env('WEBSPLAT_DATA');
const which = a.source ?? '720p';
const mkv = env(which === 'full' ? 'WEBSPLAT_RGB_FULL' : 'WEBSPLAT_RGB_SMALL');
const width = +(a.width ?? 640);
const seconds = +(a.seconds ?? 60);
const until = a.until ?? 'select';
const port = +(a.port ?? 8812);
const adapter = a.adapter ?? 'low-power';
const site = `http://127.0.0.1:${port}`;
const say = (s) => console.log(`${new Date().toISOString().slice(11, 19)} ${s}`);
let failures = 0;
const check = (name, ok, detail = '') => {
  if (!ok) failures++;
  say(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? ': ' + String(detail).slice(0, 400) : ''}`);
};

const t0 = Date.now();
const media = await indexVideo(await openAsBlob(mkv));
media.source.close();
say(`indexed ${media.frames.length} frames in node (${Date.now() - t0} ms)`);
const fps = media.track.timescale / (media.frames[1].pts - media.frames[0].pts);
// a range in the middle of the recording
const mid = media.frames.length >> 1;
const half = Math.round((seconds * fps) / 2);
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
  rgb: { width },
  source: { name: basename(mkv), size: (await openAsBlob(mkv)).size },
  range: { timescale: media.track.timescale, start: ep(mid - half), end: ep(mid + half) },
  // the orbit's centre (the viewer's median optical-axis ground point)
  target: { lat: -35.32207, lon: 148.8349, alt_amsl: 790, refine: false },
  hfov: null,
  view: null,
  title: `orbit ${which} ${width}`,
};
const nonce = [...crypto.getRandomValues(new Uint8Array(16))]
  .map((b) => b.toString(16).padStart(2, '0'))
  .join('');

const server = await serveV2({
  port,
  extra: { '/test/': new URL('./stub/', import.meta.url).pathname },
});
const profile = `${data}/harness/profiles/v2-rgb`;
mkdirSync(profile, { recursive: true });
const browser = await launchLocal({ profile, adapter });
const log = [];
try {
  const opener = await browser.context.newPage();
  await opener.goto(`${site}/test/opener.html`);
  await opener.setInputFiles('#file', mkv);
  const popup = browser.context.waitForEvent('page');
  const q = `?adapter=${adapter}&autostart=0${a.decoder ? '' : ''}`;
  await opener.evaluate((o) => window.openSplat(o), {
    url: `${site}/websplat/index.html${q}`,
    nonce,
    request,
  });
  const page = await popup;
  capture(page, log, 'rgb');
  page.on('console', (m) => m.type() === 'error' && say(`console: ${m.text().slice(0, 300)}`));
  await page.waitForFunction(
    () => ['accepted', 'failed', 'rejected'].includes(window.websplat?.request?.state),
    null,
    { timeout: 600000 },
  );
  const r = await page.evaluate(() => window.websplat.request);
  check(
    'accepted as an rgb job',
    r.state === 'accepted' && /^rgb-[0-9a-f]{16}-[0-9a-f]{16}$/.test(r.job),
    JSON.stringify(r),
  );
  await page.waitForFunction(() => typeof window.websplat?.start === 'function', null, {
    timeout: 60000,
  });
  await page.evaluate(
    ([iterations, decoder]) => {
      const c = window.websplat.testing.job().manifest.config;
      if (iterations) c.prepare = { ...c.prepare, iterations };
      if (decoder) c.rgbDecoder = decoder;
      window.websplat.start();
    },
    [a.iterations ? +a.iterations : null, a.decoder ?? null],
  );
  const times = {};
  let last = '';
  let started = false;
  for (;;) {
    const s = await page.evaluate(() => window.websplat.snapshot());
    started ||= s.running;
    const line = s.stages.map((x) => `${x.id}:${x.state}`).join(' ');
    if (line !== last) say((last = line));
    for (const x of s.stages) if (x.state === 'done') times[x.id] ??= x.elapsedMs;
    const failed = s.stages.find((x) => x.state === 'failed');
    if (failed) {
      check(`${failed.id} ran`, false, failed.error?.message);
      break;
    }
    if (s.stages.find((x) => x.id === until)?.state === 'done') break;
    if (started && !s.running && !s.needs) break;
    await page.waitForTimeout(1000);
  }
  await page.evaluate(() => window.websplat.shutdown());
  const m = await page.evaluate(() => window.websplat.job());
  const ing = m.stages.ingest?.result;
  check(
    'ingest: rgb, range and takeoff altitude',
    ing?.kind === 'rgb' && ing.range?.frames === 2 * half + 1 && Number.isFinite(ing.homeAltitude),
    JSON.stringify({ range: ing?.range, home: ing?.homeAltitude, rule: ing?.homeAltitudeRule }),
  );
  check(
    'select config from the request',
    m.config.select?.width === width &&
      m.config.select?.budget === 500 &&
      m.config.features?.maxNumFeatures === 4096,
    JSON.stringify(m.config.select),
  );
  const sel = m.stages.select?.result;
  if (sel) {
    const h = Math.round((media.track.height * width) / media.track.width);
    check(
      'select: frames at the processing size',
      sel.width === width && sel.height === h && sel.frames >= 3,
      JSON.stringify({ ...sel, log: undefined, decoderStats: undefined }),
    );
    const outs = m.stages.select.outputs;
    check(
      'select outputs: grey and RGB',
      outs['features.u8'].bytes === sel.frames * width * h &&
        outs['rgb.u8'].bytes === 3 * sel.frames * width * h,
    );
    say(
      `select: ${sel.frames} frames, decoder ${sel.decoder}, decode ${(sel.timings.decodeMs / 1000).toFixed(1)} s`,
    );
    if (a.dump) {
      const read = (path, n) =>
        page.evaluate(
          async ([path, n]) => {
            let d = await navigator.storage.getDirectory();
            const parts = ['websplat', ...path.split('/')];
            for (const p of parts.slice(0, -1)) d = await d.getDirectoryHandle(p);
            const f = await (await d.getFileHandle(parts.at(-1))).getFile();
            const u = new Uint8Array(await f.slice(0, n).arrayBuffer());
            let str = '';
            for (let i = 0; i < u.length; i += 0x8000)
              str += String.fromCharCode(...u.subarray(i, i + 0x8000));
            return btoa(str);
          },
          [path, n],
        );
      mkdirSync(a.dump, { recursive: true });
      const px = width * h;
      writeFileSync(
        `${a.dump}/frame0.ppm`,
        Buffer.concat([
          Buffer.from(`P6 ${width} ${h} 255\n`),
          Buffer.from(await read(outs['rgb.u8'].path, 3 * px), 'base64'),
        ]),
      );
      writeFileSync(
        `${a.dump}/frame0.pgm`,
        Buffer.concat([
          Buffer.from(`P5 ${width} ${h} 255\n`),
          Buffer.from(await read(outs['features.u8'].path, px), 'base64'),
        ]),
      );
    }
  }
  const map = m.stages.map?.result;
  if (map)
    say(
      `map: ${map.images} images, ${map.points} points, reprojection ${map.mean_reproj_px?.toFixed(3)} px, f ${map.focal?.toFixed(1)}, k1 ${map.k1?.toFixed(4)}`,
    );
  const train = m.stages.train?.result;
  if (train) say(`train: ${JSON.stringify(train)}`);
  const prep = m.stages.prepare?.result;
  if (prep) say(`prepare: ${JSON.stringify(prep)}`);
  const ex = m.stages.export?.result;
  if (ex)
    check(
      'export: rgb splat',
      ex.kind === 'rgb' && ex.gaussians > 0,
      JSON.stringify({ kind: ex.kind, gaussians: ex.gaussians }),
    );
  say(
    `stage times (s): ${Object.entries(times)
      .map(([k, v]) => `${k} ${(v / 1000).toFixed(1)}`)
      .join(', ')}`,
  );
} finally {
  await browser.close();
  server.close();
}
for (const e of log.filter((x) => x.type === 'pageerror')) say(`page error: ${e.text}`);
say(failures ? `${failures} checks FAILED` : 'all checks passed');
process.exit(failures ? 1 : 0);
