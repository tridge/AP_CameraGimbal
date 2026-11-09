// Stage 7 recovery in the page ('export-test' set):
//   stop    Stop during the PLY pass of the 2M export (within 3.5 s), Resume at once: done, and the
//           files equal the Node export
//   reload  reload the page during the SPZ pass (after a chunk) of run13: the page recovers
//           (no errors, the partial generation removed), Resume: done, files equal the Node export
//   pick    a trained splat chosen in the page survives a reload (stored under source/); when it
//           is missing on resume, the page asks for it again
//   node recovery_check.mjs WEB NODE_OUT [--machine laptop|win11] [--profile NAME] [--port 8799]
//        [--cdp URL] [--scenarios stop,reload,pick]
// NODE_OUT: stream_export.mjs outputs (NODE_OUT/<case>/...). Exit 1 on any failure.
import { openAsBlob } from 'node:fs';
import { args, env } from '../harness/lib/env.mjs';
import { capture, connectCdp, launchLocal } from '../harness/lib/browser.mjs';
import { serve } from '../harness/lib/serve.mjs';
import { hashBlob } from '../../app/src/storage/hash.js';

const a = args();
const [web, nodeOut] = a._;
if (!nodeOut) throw new Error('usage: node recovery_check.mjs WEB NODE_OUT');
const machine = a.machine ?? 'laptop';
const port = +(a.port ?? 8799);
const base = `http://127.0.0.1:${port}`;
const scenarios = (a.scenarios ?? 'stop,reload,pick').split(',');
const profileName = a.profile ?? `stage7-${machine}`;
const say = (s) => console.log(`${new Date().toISOString().slice(11, 19)} ${s}`);
let failures = 0;
const check = (name, ok, detail = '') => {
  say(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? `: ${detail}` : ''}`);
  if (!ok) failures++;
};
const server = await serve(web, port);
const browser =
  machine === 'win11'
    ? await connectCdp(a.cdp ?? 'http://127.0.0.1:9345')
    : await launchLocal({ profile: `${env('WEBSPLAT_DATA')}/chrome-profiles/${profileName}` });
const consoleLog = [];

async function until(page, fn, arg, ms, label) {
  const t0 = Date.now();
  for (;;) {
    const v = await page.evaluate(fn, arg).catch(() => null);
    if (v) return v;
    if (Date.now() - t0 > ms) throw new Error(`timeout: ${label}`);
    await page.waitForTimeout(100);
  }
}
const ready = (page) =>
  until(page, () => window.websplat?.ready && !!window.websplat.snapshot, null, 60000, 'ready');
const exportState = () => {
  const s = window.websplat.snapshot();
  const e = s.stages.find((x) => x.id === 'export');
  return {
    running: s.running,
    state: e.state,
    phase: e.phase,
    done: e.progress?.done ?? 0,
    total: e.progress?.total,
  };
};
// finished or failed (not merely waiting for input, which the pick scenario passes through)
const finished = () => {
  const s = window.websplat.snapshot();
  const st = s.stages.map((x) => x.state);
  if (s.running) return null;
  if (st.every((x) => x === 'done') || st.some((x) => x === 'failed' || x === 'paused'))
    return s.stages.map((x) => [x.id, x.state, x.error?.message ?? null]);
  return null;
};
const settled = () => {
  const s = window.websplat.snapshot();
  const st = s.stages.map((x) => x.state);
  if (s.running) return null;
  if (st.every((x) => x === 'done') || st.some((x) => x === 'failed' || x === 'paused') || s.needs)
    return s.stages.map((x) => [x.id, x.state, x.error?.message ?? null]);
  return null;
};

async function open(name, job, standin = name) {
  const page = await browser.context.newPage();
  capture(page, consoleLog, `${name}`);
  const q = new URLSearchParams({
    pipeline: 'export-test',
    job,
    standin: `data/${standin}.json`,
    title: `WebSplat ${name} end_header`,
    smoke: '0',
  });
  await page.goto(`${base}/index.html?${q}`);
  await ready(page);
  return page;
}

async function sameAsNode(page, name, label) {
  const outs = await page.evaluate(() => window.websplat.job().stages.export.outputs);
  for (const f of ['splat.ply', 'splat.spz', 'meta.json']) {
    const h = await hashBlob(await openAsBlob(`${nodeOut}/${name}/${f}`));
    check(`${label}: ${f} = the Node export`, outs?.[f]?.hash === h);
  }
}

async function finish(page) {
  await page.evaluate(async () => {
    await window.websplat.shutdown();
    await window.websplat.testing.job().destroy();
  });
  await page.close();
}

async function stopScenario() {
  const page = await open('synth2m', `stop-${Date.now()}`);
  page.evaluate(() => window.websplat.start()).catch(() => {});
  await until(
    page,
    () => {
      const e = window.websplat.snapshot().stages.find((x) => x.id === 'export');
      return e.state === 'running' && e.phase === 'ply' && e.progress?.done > 200000;
    },
    null,
    300000,
    'PLY pass running',
  );
  const t0 = Date.now();
  await page.evaluate(() => window.websplat.stop());
  const st = await until(
    page,
    () => (window.websplat.snapshot().running ? null : true),
    null,
    30000,
    'stopped',
  );
  const ms = Date.now() - t0;
  const e = await page.evaluate(exportState);
  check(
    'stop: Stop during the 2M PLY pass pauses within 3.5 s',
    st && e.state === 'paused' && ms <= 3500,
    `${e.state} after ${ms} ms`,
  );
  page.evaluate(() => window.websplat.start()).catch(() => {}); // at once
  await until(page, () => window.websplat.snapshot().running, null, 10000, 'resumed');
  const fin = await until(page, settled, null, 600000, 'settled');
  check(
    'stop: immediate Resume finishes',
    fin.every(([, s]) => s === 'done'),
    JSON.stringify(fin.filter(([, s]) => s !== 'done')),
  );
  await sameAsNode(page, 'synth2m', 'stop');
  await finish(page);
}

async function reloadScenario() {
  const job = `reload-${Date.now()}`;
  const page = await open('run13', job);
  page.evaluate(() => window.websplat.start()).catch(() => {});
  await until(
    page,
    () => {
      const e = window.websplat.snapshot().stages.find((x) => x.id === 'export');
      return e.phase === 'spz' && e.progress?.done > e.progress.total / 2 + 131072;
    },
    null,
    300000,
    'SPZ pass after a chunk',
  );
  await page.reload();
  await ready(page);
  const after = await page.evaluate(async () => {
    const w = window.websplat;
    const j = w.testing.job();
    return {
      errors: w.errors,
      state: w.job().stages.export.state,
      dirs: (await w.testing.list(`${j.dir}/export`)).map((d) => d.name),
    };
  });
  check(
    'reload: the page recovers during the SPZ pass (no errors, no partial generation)',
    !after.errors.length && after.state === 'interrupted' && !after.dirs.length,
    JSON.stringify(after),
  );
  page.evaluate(() => window.websplat.start()).catch(() => {});
  await until(page, () => window.websplat.snapshot().running, null, 10000, 'resumed');
  const fin = await until(page, settled, null, 600000, 'settled');
  check(
    'reload: Resume finishes',
    fin.every(([, s]) => s === 'done'),
    JSON.stringify(fin),
  );
  await sameAsNode(page, 'run13', 'reload');
  await finish(page);
}

async function pickScenario() {
  const job = `pick-${Date.now()}`;
  const page = await open('tiny', job, 'pick');
  const tiny = `${web}/data/tiny.bin`;
  page.evaluate(() => window.websplat.start()).catch(() => {});
  await page.waitForSelector('#trained', { state: 'visible', timeout: 60000 });
  check('pick: without a trained splat the page asks for one', true);
  await page.setInputFiles('#trained-file', tiny);
  const fin = await until(page, finished, null, 120000, 'finished');
  check(
    'pick: the export finishes from the chosen file',
    fin.every(([, s]) => s === 'done'),
    JSON.stringify(fin),
  );
  await page.reload();
  await ready(page);
  const kept = await page.evaluate(async () => {
    const w = window.websplat;
    return (await w.testing.list(`${w.testing.job().dir}/source/inputs`)).map((d) => d.name);
  });
  check(
    'pick: the chosen file survives a reload',
    kept.includes('params.bin'),
    JSON.stringify(kept),
  );
  // gone before the stand-in has loaded it: the page asks again instead of failing
  await page.evaluate(async () => {
    const w = window.websplat;
    const j = w.testing.job();
    await j.reset('train');
    await j.save();
    let d = await (await navigator.storage.getDirectory()).getDirectoryHandle('websplat');
    for (const p of `${j.dir}/source/inputs`.split('/')) d = await d.getDirectoryHandle(p);
    await d.removeEntry('params.bin');
  });
  await page.reload();
  await ready(page);
  page.evaluate(() => window.websplat.start()).catch(() => {});
  await page.waitForSelector('#trained', { state: 'visible', timeout: 60000 });
  const failed = await page.evaluate(() =>
    window.websplat
      .snapshot()
      .stages.filter((x) => x.state === 'failed')
      .map((x) => x.id),
  );
  check(
    'pick: a missing chosen file is asked for again, nothing fails',
    !failed.length,
    JSON.stringify(failed),
  );
  await page.setInputFiles('#trained-file', tiny);
  const fin2 = await until(page, finished, null, 120000, 'finished');
  check(
    'pick: and the export finishes again',
    fin2.every(([, s]) => s === 'done'),
    JSON.stringify(fin2),
  );
  await finish(page);
}

try {
  for (const s of scenarios) {
    try {
      await { stop: stopScenario, reload: reloadScenario, pick: pickScenario }[s]();
    } catch (e) {
      check(`${s}: driver`, false, e.stack ?? String(e));
    }
  }
} finally {
  await browser.close();
  server.close();
}
const errors = consoleLog.filter((l) => ['error', 'pageerror', 'crash', 'http'].includes(l.type));
for (const e of errors) say(`console ${e.tag} ${e.type}: ${e.text}`);
check('no console errors', !errors.length, `${errors.length}`);
say(failures ? `${failures} FAILED` : 'recovery checks: all pass');
process.exit(failures ? 1 : 0);
