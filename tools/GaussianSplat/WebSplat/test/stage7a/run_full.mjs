// Stage 7a full size: a real trained splat (run13, 591k Gaussians, SH 3, lobe baked by train.py)
// exported by JS from train.py's splat.ply and scale.json, against export_web.py's export of the
// same splat (site mode: PLY, SPZ, meta.json; cached in OUT/native).
//   node run_full.mjs SPLATDIR OUT      (exit 1 on failure)
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { exportSplat, loadSplat } from '../../app/src/stages/export/export.js';
import { apcgMeta, readPly, writePly } from '../../app/src/stages/export/ply.js';
import { decodeSpz, spzErrors } from '../../app/src/stages/export/spz.js';

const gs = fileURLToPath(new URL('../../../', import.meta.url));
const { transcodeSpz } = await import(
  `${gs}viewer/node_modules/@sparkjsdev/spark/dist/spark.module.js`
);
const [dir, out] = process.argv.slice(2);
if (!out) throw new Error('usage: node run_full.mjs SPLATDIR OUT');
const site = `${out}/native/site/2026-11-08/full/run13`;
mkdirSync(out, { recursive: true });
const cached =
  existsSync(`${site}/meta.json`) && JSON.parse(readFileSync(`${site}/meta.json`, 'utf8'));
if (!cached || cached.rasterize_mode !== 'classic') {
  const r = spawnSync(
    `${gs}.venv/bin/python`,
    [`${gs}export_web.py`, dir, `${out}/native/site`, '2026-11-08/full/run13', '--title', 'run13'],
    { stdio: 'inherit' },
  );
  if (r.status !== 0) throw new Error('export_web.py failed');
}
let failures = 0;
const check = (name, ok, detail = '') => {
  console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? `: ${detail}` : ''}`);
  if (!ok) failures++;
};
const read = (p) => new Uint8Array(readFileSync(p));
const nativeMeta = JSON.parse(readFileSync(`${site}/meta.json`, 'utf8'));
const scaleJson = JSON.parse(readFileSync(`${dir}/scale.json`, 'utf8'));

let t = performance.now();
const splat = loadSplat(read(`${dir}/splat.ply`));
const tRead = performance.now() - t;
t = performance.now();
const res = await exportSplat(splat, {
  camera: nativeMeta.camera,
  target: scaleJson.hotspot?.enu ?? scaleJson.target_enu,
  origin: scaleJson.origin,
  scale: scaleJson.temperature_scale,
  hotspot: scaleJson.hotspot,
  title: 'run13',
  transcodeSpz,
  checkSpz: true,
});
const tExport = performance.now() - t;
for (const [n, b] of Object.entries(res.files)) writeFileSync(`${out}/${n}`, b);
console.log(
  `run13: ${splat.count} Gaussians, read ${(tRead / 1000).toFixed(1)} s, export (PLY, SPZ, SPZ check) ${(tExport / 1000).toFixed(1)} s`,
);

const js = readPly(res.files['splat.ply']);
const nat = readPly(read(`${site}/splat.ply`));
const headJs = res.files['splat.ply'].subarray(
  0,
  res.files['splat.ply'].length - 4 * js.count * js.names.length,
);
const natBytes = read(`${site}/splat.ply`);
const headNat = natBytes.subarray(0, natBytes.length - 4 * nat.count * nat.names.length);
check('header byte-identical to export_web.py', Buffer.from(headJs).equals(Buffer.from(headNat)));
check(
  'apcg_splat info = export_web.py',
  JSON.stringify(apcgMeta(js.comments)) === JSON.stringify(apcgMeta(nat.comments)),
);
// fields: identical except where the inputs differ (train.py's PLY holds torch-normalised quaternions
// and its own lobe bake; export_web.py re-bakes and normalises with numpy)
const ulp = (x) => 2 ** (Math.floor(Math.log2(Math.abs(x) || 2 ** -126)) - 23);
const stats = {};
for (const k of nat.names) {
  let differ = 0;
  let maxUlp = 0;
  let maxAbs = 0;
  for (let i = 0; i < nat.count; i++) {
    const a = js.props[k][i];
    const b = nat.props[k][i];
    if (a !== b) {
      differ++;
      maxAbs = Math.max(maxAbs, Math.abs(a - b));
      maxUlp = Math.max(maxUlp, Math.abs(a - b) / ulp(b));
    }
  }
  if (differ) stats[k] = { differ, maxUlp, maxAbs };
}
const groups = {};
for (const [k, v] of Object.entries(stats)) {
  const g = k.replace(/_\d+$/, '');
  groups[g] = groups[g] ?? { values: 0, maxUlp: 0, maxAbs: 0 };
  groups[g].values += v.differ;
  groups[g].maxUlp = Math.max(groups[g].maxUlp, v.maxUlp);
  groups[g].maxAbs = Math.max(groups[g].maxAbs, v.maxAbs);
}
console.log('fields differing from export_web.py:', JSON.stringify(groups));
const onlyExpected = Object.keys(groups).every((g) => ['rot', 'f_dc', 'f_rest'].includes(g));
check(
  'positions, scales, opacities identical; SH within the lobe re-bake noise, rotations <= 4 ulp',
  onlyExpected &&
    (groups.rot?.maxUlp ?? 0) <= 4 &&
    Math.max(groups.f_dc?.maxAbs ?? 0, groups.f_rest?.maxAbs ?? 0) < 1e-4,
);
// the writer at full size: export_web.py's PLY read and written again is byte-identical
{
  const back = loadSplat(natBytes);
  const again = writePly(back, { comments: nat.comments, normalised: true });
  check(
    'full-size PLY read + write = export_web.py bytes',
    Buffer.from(again).equals(Buffer.from(natBytes)),
  );
}

const r = res.spzReport;
console.log(`SPZ v${r.version}: ${JSON.stringify(r.max)}`);
console.log(
  `  in quantisation steps: ${JSON.stringify(r.steps)}; clamped: ${JSON.stringify(r.clamped)}`,
);
check('SPZ decode error within bounds', !Object.keys(r.over).length, JSON.stringify(r.over));
const nat2 = await decodeSpz(read(`${site}/splat.spz`));
const jsDec = await decodeSpz(res.files['splat.spz']);
const e2 = spzErrors(nat2.splat, jsDec.splat, jsDec.fractionalBits);
console.log(`JS SPZ vs export_web.py SPZ (decoded): ${JSON.stringify(e2.max)}`);
check(
  'JS SPZ = export_web.py SPZ within one quantisation step',
  Object.entries(e2.steps).every(([, v]) => v <= 1.0001) && e2.max.quatAngleDeg < 0.5,
  JSON.stringify(e2.steps),
);
writeFileSync(
  `${out}/report.json`,
  JSON.stringify({ count: splat.count, tRead, tExport, groups, spz: r, spzVsNative: e2 }, null, 1),
);
console.log(failures ? `${failures} FAILED` : 'full-size checks: all pass');
process.exit(failures ? 1 : 0);
