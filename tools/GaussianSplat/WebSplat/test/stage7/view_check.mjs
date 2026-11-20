// The viewer (as deployed, from the app's copy) against gsplat at given cameras: for each
// REF.json (viewer_reference.py) of PLY, open ?url=PLY&meta=(the PLY's header info with the
// reference camera) at the reference size and compare the viewer's peak and readout.
//   node view_check.mjs VIEWER_DIR OUT PLY:REF.json [PLY:REF.json ...] [--port 8862] [--profile NAME]
//        [--mask EXT,...] [--degraded]
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { args, env } from '../harness/lib/env.mjs';
import { capture, launchLocal } from '../harness/lib/browser.mjs';
import { serve } from '../harness/lib/serve.mjs';
import { compareViewer } from './viewer_compare.mjs';
import { apcgMeta, parsePlyHeader } from '../../app/src/stages/export/ply.js';

const a = args();
const [viewer, out, ...pairs] = a._;
if (!pairs.length) throw new Error('usage: node view_check.mjs VIEWER_DIR OUT PLY:REF.json ...');
mkdirSync(out, { recursive: true });
const extra = {};
const jobs = pairs.map((p, i) => {
  const [ply, refPath] = p.split(':');
  const ref = JSON.parse(readFileSync(refPath, 'utf8'));
  const info = apcgMeta(
    parsePlyHeader(new Uint8Array(readFileSync(ply).subarray(0, 1 << 20))).comments,
  );
  writeFileSync(`${out}/meta${i}.json`, JSON.stringify({ ...info, camera: ref.camera }));
  extra[`/__data/s${i}.ply`] = ply;
  extra[`/__data/m${i}.json`] = `${out}/meta${i}.json`;
  return { i, ply, refPath, ref };
});
const port = +(a.port ?? 8862);
// --mask EXT,EXT: WebGL extensions hidden from the page (the viewer's fallbacks); --degraded: the
// half-float fallback is expected (notice and approximate readouts checked, error reported)
const mask = a.mask ? String(a.mask).split(',') : [];
function hideExtensions(names) {
  for (const C of [WebGL2RenderingContext, WebGLRenderingContext]) {
    const get = C.prototype.getExtension;
    const list = C.prototype.getSupportedExtensions;
    C.prototype.getExtension = function (n) {
      return names.includes(n) ? null : get.call(this, n);
    };
    C.prototype.getSupportedExtensions = function () {
      return list.call(this).filter((n) => !names.includes(n));
    };
  }
}
const server = await serve(viewer, port, '127.0.0.1', extra);
const browser = await launchLocal({
  profile: `${env('WEBSPLAT_DATA')}/chrome-profiles/${a.profile ?? 'stage7-view'}`,
});
const log = [];
let failures = 0;
try {
  for (const j of jobs) {
    const page = await browser.context.newPage();
    capture(page, log, `view${j.i}`);
    if (mask.length) await page.addInitScript(hideExtensions, mask);
    await page.setViewportSize({ width: j.ref.width, height: j.ref.height });
    await page.goto(
      `http://127.0.0.1:${port}/index.html?url=/__data/s${j.i}.ply&meta=/__data/m${j.i}.json`,
    );
    await page.waitForFunction(
      () =>
        !document.getElementById('thermal-controls').hidden &&
        document.getElementById('status').hidden,
      null,
      { timeout: 180000 },
    );
    const r = await compareViewer(page, j.ref);
    const what = `${j.refPath.split('/').pop()}${mask.length ? ` (masked ${mask.join(', ')})` : ''}`;
    if (a.degraded) {
      // half-float fallback: must say so; its error is reported, not bounded
      const ok = !!r.notice && r.approx && r.lit;
      console.log(
        `${ok ? 'PASS' : 'FAIL'} degraded precision is shown, ${what}: notice ${!!r.notice}, ` +
          `readouts marked approximate ${r.approx}; error peak ${r.errors.peakC.toFixed(2)} C, ` +
          `readout ${r.errors.readoutC.toFixed(2)} C (reported, not bounded)`,
      );
      if (!ok) failures++;
    } else {
      console.log(
        `${r.ok ? 'PASS' : 'FAIL'} viewer = gsplat, ${what}: ${r.detail}, image lit ${r.lit}`,
      );
      if (!r.ok) failures++;
    }
    await page.screenshot({ path: `${out}/view${j.i}.png` });
    await page.close();
  }
} finally {
  await browser.close();
  server.close();
}
const errors = log.filter((l) => ['error', 'pageerror', 'crash', 'http'].includes(l.type));
for (const e of errors) console.log(`console ${e.tag} ${e.type}: ${e.text}`);
if (errors.length) failures++;
console.log(failures ? `${failures} FAILED` : 'view checks: all pass');
process.exit(failures ? 1 : 0);
