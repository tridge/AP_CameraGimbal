// Stage 7a in Node: the JS export of the tiny model against the native exports.
//   node run_node.mjs TINY NATIVE OUT
// Writes OUT/{splat.ply, splat.spz, meta.json, train.ply, splat_nativecam.ply, report.json}.
// Exit 1 on any failed check.
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import {
  exportInputs,
  exportSplat,
  loadSplat,
  trainPly,
} from '../../app/src/stages/export/export.js';
import { apcgMeta, readPly, splatFromPly, writePly } from '../../app/src/stages/export/ply.js';
import { pyFloat, pyJson } from '../../app/src/stages/export/pyjson.js';
import { decodeSpz, encodeSpz, spzErrors } from '../../app/src/stages/export/spz.js';
import { startCamera, toCelsius } from '../../app/src/stages/export/meta.js';
import { viewValue } from './sh.mjs';
import { lookAt } from './tiny_model.mjs';

const spark = fileURLToPath(
  new URL('../../../viewer/node_modules/@sparkjsdev/spark/dist/spark.module.js', import.meta.url),
);
const { transcodeSpz } = await import(spark);

const [tiny, native, out] = process.argv.slice(2);
if (!out) throw new Error('usage: node run_node.mjs TINY NATIVE OUT');
mkdirSync(out, { recursive: true });
const site = `${native}/site/2026-11-08/tiny/run1`;
// 'end_header' in the title: readers must find the terminator line, not this text
const TITLE = 'tiny model end_header\nend_header';

let failures = 0;
const report = { checks: [] };
function check(name, ok, detail = '') {
  report.checks.push({ name, ok, detail });
  console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? `: ${detail}` : ''}`);
  if (!ok) failures++;
}
const read = (p) => new Uint8Array(readFileSync(p));
const json = (p) => JSON.parse(readFileSync(p, 'utf8'));
function firstDiff(a, b) {
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i++) if (a[i] !== b[i]) return i;
  return a.length === b.length ? -1 : n;
}
function sameBytes(name, a, b) {
  const d = firstDiff(a, b);
  check(
    name,
    d < 0,
    d < 0 ? `${a.length} bytes` : `differ at byte ${d} (${a.length} vs ${b.length})`,
  );
}

// Python json formatting (the cases the headers use)
const pyCases = [
  [8, '8.0'],
  [0.5, '0.5'],
  [1e-5, '1e-05'],
  [0.0001, '0.0001'],
  [1.5e-7, '1.5e-07'],
  [1e16, '1e+16'],
  [123456789012345.6, '123456789012345.6'],
  [-35.57991111814981, '-35.57991111814981'],
  [10.000003814697266, '10.000003814697266'],
  [-0, '-0.0'],
];
const bad = pyCases.filter(([x, s]) => pyFloat(x) !== s);
check(
  'pyFloat matches Python repr',
  !bad.length,
  bad.map(([x]) => `${x} -> ${pyFloat(x)}`).join(', '),
);
check(
  'pyJson ensure_ascii and indent',
  pyJson({ a: '°C "q"', b: [], c: {}, d: [1, 2] }, { indent: 1 }) ===
    '{\n "a": "\\u00b0C \\"q\\"",\n "b": [],\n "c": {},\n "d": [\n  1,\n  2\n ]\n}',
);

const splat = loadSplat(read(`${tiny}/params.bin`));
const inputs = exportInputs(json(`${tiny}/dataset.json`), json(`${tiny}/frames.json`));
const nativeMeta = json(`${site}/meta.json`);

// start camera: JS (float64) vs export_web.py (float32 viewmats inverted with numpy)
const cam = startCamera(inputs.views, inputs.target);
const camErr = Math.max(
  ...['position', 'target'].flatMap((k) =>
    cam[k].map((x, i) => Math.abs(x - nativeMeta.camera[k][i])),
  ),
);
const fovErr = Math.abs(cam.fov_deg - nativeMeta.camera.fov_deg);
report.camera = { js: cam, native: nativeMeta.camera, maxErrM: camErr, fovErrDeg: fovErr };
check(
  'start camera = export_web.py start_camera',
  camErr < 1e-4 && fovErr < 1e-9,
  `${camErr.toExponential(2)} m, fov ${fovErr.toExponential(2)} deg`,
);

// the oblique branch (near-vertical hot view)
{
  const C = [3, -2, 110];
  const views = { ...inputs.views, viewmats: [lookAtDown(C)], names: ['a.png'], hot: ['a.png'] };
  const c2 = startCamera(views, inputs.target);
  report.cameraNadir = c2;
  writeFileSync(
    `${out}/nadir_case.json`,
    JSON.stringify({ C, viewmat: views.viewmats[0], camera: c2 }),
  );
}
function lookAtDown(C) {
  // camera looking straight down, image up = north (y down = south)
  const R = [
    [1, 0, 0],
    [0, -1, 0],
    [0, 0, -1],
  ];
  const t = R.map((r) => -(r[0] * C[0] + r[1] * C[1] + r[2] * C[2]));
  return [
    [...R[0], t[0]],
    [...R[1], t[1]],
    [...R[2], t[2]],
    [0, 0, 0, 1],
  ];
}

const res = await exportSplat(splat, { ...inputs, title: TITLE, transcodeSpz, checkSpz: true });
for (const [name, bytes] of Object.entries(res.files)) writeFileSync(`${out}/${name}`, bytes);
writeFileSync(`${out}/train.ply`, trainPly(splat, inputs));

// with the native camera the header and body must be the native bytes exactly
const resN = await exportSplat(splat, {
  ...inputs,
  camera: nativeMeta.camera,
  title: TITLE,
  transcodeSpz,
});
writeFileSync(`${out}/splat_nativecam.ply`, resN.files['splat.ply']);
writeFileSync(`${out}/splat_nativecam.spz`, resN.files['splat.spz']);
writeFileSync(`${out}/meta_nativecam.json`, resN.files['meta.json']);
sameBytes(
  'PLY (export_web.py --ply) byte-identical, native camera',
  resN.files['splat.ply'],
  read(`${native}/export_web.ply`),
);
sameBytes('PLY = export_web.py site splat.ply', resN.files['splat.ply'], read(`${site}/splat.ply`));
sameBytes(
  'SPZ byte-identical to export_web.py (Spark encoder)',
  resN.files['splat.spz'],
  read(`${site}/splat.spz`),
);
sameBytes(
  'meta.json byte-identical to export_web.py',
  resN.files['meta.json'],
  read(`${site}/meta.json`),
);
sameBytes(
  'train.py save_ply byte-identical',
  read(`${out}/train.ply`),
  read(`${native}/train.ply`),
);

// with its own camera: the body is identical, the header differs only in camera digits
{
  const js = readPly(res.files['splat.ply']);
  const nat = readPly(read(`${native}/export_web.ply`));
  const a = apcgMeta(js.comments);
  const b = apcgMeta(nat.comments);
  const { camera: ca, ...ra } = a;
  const { camera: cb, ...rb } = b;
  check('PLY header fields = native (own camera)', JSON.stringify(ra) === JSON.stringify(rb));
  let maxd = 0;
  for (const k of nat.names)
    for (let i = 0; i < nat.count; i++)
      maxd = Math.max(maxd, Math.abs(js.props[k][i] - nat.props[k][i]));
  check(
    'PLY body = native (own camera)',
    maxd === 0 && js.names.join() === nat.names.join(),
    `max diff ${maxd}`,
  );
}

// header terminator: the title holds 'end_header'; CRLF headers read the same
{
  const ply = res.files['splat.ply'];
  const a = readPly(ply);
  check('title with end_header survives the header', apcgMeta(a.comments)?.title === TITLE);
  const n = 4 * a.count * a.names.length;
  const head = new TextDecoder('latin1')
    .decode(ply.subarray(0, ply.length - n))
    .replace(/\n/g, '\r\n');
  const h = new TextEncoder().encode(head);
  const crlf = new Uint8Array(h.length + n);
  crlf.set(h);
  crlf.set(ply.subarray(ply.length - n), h.length);
  const b = readPly(crlf);
  check(
    'CRLF header reads the same',
    JSON.stringify(b.comments) === JSON.stringify(a.comments) &&
      a.names.every((k) => b.props[k].every((x, i) => x === a.props[k][i])),
  );
}

// round trip: the written PLY reads back to the source parameters (quaternions normalised)
{
  const back = splatFromPly(readPly(read(`${out}/train.ply`)));
  let d = 0;
  for (const k of ['means', 'scales', 'opacities', 'sh0', 'shN']) {
    for (let i = 0; i < splat[k].length; i++) d = Math.max(d, Math.abs(back[k][i] - splat[k][i]));
  }
  let qd = 0;
  for (let i = 0; i < splat.count; i++) {
    const q = splat.quats.subarray(4 * i, 4 * i + 4);
    const n = Math.hypot(...q);
    for (let c = 0; c < 4; c++) qd = Math.max(qd, Math.abs(back.quats[4 * i + c] - q[c] / n));
  }
  check(
    'JS PLY reads back to the source parameters',
    d === 0 && qd < 1e-7,
    `max ${d}, quats ${qd.toExponential(2)}`,
  );
}

// Brush-style PLY (other property order, no normals): same parameters through the reader
{
  const names = [
    'x',
    'y',
    'z',
    'scale_0',
    'scale_1',
    'scale_2',
    'opacity',
    'rot_0',
    'rot_1',
    'rot_2',
    'rot_3',
    'f_dc_0',
    'f_dc_1',
    'f_dc_2',
    ...Array.from({ length: 45 }, (_, i) => `f_rest_${i}`),
  ];
  const ply = readPly(read(`${native}/train.ply`));
  const head = `ply\nformat binary_little_endian 1.0\ncomment Exported from Brush\ncomment Vertical axis: y\nelement vertex ${ply.count}\n${names.map((n) => `property float ${n}`).join('\n')}\nend_header\n`;
  const h = new TextEncoder().encode(head);
  const body = new Float32Array(ply.count * names.length);
  for (let i = 0; i < ply.count; i++)
    names.forEach((n, j) => (body[i * names.length + j] = ply.props[n][i]));
  const b = new Uint8Array(h.length + body.byteLength);
  b.set(h);
  b.set(new Uint8Array(body.buffer), h.length);
  const back = loadSplat(b);
  sameBytes(
    'Brush property order -> canonical PLY',
    writePly(back, { comments: ply.comments, normalised: true }),
    read(`${native}/train.ply`),
  );
  // normalising an already normalised quaternion again (as train.py would) moves it by <= 1 ulp
  const q2 = readPly(writePly(back)).props;
  let ulps = 0;
  for (let c = 0; c < 4; c++) {
    for (let i = 0; i < ply.count; i++) {
      const a = ply.props[`rot_${c}`][i];
      ulps = Math.max(
        ulps,
        Math.abs(q2[`rot_${c}`][i] - a) / 2 ** (Math.floor(Math.log2(Math.abs(a) || 1)) - 23),
      );
    }
  }
  check('renormalising a written PLY moves quaternions <= 1 ulp', ulps <= 1, `${ulps} ulp`);
}

// SPZ: decode error against the PLY it was made from, within the quantisation bounds
{
  const r = res.spzReport;
  report.spz = r;
  console.log(
    `SPZ v${r.version}, ${r.fractionalBits} fractional bits, antialiased flag ${r.antialiased}`,
  );
  for (const [k, v] of Object.entries(r.max)) {
    console.log(
      `  ${k}: max error ${v.toExponential(3)}${k in r.steps ? ` = ${r.steps[k].toFixed(3)} steps` : ''}`,
    );
  }
  check(
    'SPZ decode error within quantisation bounds',
    !Object.keys(r.over).length,
    JSON.stringify(r.over),
  );
  check(
    'SPZ: nothing clamped in the tiny model',
    Object.values(r.clamped).every((x) => !x),
    JSON.stringify(r.clamped),
  );
  // the axes are kept (no RUB/RDF flip): every position within the bound
  check('SPZ keeps the axes (no flip)', r.steps.means <= 0.5);
  check('SPZ antialiased flag cleared (rasterize_mode classic)', r.antialiased === false);
  check('header and meta.json say rasterize_mode classic', res.info.rasterize_mode === 'classic');
  check(
    'header and meta.json carry the trim circle (0.85 x the bounds radius)',
    res.info.trim_radius === 0.85 * 165 && JSON.stringify(res.info.trim_centre) === '[0,0]',
  );

  // rejecting controls for the error accounting
  const src = splatFromPly(readPly(res.files['splat.ply']));
  const dec = await decodeSpz(res.files['splat.spz']);
  const deg0 = await transcodeSpz({
    inputs: [{ fileBytes: res.files['splat.ply'], pathOrUrl: 'a.ply' }],
    maxSh: 0,
  });
  let threw = null;
  try {
    spzErrors(src, (await decodeSpz(deg0.fileBytes)).splat, 12);
  } catch (e) {
    threw = e.message;
  }
  check('control: SPZ truncated to SH degree 0 is rejected', !!threw, threw ?? 'accepted');
  const nanDec = { ...dec.splat, means: dec.splat.means.slice(), shN: dec.splat.shN.slice() };
  nanDec.means[4] = NaN;
  nanDec.shN[7] = NaN;
  const en = spzErrors(src, nanDec, 12);
  check(
    'control: NaN in the decoded SPZ is rejected',
    en.over.nonFinite === 2,
    JSON.stringify(en.over),
  );
  const wrongDec = { ...dec.splat, sh0: dec.splat.sh0.slice() };
  wrongDec.sh0[0] += 0.05;
  check(
    'control: a DC value off by 2 steps is rejected',
    spzErrors(src, wrongDec, 12).over.sh0 === 1,
  );
  // saturation: out-of-range values must decode to the format's limits
  const big = { ...src, scales: src.scales.slice(), sh0: src.sh0.slice(), shN: src.shN.slice() };
  big.scales[0] = 8; // > 255/16 - 10
  big.sh0[3] = -5; // rgb < 0
  big.shN[5] = 1.5; // > 127/128
  const bigSpz = await encodeSpz(writePly(big), transcodeSpz);
  const bigDec = (await decodeSpz(bigSpz)).splat;
  const eb = spzErrors(big, bigDec, 12);
  check(
    'saturated values decode to the format limits',
    !Object.keys(eb.over).length &&
      eb.clamped.scales === 1 &&
      eb.clamped.sh0 === 1 &&
      eb.clamped.sh === 1,
    `${JSON.stringify(eb.over)} ${JSON.stringify(eb.clamped)}`,
  );
  const badSat = { ...bigDec, scales: bigDec.scales.slice() };
  badSat.scales[0] = 0;
  check(
    'control: a saturated value decoded wrongly is rejected',
    spzErrors(big, badSat, 12).over.scales === 1,
  );
}

// viewer cameras (start, and from the opposite side) and the expected value at every Gaussian
// centre: alpha * SH value, from the exported PLY and from the decoded SPZ
{
  const W = 1280;
  const H = 900;
  const t = cam.target;
  const back = {
    ...cam,
    position: [2 * t[0] - cam.position[0], 2 * t[1] - cam.position[1], cam.position[2]],
  };
  const cameras = { start: nativeMeta.camera, back };
  const ply = splatFromPly(readPly(res.files['splat.ply']));
  const spz = (await decodeSpz(res.files['splat.spz'])).splat;
  writeFileSync(`${out}/spz_decoded.ply`, writePly(spz));
  const toC = toCelsius(inputs.scale);
  const sig = (x) => 1 / (1 + Math.exp(-x));
  const expected = {
    width: W,
    height: H,
    cameras: {},
    gaussians: json(`${tiny}/model.json`).gaussians.map((g) => g.name),
  };
  let worst = 0;
  for (const [name, c] of Object.entries(cameras)) {
    const vm = lookAt(c.position, c.target);
    const f = H / 2 / Math.tan((c.fov_deg * Math.PI) / 360);
    const rows = [];
    for (let i = 0; i < ply.count; i++) {
      const p = [0, 1, 2].map((a) => ply.means[3 * i + a]);
      const xc = [0, 1, 2].map(
        (r) => vm[4 * r] * p[0] + vm[4 * r + 1] * p[1] + vm[4 * r + 2] * p[2] + vm[4 * r + 3],
      );
      const vPly = viewValue(ply, i, c.position);
      const vSpz = viewValue(spz, i, c.position);
      const row = {
        u: (f * xc[0]) / xc[2] + W / 2,
        v: (f * xc[1]) / xc[2] + H / 2,
        depth: xc[2],
        value: vPly,
        alpha: sig(ply.opacities[i]),
        centreC: toC(Math.min(1, sig(ply.opacities[i]) * vPly)),
        spzCentreC: toC(Math.min(1, sig(spz.opacities[i]) * vSpz)),
      };
      worst = Math.max(worst, Math.abs(row.centreC - row.spzCentreC));
      rows.push(row);
    }
    expected.cameras[name] = { ...c, viewmat: vm, f, gaussians: rows };
  }
  report.spzCentreErrC = worst;
  console.log(
    `SPZ vs PLY temperature at the Gaussian centres (start and back views): max ${worst.toFixed(2)} C`,
  );
  writeFileSync(`${out}/expected.json`, JSON.stringify(expected, null, 1));
}

writeFileSync(`${out}/report.json`, JSON.stringify(report, null, 1));
console.log(failures ? `${failures} FAILED` : 'node checks: all pass');
process.exit(failures ? 1 : 0);
