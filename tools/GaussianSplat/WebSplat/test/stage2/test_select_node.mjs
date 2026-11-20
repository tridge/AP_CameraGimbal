/** Stage 2 acceptance in Node: proposal, then the port on native-decoded frames (stage 1 proved
 * the browser decode identical) against native extract_frames.py with the same options, for the
 * proposal in every decode mode and for option variants that exercise the other paths
 * (drift rejection, step, ranges, rotation, radius, no hot spot, fixed altitude, failures),
 * fallback proposals, and injected blank frames and incomplete telemetry (native_injected.py
 * applies the same injections to native).
 * Usage: node test_select_node.mjs [--mkv FILE] [--only NAME]
 */
import path from 'node:path';
import fs from 'node:fs';
import { parseArgs } from 'node:util';
import { dataDir } from '../stage1/harness.mjs';
import { execFileSync } from 'node:child_process';
import { injected, prepare, propose, python, runCase } from './runner.mjs';
import { hotFrames } from '../../app/src/stages/select/select.js';

const here = path.dirname(new URL(import.meta.url).pathname);
const { values: opt } = parseArgs({
  options: { mkv: { type: 'string' }, only: { type: 'string' } },
});
const mkv = opt.mkv ?? path.join(dataDir(), 'input/orbit.mkv');
const ctx = { mkv, ...(await prepare(mkv)) };
let failures = 0;
let t = performance.now();
const prop = await propose(ctx.ingest, ctx.decoder);
const proposeMs = performance.now() - t;
console.log('proposal', JSON.stringify(prop.proposal));
console.log(
  'rotations',
  JSON.stringify(prop.rotations.map((r) => [r.rotation, r.frames, +r.errorPx.toFixed(1)])),
);
for (const n of prop.notes) console.log('  ' + n);
const P = { lat: prop.proposal.lat, lon: prop.proposal.lon, rotation: prop.proposal.rotation };
// the hot spot's offset from a nearby given position (as a user typing rounded coordinates)
const near = { lat: +P.lat.toFixed(4), lon: +P.lon.toFixed(4), rotation: 0 };
// the frozen reference's extract_frames.py options (CONTRACTS.md selection example, stage 0)
const REF = { lat: -35.5802022, lon: 148.9065438, rotation: 0 };
// fallback proposals (no usable hot spot), passed whole into selection
const fallback = async (params, inject) => {
  const { ingest, decoder } = injected(ctx, inject);
  const r = await propose(ingest, decoder, { params });
  const pr = r.proposal;
  console.log(`fallback proposal for ${JSON.stringify(params)}: ${JSON.stringify(pr)}`);
  for (const n of r.notes) console.log('  ' + n);
  return {
    lat: pr.lat,
    lon: pr.lon,
    rotation: pr.rotation,
    target_alt: pr.target_alt,
    hfov: pr.hfov,
    ...params,
  };
};
const noHot = await fallback({ hotspot_min_c: 1000 });
const failedHot = await fallback({ hotspot_max_error: 5 });
const hotIdx = hotFrames(
  ctx.ingest.frames.map((f) => f.meta),
  60,
  0,
  200,
);
const blankHot = { blank: hotIdx.slice(0, 3) };
{
  // the proposal sees the blank hot frames too: 3 fewer rays
  const { ingest, decoder } = injected(ctx, blankHot);
  const r = await propose(ingest, decoder);
  const ok = r.hotspot?.frames === hotIdx.length - 3;
  console.log(
    `${ok ? 'PASS' : 'FAIL'} proposal with blank hot frames ${blankHot.blank}: ${r.hotspot?.frames} rays`,
  );
  if (!ok) failures++;
}
const tele = (patch) => ({ meta: patch });
{
  // the provisional origin comes from validated in-range poses: bad telemetry before start
  // must not change the proposal
  const base = await propose(ctx.ingest, ctx.decoder, { params: { start: 17 } });
  for (const bad of ['bad', null]) {
    const { ingest, decoder } = injected(ctx, {
      meta: { 0: { 'telemetry.position.lat_e7': bad } },
    });
    let r,
      err = null;
    try {
      r = await propose(ingest, decoder, { params: { start: 17 } });
    } catch (e) {
      err = e.message;
    }
    const ok =
      !err &&
      r.proposal.lat === base.proposal.lat &&
      r.proposal.lon === base.proposal.lon &&
      r.proposal.rotation === base.proposal.rotation;
    console.log(
      `${ok ? 'PASS' : 'FAIL'} proposal with start 17 and frame 0 lat_e7 ${JSON.stringify(bad)}: ${err ?? 'same as without'}`,
    );
    if (!ok) failures++;
  }
}
const cases = [
  ['reference', REF, 'single'],
  ['proposal-single', P, 'single'],
  ['proposal-redecode', P, 'redecode'],
  ['proposal-cache', P, 'cache'],
  ['near', near],
  ['drift-single', { ...near, max_median_dev: 0.4 }, 'single'],
  ['drift-redecode', { ...near, max_median_dev: 0.4 }, 'redecode'],
  ['drift-cache', { ...near, max_median_dev: 0.4 }, 'cache'],
  ['step-range', { ...near, start: 17, end: 150, step: 3 }],
  ['margin', { ...near, margin: 0.42 }],
  ['max-range', { ...near, max_range: 330 }],
  ['target-alt', { ...near, target_alt: 10 }],
  ['no-hotspot', { ...near, locate_hotspot: false }],
  ['no-hotspot-alt', { ...near, locate_hotspot: false, target_alt: 40 }],
  ['metadata-rotation', { lat: near.lat, lon: near.lon }],
  ['rotation-90', { ...near, rotation: 90 }],
  ['rotation-270-hfov', { ...near, rotation: 270, hfov: 30 }],
  ['hotspot-strict', { ...near, hotspot_max_error: 5 }],
  ['hotspot-far', { lat: near.lat + 0.004, lon: near.lon }],
  ['hotspot-min', { ...near, hotspot_min_c: 40 }],
  ['radius', { ...near, radius: 25 }],
  ['too-few', { ...near, start: 10, end: 11 }],
  ['proposal-no-hot', noHot, 'single', { expectFrames: 201 }],
  ['proposal-failed-hot', failedHot, 'single', { expectFrames: 201 }],
  ['blank-hot-frames', P, 'single', { inject: blankHot }],
  ['blank-frames', P, 'single', { inject: { blank: [5, 9, 120] }, expectBlank: '5,9,120' }],
  ['blank-frames-redecode', P, 'redecode', { inject: { blank: [5, 9, 120] } }],
  [
    'telemetry-null-yaw',
    near,
    'single',
    {
      inject: tele({ 7: { 'telemetry.vehicle_attitude.yaw_rad': null } }),
      nativeError: 'TypeError',
      portError: 'vehicle_attitude.yaw_rad in frame 7$',
    },
  ],
  [
    'telemetry-null-amsl',
    near,
    'single',
    {
      inject: tele({ 3: { 'telemetry.position.alt_amsl_m': null } }),
      nativeError: 'TypeError',
      portError: 'position.alt_amsl_m in frame 3$',
    },
  ],
  [
    'telemetry-string-lat',
    near,
    'single',
    {
      inject: tele({ 12: { 'telemetry.position.lat_e7': '-355800718' } }),
      nativeError: 'TypeError',
      portError: 'position.lat_e7 in frame 12$',
    },
  ],
  [
    'telemetry-null-maximum',
    near,
    'single',
    {
      inject: tele({ 40: { maximum_c: null } }),
      nativeError: 'TypeError',
      portError: 'maximum_c in frame 40$',
    },
  ],
  [
    'telemetry-null-maximum-no-hotspot',
    { ...near, locate_hotspot: false },
    'single',
    { inject: tele({ 40: { maximum_c: null } }) },
  ],
  [
    'telemetry-null-temperature-scale-unselected',
    { ...near, start: 50 },
    'single',
    { inject: tele({ 10: { temperature_scale_k: null } }) },
  ],
  [
    'telemetry-null-temperature-scale',
    near,
    'single',
    {
      inject: tele({ 10: { temperature_scale_k: null } }),
      nativeError: 'TypeError',
      portError: 'temperature_scale_k in frame 10$',
    },
  ],
  [
    'telemetry-missing-gimbal-pitch',
    near,
    'single',
    { inject: tele({ 20: { 'telemetry.gimbal_attitude.pitch_rad': '__delete__' } }) },
  ],
  [
    'telemetry-empty-gimbal',
    near,
    'single',
    { inject: tele({ 30: { 'telemetry.gimbal_attitude': {} } }) },
  ],
  [
    'telemetry-null-outside-range',
    { ...near, end: 100 },
    'single',
    { inject: tele({ 150: { 'telemetry.vehicle_attitude.yaw_rad': null } }) },
  ],
];
const results = [];
for (const [name, params, mode, extra = {}] of cases) {
  if (opt.only && name !== opt.only) continue;
  const r = await runCase(ctx, name, params, { mode, samples: prop.samples, ...extra });
  const sel = r.port?.info?.selection;
  const summary = r.error
    ? `fails: ${r.error}`
    : `${r.port.info.frames.length}/${sel.selected} kept, blank ${sel.blank.length}, drifted ${sel.drifted.length}, ` +
      `hot spot ${sel.hotspot ? (sel.hotspot.used ? 'used' : 'not used') : 'off'}`;
  if (extra.expectFrames != null && r.port?.info.frames.length !== extra.expectFrames) {
    r.ok = false;
    r.compare += `\nexpected ${extra.expectFrames} frames`;
  }
  if (extra.expectBlank != null && sel?.blank.join() !== extra.expectBlank) {
    r.ok = false;
    r.compare += `\nexpected blank ${extra.expectBlank}`;
  }
  console.log(
    `${r.ok ? 'PASS' : 'FAIL'} ${name} (${mode ?? 'single'}, ${Math.round(r.ms)} ms): ${summary}`,
  );
  if (!r.ok) {
    failures++;
    console.log(r.compare);
  }
  if (name === 'reference') {
    // and against $WEBSPLAT_DATA/ref/work itself
    const refWork = path.join(dataDir(), 'ref', 'work');
    try {
      execFileSync(
        python,
        [path.join(here, 'compare_select.py'), refWork, path.join(ctx.base, 'select-node', name)],
        { stdio: ['ignore', 'pipe', 'inherit'] },
      );
      console.log('PASS reference vs $WEBSPLAT_DATA/ref/work');
    } catch (e) {
      failures++;
      console.log('FAIL reference vs $WEBSPLAT_DATA/ref/work\n' + e.stdout);
    }
  }
  const report = /^\{.*\}$/m.exec(r.compare ?? '');
  results.push({
    name,
    mode: mode ?? 'single',
    ok: r.ok,
    ms: Math.round(r.ms),
    summary,
    compare: report ? JSON.parse(report[0]) : r.compare.trim(),
    timings: r.port?.timings,
  });
}
fs.writeFileSync(
  path.join(ctx.base, 'select-node-results.json'),
  JSON.stringify({ proposeMs, results }, null, 1),
);
console.log(
  `${results.filter((r) => r.ok).length}/${results.length} cases pass` +
    (failures ? `, ${failures} failures` : ''),
);
process.exit(failures ? 1 : 0);
