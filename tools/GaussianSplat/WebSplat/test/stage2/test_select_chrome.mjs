/** Stage 2 acceptance in headless Chrome: the stage worker indexes, decodes with the FFmpeg-wasm
 * batch decoder, proposes the parameters and selects frames into OPFS, for each decode mode and
 * case; frames.json and per-frame hashes of features.u8/thermal.u16 are compared with native
 * extract_frames.py run with the same options.
 * Usage: node test_select_chrome.mjs [modes, default single,redecode,cache]
 */
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { dataDir, openHarness } from '../stage1/harness.mjs';
import { FFMPEG_VERSION } from '../../../../viewer/src/versions.js';
import { python, runNative } from './runner.mjs';

const modes = (process.argv[2] ?? 'single,redecode,cache').split(',');
const data = dataDir();
const mkv = path.join(data, 'input/orbit.mkv');
const base = path.join(data, 'stage12', 'orbit');
const here = path.dirname(new URL(import.meta.url).pathname);
// proposal in each mode, plus drift rejection (exercises single mode's compaction in OPFS)
const cases = [
  ...modes.map((mode) => ({ name: `proposal-${mode}`, mode })),
  { name: 'drift-single', mode: 'single', extra: { max_median_dev: 0.4 } },
];

const h = await openHarness('stage12');
let runs, robust;
try {
  ({ results: runs, robust } = await h.page.evaluate(
    async ({ cases, coreURL }) => {
      const app = '/tools/GaussianSplat/WebSplat/app/src/';
      const { runWorker } = await import(app + 'workers/host.js');
      const { readJson, readFile, list, remove } = await import(app + 'storage/opfs.js');
      const file = new File(
        [await (await fetch('/websplat-data/input/orbit.mkv')).blob()],
        'orbit.mkv',
      );
      const coreHref = new URL(coreURL, location.href).href;
      const run = async (worker, msg, opts = {}) => {
        const t0 = performance.now();
        const logs = [];
        const value = await runWorker(() => new Worker(app + worker, { type: 'module' }), msg, {
          ...opts,
          handlers: { log: (level, text) => logs.push(text), ...opts.handlers },
        });
        return { value, logs, wallMs: performance.now() - t0 };
      };
      const hex = (b) =>
        [...new Uint8Array(b)].map((x) => x.toString(16).padStart(2, '0')).join('');
      async function collect(outDir) {
        const names = (await list(outDir)).map((e) => e.name);
        const framesJson = await (await readFile(outDir + '/frames.json')).text();
        const info = JSON.parse(framesJson);
        const px = info.width * info.height;
        const hashes = { features: [], thermal: [] };
        for (const [n, key, bytes] of [
          ['features.u8', 'features', 1],
          ['thermal.u16', 'thermal', 2],
        ]) {
          const buf = await (await readFile(`${outDir}/${n}`)).arrayBuffer();
          if (buf.byteLength !== info.frames.length * px * bytes)
            hashes[key + 'Size'] = buf.byteLength;
          for (let k = 0; k < info.frames.length; k++)
            hashes[key].push(
              hex(
                await crypto.subtle.digest(
                  'SHA-256',
                  buf.slice(k * px * bytes, (k + 1) * px * bytes),
                ),
              ),
            );
        }
        return { names, framesJson, hashes };
      }
      const results = [];
      for (const c of cases) {
        // fresh workers per case, so wasm loading and indexing are counted each time
        const ingestDir = `websplat-test/${c.name}/ingest`;
        const ingest = await run('stages/ingest/ingest.worker.js', {
          file,
          outDir: ingestDir,
          coreURL: coreHref,
        });
        const proposal = await readJson(ingestDir + '/proposal.json');
        const p = proposal.proposal;
        const params = { lat: p.lat, lon: p.lon, rotation: p.rotation, ...(c.extra ?? {}) };
        const outDir = `websplat-test/${c.name}/select`;
        const result = await run('stages/select/select.worker.js', {
          file,
          outDir,
          ingestDir,
          select: params,
          mode: c.mode,
          coreURL: coreHref,
        });
        const { names, framesJson, hashes } = await collect(outDir);
        results.push({
          ...c,
          proposal,
          ingest: { wallMs: ingest.wallMs, timings: ingest.value.timings },
          params,
          result: { ...result.value, wallMs: result.wallMs },
          hashes,
          framesJson,
          files: names,
        });
      }
      // robustness: source identity, quota failure, cancellation, forced termination; after each
      // failure the same output directory must be reusable (handles released)
      const robust = { checks: [] };
      {
        const ingestDir = 'websplat-test/robust/ingest';
        await run('stages/ingest/ingest.worker.js', { file, outDir: ingestDir, coreURL: coreHref });
        const p = (await readJson(ingestDir + '/proposal.json')).proposal;
        const params = { lat: p.lat, lon: p.lon, rotation: p.rotation };
        const outDir = 'websplat-test/robust/select';
        const select = (msg = {}, opts = {}) =>
          run(
            'stages/select/select.worker.js',
            { file, outDir, ingestDir, select: params, coreURL: coreHref, ...msg },
            opts,
          );
        const outcome = async (promise) => {
          try {
            await promise;
            return { ok: true };
          } catch (e) {
            return { ok: false, name: e.name, message: e.message };
          }
        };
        const files = async () => (await list(outDir)).map((e) => e.name);
        // same size, one changed byte inside a compressed packet
        const bytes = new Uint8Array(await file.arrayBuffer());
        bytes[bytes.length >> 1] ^= 1;
        const other = new File([bytes], 'orbit.mkv');
        robust.checks.push({
          check: 'changed source rejected',
          ...(await outcome(select({ file: other }))),
        });
        robust.checks.push({
          check: 'quota failure',
          ...(await outcome(select({ testFault: { writeBytes: 20 << 20 } }))),
          files: await files(),
        });
        let c = new AbortController();
        robust.checks.push({
          check: 'cancel',
          ...(await outcome(
            select(
              {},
              { signal: c.signal, handlers: { progress: (done) => done >= 30 && c.abort() } },
            ),
          )),
          files: await files(),
        });
        c = new AbortController();
        const t0 = performance.now();
        robust.checks.push({
          check: 'forced termination',
          ...(await outcome(
            select(
              {},
              {
                signal: c.signal,
                stopGraceMs: 0,
                handlers: { progress: (done) => done >= 10 && c.abort() },
              },
            ),
          )),
          ms: performance.now() - t0,
        });
        const rerun = await select();
        robust.rerun = { ...(await collect(outDir)), result: rerun.value };
      }
      await remove('websplat-test');
      return { results, robust };
    },
    { cases, coreURL: `/vendor/ffmpeg/${FFMPEG_VERSION}/ffmpeg-core.js` },
  ));
} finally {
  await h.close();
}

let ok = true;
const summary = [];
for (const run of runs) {
  const native = runNative(mkv, base, run.params);
  const out = path.join(base, 'select-chrome', run.name);
  fs.rmSync(out, { recursive: true, force: true });
  fs.mkdirSync(out, { recursive: true });
  fs.writeFileSync(path.join(out, 'frames.json'), run.framesJson);
  fs.writeFileSync(path.join(out, 'hashes.json'), JSON.stringify(run.hashes));
  console.log(
    `[${run.name}] proposal ${JSON.stringify(run.proposal.proposal)}; files ${run.files.join(' ')}`,
  );
  for (const l of [...run.proposal.notes, ...run.result.log]) console.log('  ' + l);
  let pass =
    native.ok &&
    run.files.join() === 'features.u8,frames.json,thermal.u16' &&
    !run.hashes.featuresSize &&
    !run.hashes.thermalSize;
  try {
    execFileSync(
      python,
      [
        path.join(here, 'compare_select.py'),
        native.dir,
        out,
        '--hashes',
        path.join(out, 'hashes.json'),
      ],
      { stdio: 'inherit' },
    );
  } catch {
    pass = false;
  }
  ok &&= pass;
  console.log(`${pass ? 'PASS' : 'FAIL'} ${run.name}`);
  summary.push({
    name: run.name,
    mode: run.mode,
    ingestMs: run.ingest.wallMs,
    ingest: run.ingest.timings,
    selectMs: run.result.wallMs,
    ...run.result.timings,
    framesDecoded: run.result.framesDecoded,
    bytesWritten: run.result.bytesWritten,
  });
}
// robustness checks, then the rerun into the reused directory must still equal native
const expect = {
  'changed source rejected': (c) => !c.ok && /content differs/.test(c.message),
  'quota failure': (c) => !c.ok && c.name === 'QuotaExceededError' && c.files.length === 0,
  cancel: (c) => !c.ok && c.name === 'AbortError' && c.files.length === 0,
  'forced termination': (c) => !c.ok && c.name === 'AbortError',
};
for (const c of robust.checks) {
  const pass = expect[c.check](c);
  ok &&= pass;
  console.log(
    `${pass ? 'PASS' : 'FAIL'} ${c.check}: ${c.ok ? 'succeeded' : `${c.check === 'forced termination' ? `${Math.round(c.ms)} ms, ` : ''}${c.name}: ${c.message}`}${c.files ? `; files left: [${c.files}]` : ''}`,
  );
}
{
  const p = JSON.parse(robust.rerun.framesJson).selection.options;
  const native = runNative(mkv, base, { lat: p.lat, lon: p.lon, rotation: p.rotation });
  const out = path.join(base, 'select-chrome', 'rerun-after-failures');
  fs.rmSync(out, { recursive: true, force: true });
  fs.mkdirSync(out, { recursive: true });
  fs.writeFileSync(path.join(out, 'frames.json'), robust.rerun.framesJson);
  fs.writeFileSync(path.join(out, 'hashes.json'), JSON.stringify(robust.rerun.hashes));
  let pass = robust.rerun.names.join() === 'features.u8,frames.json,thermal.u16';
  try {
    execFileSync(
      python,
      [
        path.join(here, 'compare_select.py'),
        native.dir,
        out,
        '--hashes',
        path.join(out, 'hashes.json'),
      ],
      { stdio: ['ignore', 'pipe', 'inherit'] },
    );
  } catch {
    pass = false;
  }
  ok &&= pass;
  console.log(`${pass ? 'PASS' : 'FAIL'} rerun into the same OPFS directory after the failures`);
}
fs.writeFileSync(path.join(base, 'select-chrome-timings.json'), JSON.stringify(summary, null, 1));
console.log(JSON.stringify(summary, (k, v) => (typeof v === 'number' ? Math.round(v) : v), 1));
process.exit(ok ? 0 : 1);
