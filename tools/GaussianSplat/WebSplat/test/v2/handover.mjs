// WebSplat side of the video viewer handover (PLAN-v2.md), through the page on the v2 server with
// a stub opener (stub/opener.html) in place of the viewer. Runs ingest and selection only (no
// GPU stage), so the device check must pass but no GPU work is done. H8 runs the export with a
// captured view on stage 7's prepared inputs ($WEBSPLAT_DATA/stage7/web/data, from
// test/stage7/run_tests.sh; skipped without them).
// usage: node handover.mjs [--port 8810] [--only H1,H2,..] [--adapter low-power|high-performance]   (needs app/dist: npm run build)
// Exit 1 if a check fails.
import { existsSync, mkdirSync, readFileSync } from 'node:fs';
import { args, env } from '../harness/lib/env.mjs';
import { capture, launchLocal } from '../harness/lib/browser.mjs';
import { indexMkv } from '../../app/src/stages/ingest/ingest.js';
import { LocalFrame } from '../../app/src/stages/select/geometry.js';
import { viewCamera } from '../../app/src/stages/export/view.js';
import { serveV2 } from './serve.mjs';

const a = args();
const data = env('WEBSPLAT_DATA');
const port = +(a.port ?? 8810);
const only = a.only ? a.only.split(',') : null;
const mkv = `${data}/input/orbit.mkv`;
const stub = new URL('./stub/', import.meta.url).pathname;
const site = `http://127.0.0.1:${port}`;
const foreign = `http://127.0.0.2:${port}`; // another origin, not a local development one
// the integrated GPU by default: no GPU work is done, and a GPU run elsewhere is not disturbed
const adapter = a.adapter ?? 'low-power';
const app = `${site}/websplat/index.html?adapter=${adapter}&requestWaitMs=4000`;
const say = (s) => console.log(`${new Date().toISOString().slice(11, 19)} ${s}`);
const checks = [];
let scenario = '';
const check = (name, ok, detail = '') => {
  checks.push({ scenario, name, ok: !!ok });
  say(
    `${ok ? 'PASS' : 'FAIL'} ${scenario} ${name}${detail ? ': ' + String(detail).slice(0, 300) : ''}`,
  );
};

// packet identities of the recording's frames, from the same demuxer ingest uses
const buf = readFileSync(mkv);
const indexed = await indexMkv(new File([buf], 'orbit.mkv'));
indexed.source.close();
const frames = indexed.frames;
const ep = (i) => ({
  pts: frames[i].timestampTicks,
  offset: frames[i].offset,
  size: frames[i].size,
  index: i,
});
const request = (over = {}) => ({
  format: 'websplat.request',
  version: 1,
  source: { name: 'orbit.mkv', size: buf.length, lastModified: 1762600000000 },
  range: { timescale: indexed.track.timescale, start: ep(20), end: ep(180) },
  // near the orbit's hot spot (CONTRACTS.md's proposal example)
  target: { lat: -35.5799111, lon: 148.9066524, alt_amsl: 1010.4, refine: true },
  hfov: null,
  radius: 300,
  max_height: 40,
  view: { camera: { lat: -35.5785, lon: 148.9066524, alt_amsl: 1150 }, fov_deg: 45 },
  title: 'orbit from the viewer',
  ...over,
});
const nonce = () =>
  crypto
    .getRandomValues(new Uint8Array(16))
    .reduce((s, b) => s + b.toString(16).padStart(2, '0'), '');

const stage7 = `${data}/stage7/web/data`;
const extra = { '/test/': stub, ...(existsSync(stage7) && { '/websplat/data/': stage7 }) };
const servers = [
  await serveV2({ port, host: '127.0.0.1', extra }),
  await serveV2({ port, host: '127.0.0.2', extra }),
];
const profile = `${data}/harness/profiles/v2-handover`;
mkdirSync(profile, { recursive: true });
const browser = await launchLocal({ profile, adapter });
const ctx = browser.context;
const consoleLog = [];

async function clearStorage() {
  const p = await ctx.newPage();
  await p.goto(`${site}/__blank`);
  await p.evaluate(async () => {
    localStorage.clear();
    const root = await navigator.storage.getDirectory();
    await root.removeEntry('websplat', { recursive: true }).catch(() => {});
  });
  await p.close();
}

async function closeAll() {
  for (const p of ctx.pages()) await p.close().catch(() => {});
}

// open the stub at `origin`, have it open WebSplat with `opts`; returns {opener, page}
async function handover(opts = {}, origin = site) {
  const opener = await ctx.newPage();
  capture(opener, consoleLog, `${scenario}/opener`);
  await opener.goto(`${origin}/test/opener.html`);
  await opener.setInputFiles('#file', opts.file ?? mkv);
  const popup = ctx.waitForEvent('page');
  const opened = await opener.evaluate((o) => window.openSplat(o), {
    url: `${app}${opts.query ?? '&autostart=0'}`,
    nonce: opts.nonce ?? nonce(),
    request: opts.request ?? request(),
    reply: opts.reply ?? 'ok',
  });
  if (!opened) throw new Error('popup blocked');
  const page = await popup;
  capture(page, consoleLog, scenario);
  return { opener, page };
}

const state = (page) => page.evaluate(() => window.websplat?.request ?? null);
const waitRequest = (page, states, timeout = 60000) =>
  page.waitForFunction((s) => s.includes(window.websplat?.request?.state), states, {
    timeout,
    polling: 50,
  });
const replies = (opener) => opener.evaluate(() => window.messages.map((m) => m.data));
// the job hooks exist once the page has set up the controller (just after the handover)
const manifest = async (page) => {
  await page.waitForFunction(() => typeof window.websplat?.job === 'function', null, {
    timeout: 60000,
  });
  return page.evaluate(() => window.websplat.job());
};
const stageState = (page, id) =>
  page.evaluate((id) => window.websplat.snapshot().stages.find((s) => s.id === id), id);
const opfsList = (page, path) =>
  page.evaluate(async (path) => {
    let d = await navigator.storage.getDirectory();
    try {
      for (const p of path.split('/')) d = await d.getDirectoryHandle(p);
    } catch {
      return [];
    }
    const out = [];
    for await (const k of d.keys()) out.push(k);
    return out;
  }, path);

// start, wait until selection is done (or a stage failed), then stop before the GPU stages
async function runToSelect(page) {
  await page.waitForFunction(() => typeof window.websplat?.start === 'function', null, {
    timeout: 60000,
  });
  await page.evaluate(() => {
    window.websplat.start();
  });
  await page.waitForFunction(
    () => {
      const s = window.websplat.snapshot().stages;
      return (
        s.find((x) => x.id === 'select').state === 'done' || s.some((x) => x.state === 'failed')
      );
    },
    null,
    { timeout: 300000, polling: 100 },
  );
  await page.evaluate(() => window.websplat.shutdown());
}

const scenarios = {
  // the flow without a request is unchanged: choose a file, confirm the proposal form
  async H0() {
    const page = await ctx.newPage();
    capture(page, consoleLog, scenario);
    await page.goto(`${app}`);
    await page.waitForFunction(() => window.websplat?.ready, null, { timeout: 60000 });
    check('no request panel', await page.evaluate(() => document.getElementById('request').hidden));
    await page.setInputFiles('#file', mkv);
    await page.waitForFunction(() => typeof window.websplat?.start === 'function', null, {
      timeout: 60000,
    });
    const m = await manifest(page);
    check(
      'plain job id, no request config',
      /^thermal-[0-9a-f]{16}$/.test(m.jobId) && !m.config.request && !m.config.proposal,
      m.jobId,
    );
    await page.evaluate(() => {
      window.websplat.start();
    });
    await page.waitForSelector('#proposal:not([hidden])', { timeout: 120000 });
    await page.click('#proposal-form button[type=submit]');
    await page.waitForFunction(
      () => window.websplat.snapshot().stages.find((x) => x.id === 'select').state !== 'waiting',
      null,
      { timeout: 60000 },
    );
    await page.evaluate(() => window.websplat.shutdown());
    const sel = (await manifest(page)).config.select;
    check(
      'form config has no range keys',
      sel && sel.lat != null && !('start' in sel) && !('step' in sel),
      JSON.stringify(sel),
    );
    await closeAll();
    await clearStorage();
  },

  // accepted, copied, acknowledged; job id from source hash and request key; URL ?job=
  async H1() {
    const { opener, page } = await handover();
    await waitRequest(page, ['accepted', 'failed', 'rejected']);
    const r = await state(page);
    check('accepted', r.state === 'accepted', JSON.stringify(r));
    check(
      'job id is pipeline-hash16-key',
      /^thermal-[0-9a-f]{16}-[0-9a-f]{16}$/.test(r.job),
      r.job,
    );
    const url = new URL(page.url());
    check(
      'URL names the job',
      url.searchParams.get('job') === r.job && !url.searchParams.has('request'),
      page.url(),
    );
    await opener.waitForFunction(() =>
      window.messages.some((m) => m.data.type === 'websplat.accepted'),
    );
    const msgs = await replies(opener);
    check(
      'opener got ready then accepted once',
      msgs.filter((m) => m.type === 'websplat.ready').length === 1 &&
        msgs.filter((m) => m.type === 'websplat.accepted').length === 1 &&
        msgs.find((m) => m.type === 'websplat.accepted').job === r.job,
      JSON.stringify(msgs),
    );
    const m = await manifest(page);
    check(
      'source copied',
      m.source.copied === true && m.source.size === buf.length,
      JSON.stringify(m.source),
    );
    check(
      'config.request has the identity and source',
      m.config.request.range.start.offset === frames[20].offset &&
        m.config.request.source.name === 'orbit.mkv' &&
        m.config.request.view.fov_deg === 45,
    );
    check(
      'radius and max_height: identity and config.prepare',
      m.config.request.radius === 300 &&
        m.config.request.max_height === 40 &&
        m.config.prepare?.radius === 300 &&
        m.config.prepare?.max_height === 40,
      JSON.stringify(m.config.prepare),
    );
    check(
      'config.proposal.range',
      m.config.proposal.range.end.offset === frames[180].offset && !('hfov' in m.config.proposal),
    );
    check(
      'config.export title and view',
      m.config.export.title === 'orbit from the viewer' &&
        m.config.export.view.fov_deg === 45 &&
        !('target' in m.config.export),
      JSON.stringify(m.config.export),
    );
    const incoming = await opfsList(page, 'websplat/incoming');
    check('no staged copy left', incoming.length === 0, incoming.join());
    await page.waitForFunction(() => !document.getElementById('request-summary').hidden, null, {
      timeout: 30000,
    });
    const summary = await page.textContent('#request-summary');
    check(
      'summary shown',
      /From the video viewer/.test(summary) &&
        /captured/.test(summary) &&
        /radius 300 m; at most 40 m above the target/.test(summary),
      summary,
    );
    await runToSelect(page);
    const ingest = await stageState(page, 'ingest');
    const mf = await manifest(page);
    const res = mf.stages.ingest.result;
    check(
      'ingest done with range',
      ingest.state === 'done' &&
        res.range?.start === 20 &&
        res.range?.end === 180 &&
        res.range?.frames === 161,
      JSON.stringify(res?.range ?? ingest.error),
    );
    check('ingest records homeAltitude', Number.isFinite(res.homeAltitude), res.homeAltitude);
    check(
      'proposal limited to the range',
      res.proposal.options?.start === 20 && res.proposal.options?.end === 180,
      JSON.stringify(res.proposal.options ?? res.proposal.error),
    );
    const sel = mf.config.select;
    check(
      'select config derived without the form',
      sel &&
        sel.start === 20 &&
        sel.end === 180 &&
        sel.step === 1 &&
        sel.locate_hotspot === true &&
        Math.abs(sel.target_alt - (1010.4 - res.homeAltitude)) < 1e-9,
      JSON.stringify(sel),
    );
    check(
      'proposal form never shown',
      await page.evaluate(() => document.getElementById('proposal').hidden),
    );
    const select = await stageState(page, 'select');
    check('select done', select.state === 'done', JSON.stringify(select.error));
    this.job = r.job;
    await closeAll();
  },

  // reload of ?job= resumes that job; same request with a new title reopens it, export key only
  async H2() {
    const p = await ctx.newPage();
    capture(p, consoleLog, scenario);
    await p.goto(`${app}&job=${scenarios.H1.job}`);
    await p.waitForFunction(() => window.websplat?.ready && window.websplat.snapshot, null, {
      timeout: 60000,
    });
    const s = await p.evaluate(() => window.websplat.snapshot());
    check(
      'reload resumes the job',
      s.jobId === scenarios.H1.job && s.stages.find((x) => x.id === 'select').state === 'done',
      s.jobId,
    );
    check(
      'no request panel on reload',
      await p.evaluate(() => document.getElementById('request').hidden),
    );
    await closeAll();
    const { page } = await handover({ request: request({ title: 'renamed' }) });
    await waitRequest(page, ['accepted', 'failed', 'rejected']);
    const r = await state(page);
    check('same request reopens the same job', r.job === scenarios.H1.job, JSON.stringify(r));
    await page.waitForFunction(() => window.websplat?.snapshot, null, { timeout: 60000 });
    const m = await manifest(page);
    const st = await page.evaluate(() =>
      window.websplat.snapshot().stages.map((x) => [x.id, x.state]),
    );
    check('title updated', m.config.export.title === 'renamed');
    check(
      'ingest and select kept',
      st.find(([id]) => id === 'ingest')[1] === 'done' &&
        st.find(([id]) => id === 'select')[1] === 'done',
      JSON.stringify(st),
    );
    await closeAll();
  },

  // two request tabs on one file: separate jobs; reload of the first resumes the first
  async H3() {
    const one = await handover({
      request: request({ range: { timescale: 1000, start: ep(30), end: ep(100) } }),
    });
    await waitRequest(one.page, ['accepted', 'failed', 'rejected']);
    const two = await handover({
      request: request({ range: { timescale: 1000, start: ep(40), end: ep(120) } }),
    });
    await waitRequest(two.page, ['accepted', 'failed', 'rejected']);
    const r1 = await state(one.page);
    const r2 = await state(two.page);
    check(
      'both accepted as separate jobs',
      r1.state === 'accepted' &&
        r2.state === 'accepted' &&
        r1.job !== r2.job &&
        r1.job !== scenarios.H1.job,
      `${r1.job} ${r2.job}`,
    );
    await one.page.reload();
    await one.page.waitForFunction(() => window.websplat?.snapshot, null, { timeout: 60000 });
    const s = await one.page.evaluate(() => window.websplat.snapshot());
    check('reload of the first resumes the first', s.jobId === r1.job, s.jobId);
    await closeAll();
  },

  // a stored job under the same id with another identity is refused
  async H4() {
    const p = await ctx.newPage();
    await p.goto(`${app}&job=${scenarios.H1.job}`);
    await p.waitForFunction(() => window.websplat?.testing, null, { timeout: 60000 });
    await p.evaluate(async () => {
      const job = window.websplat.testing.job();
      job.manifest.config.request.target.alt_amsl += 1;
      await job.save();
    });
    await closeAll();
    // ?job= names the refused job too: choosing a file must replace it
    const { opener, page } = await handover({ query: `&autostart=0&job=${scenarios.H1.job}` });
    await waitRequest(page, ['accepted', 'failed', 'rejected']);
    const r = await state(page);
    check(
      'identity mismatch refused',
      r.state === 'failed' && /different request/.test(r.error),
      JSON.stringify(r),
    );
    await opener.waitForFunction(() =>
      window.messages.some((m) => m.data.type === 'websplat.rejected'),
    );
    const msgs = await replies(opener);
    check(
      'opener got rejected',
      /different request/.test(msgs.find((m) => m.type === 'websplat.rejected').reason),
    );
    check(
      'file chooser offered',
      await page.evaluate(() => !document.getElementById('source').hidden),
    );
    await page.setInputFiles('#file', mkv);
    await page.waitForFunction(() => typeof window.websplat?.start === 'function', null, {
      timeout: 60000,
    });
    const chosen = (await manifest(page)).jobId;
    const url = new URL(page.url());
    check(
      'URL names the chosen job',
      !url.searchParams.has('request') && url.searchParams.get('job') === chosen,
      url.search,
    );
    await closeAll();
  },

  // bad handovers: wrong nonce, wrong origin, wrong size, invalid request, no opener
  async H5() {
    let h = await handover({ reply: 'wrongNonce' });
    await waitRequest(h.page, ['accepted', 'failed', 'rejected']);
    let r = await state(h.page);
    check(
      'wrong nonce ignored (times out)',
      r.state === 'failed' && /no request/.test(r.error),
      JSON.stringify(r),
    );
    check(
      'notice and latest job offered',
      await h.page.evaluate(
        () =>
          /Create splat again/.test(document.getElementById('request-status').textContent) &&
          !document.getElementById('request-latest').hidden &&
          !document.getElementById('source').hidden,
      ),
    );
    await closeAll();

    h = await handover({}, foreign);
    await waitRequest(h.page, ['accepted', 'failed', 'rejected']);
    r = await state(h.page);
    check(
      'wrong origin ignored (times out)',
      r.state === 'failed' && /no request/.test(r.error),
      JSON.stringify(r),
    );
    await closeAll();

    h = await handover({
      request: request({ source: { name: 'orbit.mkv', size: buf.length + 1 } }),
    });
    await waitRequest(h.page, ['accepted', 'failed', 'rejected']);
    r = await state(h.page);
    check(
      'wrong size rejected',
      r.state === 'failed' && /is not the request's/.test(r.error),
      JSON.stringify(r),
    );
    await h.opener.waitForFunction(() =>
      window.messages.some((m) => m.data.type === 'websplat.rejected'),
    );
    await closeAll();

    h = await handover({
      request: request({ target: { lat: 95, lon: 0, alt_amsl: 0, refine: true } }),
    });
    await waitRequest(h.page, ['accepted', 'failed', 'rejected']);
    r = await state(h.page);
    check(
      'invalid request rejected naming the field',
      r.state === 'failed' && /target\.lat/.test(r.error),
      JSON.stringify(r),
    );
    await closeAll();

    const p = await ctx.newPage();
    await p.goto(`${app}&request=${nonce()}`);
    await p.waitForFunction(() => ['failed'].includes(window.websplat?.request?.state), null, {
      timeout: 30000,
    });
    r = await state(p);
    check(
      'no opener: notice',
      /no opener/.test(r.error) &&
        (await p.evaluate(() => !document.getElementById('source').hidden)),
      JSON.stringify(r),
    );
    await closeAll();
  },

  // endpoint mismatch fails ingest; refine false sets the picked-target policy
  async H6() {
    const bad = request({
      range: { timescale: 1000, start: ep(20), end: { ...ep(180), pts: ep(180).pts + 1 } },
    });
    let h = await handover({ request: bad });
    await waitRequest(h.page, ['accepted', 'failed', 'rejected']);
    await runToSelect(h.page);
    const ingest = await stageState(h.page, 'ingest');
    check(
      'endpoint mismatch fails ingest',
      ingest.state === 'failed' && /range end does not match frame 180/.test(ingest.error?.message),
      JSON.stringify(ingest.error),
    );
    await closeAll();

    h = await handover({
      request: request({
        target: { lat: -35.5799111, lon: 148.9066524, alt_amsl: 1010.4, refine: false },
        view: null,
      }),
    });
    await waitRequest(h.page, ['accepted', 'failed', 'rejected']);
    const m = await manifest(h.page);
    check(
      "refine false: export target 'picked', no view",
      m.config.export.target === 'picked' && !('view' in m.config.export),
      JSON.stringify(m.config.export),
    );
    await runToSelect(h.page);
    const sel = (await manifest(h.page)).config.select;
    check('refine false: locate_hotspot off', sel?.locate_hotspot === false, JSON.stringify(sel));
    await closeAll();
  },

  // a proposal that fails (a single-frame range) still yields the range and a select config
  async H7() {
    const h = await handover({
      request: request({ range: { timescale: 1000, start: ep(5), end: ep(5) } }),
    });
    await waitRequest(h.page, ['accepted', 'failed', 'rejected']);
    await runToSelect(h.page);
    const m = await manifest(h.page);
    const res = m.stages.ingest.result;
    check(
      'ingest done, proposal failed, range kept',
      m.stages.ingest.state === 'done' &&
        !!res.proposal.error &&
        res.range.start === 5 &&
        res.range.frames === 1,
      JSON.stringify({ range: res?.range, error: res?.proposal?.error }),
    );
    check(
      'select config falls back to defaults',
      m.config.select?.rotation === null && m.config.select?.start === 5,
      JSON.stringify(m.config.select),
    );
    await closeAll();
  },

  // storage failures during the handover copy: rejected once, no staged copy or new job left;
  // a staged copy left by a closed tab is removed by the next handover
  async H9() {
    const p = await ctx.newPage();
    await p.goto(`${site}/__blank`);
    await p.evaluate(async () => {
      let d = await navigator.storage.getDirectory();
      d = await d.getDirectoryHandle('websplat', { create: true });
      d = await d.getDirectoryHandle('incoming', { create: true });
      const w = await (await d.getFileHandle('0'.repeat(32), { create: true })).createWritable();
      await w.write('left by a crashed tab');
      await w.close();
    });
    const jobsBefore = await opfsList(p, 'websplat/jobs');
    const fresh = (k) => request({ range: { timescale: 1000, start: ep(50 + k), end: ep(90) } });
    for (const [k, re] of [
      [0, '^incoming/'],
      [1, 'source/source\\.mkv$'],
    ]) {
      const fault = encodeURIComponent(JSON.stringify({ quota: re }));
      const h = await handover({ request: fresh(k), query: `&autostart=0&pageFault=${fault}` });
      await waitRequest(h.page, ['accepted', 'failed', 'rejected']);
      const r = await state(h.page);
      check(
        `fault ${re}: failed with the storage error`,
        r.state === 'failed' && /quota/.test(r.error),
        JSON.stringify(r),
      );
      await h.opener.waitForFunction(() =>
        window.messages.some((m) => m.data.type === 'websplat.rejected'),
      );
      await h.page.waitForTimeout(500);
      const msgs = await replies(h.opener);
      check(
        `fault ${re}: one rejected, no accepted`,
        msgs.filter((m) => m.type === 'websplat.rejected').length === 1 &&
          !msgs.some((m) => m.type === 'websplat.accepted'),
        JSON.stringify(msgs.map((m) => m.type)),
      );
      check(
        `fault ${re}: no staged copy left`,
        (await opfsList(h.page, 'websplat/incoming')).length === 0,
        (await opfsList(h.page, 'websplat/incoming')).join(),
      );
      const jobs = await opfsList(h.page, 'websplat/jobs');
      check(
        `fault ${re}: no new job`,
        jobs.length === jobsBefore.length && jobs.every((j) => jobsBefore.includes(j)),
        jobs.filter((j) => !jobsBefore.includes(j)).join(),
      );
      await h.page.close();
      await h.opener.close();
    }
    await closeAll();
  },

  // the standalone page with an RGB recording: an rgb job, the form with the processing width
  // (WEBSPLAT_RGB_SMALL; skipped without it)
  async H10() {
    let ts;
    try {
      ts = env('WEBSPLAT_RGB_SMALL');
    } catch {
      return say('skipped: WEBSPLAT_RGB_SMALL is not set');
    }
    await clearStorage();
    const page = await ctx.newPage();
    capture(page, consoleLog, scenario);
    await page.goto(app);
    await page.waitForFunction(() => window.websplat?.ready, null, { timeout: 60000 });
    await page.setInputFiles('#file', ts);
    await page.waitForFunction(() => typeof window.websplat?.start === 'function', null, {
      timeout: 120000,
    });
    const m = await manifest(page);
    check('rgb job from a .ts', /^rgb-[0-9a-f]{16}$/.test(m.jobId), m.jobId);
    await page.evaluate(() => {
      window.websplat.start();
    });
    await page.waitForSelector('#proposal:not([hidden])', { timeout: 120000 });
    const form = await page.evaluate(() => ({
      width: !document.getElementById('width-label').hidden,
      rotation: !document.getElementById('rotation-label').hidden,
      value: document.querySelector('#proposal-form [name=width]').value,
      lat: document.querySelector('#proposal-form [name=lat]').value,
    }));
    check(
      'form: width, no rotation, proposed target',
      form.width && !form.rotation && form.value === '1280' && form.lat !== '',
      JSON.stringify(form),
    );
    await page.fill('#proposal-form [name=width]', '640');
    await page.click('#proposal-form button[type=submit]');
    await page.waitForFunction(
      () => window.websplat.snapshot().stages.find((x) => x.id === 'select').state !== 'waiting',
      null,
      { timeout: 60000 },
    );
    await page.evaluate(() => window.websplat.shutdown());
    const mf = await manifest(page);
    const sel = mf.config.select;
    check(
      'rgb jobs: 8192 SIFT features and RADIAL',
      mf.config.features?.maxNumFeatures === 8192 && mf.config.map?.camera === 'RADIAL',
    );
    check(
      'rgb select config from the form',
      sel?.width === 640 && sel.budget === 500 && !('rotation' in sel),
      JSON.stringify(sel),
    );
    await closeAll();
  },

  // export with a captured view: frames.origin ENU, the clamp to [1, 8] x bounds radius, and
  // the 'picked' target policy (the export-test set: stand-ins, no WebGPU needed)
  async H8() {
    if (!extra['/websplat/data/']) return say(`skipped: no ${stage7}`);
    const frames7 = JSON.parse(readFileSync(`${stage7}/frames.json`, 'utf8'));
    const dataset7 = JSON.parse(readFileSync(`${stage7}/dataset.json`, 'utf8'));
    const local = new LocalFrame(frames7.origin.lat, frames7.origin.lon, frames7.origin.alt_amsl);
    const view = (enu, fov_deg) => {
      const [lat, lon, alt_amsl] = local.toGeodetic(enu);
      return { camera: { lat, lon, alt_amsl }, fov_deg };
    };
    const R = dataset7.bounds.radius;
    const cases = [
      // inside the allowed distance, the picked target
      { view: view([300, -200, 400], 40), target: 'picked', want: frames7.target_enu },
      // too close: moved out to 1 x radius; prepare's hot spot stays the target
      { view: view([10, 10, 60], 55), target: undefined, want: dataset7.hotspot.enu },
    ];
    for (const [k, c] of cases.entries()) {
      const page = await ctx.newPage();
      capture(page, consoleLog, scenario);
      const q = new URLSearchParams({
        pipeline: 'export-test',
        job: `v2-view-${k}-${Date.now()}`,
        standin: 'data/g1_laptop.json',
        smoke: '0',
      });
      await page.goto(`${site}/websplat/index.html?${q}`);
      await page.waitForFunction(() => window.websplat?.ready && window.websplat.testing, null, {
        timeout: 60000,
      });
      await page.evaluate(
        async ([v, t]) => {
          const job = window.websplat.testing.job();
          Object.assign(job.manifest.config.export, { view: v, ...(t && { target: t }) });
          await job.save();
          window.websplat.start();
        },
        [c.view, c.target],
      );
      await page.waitForFunction(
        () =>
          window.websplat.result?.meta ||
          window.websplat.snapshot().stages.some((x) => x.state === 'failed'),
        null,
        { timeout: 600000, polling: 250 },
      );
      const meta = await page.evaluate(() => window.websplat.result?.meta ?? null);
      const want = viewCamera(c.view, frames7.origin, c.want, R).camera;
      const near = (x, y) => x.length === y.length && x.every((v, i) => Math.abs(v - y[i]) < 1e-6);
      check(
        `case ${k}: export done`,
        !!meta,
        JSON.stringify(
          await page.evaluate(() =>
            window.websplat.snapshot().stages.map((x) => [x.id, x.state, x.error?.message]),
          ),
        ),
      );
      if (meta) {
        check(`case ${k}: target`, near(meta.target, c.want), JSON.stringify(meta.target));
        check(
          `case ${k}: start camera from the view`,
          near(meta.camera.position, want.position) &&
            near(meta.camera.target, c.want) &&
            Math.abs(meta.camera.fov_deg - c.view.fov_deg) < 1e-9,
          JSON.stringify(meta.camera),
        );
        const d = Math.hypot(...meta.camera.position.map((v, i) => v - c.want[i]));
        check(
          `case ${k}: distance within [1, 8] x radius`,
          d >= R - 1e-6 && d <= 8 * R + 1e-6,
          d.toFixed(1),
        );
      }
      await closeAll();
    }
  },
};

try {
  await clearStorage();
  for (const [name, fn] of Object.entries(scenarios)) {
    if (only && !only.includes(name)) continue;
    scenario = name;
    say(`--- ${name}`);
    try {
      await fn.call(scenarios[name]);
    } catch (e) {
      check('ran', false, e.stack);
      await closeAll();
    }
  }
} finally {
  await browser.close();
  for (const s of servers) s.close();
}
const failed = checks.filter((c) => !c.ok);
const errors = consoleLog.filter((m) => m.type === 'pageerror');
for (const e of errors) say(`page error (${e.tag}): ${e.text}`);
say(`${checks.length - failed.length}/${checks.length} checks passed`);
process.exit(failed.length ? 1 : 0);
