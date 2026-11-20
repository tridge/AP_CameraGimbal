/** NumPy-exactness of the stage 2 statistics on synthetic and real frames (numeric_cases.py),
 * including the blank-frame test the canonical orbit never triggers. */
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { dataDir } from '../stage1/harness.mjs';
import { python } from './runner.mjs';
import {
  RawStats,
  celsiusFn,
  percentileArray,
  percentileScalarF32,
  medianF32,
} from '../../app/src/stages/select/numeric.js';
import { isBlank, featureInput } from '../../app/src/stages/select/select.js';

const dir = path.join(dataDir(), 'stage12', 'numeric');
const here = path.dirname(new URL(import.meta.url).pathname);
const rawDir = path.join(dataDir(), 'stage12', 'orbit', 'native', 'raw');
execFileSync(
  python,
  [path.join(here, 'numeric_cases.py'), dir, ...(fs.existsSync(rawDir) ? [rawDir] : [])],
  { stdio: 'inherit' },
);
let failures = 0;
for (const e of JSON.parse(fs.readFileSync(path.join(dir, 'expected.json'), 'utf8'))) {
  const b = fs.readFileSync(path.join(dir, e.name + '.u16'));
  const raw = new Uint16Array(b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength));
  const stats = new RawStats(raw);
  const c = celsiusFn(e.meta);
  const [lo, hi] = percentileArray(stats, [1, 99], c);
  const got = {
    blank: isBlank(stats, e.meta),
    median_c: medianF32(stats, c),
    p1_c: percentileScalarF32(stats, 1, c),
    lo,
    hi,
    min_c: c(stats.min),
    max_c: c(stats.max),
    feature_sha256: crypto
      .createHash('sha256')
      .update(featureInput(raw, stats, e.meta))
      .digest('hex'),
  };
  const bad = Object.keys(got).filter((k) => got[k] !== e[k]);
  if (bad.length) failures++;
  console.log(
    `${bad.length ? 'FAIL' : 'PASS'} ${e.name} blank=${e.blank}` +
      (bad.length ? ' ' + bad.map((k) => `${k}: ${got[k]} vs ${e[k]}`).join(', ') : ''),
  );
}
process.exit(failures ? 1 : 0);
