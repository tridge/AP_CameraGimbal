/** Non-keyframe FFV1 decoding in headless Chrome: the batch decoder on make_gop_fixture.py's
 * GOP-8 file, with small batch limits so chains are continued, restarted and rejected; every
 * decoded frame must equal native in-order decoding. */
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { dataDir, openHarness, TOOLS } from './harness.mjs';
import { FFMPEG_VERSION } from '../../../../viewer/src/versions.js';

const dir = path.join(dataDir(), 'stage12', 'fixtures');
const here = path.dirname(new URL(import.meta.url).pathname);
if (!fs.existsSync(path.join(dir, 'gop.json')))
  execFileSync(
    path.join(TOOLS, 'GaussianSplat/.venv/bin/python'),
    [path.join(here, 'make_gop_fixture.py'), dir],
    { stdio: 'inherit' },
  );
const ref = JSON.parse(fs.readFileSync(path.join(dir, 'gop.json'), 'utf8'));
const requests = [
  [...Array(40).keys()],
  [3, 4, 11, 17, 23, 39],
  [7],
  [15, 16],
  [31, 32, 33],
  [0, 8, 16, 24, 32],
  [38, 2],
];
const h = await openHarness('stage12');
let r;
try {
  r = await h.page.evaluate(
    async ({ requests, coreURL }) => {
      const root = '/tools/GaussianSplat/WebSplat/app/src/stages/ingest/';
      const { indexMkv } = await import(root + 'ingest.js');
      const { FrameDecoder } = await import(root + 'frame-decoder.js');
      const file = await (await fetch('/websplat-data/stage12/fixtures/gop.mkv')).blob();
      const ingest = await indexMkv(file);
      const hex = (b) =>
        [...new Uint8Array(b)].map((x) => x.toString(16).padStart(2, '0')).join('');
      const dec = new FrameDecoder(ingest, {
        coreURL: new URL(coreURL, location.href).href,
        workers: 2,
        batchFrames: 3,
        maxChainFrames: 8,
      });
      const out = {
        keyframes: ingest.frames.filter((f) => f.keyframe).map((f) => f.index),
        runs: [],
      };
      for (const want of requests) {
        const got = {};
        for await (const f of dec.decode(want))
          got[f.index] = hex(await crypto.subtle.digest('SHA-256', f.data));
        out.runs.push(got);
      }
      out.packets = dec.stats.packets;
      dec.close();
      // a chain beyond the limit is rejected before anything is read
      const small = new FrameDecoder(ingest, {
        coreURL: new URL(coreURL, location.href).href,
        maxChainFrames: 4,
      });
      try {
        for await (const f of small.decode([7])) out.unexpected = f.index;
      } catch (e) {
        out.rejected = {
          name: e.constructor.name,
          message: e.message,
          packetsRead: small.stats.packets,
        };
      }
      small.close();
      return out;
    },
    { requests, coreURL: `/vendor/ffmpeg/${FFMPEG_VERSION}/ffmpeg-core.js` },
  );
} finally {
  await h.close();
}
let ok = r.keyframes.join() === '0,8,16,24,32';
requests.forEach((want, k) => {
  const got = r.runs[k];
  const bad = want.filter((i) => got[i] !== ref.frames[i].sha256);
  const extra = Object.keys(got).length !== new Set(want).size;
  if (bad.length || extra) ok = false;
  console.log(
    `${bad.length || extra ? 'FAIL' : 'PASS'} frames ${want.join(',')}${bad.length ? ' differ: ' + bad : ''}`,
  );
});
const rej = r.rejected;
const rejOk =
  rej?.name === 'BatchLimitError' && rej.packetsRead === 0 && r.unexpected === undefined;
console.log(`${rejOk ? 'PASS' : 'FAIL'} over-limit chain rejected before reading: ${rej?.message}`);
console.log(`decoded ${r.packets} packets for the requests`);
process.exit(ok && rejOk ? 0 : 1);
