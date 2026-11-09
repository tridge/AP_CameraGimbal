// Copy files out of a harness Chrome profile's OPFS (origin 127.0.0.1:PORT), e.g. a finished
// job's map/final model, which e2e.mjs does not copy.
// usage: node opfs_copy.mjs PROFILE_NAME PATTERN OUT_DIR [--port 8797]
//   PATTERN: a regular expression on OPFS paths under websplat/
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname } from 'node:path';
import { APP_DIST, args, env } from '../harness/lib/env.mjs';
import { launchLocal } from '../harness/lib/browser.mjs';
import { serve } from '../harness/lib/serve.mjs';

const a = args();
const [profileName, pattern, out] = a._;
const port = +(a.port ?? 8797);
const server = await serve(APP_DIST, port, '127.0.0.1', {});
const browser = await launchLocal({ profile: `${env('WEBSPLAT_DATA')}/harness/profiles/${profileName}`, adapter: 'high-performance' });
try {
  const page = await browser.context.newPage();
  await page.goto(`http://127.0.0.1:${port}/__blank`);
  const files = await page.evaluate(async (pat) => {
    const re = new RegExp(pat);
    const out = [];
    const walk = async (d, p) => {
      for await (const [n, h] of d.entries()) {
        const q = p ? `${p}/${n}` : n;
        if (h.kind === 'directory') await walk(h, q);
        else if (re.test(q)) {
          const u = new Uint8Array(await (await h.getFile()).arrayBuffer());
          let s = '';
          for (let i = 0; i < u.length; i += 0x8000) s += String.fromCharCode(...u.subarray(i, i + 0x8000));
          out.push([q, btoa(s)]);
        }
      }
    };
    await walk(await (await navigator.storage.getDirectory()).getDirectoryHandle('websplat'), '');
    return out;
  }, pattern);
  for (const [p, b64] of files) {
    mkdirSync(dirname(`${out}/${p}`), { recursive: true });
    writeFileSync(`${out}/${p}`, Buffer.from(b64, 'base64'));
    console.log(p);
  }
} finally {
  await browser.close();
  server.close();
}
process.exit(0);
