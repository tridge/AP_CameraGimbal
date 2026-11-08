// The real stages registered so far on the canonical input, through the page: choose the MKV,
// confirm the proposal, run to the end; records per-stage timings, GPU memory and console.
// usage: node thermal.mjs [--machine laptop|win11] [--mkv PATH] [--port 8790] [--cdp URL] [--reference]
//   --select-mode single|redecode|cache (default single)
//   --reference: use the reference extraction's target (CONTRACTS.md stage 2) instead of the proposal
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { APP_DIST, args, env } from './lib/env.mjs';
import { capture, connectCdp, launchLocal } from './lib/browser.mjs';
import { GpuSampler } from './lib/gpumem.mjs';
import { serve } from './lib/serve.mjs';

const a = args();
const machine = a.machine ?? 'laptop';
const port = +(a.port ?? 8790);
const adapter = a.adapter ?? 'high-performance';
const data = env('WEBSPLAT_DATA');
const mkv = a.mkv ?? `${data}/input/orbit.mkv`;
const out = `${data}/harness/runs/thermal-${machine}-${a['select-mode'] ?? 'single'}-${new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19)}`;
mkdirSync(out, { recursive: true });
const base = `http://127.0.0.1:${port}`;
const say = (s) => console.log(`${new Date().toISOString().slice(11, 19)} ${s}`);
const consoleLog = [];
const checks = [];
const check = (name, ok, detail = '') => {
  checks.push({ name, ok: !!ok, detail });
  say(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? ': ' + detail : ''}`);
};

const server = await serve(APP_DIST, port);
const profile = `${data}/harness/profiles/thermal-${machine}-${adapter}`;
const gpu = new GpuSampler({
  sshHost: machine === 'win11' ? 'win11' : null,
  match: machine === 'win11' ? 'chrome.exe' : profile,
});
let browser;
try {
  browser =
    machine === 'win11'
      ? await connectCdp(a.cdp ?? 'http://127.0.0.1:9345')
      : await launchLocal({ profile, adapter });
  const page = await browser.context.newPage();
  capture(page, consoleLog, 'tab1');
  await gpu.start();
  await page.goto(`${base}/__blank`);
  await page.evaluate(async () => {
    localStorage.clear();
    await (await navigator.storage.getDirectory())
      .removeEntry('websplat', { recursive: true })
      .catch(() => {});
  });
  const t0 = Date.now();
  await page.goto(
    `${base}/index.html?pipeline=thermal&adapter=${adapter}${a['select-mode'] ? `&selectMode=${a['select-mode']}` : ''}`,
  );
  await page.waitForSelector('#file', { state: 'visible' });
  if (machine === 'win11') {
    // a remote browser gets the file's bytes
    await page.setInputFiles('#file', {
      name: 'orbit.mkv',
      mimeType: 'video/x-matroska',
      buffer: readFileSync(mkv),
    });
  } else {
    await page.setInputFiles('#file', mkv);
  }
  await page.waitForFunction(() => window.websplat?.snapshot, null, {
    timeout: 120000,
  });
  await page.click('#start');
  await page.waitForSelector('#proposal', {
    state: 'visible',
    timeout: 300000,
  });
  const proposed = await page.evaluate(() =>
    Object.fromEntries(
      ['lat', 'lon', 'target_alt', 'rotation', 'hfov'].map((k) => [
        k,
        document.getElementById('proposal-form').elements[k].value,
      ]),
    ),
  );
  say(`proposal ${JSON.stringify(proposed)}`);
  check(
    'ingest proposes rotation 0 and a target',
    proposed.rotation === '0' && proposed.lat !== '' && proposed.lon !== '',
  );
  if (a.reference) {
    await page.fill('[name=lat]', '-35.5802022');
    await page.fill('[name=lon]', '148.9065438');
  }
  await page.click('#proposal-form button[type=submit]');
  await page.waitForFunction(
    () => {
      const s = window.websplat.snapshot();
      return !s.running && s.stages.every((x) => ['done', 'failed'].includes(x.state));
    },
    null,
    { timeout: 600000, polling: 500 },
  );
  const s = await page.evaluate(() => window.websplat.snapshot());
  const tEnd = Date.now();
  for (const st of s.stages) check(`${st.id} done`, st.state === 'done', st.error?.message ?? '');
  // the stages' own last progress count must have reached their total
  const persisted = await page.evaluate(() =>
    window.websplat
      .verify()
      .then((v) =>
        Object.fromEntries(
          Object.entries(v.manifest.stages).map(([k, x]) => [
            k,
            { state: x.state, progress: x.progress },
          ]),
        ),
      ),
  );
  for (const [id, x] of Object.entries(persisted)) {
    check(
      `${id} persisted done, progress reached its total`,
      x.state === 'done' && x.progress?.reported === x.progress?.total,
      JSON.stringify(x.progress),
    );
  }
  // select's published artifacts: frames.json and per-frame hashes of the pixel arrays
  const got = await page.evaluate(async () => {
    const m = window.websplat.job();
    const outs = m.stages.select?.outputs ?? {};
    const open = async (p) => {
      let d = await (await navigator.storage.getDirectory()).getDirectoryHandle('websplat');
      const parts = p.split('/');
      for (const x of parts.slice(0, -1)) d = await d.getDirectoryHandle(x);
      return (await d.getFileHandle(parts.at(-1))).getFile();
    };
    const missing = ['frames.json', 'features.u8', 'thermal.u16'].filter((n) => !outs[n]);
    if (missing.length) return { missing };
    const frames = JSON.parse(await (await open(outs['frames.json'].path)).text());
    const px = frames.width * frames.height;
    const hex = (b) => [...new Uint8Array(b)].map((x) => x.toString(16).padStart(2, '0')).join('');
    const hashes = async (f, bpp) => {
      const out = [];
      for (let k = 0; k < frames.frames.length; k++) {
        const buf = await f.slice(k * px * bpp, (k + 1) * px * bpp).arrayBuffer();
        out.push(
          buf.byteLength === px * bpp ? hex(await crypto.subtle.digest('SHA-256', buf)) : null,
        );
      }
      return { out, bytes: f.size };
    };
    return {
      frames,
      thermal: await hashes(await open(outs['thermal.u16'].path), 2),
      features: await hashes(await open(outs['features.u8'].path), 1),
    };
  });
  const ref = JSON.parse(readFileSync(`${data}/ref/work/frames.json`, 'utf8'));
  const refPx = JSON.parse(readFileSync(`${data}/ref/work_pixel_hashes.json`, 'utf8'));
  check(
    'select published frames.json, features.u8, thermal.u16',
    !got.missing,
    got.missing?.join(','),
  );
  if (!got.missing) {
    const f = got.frames.frames;
    const n = f.length;
    check(
      'frame identities (index, name) match',
      n === ref.frames.length &&
        f.every((x, k) => x.index === ref.frames[k].index && x.name === ref.frames[k].name),
      `${n} frames`,
    );
    const head = ['width', 'height', 'hfov_deg', 'rotation_deg'].filter(
      (k) => got.frames[k] !== ref[k],
    );
    check('image size, HFOV and rotation match', head.length === 0, head.join(','));
    const maxDiff = (get) =>
      Math.max(
        ...f.map((x, k) =>
          Math.max(
            ...get(x)
              .flat()
              .map((v, i) => Math.abs(v - get(ref.frames[k]).flat()[i])),
          ),
        ),
      );
    const tol = a.reference ? 1e-6 : 1e-3;
    const dPos = maxDiff((x) => x.position);
    const dRot = maxDiff((x) => x.rotation);
    check(
      'positions match the reference',
      dPos <= tol,
      `max ${dPos.toExponential(2)} m (tolerance ${tol})`,
    );
    check(
      'rotations match the reference',
      dRot <= (a.reference ? 1e-12 : 1e-9),
      `max ${dRot.toExponential(2)}`,
    );
    const radio = [
      'minimum_c',
      'maximum_c',
      'temperature_scale_k',
      'temperature_offset_k',
      'utc_us',
    ];
    const radBad = f
      .filter((x, k) => radio.some((r) => x[r] !== ref.frames[k][r]))
      .map((x) => x.name);
    check('radiometry and timestamps identical', radBad.length === 0, radBad.slice(0, 5).join(','));
    const tBad = refPx.frames.filter((r, k) => got.thermal.out[k] !== r.thermal).map((r) => r.name);
    const cBad = refPx.frames
      .filter((r, k) => got.features.out[k] !== r.features)
      .map((r) => r.name);
    const px = got.frames.width * got.frames.height;
    check(
      'thermal.u16 pixels identical to native thermal/ PNGs',
      tBad.length === 0 && got.thermal.bytes === n * px * 2,
      `${tBad.length} differ`,
    );
    check(
      'features.u8 pixels identical to native images/ (CLAHE) PNGs',
      cBad.length === 0 && got.features.bytes === n * px,
      `${cBad.length} differ`,
    );
  }
  await gpu.stop();
  const errors = consoleLog.filter(
    (m) => ['error', 'pageerror', 'crash'].includes(m.type) || m.type === 'http',
  );
  check(
    'no page or console errors',
    errors.length === 0,
    errors
      .slice(0, 3)
      .map((e) => e.text)
      .join(' | '),
  );
  const timings = s.stages.map((x) => ({
    id: x.id,
    elapsedMs: x.timings?.elapsedMs,
    phases: x.timings?.phases,
  }));
  say(
    `timings: ${timings.map((x) => `${x.id} ${(x.elapsedMs / 1000).toFixed(2)} s`).join(', ')}; wall ${((tEnd - t0) / 1000).toFixed(1)} s`,
  );
  const g = gpu.peak();
  say(`GPU: whole ${g?.peakMiB} MiB peak, test Chrome process ${g?.processPeakMiB ?? 'n/a'} MiB`);
  writeFileSync(
    `${out}/report.json`,
    JSON.stringify(
      {
        machine,
        adapter,
        proposed,
        timings,
        gpu: g,
        checks,
        console: consoleLog,
      },
      null,
      1,
    ),
  );
  say(`report: ${out}/report.json`);
  process.exitCode = checks.every((c) => c.ok) ? 0 : 1;
} catch (e) {
  console.error(e);
  writeFileSync(
    `${out}/report.json`,
    JSON.stringify({ error: String(e.stack ?? e), checks, console: consoleLog }, null, 1),
  );
  process.exitCode = 2;
} finally {
  await gpu.stop().catch(() => {});
  await browser?.close().catch(() => {});
  server.close();
}
