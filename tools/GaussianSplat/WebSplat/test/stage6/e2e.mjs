// Stage 6 acceptance through the page: the 'thermal' set from the MKV (ingest .. prepare, train,
// export), then G1's metrics on the run's held-out renders and exported PLY.
// usage: node e2e.mjs [--machine laptop|win11] [--port 8796] [--cdp URL] [--profile NAME]
//          [--reference] [--stop-at N]
//   --train-config JSON: the job's config.train (stage options, e.g. '{"refineEvery":100}')
//   --stop-at N: once a training checkpoint at or after iteration N is published, press Stop,
//     reload the page and Resume; training must continue from that checkpoint (not restart)
// Writes $WEBSPLAT_DATA/stage6/runs/<name>/ (report.json, prepare/, train/, scores); exits 1 if a
// check fails. GPU errors, shader errors or device loss fail the train stage itself.
import { existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { APP_DIST, WEBSPLAT, args, env } from '../harness/lib/env.mjs';
import { capture, connectCdp, launchLocal } from '../harness/lib/browser.mjs';
import { GpuSampler } from '../harness/lib/gpumem.mjs';
import { serve } from '../harness/lib/serve.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const a = args();
const machine = a.machine ?? 'laptop';
const port = +(a.port ?? 8796);
const data = env('WEBSPLAT_DATA');
const py = `${WEBSPLAT}../.venv/bin/python`;
const mkv = a.mkv ?? `${data}/input/orbit.mkv`;
const stopAt = a['stop-at'] ? Number(a['stop-at']) : null;
const trainConfig = a['train-config'] ? JSON.parse(a['train-config']) : null;
const name = `${machine}${stopAt ? `-stop${stopAt}` : ''}${trainConfig ? '-cfg' : ''}-${new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19)}`;
const out = `${a['runs-dir'] ?? `${data}/stage6/runs`}/${name}`; // --runs-dir DIR: elsewhere (e.g. stage 8)
mkdirSync(out, { recursive: true });
const base = `http://127.0.0.1:${port}`;
const say = (s) => console.log(`${new Date().toISOString().slice(11, 19)} ${s}`);
const consoleLog = [];
const checks = [];
const check = (n, ok, detail = '') => {
  checks.push({ name: n, ok: !!ok, detail });
  say(`${ok ? 'PASS' : 'FAIL'} ${n}${detail ? ': ' + detail : ''}`);
};

// the build under test: the Brush wasm and the bundled training worker scripts
const sha = (f) => createHash('sha256').update(readFileSync(f)).digest('hex');
// a plain build has assets/ and vendor/ at its root; a deploy release (scripts/deploy.sh) is a site
// root with them in its one releases/<release>/ directory
const appRoot = existsSync(`${APP_DIST}/assets`) ? APP_DIST
  : `${APP_DIST}/releases/${readdirSync(`${APP_DIST}/releases`).find((d) => !d.endsWith('.partial'))}`;
const assets = readdirSync(`${appRoot}/assets`);
const build = {
  wasm: sha(`${appRoot}/vendor/brush/brush_js_bg.wasm`),
  ...Object.fromEntries(['train.stage.worker', 'brush-worker'].map((n) => {
    const f = assets.filter((a) => a.startsWith(`${n}-`) && a.endsWith('.js'));
    if (f.length !== 1) throw new Error(`build: ${f.length} ${n} assets`);
    return [n, sha(`${appRoot}/assets/${f[0]}`)];
  })),
};
build.id = createHash('sha256').update(JSON.stringify(build)).digest('hex').slice(0, 16);
// the laptop GPU must be idle when a run starts (other compute processes invalidate it)
// gpuBefore: the compute processes nvidia-smi listed (an empty list = idle), or null if the query
// failed (no evidence: the scorer counts such a laptop run invalid)
let gpuBefore = null;
let gpuQuery = null;
if (machine !== 'win11') {
  const q = spawnSync('nvidia-smi', ['--query-compute-apps=pid,process_name,used_memory', '--format=csv,noheader'],
    { encoding: 'utf8' });
  gpuQuery = { status: q.status, error: q.error ? String(q.error) : null, stderr: (q.stderr ?? '').slice(0, 300) };
  if (q.status === 0 && !q.error) gpuBefore = q.stdout.trim().split('\n').filter(Boolean);
  if (gpuBefore === null) console.log(`GPU idle check failed: ${JSON.stringify(gpuQuery)}`);
  else if (gpuBefore.length) console.log(`GPU not idle at start: ${gpuBefore.join('; ')}`);
}
const server = await serve(APP_DIST, port, '127.0.0.1', {});
const profile = `${data}/harness/profiles/${a.profile ?? `stage6-${machine}`}`;
const gpu = new GpuSampler({ sshHost: machine === 'win11' ? 'win11' : null, match: machine === 'win11' ? 'chrome.exe' : profile });
let browser;
let ticker = null;
try {
  browser = machine === 'win11' ? await connectCdp(a.cdp ?? 'http://127.0.0.1:9345') : await launchLocal({ profile, adapter: 'high-performance' });
  const page = await browser.context.newPage();
  capture(page, consoleLog, 'tab1');
  page.on('console', (m) => m.type() === 'error' && say(`console.error: ${m.text().slice(0, 300)}`));
  page.on('pageerror', (e) => say(`pageerror: ${e.message}`));
  await gpu.start();
  await page.goto(`${base}/__blank`);
  await page.evaluate(async () => {
    localStorage.clear();
    await (await navigator.storage.getDirectory()).removeEntry('websplat', { recursive: true }).catch(() => {});
  });
  const snap = () => page.evaluate(() => window.websplat.snapshot());
  const stage = (s, id) => s.stages.find((x) => x.id === id);
  const setFile = async () => {
    if (machine === 'win11') {
      await page.setInputFiles('#file', { name: 'orbit.mkv', mimeType: 'video/x-matroska', buffer: readFileSync(mkv) });
    } else {
      await page.setInputFiles('#file', mkv);
    }
  };
  const t0 = Date.now();
  await page.goto(`${base}/index.html?pipeline=thermal&adapter=high-performance`);
  await page.waitForSelector('#file', { state: 'visible' });
  await setFile();
  await page.waitForFunction(() => window.websplat?.snapshot, null, { timeout: 120000 });
  const device = await page.evaluate(() => window.websplat.device);
  check('WebGPU adapter is the discrete NVIDIA GPU', device?.ok && /nvidia/i.test(device?.adapter?.vendor ?? JSON.stringify(device)),
    JSON.stringify({ ok: device?.ok, gpuClass: device?.gpuClass, vendor: device?.adapter?.vendor, arch: device?.adapter?.architecture }));
  if (trainConfig) {
    // the controller holds the job's config object; stage options are read when train starts
    await page.evaluate((c) => { window.websplat.job().config.train = c; }, trainConfig);
  }
  await page.click('#start');
  await page.waitForSelector('#proposal', { state: 'visible', timeout: 300000 });
  if (a.reference) {
    await page.fill('[name=lat]', '-35.5802022');
    await page.fill('[name=lon]', '148.9065438');
  }
  await page.click('#proposal-form button[type=submit]');
  ticker = setInterval(async () => {
    try {
      const x = await snap();
      say(x.stages.map((t) => `${t.id}:${t.state}${t.phase ? '/' + t.phase : ''}`
        + (t.progress ? ` ${+(+t.progress.done).toFixed(0)}/${t.progress.total ?? '?'}` : '')
        + (t.progress?.extra?.gaussians ? ` ${t.progress.extra.gaussians}g` : '')
        + (t.id === 'train' && t.remainingSec != null ? ` eta ${Math.round(t.remainingSec)}s` : '')
        + (t.error ? ` ERROR ${t.error.message}` : '')).join(' | '));
    } catch { /* page reloading */ }
  }, 15000);

  const etaSamples = []; // [elapsed s in train, iterations, remaining s] for the ETA check
  const etaTimer = setInterval(async () => {
    try {
      const t = stage(await snap(), 'train');
      if (t?.state === 'running' && t.remainingSec != null)
        etaSamples.push({ wall: Date.now(), done: t.progress?.done, remaining: t.remainingSec, gaussians: t.progress?.extra?.gaussians });
    } catch { /* reloading */ }
  }, 10000);

  // every 2 s: stage states, priors, remaining times and the overall estimate (test/stage8/eta_check.mjs)
  const samples = [];
  const sampleTimer = setInterval(async () => {
    try {
      const x = await snap();
      samples.push({
        wall: Date.now(),
        overall: x.overall?.remainingSec ?? null,
        stages: x.stages.map((t) => ({ id: t.id, state: t.state, done: t.progress?.done ?? null,
          total: t.progress?.total ?? null, remaining: t.remainingSec, prior: t.priorSec })),
      });
    } catch { /* reloading */ }
  }, 2000);

  let resume = null;
  if (stopAt) {
    await page.waitForFunction((n) => {
      const t = window.websplat.snapshot().stages.find((x) => x.id === 'train');
      return t.state === 'running' && (t.checkpoint?.done ?? 0) >= n;
    }, stopAt, { timeout: 4 * 3600000, polling: 500 });
    const before = stage(await snap(), 'train');
    const tStop = Date.now();
    await page.click('#stop');
    await page.waitForFunction(() => !window.websplat.snapshot().running, null, { timeout: 30000, polling: 100 });
    const stopMs = Date.now() - tStop;
    const paused = stage(await snap(), 'train');
    check('Stop pauses training within 3.5 s', paused.state === 'paused' && stopMs <= 3500, `${paused.state} after ${stopMs} ms`);
    check('training kept its published checkpoint', paused.checkpoint?.done >= stopAt, JSON.stringify(paused.checkpoint));
    await page.reload();
    await page.waitForFunction(() => window.websplat?.ready, null, { timeout: 120000 });
    if (await page.locator('#source').isVisible()) await setFile();
    const after = stage(await snap(), 'train');
    check('after reload the job reopens with the checkpoint', after.checkpoint?.generation === paused.checkpoint?.generation,
      `${after.state}, ${JSON.stringify(after.checkpoint)}`);
    resume = { stoppedAt: before.progress?.done, checkpoint: paused.checkpoint };
    // start() resolves when the run ends: don't wait for it here
    await page.evaluate(() => { window.websplat.start(); });
    // training continues: its first progress after resuming is at the checkpoint, not 0
    const first = await page.waitForFunction(() => {
      const t = window.websplat.snapshot().stages.find((x) => x.id === 'train');
      return t.state === 'running' && t.phase === 'train' && t.progress ? t.progress.done : false;
    }, null, { timeout: 600000, polling: 200 }).then((h) => h.jsonValue());
    check('resumed training continues from the checkpoint', first >= resume.checkpoint.done,
      `first progress ${first}, checkpoint ${resume.checkpoint.done}`);
  }
  await page.waitForFunction(() => {
    const s = window.websplat.snapshot();
    return !s.running && (s.stages.every((x) => x.state === 'done') || s.stages.some((x) => x.state === 'failed'));
  }, null, { timeout: 4 * 3600000, polling: 1000 });
  clearInterval(ticker);
  clearInterval(etaTimer);
  clearInterval(sampleTimer);
  { // a final sample: the 2 s sampler may stop before the last stage reports done
    const x = await snap();
    samples.push({ wall: Date.now(), overall: x.overall?.remainingSec ?? null, stages: x.stages.map((t) => ({ id: t.id,
      state: t.state, done: t.progress?.done ?? null, total: t.progress?.total ?? null, remaining: t.remainingSec, prior: t.priorSec })) });
  }
  writeFileSync(`${out}/samples.json`, JSON.stringify(samples));
  const v = await page.evaluate(() => window.websplat.verify().then((r) => ({ ok: r.ok, files: r.files, bad: r.bad ?? r.error }), (e) => ({ ok: false, bad: String(e) })));
  check('the app\'s verify() passes (manifest, output hashes)', v.ok, `${v.files} files${v.ok ? '' : ', bad: ' + JSON.stringify(v.bad).slice(0, 300)}`);
  ticker = null;
  const s = await snap();
  const tEnd = Date.now();
  for (const st of s.stages) check(`${st.id} done`, st.state === 'done', st.error?.message ?? '');
  const job = await page.evaluate(() => window.websplat.job());
  const tr = job.stages.train;
  const exp = job.stages.export;
  check('export published its downloads (splat.ply, splat.spz, meta.json)',
    exp?.state === 'done' && ['splat.ply', 'splat.spz', 'meta.json'].every((n) => exp.outputs?.[n]?.bytes > 0),
    Object.keys(exp?.outputs ?? {}).join(','));

  // copy files out of OPFS in 8 MB chunks
  const copy = async (path, dest) => {
    const parts = [];
    for (let off = 0; ; off += 8 << 20) {
      const b64 = await page.evaluate(async ([p, off]) => {
        let d = await (await navigator.storage.getDirectory()).getDirectoryHandle('websplat');
        const ps = p.split('/');
        for (const x of ps.slice(0, -1)) d = await d.getDirectoryHandle(x);
        const f = await (await d.getFileHandle(ps.at(-1))).getFile();
        if (off >= f.size) return null;
        const u = new Uint8Array(await f.slice(off, off + (8 << 20)).arrayBuffer());
        let str = '';
        for (let i = 0; i < u.length; i += 0x8000) str += String.fromCharCode(...u.subarray(i, i + 0x8000));
        return btoa(str);
      }, [path, off]);
      if (b64 == null) break;
      parts.push(Buffer.from(b64, 'base64'));
    }
    mkdirSync(dirname(dest), { recursive: true });
    writeFileSync(dest, Buffer.concat(parts));
  };
  if (tr?.state === 'done') {
    for (const [n, o] of Object.entries(job.stages.prepare.outputs)) await copy(o.path, `${out}/prepare/${n}`);
    for (const [n, o] of Object.entries(tr.outputs)) await copy(o.path, `${out}/train/${n}`);
    const train = JSON.parse(readFileSync(`${out}/train/train.json`, 'utf8'));
    if (trainConfig?.refineEvery) check('train config applied', train.brush_config['refine-every'] === trainConfig.refineEvery, `${train.brush_config['refine-every']}`);
    check('train.json follows the stage 6 contract', train.format === 'websplat.train' && train.version === 1
      && train.splat === 'splat.ply' && train.rasterize_mode === 'classic' && train.eval?.views?.length === 13 && train.standin === false,
      JSON.stringify({ gaussians: train.gaussians, views: train.eval?.views?.length }));
    check('no Brush working files published', !Object.keys(tr.outputs).some((n) => n.startsWith('brush/')));
    check('Gaussian cap enforced', train.gaussians <= train.brush_config['max-splats'], `${train.gaussians}`);
    if (stopAt) {
      check('result records the resume', tr.result?.resumed_from?.includes(resume.checkpoint.done), JSON.stringify(tr.result));
    }
    // G1's metrics on this run's own dataset (its float16 targets as ground truth)
    const run = (cmd, argv) => spawnSync(cmd, argv, { encoding: 'utf8' });
    const g1 = `${WEBSPLAT}test/g1`;
    run(py, [`${here}/g1_view.py`, `${out}/prepare`, `${out}/g1view`]);
    const sc = run(py, [`${g1}/metrics.py`, 'score', `${out}/train/eval`, `${out}/g1view`, `${out}/metrics.json`]);
    check('held-out renders score (complete, finite)', sc.status === 0, sc.stderr.slice(-300));
    const rp = run(py, [`${g1}/render_ply.py`, `${out}/train/splat.ply`, `${out}/g1view`, `${out}/ply_render.npz`]);
    const ex = run(py, [`${g1}/metrics.py`, 'export', `${out}/train/eval`, `${out}/ply_render.npz`, `${out}/g1view`]);
    check('exported PLY decodes to the trainer renders (criterion 9)', rp.status === 0 && ex.status === 0, (ex.stdout + ex.stderr).replace(/\s+/g, ' ').slice(0, 200));
    const cmp = run(py, [`${g1}/metrics.py`, 'compare', `${out}/metrics.json`, `${data}/g1/metrics_baseline_sh.json`, `${data}/g1/baseline_sh_rep2/metrics_g1.json`]);
    writeFileSync(`${out}/compare.txt`, cmp.stdout + cmp.stderr);
    for (const line of cmp.stdout.trim().split('\n')) check(`G1 criterion ${line.slice(0, 26).trim()}`, line.includes('PASS'), line.slice(26).trim());
    // train.py's refinement rules on the result: nothing outside the bounds, no growth after refine stop
    writeFileSync(`${out}/eta.json`, JSON.stringify({ eta: etaSamples }));
    const bg = run(py, [`${here}/check_bounds_growth.py`, `${out}/prepare`, `${out}/train/splat.ply`, '--report', `${out}/eta.json`]);
    check('no Gaussian more than 1 cm outside the bounds (train.py drift), no growth after refine stop', bg.status === 0, bg.stdout.trim().replace(/\n/g, ' | ').slice(0, 300));
    // the stage's Brush input for this run's dataset (the stage removes its copy after training)
    const mk = run(process.execPath, [`${here}/make_brush_dataset.mjs`, `${out}/prepare`, `${out}/brush-check`]);
    const bd = run(py, [`${here}/check_brush_dataset.py`, `${out}/prepare`, `${out}/brush-check`]);
    check('Brush input for this run passes check_brush_dataset.py', mk.status === 0 && bd.status === 0,
      (mk.stderr + bd.stdout.trim().split('\n').at(-1)).slice(-200));
  }
  await gpu.stop();
  const errors = consoleLog.filter((m) => ['error', 'pageerror', 'crash'].includes(m.type) || m.type === 'http');
  check('no page or console errors', errors.length === 0, errors.slice(0, 3).map((e) => e.text).join(' | '));
  const g = gpu.peak();
  const timings = s.stages.map((x) => ({ id: x.id, elapsedMs: x.timings?.elapsedMs, phases: x.timings?.phases }));
  say(`timings: ${timings.map((x) => `${x.id} ${((x.elapsedMs ?? 0) / 1000).toFixed(1)} s`).join(', ')}; wall ${((tEnd - t0) / 1000).toFixed(0)} s`);
  say(`GPU: whole ${g?.peakMiB} MiB peak, test Chrome ${g?.processPeakMiB ?? 'n/a'} MiB`);
  writeFileSync(`${out}/report.json`, JSON.stringify({ machine, build, gpuBefore, gpuQuery, trainConfig, timings, train: tr?.result, trainTimings: tr?.timings, resume, eta: etaSamples, gpu: g, checks, console: consoleLog }, null, 1));
  say(`report: ${out}/report.json`);
  process.exitCode = checks.every((c) => c.ok) ? 0 : 1;
} catch (e) {
  console.error(e);
  writeFileSync(`${out}/report.json`, JSON.stringify({ machine, build, gpuBefore, gpuQuery, error: String(e.stack ?? e), checks, console: consoleLog }, null, 1));
  process.exitCode = 2;
} finally {
  if (ticker) clearInterval(ticker);
  await gpu.stop().catch(() => {});
  await browser?.close().catch(() => {});
  server.close();
}
process.exit(process.exitCode ?? 0);
