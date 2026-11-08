/** CLAHE bit-exactness against cv2 (clahe_cases.py), including tiny images and sizes where one
 * or neither dimension is divisible by the 8x8 tile grid. */
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { dataDir } from '../stage1/harness.mjs';
import { python } from './runner.mjs';
import { clahe } from '../../app/src/stages/select/clahe.js';

const dir = path.join(dataDir(), 'stage12', 'clahe');
const here = path.dirname(new URL(import.meta.url).pathname);
const rawDir = path.join(dataDir(), 'stage12', 'orbit', 'native', 'raw');
fs.rmSync(dir, { recursive: true, force: true });
execFileSync(
  python,
  [path.join(here, 'clahe_cases.py'), dir, ...(fs.existsSync(rawDir) ? [rawDir] : [])],
  { stdio: 'inherit' },
);
let failures = 0,
  n = 0;
for (const f of fs
  .readdirSync(dir)
  .filter((f) => f.endsWith('.in.u8'))
  .sort()) {
  const [, w, h] = /_(\d+)x(\d+)\.in\.u8$/.exec(f).map(Number);
  const src = new Uint8Array(fs.readFileSync(path.join(dir, f)));
  const ref = new Uint8Array(fs.readFileSync(path.join(dir, f.replace('.in.', '.out.'))));
  const out = clahe(src, w, h);
  let diff = 0;
  for (let i = 0; i < ref.length; i++) if (out[i] !== ref[i]) diff++;
  n++;
  if (diff || out.length !== ref.length) {
    failures++;
    console.log(`FAIL ${f}: ${diff} pixels differ`);
  }
}
console.log(`CLAHE: ${n - failures}/${n} cases bit-identical to cv2`);
process.exit(failures ? 1 : 0);
