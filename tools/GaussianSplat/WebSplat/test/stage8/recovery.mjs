// Stage 8 recovery tests through the page (notes/stage8.md R1-R8), laptop Chrome, thermal set
// from the MKV with training shortened to 6000 iterations. Each scenario checks the message the
// page shows, the app's verify() afterwards and, where registered, that the job then completes.
// usage: node recovery.mjs [--only R1,R2,..] [--port 8798] [--iterations 6000]
//   (WEBSPLAT_APP_DIST: the build to serve, e.g. a deploy release)
// Writes $WEBSPLAT_DATA/stage8/recovery-<time>/report.json; exit 1 if a check fails.
import { mkdirSync, writeFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { APP_DIST, args, env } from '../harness/lib/env.mjs';
import { capture, launchLocal } from '../harness/lib/browser.mjs';
import { serve } from '../harness/lib/serve.mjs';

const a = args();
const data = env('WEBSPLAT_DATA');
const port = +(a.port ?? 8798);
const iterations = +(a.iterations ?? 6000);
const only = a.only ? a.only.split(',') : null;
const mkv = `${data}/input/orbit.mkv`;
const noTelemetry = `${data}/stage8/orbit-notelemetry.mkv`;
const out = `${data}/stage8/recovery-${new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19)}`;
mkdirSync(out, { recursive: true });
const base = `http://127.0.0.1:${port}`;
const say = (s) => console.log(`${new Date().toISOString().slice(11, 19)} ${s}`);
const checks = [];
let scenario = '';
const check = (name, ok, detail = '') => {
  checks.push({ scenario, name, ok: !!ok, detail: String(detail).slice(0, 400) });
  say(`${ok ? 'PASS' : 'FAIL'} ${scenario} ${name}${detail ? ': ' + String(detail).slice(0, 300) : ''}`);
};
const consoleLog = [];

const server = await serve(APP_DIST, port, '127.0.0.1', {});
const browser = await launchLocal({ profile: `${data}/harness/profiles/stage8-recovery`, adapter: 'high-performance' });
let page;

const gpuIdle = () => {
  const q = spawnSync('nvidia-smi', ['--query-compute-apps=pid,process_name', '--format=csv,noheader'], { encoding: 'utf8' });
  return q.status === 0 ? q.stdout.trim().split('\n').filter(Boolean) : null;
};
const snap = () => page.evaluate(() => window.websplat.snapshot());
const stage = (s, id) => s.stages.find((x) => x.id === id);
const logText = () => page.evaluate(() => document.getElementById('log')?.textContent ?? '');
const waitFor = (fn, arg, timeout = 1800000, polling = 100) =>
  page.waitForFunction(fn, arg, { timeout, polling });
const waitStage = (id, pred, timeout) =>
  waitFor(([id, pred]) => {
    const t = window.websplat?.snapshot?.()?.stages.find((x) => x.id === id);
    return t && new Function('t', `return ${pred}`)(t);
  }, [id, pred], timeout, 20);
const notRunning = (timeout = 30000) => waitFor(() => window.websplat?.snapshot && !window.websplat.snapshot().running, null, timeout, 50);
const finished = (timeout = 3600000) =>
  waitFor(() => {
    const s = window.websplat?.snapshot?.();
    return s && !s.running && (s.stages.every((x) => x.state === 'done') || s.stages.some((x) => x.state === 'failed'));
  }, null, timeout, 500);
const verify = () => page.evaluate(() => window.websplat.verify().then((r) => ({ ok: r.ok, files: r.files, bad: r.bad ?? r.error }), (e) => ({ ok: false, bad: String(e) })));
// Resume; the page defines start() once the job is open (it may first ask for the recording)
async function resume() {
  try {
    await page.waitForFunction(() => typeof window.websplat?.start === 'function', null, { timeout: 60000 });
  } catch {
    const why = await page.evaluate(() => ({
      source: !document.getElementById('source')?.hidden && document.getElementById('source-status')?.textContent,
      unreadable: !document.getElementById('unreadable')?.hidden && document.getElementById('unreadable-text')?.textContent,
      blocked: !document.getElementById('blocked')?.hidden,
      errors: window.websplat?.errors,
      log: document.getElementById('log')?.textContent?.slice(0, 400),
    }));
    throw new Error(`the page offers no Resume: ${JSON.stringify(why)}`);
  }
  await page.evaluate(() => { window.websplat.start(); });
}
const url = (params = {}) => `${base}/index.html?${new URLSearchParams({ pipeline: 'thermal', adapter: 'high-performance', ...params })}`;

async function newPage() {
  if (page) await page.close().catch(() => {});
  page = await browser.context.newPage();
  capture(page, consoleLog, scenario);
  // count the page's workers (R1: a stage must be mid-work in its worker when Stop arrives)
  await page.addInitScript(() => {
    const W = window.Worker;
    window.__workers = { made: 0, live: 0 };
    window.Worker = class extends W {
      constructor(...a) {
        super(...a);
        this.__alive = true;
        window.__workers.made++;
        window.__workers.live++;
      }
      terminate() {
        if (this.__alive) window.__workers.live--;
        this.__alive = false;
        super.terminate();
      }
    };
  });
}

// a fresh job: empty storage, choose the MKV, shorten training, confirm the proposal
async function openFresh({ params = {}, file = mkv, confirm = true, beforeStart = null } = {}) {
  await newPage();
  await page.goto(`${base}/__blank`);
  await page.evaluate(async () => {
    localStorage.clear();
    await (await navigator.storage.getDirectory()).removeEntry('websplat', { recursive: true }).catch(() => {});
  });
  await page.goto(url(params));
  await page.waitForSelector('#file', { state: 'visible' });
  await page.setInputFiles('#file', file);
  await page.waitForFunction(() => window.websplat?.snapshot, null, { timeout: 120000 });
  await page.evaluate((n) => { window.websplat.job().config.prepare = { iterations: n }; }, iterations);
  if (beforeStart) await beforeStart();
  await page.click('#start');
  if (!confirm) return;
  await page.waitForSelector('#proposal', { state: 'visible', timeout: 300000 });
  await page.click('#proposal-form button[type=submit]');
}

// reload with other URL parameters (e.g. without a fault); the remembered job reopens
async function reopen(params = {}) {
  await page.goto(url(params));
  await page.waitForFunction(() => window.websplat?.ready, null, { timeout: 120000 });
}

// a job file out of OPFS in 8 MB chunks (to keep evidence, e.g. a splat export refused)
async function saveOpfs(path, dest) {
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
  writeFileSync(dest, Buffer.concat(parts));
}

async function completes(what) {
  await finished();
  const s = await snap();
  const failed = s.stages.find((x) => x.state === 'failed');
  if (failed?.id === 'export') {
    const p = await page.evaluate(() => window.websplat.job().stages.train?.outputs?.['splat.ply']?.path);
    if (p) {
      await saveOpfs(p, `${out}/${scenario}-export-refused-splat.ply`);
      say(`kept the trained splat the export refused: ${out}/${scenario}-export-refused-splat.ply`);
    }
  }
  check(`${what}: the job completes`, s.stages.every((x) => x.state === 'done'), failed ? `${failed.id}: ${failed.error?.message}` : '');
  const v = await verify();
  check(`${what}: verify() passes`, v.ok, `${v.files} files ${v.ok ? '' : JSON.stringify(v.bad)}`);
}

const ORDER = ['ingest', 'select', 'features', 'match', 'map', 'prepare', 'train', 'export'];

// the page's expected seconds until stage `id` has run (running stage's remaining + priors), x2
async function timeoutFor(id) {
  const s = await snap();
  let sec = 0;
  for (const t of s.stages) {
    if (t.state !== 'done') sec += t.remaining ?? t.remainingSec ?? t.priorSec ?? 60;
    if (t.id === id) break;
  }
  return Math.max(30000, 2000 * sec);
}

// Stop from inside the controller's 'update' event in which stage `id` becomes running (no
// polling: ingest and select last 1-4 s); records page times of the Stop and of the pause
async function armStop(id) {
  await page.evaluate((id) => {
    const c = window.websplat.testing.controller();
    window.__stop = null;
    let madeAtStart = null;
    const h = () => {
      const t = c.snapshot().stages.find((x) => x.id === id);
      if (madeAtStart === null && t.state === 'running') madeAtStart = window.__workers.made;
      // Stop once the stage's worker reports progress, before it finishes
      const d = t.progress?.done ?? 0;
      const mid = d > 0 && (t.progress?.total == null || d < t.progress.total);
      if (!window.__stop && t.state === 'running' && mid) {
        window.__stop = { id, issued: performance.now(), paused: null, done: d, total: t.progress?.total ?? null,
          workersMade: window.__workers.made - madeAtStart, workersLive: window.__workers.live };
        c.stop();
      } else if (window.__stop && window.__stop.paused == null && !c.snapshot().running) {
        window.__stop.paused = performance.now();
        c.removeEventListener('update', h);
      }
    };
    c.addEventListener('update', h);
  }, id);
}
async function stopped(timeout) {
  await waitFor(() => window.__stop?.paused != null, null, timeout, 50);
  return page.evaluate(() => window.__stop);
}
async function stopAtStart(id) {
  const timeout = await timeoutFor(id);
  await armStop(id);
  return stopped(timeout);
}

const scenarios = {
  // R1: Stop once in every stage, Resume each time; checkpointed stages continue from their checkpoint
  async R1() {
    // ingest and select start on a user action: arm the Stop before it
    const pause = async (id, r) => {
      const after = stage(await snap(), id);
      const ok = (after.state === 'paused' || after.state === 'interrupted') && r.paused - r.issued <= 3500;
      check(`Stop during ${id} pauses it within 3.5 s`, ok, `${after.state} after ${Math.round(r.paused - r.issued)} ms`);
      check(`Stop during ${id} arrived mid-work in a running worker`, r.workersMade >= 1 && r.workersLive >= 1 && r.done > 0,
        `progress ${r.done}/${r.total}, workers made for the stage ${r.workersMade}, live ${r.workersLive}`);
    };
    try {
      await openFresh({ confirm: false, beforeStart: () => armStop('ingest') });
      await pause('ingest', await stopped(30000));
      await resume();
      await page.waitForSelector('#proposal', { state: 'visible', timeout: 120000 });
      await armStop('select');
      await page.click('#proposal-form button[type=submit]');
      await pause('select', await stopped(30000));
      await resume();
    } catch (e) {
      check('Stop during ingest and select', false, `not caught within its timeout (${String(e.message).split('\n')[0]})`);
      return;
    }
    for (const id of ORDER.slice(2)) {
      const longStage = ['match', 'map', 'train'].includes(id);
      let ms;
      let before;
      try {
        if (longStage) {
          // after some progress (train: after its first checkpoint), so the resume point is visible
          const pred = id === 'train'
            ? "t.state === 'running' && t.checkpoint?.done >= 5000"
            : "t.state === 'running' && t.progress?.done > 0.3 * (t.progress?.total ?? 1)";
          await waitStage(id, pred, await timeoutFor(id) + (id === 'train' ? 600000 : 0));
          before = stage(await snap(), id);
          const t0 = Date.now();
          await page.click('#stop');
          await notRunning();
          ms = Date.now() - t0;
        } else {
          const r = await stopAtStart(id);
          ms = r.paused - r.issued;
          before = { progress: { done: r.done } };
          check(`Stop during ${id} arrived mid-work in a running worker`, r.workersMade >= 1 && r.workersLive >= 1 && r.done > 0,
            `progress ${r.done}/${r.total}, workers made for the stage ${r.workersMade}, live ${r.workersLive}`);
        }
      } catch (e) {
        check(`Stop during ${id}`, false, `not caught within its timeout (${String(e.message).split('\n')[0]})`);
        return;
      }
      const after = stage(await snap(), id);
      const caught = after.state === 'paused' || after.state === 'interrupted';
      check(`Stop during ${id} pauses it within 3.5 s`, caught && ms <= 3500, `${after.state} after ${Math.round(ms)} ms`);
      if (['map', 'train'].includes(id) && caught) {
        const ck = after.checkpoint?.done ?? null;
        await resume();
        await waitStage(id, "t.state === 'running' && t.phase !== 'load' && t.progress", 600000);
        const p = stage(await snap(), id).progress?.done;
        check(`${id} resumes from its checkpoint`, ck != null && p >= ck, `checkpoint ${ck}, first progress ${p} (before Stop ${before.progress?.done})`);
      } else {
        await resume();
      }
    }
    await completes('after a Stop in every stage');
  },

  // R2: reload during a map round checkpoint and during a training checkpoint
  async R2() {
    await openFresh();
    for (const id of ['map', 'train']) {
      try {
        await waitStage(id, "t.state === 'checkpointing'", 1800000);
      } catch {
        check(`reload during a ${id} checkpoint`, false, 'no checkpoint write seen');
        continue;
      }
      await reopen();
      const s = await snap();
      const t = stage(s, id);
      const v = await verify();
      check(`after a reload during a ${id} checkpoint write: only published checkpoints, verify() passes`, v.ok && t.state !== 'running',
        `${t.state}, checkpoint ${JSON.stringify(t.checkpoint)}, ${v.files} files ${v.ok ? '' : JSON.stringify(v.bad)}`);
      await resume();
    }
    await completes('after reloads during checkpoint writes');
  },

  // R3: storage full on prepare's outputs, then on a training checkpoint
  async R3() {
    for (const [what, re] of [['prepare outputs', '/prepare/'], ['a training checkpoint', '/train/ckpt-']]) {
      await openFresh({ params: { testFault: JSON.stringify({ quota: re }) } });
      await finished();
      const s = await snap();
      const failed = s.stages.find((x) => x.state === 'failed');
      const text = `${failed?.error?.message ?? ''} ${s.storageError?.message ?? ''} ${await logText()}`;
      check(`storage full on ${what}: the page says the storage is full`, /storage is full/.test(text),
        `${failed?.id}: ${failed?.error?.message ?? s.storageError?.message}`);
      const v = await verify();
      check(`storage full on ${what}: verify() passes`, v.ok, `${v.files} files ${v.ok ? '' : JSON.stringify(v.bad)}`);
      await reopen();
      await resume();
      await completes(`storage full on ${what}, fault removed, Resume`);
    }
  },

  // R4: a manifest from a newer app version; stage records from an older stage version
  async R4() {
    await openFresh();
    await waitStage('select', "t.state === 'done'", 600000);
    await page.click('#stop');
    await notRunning();
    // (ii) prepare's code version changes: run to prepare done, then mark its record as older
    await resume();
    await waitStage('prepare', "t.state === 'done'", 1800000);
    await page.click('#stop');
    await notRunning();
    const kept = await page.evaluate(async () => {
      const T = window.websplat.testing;
      const job = T.job();
      job.manifest.stages.prepare.version = -1; // a version no build has
      await job.save();
      return Object.fromEntries(Object.entries(job.manifest.stages).map(([k, v]) => [k, v.generation]));
    });
    await reopen();
    await resume();
    await waitStage('prepare', "t.state === 'running' || t.state === 'done'", 600000);
    const s = await snap();
    const redo = stage(s, 'prepare');
    const upstream = ['ingest', 'select', 'features', 'match', 'map'].every((id) => stage(s, id).state === 'done' && stage(s, id).generation === kept[id]);
    check('an older stage version: earlier stages kept, that stage redone', upstream && redo.generation !== kept.prepare,
      `prepare generation ${kept.prepare} -> ${redo.generation}, upstream kept ${upstream}`);
    await completes('after an older stage version');
    // (i) a manifest from a newer app version is refused, the job's files kept
    const files = await page.evaluate(async () => {
      const T = window.websplat.testing;
      const job = T.job();
      const m = { ...job.manifest, version: job.manifest.version + 1 };
      await T.saveManifest(job.dir, m);
      return (await T.list(job.dir)).length;
    });
    await reopen();
    const un = await page.evaluate(() => ({ api: window.websplat.unreadable, text: document.getElementById('unreadable-text')?.textContent, shown: !document.getElementById('unreadable')?.hidden }));
    check('a manifest from a newer app version is refused with a message', un.shown && /another app version/.test(un.text ?? ''), un.text);
    const left = await page.evaluate(async () => {
      let d = await (await navigator.storage.getDirectory()).getDirectoryHandle('websplat');
      d = await d.getDirectoryHandle('jobs');
      let n = 0;
      for await (const [name, h] of d.entries()) if (h.kind === 'directory') for await (const _ of h.entries()) n++;
      return n;
    });
    check('the job\'s files are kept', left >= files && files > 0, `${files} entries before, ${left} after`);
  },

  // R5: a second tab on the running job is refused; the first completes
  async R5() {
    await openFresh();
    await waitStage('features', "t.state === 'running'", 600000);
    const page2 = await browser.context.newPage();
    capture(page2, consoleLog, 'R5-tab2');
    await page2.goto(url());
    await page2.waitForFunction(() => window.websplat?.ready, null, { timeout: 60000 });
    const b = await page2.evaluate(() => ({ blocked: window.websplat.blocked, text: document.getElementById('blocked')?.textContent, shown: !document.getElementById('blocked')?.hidden }));
    check('the second tab is refused with a message', b.blocked && b.shown && /another tab/i.test(b.text ?? ''), (b.text ?? '').trim().slice(0, 200));
    await page2.close();
    const s = await snap();
    check('the first tab keeps running', s.running && !s.stages.some((x) => x.state === 'failed'));
    await completes('the first tab');
  },

  // R6 / R7: device loss and a refused allocation during training
  async R6() { await gpuFault('device lost', { deviceLostAt: 2000 }, /device lost/i); },
  async R7() { await gpuFault('allocation failure', { allocAt: 2000 }, /GPU error|out of memory|allocation/i); },

  // R8: an MKV whose frames carry no telemetry
  async R8() {
    await openFresh({ file: noTelemetry, confirm: false });
    await finished(600000).catch(() => {});
    const s = await snap();
    const ingest = stage(s, 'ingest');
    const text = `${ingest.error?.message ?? ''} ${await logText()}`;
    check('the page reports the missing telemetry', /no vehicle telemetry/.test(text), ingest.error?.message ?? ingest.state);
    check('no GPU stage started', ['features', 'match', 'map', 'train'].every((id) => ['waiting', 'idle', undefined].includes(stage(s, id).state) || stage(s, id).runs === 0),
      s.stages.map((x) => `${x.id}:${x.state}`).join(' '));
    const v = await verify();
    check('verify() passes', v.ok, `${v.files} files ${v.ok ? '' : JSON.stringify(v.bad)}`);
  },
};

async function gpuFault(what, fault, re) {
  await openFresh({ params: { testFault: JSON.stringify({ train: fault }) } });
  await finished();
  const s = await snap();
  const t = stage(s, 'train');
  check(`${what}: training fails with a message naming it`, t.state === 'failed' && re.test(t.error?.message ?? ''), t.error?.message ?? t.state);
  const job = await page.evaluate(() => window.websplat.job().stages.train);
  check(`${what}: no training output published`, !job.outputs || Object.keys(job.outputs).length === 0, Object.keys(job.outputs ?? {}).join(','));
  const v = await verify();
  check(`${what}: verify() passes`, v.ok, `${v.files} files ${v.ok ? '' : JSON.stringify(v.bad)}`);
  await reopen();
  await resume();
  await completes(`${what}, fault removed, Resume`);
}

try {
  for (const [name, fn] of Object.entries(scenarios)) {
    if (only && !only.includes(name)) continue;
    scenario = name;
    const busy = gpuIdle();
    check('laptop GPU idle at start', busy !== null && busy.length === 0, busy === null ? 'nvidia-smi failed' : busy.join('; '));
    const t0 = Date.now();
    try {
      await fn();
    } catch (e) {
      check('scenario ran to the end', false, e.stack?.split('\n').slice(0, 3).join(' | ') ?? String(e));
    }
    say(`${name} took ${((Date.now() - t0) / 60000).toFixed(1)} min`);
  }
} finally {
  writeFileSync(`${out}/report.json`, JSON.stringify({ app: APP_DIST, iterations, checks, console: consoleLog }, null, 1));
  await browser.close().catch(() => {});
  server.close();
}
const failed = checks.filter((c) => !c.ok);
for (const s of [...new Set(checks.map((c) => c.scenario))])
  say(`${s}: ${checks.filter((c) => c.scenario === s && !c.ok).length ? 'FAIL' : 'PASS'}`);
say(`report: ${out}/report.json`);
process.exit(failed.length ? 1 : 0);
