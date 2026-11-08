/** Stage 1 acceptance: index in Node, then index + decode every frame in a headless Chrome
 * module worker; both compared with native_dump.py (run here if its output is missing).
 * Usage: node test_ingest.mjs [mkv]   (default $WEBSPLAT_DATA/input/orbit.mkv)
 */
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { dataDir, openHarness, TOOLS } from './harness.mjs';
import { indexMkv, ingestRecord, sourceIdentity } from '../../app/src/stages/ingest/ingest.js';
import { FFMPEG_VERSION } from '../../../../viewer/src/versions.js';

const data = dataDir();
const mkv = process.argv[2] ?? path.join(data, 'input/orbit.mkv');
const out = path.join(data, 'stage12', path.basename(mkv, '.mkv'));
const python = path.join(TOOLS, 'GaussianSplat/.venv/bin/python');
const here = path.dirname(new URL(import.meta.url).pathname);
fs.mkdirSync(out, { recursive: true });
const nativeJson = path.join(out, 'native/native.json');
if (!fs.existsSync(nativeJson))
  execFileSync(
    python,
    [path.join(here, 'native_dump.py'), mkv, path.join(out, 'native'), '--raw'],
    { stdio: 'inherit' },
  );

const compare = (file, ...flags) => {
  try {
    execFileSync(python, [path.join(here, 'compare.py'), nativeJson, file, ...flags], {
      stdio: 'inherit',
    });
    return true;
  } catch {
    return false;
  }
};

// Node: demux and telemetry only
const blob = await fs.openAsBlob(mkv);
let t = performance.now();
const ingest = await indexMkv(blob);
const nodeMs = performance.now() - t;
const nodeSource = await sourceIdentity(Object.assign(blob, { name: path.basename(mkv) }));
fs.writeFileSync(
  path.join(out, 'ingest-node.json'),
  JSON.stringify({ index: ingestRecord(ingest, nodeSource) }),
);
ingest.source.close();
const nodeOk = compare(path.join(out, 'ingest-node.json'));

// Chrome: the stage worker, with every frame decoded
const rel = path.relative(data, mkv);
if (rel.startsWith('..')) throw new Error('the mkv must be under $WEBSPLAT_DATA for the browser');
const h = await openHarness('stage12');
let web;
try {
  web = await h.page.evaluate(
    async ({ rel, coreURL }) => {
      const app = '/tools/GaussianSplat/WebSplat/app/src/';
      const { runWorker } = await import(app + 'workers/host.js');
      const { readJson, remove } = await import(app + 'storage/opfs.js');
      const file = new File(
        [await (await fetch('/websplat-data/' + rel)).blob()],
        rel.split('/').pop(),
      );
      const progress = { messages: 0, phases: [] };
      const outDir = 'websplat-test/ingest';
      const result = await runWorker(
        () => new Worker(app + 'stages/ingest/ingest.worker.js', { type: 'module' }),
        { file, outDir, verify: true, coreURL: new URL(coreURL, location.href).href },
        {
          handlers: { progress: () => progress.messages++, phase: (p) => progress.phases.push(p) },
        },
      );
      const index = await readJson(outDir + '/index.json');
      const proposal = await readJson(outDir + '/proposal.json');
      await remove('websplat-test');
      return {
        index,
        hashes: result.hashes,
        timings: result.timings,
        telemetry: result.telemetry,
        proposal,
        progress,
      };
    },
    { rel, coreURL: `/vendor/ffmpeg/${FFMPEG_VERSION}/ffmpeg-core.js` },
  );
} finally {
  await h.close();
}
fs.writeFileSync(path.join(out, 'ingest-chrome.json'), JSON.stringify(web));
let chromeOk = compare(path.join(out, 'ingest-chrome.json'), '--require-hashes');
// source identity: the same sha256c in Node and Chrome, and equal to an independent computation
const ref = execFileSync(
  python,
  [
    '-c',
    `import hashlib,sys
d=b''.join(hashlib.sha256(c).digest() for c in iter(lambda f=open(sys.argv[1],'rb'): f.read(4<<20), b''))
print('sha256c:'+hashlib.sha256(d).hexdigest())`,
    mkv,
  ],
  { encoding: 'utf8' },
).trim();
if (web.index.source.hash !== ref || nodeSource.hash !== ref) {
  console.log('source hash mismatch', web.index.source.hash, nodeSource.hash, ref);
  chromeOk = false;
} else console.log('source identity', ref);
const n = web.index.frames.length;
console.log(
  'telemetry',
  JSON.stringify(web.telemetry),
  'proposal',
  JSON.stringify(web.proposal.proposal),
);
console.log(
  JSON.stringify({
    nodeIndexMs: Math.round(nodeMs),
    chrome: web.timings,
    decodeFramesPerS: +((n / web.timings.decodeMs) * 1000).toFixed(1),
    progress: web.progress,
  }),
);
fs.writeFileSync(
  path.join(out, 'ingest-chrome-timings.json'),
  JSON.stringify({ nodeIndexMs: nodeMs, ...web.timings }),
);
process.exit(nodeOk && chromeOk ? 0 : 1);
