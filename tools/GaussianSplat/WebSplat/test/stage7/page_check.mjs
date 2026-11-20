// Stage 7 in the page: the 'export-test' set (stand-ins load frames.json, dataset.json and a
// trained splat; then the export stage) for each case, with the result view, downloads and the
// acceptance check: the downloaded PLY opens with temperatures in the GaussianSplat viewer's
// open-a-file page (the code deployed at plot.ardupilot.org/splat/, here from the app's copy).
//   node page_check.mjs WEB OUT [--machine laptop|win11] [--profile NAME] [--port 8799]
//        [--cdp URL] [--cases a,b] [--stop CASE] [--no-download CASE,...] [--max-export-mib N]
//        [--colmap DIR]   (the model the map stand-in loads: the archive must hold its files)
//        [--viewer-ref DIR]   (DIR/<case>.json from viewer_reference.py: the viewer must match)
// WEB holds the app build and data/<case>.json (stand-in URLs). Exit 1 on any failure.
import {
  closeSync,
  mkdirSync,
  openAsBlob,
  openSync,
  readFileSync,
  writeFileSync,
  writeSync,
} from 'node:fs';
import { args, env } from '../harness/lib/env.mjs';
import { capture, connectCdp, launchLocal } from '../harness/lib/browser.mjs';
import { serve } from '../harness/lib/serve.mjs';
import { hashBlob } from '../../app/src/storage/hash.js';
import { RssSampler } from './rss.mjs';
import { compareViewer } from './viewer_compare.mjs';

const a = args();
const [web, out] = a._;
if (!out) throw new Error('usage: node page_check.mjs WEB OUT');
const machine = a.machine ?? 'laptop';
const port = +(a.port ?? 8799);
const base = `http://127.0.0.1:${port}`;
const cases = (a.cases ?? 'tiny,g1_laptop,run13,synth2m').split(',');
const noDownload = new Set((a['no-download'] ?? '').split(',').filter(Boolean));
const profileName = a.profile ?? `stage7-${machine}`;
mkdirSync(out, { recursive: true });
const say = (s) => console.log(`${new Date().toISOString().slice(11, 19)} ${s}`);
let failures = 0;
const check = (name, ok, detail = '') => {
  say(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? `: ${detail}` : ''}`);
  if (!ok) failures++;
};

const server = await serve(web, port);
const profile = `${env('WEBSPLAT_DATA')}/chrome-profiles/${profileName}`;
const browser =
  machine === 'win11'
    ? await connectCdp(a.cdp ?? 'http://127.0.0.1:9345')
    : await launchLocal({ profile });
const consoleLog = [];
const report = { machine, cases: {} };

// members of a ustar archive: {name: Buffer}
function untar(buf) {
  const out = {};
  for (let at = 0; at + 512 <= buf.length; ) {
    const h = buf.subarray(at, at + 512);
    if (h.every((b) => b === 0)) break;
    const name = h.subarray(0, 100).toString('latin1').replace(/\0.*$/s, '');
    const size = parseInt(h.subarray(124, 136).toString('latin1').replace(/\0.*$/s, ''), 8);
    out[name] = buf.subarray(at + 512, at + 512 + size);
    at += 512 + Math.ceil(size / 512) * 512;
  }
  return out;
}

// the file behind a download link (its blob URL), copied here 8 MB at a time
async function savePageBlob(page, sel, path) {
  const info = await page.evaluate(async (s) => {
    const el = document.querySelector(s);
    window.__blob = await (await fetch(el.href)).blob();
    return { size: window.__blob.size, name: el.download };
  }, sel);
  const fd = openSync(path, 'w');
  for (let at = 0; at < info.size; at += 8 << 20) {
    const b64 = await page.evaluate(
      async ([a, e]) => {
        const u = new Uint8Array(await window.__blob.slice(a, e).arrayBuffer());
        let str = '';
        for (let i = 0; i < u.length; i += 0x8000)
          str += String.fromCharCode(...u.subarray(i, i + 0x8000));
        return btoa(str);
      },
      [at, Math.min(info.size, at + (8 << 20))],
    );
    writeSync(fd, Buffer.from(b64, 'base64'));
  }
  closeSync(fd);
  await page.evaluate(() => delete window.__blob);
  return info.name;
}

async function waitFor(page, fn, arg, timeoutMs, label) {
  const t0 = Date.now();
  for (;;) {
    const v = await page.evaluate(fn, arg).catch(() => null);
    if (v) return v;
    if (Date.now() - t0 > timeoutMs) throw new Error(`timeout: ${label}`);
    await page.waitForTimeout(250);
  }
}

const snapState = () => {
  const s = window.websplat.snapshot?.();
  if (!s) return null;
  const ex = s.stages.find((x) => x.id === 'export');
  return { running: s.running, states: s.stages.map((x) => x.state), export: ex };
};

async function runCase(name) {
  const rep = { name };
  const page = await browser.context.newPage();
  await page.setViewportSize({ width: 1280, height: 1000 });
  capture(page, consoleLog, name);
  page.on('pageerror', (e) => say(`${name} pageerror: ${e.message}`));
  const job = `${name}-${Date.now()}`;
  const q = new URLSearchParams({
    pipeline: 'export-test',
    job,
    standin: `data/${name}.json`,
    title: `WebSplat ${name} end_header`,
    smoke: '0',
  });
  await page.goto(`${base}/index.html?${q}`);
  await waitFor(
    page,
    () => window.websplat?.ready && !!window.websplat.snapshot,
    null,
    60000,
    'page ready',
  );
  const rss = new RssSampler({
    match: machine === 'win11' ? profileName : profile,
    host: machine === 'win11' ? 'win11' : null,
  });
  await rss.start();
  const t0 = Date.now();
  // tag memory samples with the running stage and phase
  const tagger = setInterval(async () => {
    rss.tag = await page
      .evaluate(() => {
        const r = window.websplat.snapshot().stages.find((x) => x.state === 'running');
        return r ? `${r.id}:${r.phase ?? ''}` : null;
      })
      .catch(() => null);
  }, 200);
  page.evaluate(() => window.websplat.start()).catch(() => {});
  if (a.stop === name) {
    // Stop during the export, then resume: the stage restarts and gives the same files
    await waitFor(page, snapState, null, 600000, 'export running').then(() =>
      waitFor(
        page,
        () => {
          const s = window.websplat.snapshot();
          const ex = s.stages.find((x) => x.id === 'export');
          return ex.state === 'running' && ex.progress?.done > 0;
        },
        null,
        600000,
        'export progress',
      ),
    );
    await page.evaluate(() => window.websplat.stop());
    const st = await waitFor(
      page,
      () => {
        const s = window.websplat.snapshot();
        return !s.running && s.stages.find((x) => x.id === 'export').state;
      },
      null,
      60000,
      'stopped',
    );
    check(`${name}: Stop during the export pauses it`, st === 'paused', st);
    rep.stopped = st;
    page.evaluate(() => window.websplat.start()).catch(() => {});
    await waitFor(
      page,
      () => window.websplat.snapshot().stages.find((x) => x.id === 'export').state !== 'paused',
      null,
      60000,
      'resumed',
    );
  }
  const fin = await waitFor(
    page,
    () => {
      const s = window.websplat.snapshot();
      const st = s.stages.map((x) => x.state);
      const settled =
        st.every((x) => x === 'done') ||
        st.some((x) => x === 'failed' || x === 'paused') ||
        s.needs;
      if (s.running || !settled) return null;
      return s.stages.map((x) => [x.id, x.state, x.error?.message ?? null]);
    },
    null,
    1800000,
    'pipeline finished',
  );
  rep.wallMs = Date.now() - t0;
  const win = await page.evaluate(() => {
    const ex = window.websplat.job().stages.export;
    return ex?.finishedAt
      ? [Date.parse(ex.lastStartedAt ?? ex.startedAt), Date.parse(ex.finishedAt)]
      : null;
  });
  clearInterval(tagger);
  rep.memory = await rss.stop(win);
  say(
    `${name}: memory above the base by phase (MiB): ${JSON.stringify(rep.memory.peakAboveBaseByPhaseMiB)}`,
  );
  if (a['max-export-mib']) {
    const ex = Object.entries(rep.memory.peakAboveBaseByPhaseMiB)
      .filter(([k]) => k.startsWith('export:'))
      .map(([, v]) => v);
    const peak = ex.length ? Math.max(...ex) : rep.memory.peakAboveBaseMiB;
    // a run of a few seconds may fall between samples (win11 samples once a second)
    if (!Number.isFinite(peak) && rep.wallMs < 5000) say(`${name}: too short to sample memory`);
    else
      check(
        `${name}: export memory within ${a['max-export-mib']} MiB above the idle page`,
        Number.isFinite(peak) && peak <= +a['max-export-mib'],
        `${peak} MiB`,
      );
  }
  const ok = fin.every(([, st]) => st === 'done');
  check(`${name}: all stages done`, ok, JSON.stringify(fin.filter(([, st]) => st !== 'done')));
  if (!ok) return rep;
  const m = await page.evaluate(() => {
    const ex = window.websplat.job().stages.export;
    return { result: ex.result, outputs: ex.outputs, timings: ex.timings };
  });
  rep.export = m;
  say(
    `${name}: ${m.result.gaussians} Gaussians, export ${(m.timings.elapsedMs / 1000).toFixed(1)} s ` +
      `(ply ${(m.result.timesMs.ply / 1000).toFixed(1)}, spz ${(m.result.timesMs.spz / 1000).toFixed(1)}), ` +
      `memory peak +${rep.memory.peakAboveBaseMiB} MiB ` +
      `(largest process ${rep.memory.peakProcessMiB} MiB)`,
  );
  check(
    `${name}: SPZ decode error within bounds`,
    !Object.keys(m.result.spz.over).length,
    JSON.stringify(m.result.spz.over),
  );

  // result view: the embedded viewer shows the splat with temperatures
  await waitFor(page, () => window.websplat.result?.opened, null, 120000, 'result view');
  const frame = page.frames().find((f) => f.url().includes('/vendor/splat-viewer/index.html'));
  const view = await frame.evaluate(() => ({
    thermal: !document.getElementById('thermal-controls').hidden,
    title: document.title,
  }));
  check(
    `${name}: result view shows a thermal splat`,
    view.thermal && view.title.includes(name),
    JSON.stringify(view),
  );
  await page.locator('#result').scrollIntoViewIfNeeded();
  await page.waitForTimeout(2000);
  await page.locator('#result').screenshot({ path: `${out}/${name}-result.png` });
  const box = await page.locator('#result-viewer iframe').boundingBox();
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.waitForTimeout(300);
  rep.centreReadout = await frame.evaluate(() => document.getElementById('hover').textContent);
  rep.viewerStats = await frame.evaluate(() => document.getElementById('stats').textContent);
  const summary = await page.locator('#result-summary').innerText();
  rep.summary = summary;
  check(
    `${name}: summary`,
    summary.includes('Gaussians') && summary.includes('°C') && summary.includes('Total'),
  );

  // downloads: the files the buttons give are the published outputs
  if (!noDownload.has(name)) {
    mkdirSync(`${out}/${name}`, { recursive: true });
    for (const [id, file] of [
      ['ply', 'splat.ply'],
      ['spz', 'splat.spz'],
      ['meta', 'meta.json'],
    ]) {
      const sel = `#download-${id === 'meta' ? 'json' : id}`;
      const path = `${out}/${name}/${file}`;
      let suggested;
      if (machine === 'win11') {
        // downloads over CDP are refused: read the link's blob in pieces instead
        suggested = await savePageBlob(page, sel, path);
      } else {
        const [dl] = await Promise.all([page.waitForEvent('download'), page.click(sel)]);
        await dl.saveAs(path);
        suggested = dl.suggestedFilename();
      }
      const h = await hashBlob(await openAsBlob(path));
      check(
        `${name}: downloaded ${file} = the stage output`,
        h === m.outputs[file].hash,
        suggested,
      );
    }
    // the COLMAP model archive (map's final model) holds the model's files unchanged
    if (a.colmap) {
      const path = `${out}/${name}/colmap.tar`;
      if (machine === 'win11') await savePageBlob(page, '#download-colmap', path);
      else {
        const [dl] = await Promise.all([
          page.waitForEvent('download'),
          page.click('#download-colmap'),
        ]);
        await dl.saveAs(path);
      }
      const members = untar(readFileSync(path));
      const want = ['cameras.bin', 'images.bin', 'points3D.bin'];
      const ok = want.every((f) =>
        members[`sparse/0/${f}`]?.equals(readFileSync(`${a.colmap}/${f}`)),
      );
      check(
        `${name}: COLMAP archive holds the map's final model`,
        ok,
        Object.keys(members).join(' '),
      );
    }
    // acceptance: the downloaded PLY in the viewer's open-a-file page
    const v = await browser.context.newPage();
    await v.setViewportSize({ width: 1280, height: 900 });
    capture(v, consoleLog, `${name}-viewer`);
    await v.goto(`${base}/vendor/splat-viewer/index.html`);
    if (machine === 'win11') {
      // files over 50 MB cannot be sent to a remote browser: choose the same bytes (hash checked
      // above) from the job's storage, through the same file input
      await v.evaluate(
        async ([path, fname]) => {
          let d = await (await navigator.storage.getDirectory()).getDirectoryHandle('websplat');
          const parts = path.split('/');
          for (const p of parts.slice(0, -1)) d = await d.getDirectoryHandle(p);
          const f = await (await d.getFileHandle(parts.at(-1))).getFile();
          const dt = new DataTransfer();
          dt.items.add(new File([f], fname));
          const input = document.getElementById('file');
          input.files = dt.files;
          input.dispatchEvent(new Event('change'));
        },
        [m.outputs['splat.ply'].path, 'downloaded.ply'],
      );
    } else {
      await v.setInputFiles('#file', `${out}/${name}/splat.ply`);
    }
    await v.waitForFunction(
      () =>
        !document.getElementById('thermal-controls').hidden &&
        document.getElementById('status').hidden,
      null,
      { timeout: 180000 },
    );
    await v.waitForTimeout(2000);
    await v.screenshot({ path: `${out}/${name}-viewer.png` });
    await v.mouse.move(640, 450);
    await v.waitForTimeout(300);
    const acc = await v.evaluate(() => ({
      title: document.title,
      stats: document.getElementById('stats').textContent,
      hover: document.getElementById('hover').textContent,
      lo: document.getElementById('lo').value,
      hi: document.getElementById('hi').value,
    }));
    rep.viewer = acc;
    check(
      `${name}: downloaded PLY opens with temperatures in the viewer`,
      /peak -?[\d.]+ °C in view/.test(acc.stats) && acc.title.includes(name),
      JSON.stringify(acc),
    );
    // and its temperatures are the trained splat's: peak and readout against gsplat at the
    // start camera (viewer_reference.py on the same bytes)
    if (a['viewer-ref']) {
      const ref = JSON.parse(readFileSync(`${a['viewer-ref']}/${name}.json`, 'utf8'));
      const r = await compareViewer(v, ref);
      rep.viewerVsGsplat = r;
      check(`${name}: the viewer's temperatures = gsplat's at the start view`, r.ok, r.detail);
    }
    await v.close();
  }
  await page.evaluate(async () => {
    await window.websplat.shutdown();
    await window.websplat.testing.job().destroy();
  });
  await page.close();
  return rep;
}

try {
  for (const c of cases) {
    try {
      report.cases[c] = await runCase(c);
    } catch (e) {
      check(`${c}: driver`, false, e.stack ?? String(e));
    }
  }
} finally {
  await browser.close();
  server.close();
}
const errors = consoleLog.filter((l) => ['error', 'pageerror', 'crash', 'http'].includes(l.type));
for (const e of errors) say(`console ${e.tag} ${e.type}: ${e.text}`);
check('no console errors', !errors.length, `${errors.length}`);
report.console = consoleLog;
writeFileSync(`${out}/page-${machine}.json`, JSON.stringify(report, null, 1));
say(failures ? `${failures} FAILED` : 'page checks: all pass');
process.exit(failures ? 1 : 0);
