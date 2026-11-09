// Estimate accuracy of one full page run (notes/stage8.md, E1-E3), from the run's samples.json
// (every 2 s: stage states, priors, remaining times, overall estimate) and report.json (stage
// timings; `eta`: the train stage's remaining time every 10 s).
//   E1: a stage lasting >= 30 s: the prior the page held when it started, within +-25% of its duration
//   E2: training, from 10% of its iterations: every remaining time shown, within +-15% of the actual
//   E3: the whole-job remaining time when training starts, within +-20% of the actual
// usage: node eta_check.mjs RUN_DIR [--json OUT]       (exit 1 if a criterion fails)
import { readFileSync, writeFileSync } from 'node:fs';

const args = process.argv.slice(2);
const dir = args[0];
const jsonOut = args.includes('--json') ? args[args.indexOf('--json') + 1] : null;
const samples = JSON.parse(readFileSync(`${dir}/samples.json`, 'utf8'));
const report = JSON.parse(readFileSync(`${dir}/report.json`, 'utf8'));
const st = (x, id) => x.stages.find((s) => s.id === id);
const res = { e1: [], e2: null, e3: null };
let ok = true;

// stage start and end walls from the samples (2 s resolution)
const firstWhere = (f) => samples.find(f);
const lastBefore = (wall, f) => [...samples].reverse().find((x) => x.wall < wall && f(x));
for (const t of report.timings) {
  const sec = (t.elapsedMs ?? 0) / 1000;
  if (sec < 30) continue;
  const start = firstWhere((x) => st(x, t.id)?.state === 'running');
  const before = start && lastBefore(start.wall, (x) => st(x, t.id)?.prior != null);
  const prior = before ? st(before, t.id).prior : null;
  const err = prior == null ? null : (prior - sec) / sec;
  const pass = err != null && Math.abs(err) <= 0.25;
  ok &&= pass;
  res.e1.push({ stage: t.id, seconds: sec, prior, err, pass });
  console.log(`E1 ${t.id}: prior ${prior?.toFixed(0)} s vs ${sec.toFixed(0)} s (${err == null ? 'no prior' : (100 * err).toFixed(1) + '%'}) ${pass ? 'PASS' : 'FAIL'}`);
}

const trainDone = firstWhere((x) => st(x, 'train')?.state === 'done');
if (trainDone) {
  const rows = (report.eta ?? []).filter((e) => e.done != null && e.remaining != null);
  const total = st(trainDone, 'train').total ?? 30000;
  const from = rows.filter((e) => e.done >= 0.1 * total);
  const errs = from.map((e) => ({ done: e.done, shown: e.remaining, actual: (trainDone.wall - e.wall) / 1000 }))
    .map((e) => ({ ...e, err: (e.shown - e.actual) / e.actual }));
  // v2/v3 (notes/stage8.md): +-15% or +-10 s, whichever is larger; --v1: +-15% only
  const v1 = args.includes('--v1');
  const over = (e) => Math.abs(e.shown - e.actual) - Math.max(0.15 * e.actual, v1 ? 0 : 10);
  const worst = errs.reduce((a, e) => (a === null || over(e) > over(a) ? e : a), null);
  const failing = errs.filter((e) => over(e) > 0).length;
  const pass = errs.length > 0 && failing === 0;
  ok &&= pass;
  res.e2 = { samples: errs.length, failing, worst, pass };
  console.log(`E2 training remaining time: ${errs.length} samples, ${failing} outside the bound, worst ${worst ? (100 * worst.err).toFixed(1) + '% at ' + worst.done + ' (shown ' + worst.shown.toFixed(0) + ' s, actual ' + worst.actual.toFixed(0) + ' s)' : '-'} ${pass ? 'PASS' : 'FAIL'}`);
}

const trainStart = firstWhere((x) => st(x, 'train')?.state === 'running' && x.overall != null);
const end = firstWhere((x) => st(x, 'export')?.state === 'done');
if (trainStart && end) {
  const actual = (end.wall - trainStart.wall) / 1000;
  const err = (trainStart.overall - actual) / actual;
  const pass = Math.abs(err) <= 0.2;
  ok &&= pass;
  res.e3 = { shown: trainStart.overall, actual, err, pass };
  console.log(`E3 job remaining at train start: ${trainStart.overall.toFixed(0)} s vs ${actual.toFixed(0)} s (${(100 * err).toFixed(1)}%) ${pass ? 'PASS' : 'FAIL'}`);
} else {
  ok = false;
  console.log('E3: no train start or export end in the samples FAIL');
}
if (jsonOut) writeFileSync(jsonOut, JSON.stringify({ ...res, pass: ok }, null, 1));
console.log(`estimates: ${ok ? 'PASS' : 'FAIL'}`);
process.exit(ok ? 0 : 1);
