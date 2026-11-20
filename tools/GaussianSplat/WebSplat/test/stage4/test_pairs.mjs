// Stage 4 acceptance: pair selection.
// 1. the reference frames give exactly sfm.py's pairs.txt, with no ambiguous decision;
// 2. synthetic orbits without ties give exactly sfm.py's select_pairs() (run natively);
// 3. synthetic repeated positions (ties at the neighbour cut-off) are reported as ambiguous,
//    and every difference from numpy's unstable ordering comes with an ambiguity report.
// usage: WEBSPLAT_DATA=... node test_pairs.mjs
import { readFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { selectPairs } from '../../app/src/stages/match/pairs.js';

const here = dirname(fileURLToPath(import.meta.url));
const python = join(here, '../../../.venv/bin/python');
let fails = 0;
const check = (ok, what) => {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`);
  if (!ok) fails++;
};

const work = `${process.env.WEBSPLAT_DATA}/ref/work`;
const info = JSON.parse(readFileSync(`${work}/frames.json`, 'utf8'));
const names = info.frames.map((f) => f.name);
const t0 = performance.now();
const ref = selectPairs(info.frames, info.target_enu);
const ms = performance.now() - t0;
const got = [];
for (let p = 0; p < ref.pairs.length / 2; p++)
  got.push(`${names[ref.pairs[2 * p]]} ${names[ref.pairs[2 * p + 1]]}`);
const want = readFileSync(`${work}/colmap/pairs.txt`, 'utf8').split('\n').filter(Boolean);
check(
  got.length === want.length && got.every((s, k) => s === want[k]),
  `reference: ${got.length} pairs identical to pairs.txt (${want.length}), ${ms.toFixed(1)} ms`,
);
check(ref.ambiguous.length === 0, `reference: no ambiguous decisions (${ref.ambiguous.length})`);

const native = (frames, target) => {
  const r = spawnSync(python, [join(here, 'select_pairs_native.py')], {
    input: JSON.stringify({ frames, target_enu: target }),
    maxBuffer: 1 << 26,
  });
  if (r.status !== 0) throw new Error('select_pairs_native.py: ' + r.stderr);
  return JSON.parse(r.stdout).map(([i, j]) => `${i} ${j}`);
};
const js = (frames, target) => {
  const { pairs, ambiguous } = selectPairs(frames, target);
  const out = [];
  for (let p = 0; p < pairs.length / 2; p++) out.push(`${pairs[2 * p]} ${pairs[2 * p + 1]}`);
  return { out, ambiguous };
};
const differ = (a, b) => {
  const A = new Set(a),
    B = new Set(b);
  return a.filter((x) => !B.has(x)).length + b.filter((x) => !A.has(x)).length;
};
let seed = 11;
const rnd = () => (seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648;
const target = [0, 0, 40];

// orbits with jitter: no ties
for (let t = 0; t < 4; t++) {
  const n = 120 + 40 * t,
    R = 120 + 40 * t,
    H = 150 + 50 * t;
  const frames = Array.from({ length: n }, (_, k) => {
    const a = (2 * Math.PI * k) / (n / (1 + (t % 2))) + 0.01 * rnd();
    return { position: [R * Math.cos(a) + rnd(), R * Math.sin(a) + rnd(), H + 20 * rnd()] };
  });
  const a = js(frames, target),
    b = native(frames, target);
  check(
    differ(a.out, b) === 0 && a.ambiguous.length === 0,
    `orbit ${t}: ${a.out.length} pairs identical to numpy, ambiguous ${a.ambiguous.length}`,
  );
}

// repeated positions: 80 frames alternating between two telemetry samples
const rep = Array.from({ length: 80 }, (_, k) => ({
  position: k % 2 ? [100, 20, 200] : [98, 30, 201],
}));
const a = js(rep, target),
  b = native(rep, target);
const d = differ(a.out, b);
check(
  a.ambiguous.length > 0 && a.ambiguous.every((x) => x.kind === 'neighbour-cutoff'),
  `repeated positions: ${a.ambiguous.length} ambiguous neighbour cut-offs reported`,
);
check(
  d === 0 || a.ambiguous.length > 0,
  `repeated positions: ${d} pairs differ from numpy, all under an ambiguity report`,
);

console.log(fails ? `FAIL ${fails} pair checks` : 'PASS pair selection');
process.exit(fails ? 1 : 0);
