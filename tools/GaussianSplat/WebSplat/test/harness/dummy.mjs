// Acceptance of the stage 0 skeleton with the dummy pipeline (PLAN.md stage 0):
// progress, states and ETA; resume after a reload mid-stage and during a checkpoint write; stop
// and resume; refusal of a second tab; per-stage timings and GPU memory.
// usage: node dummy.mjs [--machine laptop|win11] [--speed 30] [--adapter low-power] [--port 8790]
import { mkdirSync, writeFileSync } from 'node:fs';
import { hostname } from 'node:os';
import { APP_DIST, args, env } from './lib/env.mjs';
import { capture, connectCdp, launchLocal } from './lib/browser.mjs';
import { GpuSampler } from './lib/gpumem.mjs';
import { serve } from './lib/serve.mjs';
import { DESKTOP_FACTOR, PROFILES } from '../../app/src/stages/dummy/profile.js';

const a = args();
const machine = a.machine ?? 'laptop';
const speed = +(a.speed ?? 30);
const port = +(a.port ?? 8790);
const adapter = a.adapter ?? 'high-performance';
const stamp = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
const out = `${env('WEBSPLAT_DATA')}/harness/runs/${machine}-${adapter}-${stamp}`;
mkdirSync(out, { recursive: true });

const base = `http://127.0.0.1:${port}`;
const appUrl = `${base}/index.html?pipeline=dummy&speed=${speed}&adapter=${adapter}`;
const consoleLog = [];
const checks = [];
const samples = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const say = (s) => console.log(`${new Date().toISOString().slice(11, 19)} ${s}`);

function check(name, ok, detail = '') {
  checks.push({ name, ok: !!ok, detail });
  say(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? ': ' + detail : ''}`);
}

const server = await serve(APP_DIST, port);
const profile = `${env('WEBSPLAT_DATA')}/harness/profiles/${machine}-${adapter}`;
const gpu = new GpuSampler({
  sshHost: machine === 'win11' ? 'win11' : null,
  match: machine === 'win11' ? (a.gpuMatch ?? 'chrome.exe') : profile,
});
let browser;
try {
  browser =
    machine === 'win11'
      ? await connectCdp(a.cdp ?? 'http://127.0.0.1:9333')
      : await launchLocal({ profile, adapter });
  const ctx = browser.context;
  const page = await ctx.newPage();
  capture(page, consoleLog, 'tab1');
  const gpuBase = await gpu.start();

  // fresh origin storage
  await page.goto(`${base}/__blank`);
  await page.evaluate(async () => {
    const r = await navigator.storage.getDirectory();
    await r.removeEntry('websplat', { recursive: true }).catch(() => {});
  });

  const statesSeen = new Set();
  const snap = async (p = page) => {
    const s = await p.evaluate(() =>
      window.websplat?.snapshot ? window.websplat.snapshot() : null,
    );
    s?.stages.forEach((x) => statesSeen.add(x.state));
    return s;
  };
  const ready = (p = page) =>
    p.waitForFunction(() => window.websplat?.ready, null, { timeout: 60000 });
  const stage = (s, id) => s.stages.find((x) => x.id === id);
  const t0 = Date.now();

  // sampler: snapshots every 200 ms for the progress and ETA checks
  let sampling = true;
  const sampler = (async () => {
    while (sampling) {
      const s = await snap().catch(() => null);
      if (s) samples.push({ t: Date.now(), s });
      await sleep(200);
    }
  })();
  const waitFor = async (pred, what, timeoutMs = 600000) => {
    const end = Date.now() + timeoutMs;
    while (Date.now() < end) {
      const s = await snap().catch(() => null);
      if (s && pred(s)) return s;
      await sleep(20);
    }
    throw new Error(`timed out waiting for ${what}`);
  };

  await page.goto(appUrl + '&autostart=1');
  await ready();
  const device = await page.evaluate(() => window.websplat.device);
  say(
    `device: ${JSON.stringify({ ok: device.ok, adapter: device.adapter, gpuClass: device.gpuClass, smoke: device.smoke })}`,
  );
  check(
    'device check runs and reports adapter',
    device.adapter || device.reason,
    device.reason ?? device.adapter.description,
  );
  if (device.smoke?.warnings?.length) say(`device warnings: ${device.smoke.warnings.join('; ')}`);
  check(
    'WebGPU smoke test',
    device.smoke?.ok,
    device.smoke ? device.smoke.errors.join('; ') : device.reason,
  );

  // 1. reload mid-stage (no checkpoints): earlier stages keep their outputs, the stage restarts
  const before = await waitFor((s) => {
    const m = stage(s, 'match');
    return m.state === 'running' && m.progress?.done > 0.3 * m.progress.total;
  }, 'match at 30%');
  gpu.mark('reload-mid-stage');
  await page.reload();
  await ready();
  let s = await snap();
  const kept = ['ingest', 'select', 'features'].every((id) => {
    const x = stage(s, id),
      y = stage(before, id);
    return x.state === 'done' && x.generation === y.generation;
  });
  check('reload mid-stage keeps completed stages', kept);
  check(
    'reload mid-stage marks the stage interrupted',
    stage(s, 'match').state === 'interrupted',
    stage(s, 'match').state,
  );
  check(
    'page offers Resume',
    await page
      .waitForFunction(() => document.getElementById('start').textContent === 'Resume', null, {
        timeout: 5000,
      })
      .then(
        () => true,
        () => false,
      ),
  );
  await page.click('#start');
  s = await waitFor((x) => stage(x, 'match').state === 'running', 'match resumed');
  check(
    'interrupted stage restarts',
    stage(s, 'match').runs === 2,
    `runs ${stage(s, 'match').runs}`,
  );

  // 2. stop and resume (paused)
  await waitFor((x) => stage(x, 'prepare').state === 'running', 'prepare running');
  await page.click('#stop');
  s = await waitFor((x) => !x.running, 'stopped');
  check(
    'stop pauses the running stage',
    stage(s, 'prepare').state === 'paused',
    stage(s, 'prepare').state,
  );
  await page.click('#start');

  // 3. second tab refused while the first runs
  await waitFor((x) => stage(x, 'train').state === 'running', 'train running');
  const page2 = await ctx.newPage();
  capture(page2, consoleLog, 'tab2');
  await page2.goto(appUrl + '&autostart=1');
  await ready(page2);
  const blocked = await page2.evaluate(() => window.websplat.blocked);
  const visible = await page2.isVisible('#blocked');
  check('second tab refuses the job', blocked && visible);
  s = await snap();
  check('first tab keeps running', s.running && stage(s, 'train').state !== 'failed');
  await page2.close();

  // 4. reload during a checkpoint write after one checkpoint was published
  const during = await waitFor((x) => {
    const t = stage(x, 'train');
    return t.state === 'checkpointing' && t.checkpoint;
  }, 'second checkpoint write');
  const published = stage(during, 'train').checkpoint;
  gpu.mark('reload-during-checkpoint');
  await page.reload();
  await ready();
  s = await snap();
  const tr = stage(s, 'train');
  check('reload during checkpoint write: stage interrupted', tr.state === 'interrupted', tr.state);
  check(
    'reload during checkpoint write: last published checkpoint kept',
    tr.checkpoint?.generation === published.generation && tr.checkpoint?.done === published.done,
    `${JSON.stringify(tr.checkpoint)} vs ${JSON.stringify(published)}`,
  );
  const ckptDirs = await page.evaluate(async () => {
    const r = await navigator.storage.getDirectory();
    const d = await (
      await (
        await (await r.getDirectoryHandle('websplat')).getDirectoryHandle('jobs')
      ).getDirectoryHandle('dummy')
    ).getDirectoryHandle('train');
    const names = [];
    for await (const k of d.keys()) names.push(k);
    return names;
  });
  check(
    'unpublished checkpoint generation removed',
    ckptDirs.length === 1 && ckptDirs[0] === `ckpt-${published.generation}`,
    ckptDirs.join(','),
  );
  const resumeAt = Date.now();
  await page.click('#start');
  s = await waitFor(
    (x) =>
      stage(x, 'train').state === 'running' && stage(x, 'train').progress?.done >= published.done,
    'train resumed',
  );
  const firstDone = stage(s, 'train').progress.done;
  check(
    'training resumes from the checkpoint',
    firstDone >= published.done && firstDone < published.done + 0.1 * 30000,
    `at ${firstDone}, checkpoint ${published.done}`,
  );
  const trainStart = Date.now();

  // 5. completion
  s = await waitFor((x) => x.stages.every((y) => y.state === 'done'), 'all done');
  s = await waitFor((x) => !x.running, 'controller stopped');
  const tEnd = Date.now();
  sampling = false;
  await sampler;
  check('all stages done', true);
  // the app's own manifest validator (checksum, version, newest slot) and output re-hash
  const verify = await page.evaluate(async () => {
    const v = await window.websplat.verify();
    const snapAll = window.websplat.snapshot();
    return {
      ok: v.ok,
      seq: v.seq,
      files: v.files,
      bad: v.bad ?? [v.error],
      manifest: {
        stages: Object.fromEntries(
          Object.entries(v.manifest?.stages ?? {}).map(([k, x]) => [k, { state: x.state }]),
        ),
      },
      stages: snapAll.stages.map((x) => ({
        id: x.id,
        runs: x.runs,
        timings: x.timings,
        generation: x.generation,
      })),
    };
  });
  check(
    'completion is persisted for every stage',
    Object.values(verify.manifest?.stages ?? {}).filter((x) => x.state === 'done').length ===
      Object.keys(PROFILES).length,
    Object.entries(verify.manifest?.stages ?? {})
      .map(([k, x]) => `${k}:${x.state}`)
      .join(' '),
  );
  check(
    'manifest validates and published outputs match their hashes',
    verify.ok && verify.files >= 16,
    `${verify.files} files, bad: ${verify.bad.join(',')}`,
  );

  // progress: monotonic within a run, within totals, in the stage's unit
  const byStage = {};
  for (const { t, s: x } of samples) {
    for (const st of x.stages) {
      if (!st.progress || !['running', 'checkpointing'].includes(st.state)) continue;
      (byStage[st.id] ??= []).push({
        t,
        runs: st.runs,
        done: st.progress.done,
        total: st.progress.total,
        unit: st.progress.unit,
        rem: st.remainingSec,
        overall: x.overall.remainingSec,
        text: x.overall.text,
      });
    }
  }
  let mono = true;
  const monoBad = [];
  for (const [id, rows] of Object.entries(byStage)) {
    for (let k = 1; k < rows.length; k++) {
      const p = rows[k - 1],
        c = rows[k];
      if (c.runs === p.runs && c.done < p.done) ((mono = false), monoBad.push(id));
      if (c.total != null && c.done > c.total) ((mono = false), monoBad.push(id));
    }
  }
  check('progress is monotonic and within totals', mono, [...new Set(monoBad)].join(','));
  check(
    'progress units',
    byStage.ingest?.[0]?.unit === 'bytes' &&
      byStage.match?.[0]?.unit === 'pairs' &&
      byStage.train?.[0]?.unit === 'iterations',
  );
  const seen = statesSeen;
  const allStates = ['waiting', 'running', 'checkpointing', 'paused', 'interrupted', 'done'];
  check(
    'states observed',
    allStates.every((x) => seen.has(x)),
    [...seen].join(','),
  );

  // ETA: per-stage remaining at 25/50/75% of each stage's final run vs the actual remaining
  const etaRows = [];
  for (const [id, rows] of Object.entries(byStage)) {
    const last = rows.filter((r) => r.runs === rows.at(-1).runs);
    const end = samples.find(
      ({ t, s: x }) => t > last.at(-1).t && stage(x, id).state === 'done',
    )?.t;
    if (!end || end - last[0].t < 3000) continue;
    for (const f of [0.25, 0.5, 0.75]) {
      const tt = last[0].t + f * (end - last[0].t);
      const r = last.find((x) => x.t >= tt);
      if (!r || r.rem == null) continue;
      const actual = (end - r.t) / 1000;
      etaRows.push({
        id,
        at: f,
        predicted: +r.rem.toFixed(1),
        actual: +actual.toFixed(1),
        err: +((r.rem - actual) / actual).toFixed(3),
      });
    }
  }
  // every stage expected to run at least 3 s must have been sampled at 25, 50 and 75%
  const factor = (device.gpuClass === 'desktop' ? DESKTOP_FACTOR : 1) / speed;
  const required = Object.entries(PROFILES)
    .filter(([, p]) => p.seconds * factor >= 3)
    .map(([id]) => id);
  const missing = required.filter((id) => etaRows.filter((r) => r.id === id).length < 3);
  check(
    'ETA coverage',
    required.length > 0 && missing.length === 0,
    `required ${required.join(',')}; missing ${missing.join(',') || 'none'}`,
  );
  const late = etaRows.filter((r) => r.at >= 0.5);
  const worst = late.length ? late.reduce((m, r) => Math.max(m, Math.abs(r.err)), 0) : Infinity;
  check(
    'stage ETA within 25% from half way',
    worst <= 0.25,
    `worst ${(worst * 100).toFixed(0)}% over ${late.length} samples`,
  );
  // overall ETA during the final uninterrupted run (training resumed to the end)
  const overallRows = samples
    .filter(({ t }) => t >= trainStart && t <= tEnd)
    .filter((_, k, arr) => k % Math.max(1, Math.floor(arr.length / 6)) === 0)
    .map(({ t, s: x }) => ({
      predicted: x.overall.remainingSec == null ? null : +x.overall.remainingSec.toFixed(1),
      actual: +((tEnd - t) / 1000).toFixed(1),
      text: x.overall.text,
    }));
  const overallUsed = overallRows.filter((r) => r.actual > 5);
  const overallWorst =
    overallUsed.length >= 3
      ? overallUsed.reduce((m, r) => Math.max(m, Math.abs(r.predicted - r.actual) / r.actual), 0)
      : Infinity;
  check(
    'overall ETA within 30% during the last run',
    overallWorst <= 0.3,
    `worst ${(overallWorst * 100).toFixed(0)}%`,
  );

  // 6. a manifest from another version is refused and the job's data kept
  const opfs = `async (op, arg) => {
    const root = await (await navigator.storage.getDirectory()).getDirectoryHandle('websplat');
    const job = await (await root.getDirectoryHandle('jobs')).getDirectoryHandle('dummy');
    const slots = ['manifest.a.json', 'manifest.b.json'];
    const write = async (n, text) => { const w = await (await job.getFileHandle(n)).createWritable(); await w.write(text); await w.close(); };
    if (op === 'bump') {
      const old = [];
      for (const n of slots) {
        const text = await (await (await job.getFileHandle(n)).getFile()).text();
        old.push(text);
        const m = JSON.parse(text); m.version = 2; await write(n, JSON.stringify(m));
      }
      return old;
    }
    if (op === 'mixed') {
      // only the newest slot from a newer app version, validly checksummed; the other stays v1
      const texts = [];
      for (const n of slots) texts.push(await (await (await job.getFileHandle(n)).getFile()).text());
      const ms = texts.map((t) => JSON.parse(t));
      const k = ms[0].seq > ms[1].seq ? 0 : 1;
      const m = { ...ms[k], version: 2, seq: ms[k].seq + 1, checksum: '' };
      m.checksum = await window.websplat.testing.hashJson(m);
      await write(slots[1 - k], JSON.stringify(m));
      return texts;
    }
    if (op === 'restore') { for (const [k, n] of slots.entries()) await write(n, arg[k]); return true; }
    if (op === 'list') { const out = []; for await (const k of (await job.getDirectoryHandle('train')).keys()) out.push(k); return out; }
  }`;
  const callOpfs = (op, arg) =>
    page.evaluate(([src, op, arg]) => eval(src)(op, arg), [opfs, op, arg]);
  for (const mode of ['bump', 'mixed']) {
    const original = await callOpfs(mode);
    await page.reload();
    await ready();
    const unreadable = await page.evaluate(() => window.websplat.unreadable);
    const keptDirs = await callOpfs('list');
    check(
      `${mode === 'bump' ? 'unsupported' : 'mixed v1/newer-version'} manifest refused, data kept`,
      unreadable?.kind === 'unsupported' &&
        keptDirs.some((n) => n.startsWith('gen-')) &&
        (await page.isVisible('#unreadable')),
      `${JSON.stringify(unreadable)}; train/: ${keptDirs.join(',')}`,
    );
    await callOpfs('restore', original);
    await page.reload();
    await ready();
    s = await snap();
    check(
      'restored manifest reopens the finished job',
      s && s.stages.every((x) => x.state === 'done'),
    );
  }

  // 7. reset while running: the old run must not write into the new job
  await page.click('#reset');
  await page.waitForLoadState('load');
  await ready();
  await waitFor((x) => stage(x, 'match').state === 'running', 'fresh run at match');
  await page.click('#reset');
  await page.waitForLoadState('load');
  await ready();
  await sleep(1500);
  s = await snap();
  const clean =
    s &&
    s.stages.every((x) => x.runs <= 1 && !['paused', 'failed', 'interrupted'].includes(x.state));
  check(
    'reset during a run leaves a clean new job',
    clean,
    s?.stages.map((x) => `${x.id}:${x.state}/${x.runs}`).join(' '),
  );
  await page.evaluate(() => window.websplat.shutdown());

  // 8. a checkpoint transaction whose own write fails (quota) while ordinary saves are queued
  // around it: neither memory nor storage may reference the failed checkpoint, and the
  // previous checkpoint's files must survive cleanup
  const tx = await page.evaluate(async () => {
    const T = window.websplat.testing;
    const src = {
      format: 'websplat.source',
      version: 1,
      name: 'x',
      size: 0,
      hash: 'sha256:x',
      copied: false,
    };
    const j = await T.Job.open('faulttest', { source: src, config: {} });
    try {
      await T.writeFile(`${j.dir}/s/ckpt-1/a.bin`, new Uint8Array(10));
      await j.publishCheckpoint('s', 1, { done: 1 });
      await T.writeFile(`${j.dir}/s/ckpt-2/a.bin`, new Uint8Array(20));
      j.injectFault = (kind) =>
        kind === 'transact' ? new DOMException('injected', 'QuotaExceededError') : null;
      let settled = false;
      const p = j
        .publishCheckpoint('s', 2, { done: 2 })
        .then(
          () => 'published',
          (e) => e.name,
        )
        .finally(() => (settled = true));
      const saves = [];
      while (!settled) {
        saves.push(j.save());
        await new Promise((r) => setTimeout(r, 1));
      }
      const err = await p;
      await Promise.allSettled(saves);
      j.injectFault = null;
      await j.removeUnpublished('s');
      const disk = await T.loadManifest(j.dir);
      const files = (await T.list(`${j.dir}/s`)).map((e) => e.name);
      return {
        err,
        saves: saves.length,
        live: j.stage('s').checkpoint?.generation,
        disk: disk.manifest.stages.s?.checkpoint?.generation,
        files,
      };
    } finally {
      await j.destroy();
    }
  });
  check(
    'failed checkpoint transaction leaves the previous checkpoint published and intact',
    tx.err === 'QuotaExceededError' &&
      tx.live === 1 &&
      tx.disk === 1 &&
      tx.files.join() === 'ckpt-1' &&
      tx.saves > 0,
    JSON.stringify(tx),
  );

  const errors = consoleLog.filter((m) => ['error', 'pageerror', 'crash'].includes(m.type));
  const dawn = consoleLog.filter((m) =>
    /allocation size|smaller than the image requires|Invalid|device lost|out of memory/i.test(
      m.text,
    ),
  );
  check(
    'no page errors or console errors',
    errors.length === 0,
    errors
      .slice(0, 3)
      .map((e) => e.text)
      .join(' | '),
  );
  check(
    'no Dawn allocation/validation warnings',
    dawn.length === 0,
    dawn
      .slice(0, 3)
      .map((e) => e.text)
      .join(' | '),
  );

  await gpu.stop();
  const trainGpu = gpu.peak(trainStart, tEnd);
  const report = {
    machine,
    host: machine === 'win11' ? 'win11' : hostname(),
    adapter,
    speed,
    when: new Date().toISOString(),
    wallSec: (tEnd - t0) / 1000,
    device,
    gpu: { start: gpuBase, train: trainGpu, overall: gpu.peak() },
    stages: verify.stages,
    eta: { stage: etaRows, overall: overallRows },
    checks,
    console: consoleLog,
  };
  writeFileSync(`${out}/report.json`, JSON.stringify(report, null, 1));
  writeFileSync(`${out}/gpu.json`, JSON.stringify(gpu, null, 1));
  say(
    `stage timings (s, work/overhead): ${verify.stages.map((x) => `${x.id} ${((x.timings.workMs ?? 0) / 1000).toFixed(1)}/${((x.timings.elapsedMs - (x.timings.workMs ?? 0)) / 1000).toFixed(1)}`).join(', ')}`,
  );
  say(
    `GPU memory: whole GPU ${gpuBase?.usedMiB} MiB at start, peak ${trainGpu?.peakMiB} MiB in training; test Chrome GPU process peak ${trainGpu?.processPeakMiB ?? 'n/a'} MiB`,
  );
  say(`report: ${out}/report.json`);
  const failed = checks.filter((c) => !c.ok);
  say(
    failed.length
      ? `${failed.length} of ${checks.length} checks FAILED`
      : `all ${checks.length} checks passed`,
  );
  process.exitCode = failed.length ? 1 : 0;
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
