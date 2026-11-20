// The real stages registered so far on the canonical input, through the page: choose the MKV,
// confirm the proposal, run to the end; records per-stage timings, GPU memory and console.
// After select: features (3), match + verify (4) and map (5); the model is checked against the
// native reference with G2's tolerances (test/g2/compare_models.py) and the verified geometry
// against the reference database (>= 90% of its pairs and inliers). Scene preparation (5b): the
// dataset against train.py's own scene loading on the run's model (test/stage5b).
// usage: node thermal.mjs [--machine laptop|win11] [--mkv PATH] [--port 8790] [--cdp URL] [--reference]
//   --stop-map: Stop during map round 1 and resume from its checkpoint
//   --select-mode single|redecode|cache (default single)
//   --reference: use the reference extraction's target (CONTRACTS.md stage 2) instead of the proposal
//   --profile NAME: Chrome profile directory name under $WEBSPLAT_DATA/harness/profiles
//   --trained PLY: the trained splat the training stand-in loads before the export stage
//     (default G1's laptop browser run, $WEBSPLAT_DATA/g1/final_laptop/export.ply)
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { APP_DIST, WEBSPLAT, args, env } from './lib/env.mjs';
import { capture, connectCdp, launchLocal } from './lib/browser.mjs';
import { GpuSampler } from './lib/gpumem.mjs';
import { serve } from './lib/serve.mjs';

const a = args();
const machine = a.machine ?? 'laptop';
const port = +(a.port ?? 8790);
const adapter = a.adapter ?? 'high-performance';
const data = env('WEBSPLAT_DATA');
const mkv = a.mkv ?? `${data}/input/orbit.mkv`;
const out = `${data}/harness/runs/thermal-${machine}-${a['select-mode'] ?? 'single'}${a['stop-map'] ? '-stopmap' : ''}-${new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19)}`;
mkdirSync(out, { recursive: true });
const base = `http://127.0.0.1:${port}`;
const say = (s) => console.log(`${new Date().toISOString().slice(11, 19)} ${s}`);
const consoleLog = [];
const checks = [];
const check = (name, ok, detail = '') => {
  checks.push({ name, ok: !!ok, detail });
  say(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? ': ' + detail : ''}`);
};

// stage 6 is a stand-in until training is in the app: it loads this trained splat
const trained = a.trained ?? `${data}/g1/final_laptop/export.ply`;
writeFileSync(
  `${out}/standin.json`,
  JSON.stringify({ train: { files: { 'splat.ply': '/__data/trained.ply' } } }),
);
const server = await serve(APP_DIST, port, '127.0.0.1', {
  '/__data/trained.ply': trained,
  '/__data/standin.json': `${out}/standin.json`,
});
// --profile NAME: a separate Chrome profile (another harness run may hold the default one)
const profile = `${data}/harness/profiles/${a.profile ?? `thermal-${machine}-${adapter}`}`;
const gpu = new GpuSampler({
  sshHost: machine === 'win11' ? 'win11' : null,
  match: machine === 'win11' ? 'chrome.exe' : profile,
});
let browser;
let ticker = null; // progress lines; cleared in finally so an error or timeout cannot keep the process alive
try {
  browser =
    machine === 'win11'
      ? await connectCdp(a.cdp ?? 'http://127.0.0.1:9345')
      : await launchLocal({ profile, adapter });
  const page = await browser.context.newPage();
  capture(page, consoleLog, 'tab1');
  page.on(
    'console',
    (m) => m.type() === 'error' && say(`console.error: ${m.text().slice(0, 300)}`),
  );
  page.on('pageerror', (e) => say(`pageerror: ${e.message}`));
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
    `${base}/index.html?pipeline=thermal&adapter=${adapter}&standin=/__data/standin.json${a['select-mode'] ? `&selectMode=${a['select-mode']}` : ''}`,
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
  // a progress line every 15 s (stage states, progress, phase)
  ticker = setInterval(async () => {
    try {
      const x = await page.evaluate(() => window.websplat.snapshot());
      say(
        x.stages
          .map(
            (t) =>
              `${t.id}:${t.state}${t.phase ? '/' + t.phase : ''}` +
              (t.progress ? ` ${+(+t.progress.done).toFixed(2)}/${t.progress.total ?? '?'}` : '') +
              (t.error ? ` ERROR ${t.error.message}` : ''),
          )
          .join(' | ') + (x.needs ? ` needs ${x.needs}` : ''),
      );
    } catch {}
  }, 15000);
  if (a['stop-map']) {
    // Stop inside map round 1 (after round 0's checkpoint), then start again: map must pause
    // within the 3 s Stop contract and resume after round 0 (result.resumedAfter === 0)
    await page.waitForFunction(
      () => {
        const m = window.websplat.snapshot().stages.find((x) => x.id === 'map');
        return m.state === 'running' && (m.progress?.done ?? 0) >= 1.3;
      },
      null,
      { timeout: 3600000, polling: 200 },
    );
    const tStop = Date.now();
    await page.click('#stop');
    await page.waitForFunction(() => !window.websplat.snapshot().running, null, {
      timeout: 30000,
      polling: 100,
    });
    const stopMs = Date.now() - tStop;
    const m = (await page.evaluate(() => window.websplat.snapshot())).stages.find(
      (x) => x.id === 'map',
    );
    check(
      'Stop pauses map within 3.5 s',
      m.state === 'paused' && stopMs <= 3500,
      `${m.state} after ${stopMs} ms`,
    );
    check(
      'map kept its round checkpoint',
      m.checkpoint?.generation != null,
      JSON.stringify(m.checkpoint),
    );
    await page.evaluate(() => window.websplat.start());
  }
  await page.waitForFunction(
    () => {
      const s = window.websplat.snapshot();
      return (
        !s.running &&
        (s.stages.every((x) => x.state === 'done') || s.stages.some((x) => x.state === 'failed'))
      );
    },
    null,
    { timeout: 3600000, polling: 1000 },
  );
  clearInterval(ticker);
  ticker = null;
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
  // stages 3-5: fetch the model and geometry from OPFS, compare natively
  const files = await page.evaluate(async () => {
    const m = window.websplat.job();
    const open = async (p) => {
      let d = await (await navigator.storage.getDirectory()).getDirectoryHandle('websplat');
      const parts = p.split('/');
      for (const x of parts.slice(0, -1)) d = await d.getDirectoryHandle(x);
      return (await d.getFileHandle(parts.at(-1))).getFile();
    };
    const b64 = async (f) => {
      const u = new Uint8Array(await f.arrayBuffer());
      let s = '';
      for (let i = 0; i < u.length; i += 0x8000)
        s += String.fromCharCode(...u.subarray(i, i + 0x8000));
      return btoa(s);
    };
    const want = {
      map: [
        'final/cameras.bin',
        'final/images.bin',
        'final/points3D.bin',
        'final/rigs.bin',
        'final/frames.bin',
        'model.json',
      ],
      match: ['geometry.bin', 'geometry.json', 'matches.json', 'pairs.json'],
      features: ['features.json'],
    };
    const out = {};
    for (const [stage, names] of Object.entries(want)) {
      const outs = m.stages[stage]?.outputs ?? {};
      for (const n of names)
        out[`${stage}/${n}`] = outs[n] ? await b64(await open(outs[n].path)) : null;
    }
    return out;
  });
  const missing = Object.entries(files)
    .filter(([, v]) => v == null)
    .map(([k]) => k);
  check('features, match and map published their outputs', missing.length === 0, missing.join(','));
  for (const [k, v] of Object.entries(files)) {
    if (v == null) continue;
    mkdirSync(`${out}/${k.split('/').slice(0, -1).join('/')}`, { recursive: true });
    writeFileSync(`${out}/${k}`, Buffer.from(v, 'base64'));
  }
  const py = `${WEBSPLAT}../.venv/bin/python`;
  const runPy = (script, argv) => {
    const r = spawnSync(py, [`${WEBSPLAT}test/g2/${script}`, ...argv], { encoding: 'utf8' });
    return { ok: r.status === 0, out: r.stdout + r.stderr };
  };
  if (a['stop-map']) {
    const r = await page.evaluate(() => window.websplat.job().stages.map.result);
    check('map resumed after round 0', r?.resumedAfter === 0, `resumedAfter ${r?.resumedAfter}`);
  }
  if (!missing.length) {
    const fj = JSON.parse(readFileSync(`${out}/features/features.json`, 'utf8'));
    check(
      'features: 201 images with SIFT keypoints',
      fj.images === 201 && fj.counts.every((c) => c > 1000),
      `${fj.keypoints} keypoints`,
    );
    const g = runPy('compare_geometry.py', [
      `${data}/stage4/native/geometry.bin`,
      `${out}/match/geometry.bin`,
      '--min-ratio',
      '0.9',
      '--json',
      `${out}/geometry_vs_ref.json`,
    ]);
    const gj = JSON.parse(readFileSync(`${out}/geometry_vs_ref.json`, 'utf8'));
    check(
      'verified pairs and inliers >= 90% of the native database',
      g.ok,
      `pairs ${gj.b.verified}/${gj.a.verified}, inliers ${gj.b.inliers}/${gj.a.inliers}`,
    );
    const mr = runPy('compare_models.py', [
      `${data}/ref/work/colmap/sparse/final`,
      `${out}/map/final`,
      '--json',
      `${out}/model_vs_ref.json`,
    ]);
    const mj = JSON.parse(readFileSync(`${out}/model_vs_ref.json`, 'utf8'));
    check(
      'model within G2 tolerances of the native reference',
      mr.ok,
      `position median ${mj.position_m.median.toFixed(3)} m p95 ${mj.position_m.p95.toFixed(3)} m, rotation median ${mj.rotation_deg.median.toFixed(3)} deg p95 ${mj.rotation_deg.p95.toFixed(3)} deg, ${mj.points.model} points, reprojection ${mj.reproj_px.model.toFixed(3)} px; ${Object.entries(
        mj.checks,
      )
        .filter(([, v]) => !v)
        .map(([k]) => k)
        .join(', ')}`,
    );
  }
  // stage 5b: copy the dataset out of OPFS (chunked: CDP strings are limited) and compare it with
  // train.py's own scene loading on this run's model (test/stage5b)
  const prep = await page.evaluate(() => window.websplat.job().stages.prepare?.outputs ?? null);
  const prepFiles = ['dataset.json', 'points.f32', 'ceiling.f32', 'targets.f16'];
  check(
    'prepare published its outputs',
    prep && prepFiles.every((n) => prep[n]),
    prep ? prepFiles.filter((n) => !prep[n]).join(',') : 'no outputs',
  );
  if (prep && prepFiles.every((n) => prep[n]) && !missing.length) {
    mkdirSync(`${out}/prepare`, { recursive: true });
    const CHUNK = 8 << 20;
    for (const n of prepFiles) {
      const parts = [];
      for (let off = 0; off < prep[n].bytes; off += CHUNK) {
        const b64 = await page.evaluate(
          async ([p, off, len]) => {
            let d = await (await navigator.storage.getDirectory()).getDirectoryHandle('websplat');
            const ps = p.split('/');
            for (const x of ps.slice(0, -1)) d = await d.getDirectoryHandle(x);
            const f = await (await d.getFileHandle(ps.at(-1))).getFile();
            const u = new Uint8Array(await f.slice(off, off + len).arrayBuffer());
            let s = '';
            for (let i = 0; i < u.length; i += 0x8000)
              s += String.fromCharCode(...u.subarray(i, i + 0x8000));
            return btoa(s);
          },
          [prep[n].path, off, CHUNK],
        );
        parts.push(Buffer.from(b64, 'base64'));
      }
      writeFileSync(`${out}/prepare/${n}`, Buffer.concat(parts));
    }
    const s5 = `${WEBSPLAT}test/stage5b`;
    const dump = spawnSync(
      py,
      [`${s5}/dump_scene.py`, `${data}/ref/work`, `${out}/map/final`, `${out}/prepare-native`],
      { encoding: 'utf8' },
    );
    check(
      'train.py scene loading on this model (dump_scene.py)',
      dump.status === 0,
      dump.stderr.slice(-300),
    );
    if (dump.status === 0) {
      const c = spawnSync(
        py,
        [
          `${s5}/compare_dataset.py`,
          `${out}/prepare-native`,
          `${out}/prepare`,
          ...(a.reference ? ['--frozen'] : []),
          '--json',
          `${out}/dataset_vs_native.json`,
        ],
        { encoding: 'utf8' },
      );
      const fails = c.stdout.split('\n').filter((l) => l.startsWith('FAIL'));
      let info = {};
      try {
        info = JSON.parse(readFileSync(`${out}/dataset_vs_native.json`, 'utf8')).info;
      } catch {}
      check(
        'dataset identical to train.py scene loading (views, holdouts, scale, targets, points, settings)',
        c.status === 0,
        fails.length
          ? fails.slice(0, 4).join(' | ')
          : `${c.stdout.trim().split('\n').at(-1)}; ${JSON.stringify(info)}`,
      );
    }
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
        results: Object.fromEntries(s.stages.map((x) => [x.id, x.timings])),
        stageResults: await page.evaluate(() =>
          Object.fromEntries(
            Object.entries(window.websplat.job().stages).map(([k, v]) => [k, v.result]),
          ),
        ),
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
  if (ticker) clearInterval(ticker);
  await gpu.stop().catch(() => {});
  await browser?.close().catch(() => {});
  server.close();
}
// nothing (sampler, server sockets) may keep a failed run alive
process.exit(process.exitCode ?? 0);
