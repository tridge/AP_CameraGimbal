// Mark existing splats as trained with gsplat classic rasterisation (train.py, train_rgb.py), so
// the viewer renders them as trained: `"rasterize_mode":"classic"` in meta.json sidecars and in
// PLY `comment apcg_splat` headers, and the SPZ antialiased flag cleared (as ply2spz.mjs now
// writes it). Only the marker changes: meta.json and PLY header text gain the field (nothing else
// is rewritten, PLY bodies are copied unchanged), and an SPZ is re-gzipped with only its flags
// byte changed (and the rasterize_mode of its embedded metadata, if it has any).
//   node migrate-classic.mjs [--write] [--kinds thermal,rgb] PATH ...
// PATH: an export_web.py site (every DATE/FLIGHT/RUN/meta.json with that run's splat.spz and
// splat.ply), a NAME.json (with NAME.spz and NAME.ply beside it, if present; meta.json pairs
// with splat.*), or a PLY. Without --write it only reports what would change. --kinds: default
// thermal.
// Every file of a run is checked first (JSON, SPZ magic, version and lengths, PLY preamble,
// format and length; the PLY's kind against the sidecar's); a run with any problem is refused
// whole and nothing of it is changed. Files are written PLY and SPZ first, the meta.json marker
// last, so a run whose meta.json is unmarked is unfinished and a later run completes it.
// Test hook: MIGRATE_FAIL_BEFORE=SUBSTRING throws before writing a file whose path contains it.
import { createHash } from 'node:crypto';
import {
  existsSync,
  readdirSync,
  readFileSync,
  renameSync,
  statSync,
  writeFileSync,
} from 'node:fs';
import { basename, dirname, join } from 'node:path';
import { setAntialiased, spzDecoded } from './spz-flags.mjs';

const argv = process.argv.slice(2);
const write = argv.includes('--write');
const ki = argv.indexOf('--kinds');
const kinds = new Set((ki >= 0 ? argv[ki + 1] : 'thermal').split(','));
const paths = argv.filter((a, i) => !a.startsWith('--') && !(ki >= 0 && i === ki + 1));
if (!paths.length) {
  console.error('usage: node migrate-classic.mjs [--write] [--kinds thermal,rgb] PATH ...');
  process.exit(2);
}
const sha = (b) => createHash('sha256').update(b).digest('hex').slice(0, 16);
const report = [];
let refused = 0;
const note = (r) => {
  report.push(r);
  console.log(
    `${r.action.padEnd(9)} ${r.file}${r.detail ? `: ${r.detail}` : ''}` +
      (r.before ? ` (${r.before.bytes} -> ${r.after.bytes} bytes)` : ''),
  );
};
function replace(file, bytes) {
  if (!write) return;
  const hook = process.env.MIGRATE_FAIL_BEFORE;
  if (hook && file.includes(hook)) throw new Error(`test failure before writing ${file}`);
  writeFileSync(`${file}.migrating`, bytes);
  renameSync(`${file}.migrating`, file);
}

// the field inserted right after the object's opening brace, in the file's own layout
function markJsonText(text) {
  const m = text.match(/^(\s*)\{(\s*)/);
  if (!m) throw new Error('not a JSON object');
  const ws = m[2];
  const sep = ws.includes('\n')
    ? `"rasterize_mode": "classic",${ws}`
    : '"rasterize_mode":"classic",';
  const out = text.slice(0, m[0].length) + sep + text.slice(m[0].length);
  if (
    JSON.stringify({ rasterize_mode: 'classic', ...JSON.parse(text) }) !==
    JSON.stringify(JSON.parse(out))
  )
    throw new Error('marking changed more than the field');
  return out;
}

const SIZES = { char: 1, uchar: 1, int8: 1, uint8: 1, short: 2, ushort: 2, int16: 2, uint16: 2 };
for (const t of ['int', 'uint', 'float', 'int32', 'uint32', 'float32']) SIZES[t] = 4;
SIZES.double = SIZES.float64 = 8;

// a PLY checked (preamble, binary little-endian, a leading vertex element of scalar properties,
// the file length its count implies); its bytes, header end offset and apcg_splat info
function readPly(file) {
  const b = readFileSync(file);
  const head = b.subarray(0, Math.min(b.length, 1 << 20)).toString('latin1');
  if (!/^ply\r?\n/.test(head)) throw new Error('not a PLY file (preamble)');
  const end = head.search(/\nend_header\r?\n/);
  if (end < 0) throw new Error('PLY header has no end_header line');
  const lines = head
    .slice(0, end)
    .split('\n')
    .map((l) => l.replace(/\r$/, ''));
  if (lines[1] !== 'format binary_little_endian 1.0') throw new Error(`PLY ${lines[1]}`);
  let count = null;
  let stride = 0;
  let element = null;
  for (const l of lines.slice(2)) {
    const w = l.split(/\s+/);
    if (w[0] === 'element') {
      if (element) throw new Error('PLY has more than one element');
      [element, count] = [w[1], Number(w[2])];
    } else if (w[0] === 'property') {
      if (w[1] === 'list' || !SIZES[w[1]]) throw new Error(`PLY property ${l}`);
      stride += SIZES[w[1]];
    }
  }
  if (element !== 'vertex' || !Number.isSafeInteger(count)) {
    throw new Error('PLY has no vertex element');
  }
  const body = end + head.slice(end).match(/^\nend_header\r?\n/)[0].length;
  if (b.length !== body + count * stride) {
    throw new Error(`PLY is ${b.length} bytes, ${body + count * stride} expected`);
  }
  const li = lines.findIndex((l) => l.startsWith('comment apcg_splat '));
  const info = li >= 0 ? JSON.parse(lines[li].slice('comment apcg_splat '.length)) : null;
  return { b, end, info };
}

// the planned change of one file ({file, action, bytes, ...}) or a skip
function planPly(file, kindHint) {
  const { b, end, info } = readPly(file);
  if (!info) return { file, action: 'skip', detail: 'no apcg_splat header' };
  if (kindHint && info.kind && info.kind !== kindHint) {
    throw new Error(`${file}: PLY kind ${info.kind}, its sidecar says ${kindHint}`);
  }
  const kind = info.kind ?? kindHint;
  if (!kinds.has(kind)) return { file, action: 'skip', detail: `kind ${kind}` };
  if (info.rasterize_mode)
    return { file, action: 'skip', detail: `already ${info.rasterize_mode}` };
  const lines = b.subarray(0, end).toString('latin1').split('\n');
  const li = lines.findIndex((l) => l.startsWith('comment apcg_splat '));
  const cr = lines[li].endsWith('\r') ? '\r' : '';
  const json = lines[li].slice('comment apcg_splat '.length, cr ? -1 : undefined);
  lines[li] = `comment apcg_splat ${markJsonText(json)}${cr}`;
  const newHead = Buffer.from(lines.join('\n'), 'latin1');
  const out = Buffer.concat([newHead, b.subarray(end)]);
  return {
    file,
    action: 'ply',
    bytes: out,
    detail: `header ${end} -> ${newHead.length} bytes, body (${b.length - end} bytes) unchanged`,
    before: { bytes: b.length, sha: sha(b) },
    after: { bytes: out.length, sha: sha(out) },
  };
}

function planSpz(file) {
  const b = readFileSync(file);
  let f;
  try {
    f = spzDecoded(b)[14];
  } catch (e) {
    throw new Error(`${file}: ${e.message}`);
  }
  if (!(f & 1)) return { file, action: 'skip', detail: 'antialiased flag already clear' };
  const out = setAntialiased(b, false);
  return {
    file,
    action: 'spz',
    bytes: out,
    detail: `flags ${f} -> ${f & ~1} (decoded bytes otherwise identical)`,
    before: { bytes: b.length, sha: sha(b) },
    after: { bytes: out.length, sha: sha(out) },
  };
}

// one run: a sidecar and its paired files (meta.json -> splat.*, NAME.json -> NAME.*)
function migrateRun(metaFile) {
  let plan;
  try {
    const text = readFileSync(metaFile, 'utf8');
    const m = JSON.parse(text);
    if (!kinds.has(m.kind)) {
      return note({ file: metaFile, action: 'skip', detail: `kind ${m.kind}` });
    }
    const dir = dirname(metaFile);
    const stem = basename(metaFile, '.json') === 'meta' ? 'splat' : basename(metaFile, '.json');
    const plyFile = join(dir, `${stem}.ply`);
    const spzFile = join(dir, `${stem}.spz`);
    // the files' markers must agree: the sidecar's mode, the PLY header's (absent in legacy
    // exports) and the SPZ flag (antialiased unless cleared). Only a classic PLY or a cleared SPZ
    // beside an unmarked sidecar is a finished step of an interrupted run.
    const plyInfo = existsSync(plyFile) ? readPly(plyFile).info : null;
    const plyMode = plyInfo?.rasterize_mode ?? null;
    let spzAntialiased = null;
    if (existsSync(spzFile)) {
      try {
        spzAntialiased = !!(spzDecoded(readFileSync(spzFile))[14] & 1);
      } catch (e) {
        throw new Error(`${spzFile}: ${e.message}`);
      }
    }
    const conflict = (why) => {
      throw new Error(`conflicting markers: ${why}`);
    };
    if (m.rasterize_mode) {
      const classic = m.rasterize_mode === 'classic';
      if (plyInfo && plyMode !== m.rasterize_mode)
        conflict(`sidecar ${m.rasterize_mode}, PLY ${plyMode ?? 'unmarked'}`);
      if (spzAntialiased !== null && spzAntialiased === classic)
        conflict(
          `sidecar ${m.rasterize_mode}, SPZ flag ${spzAntialiased ? 'antialiased' : 'cleared'}`,
        );
      return note({ file: metaFile, action: 'skip', detail: `already ${m.rasterize_mode}` });
    }
    if (plyMode && plyMode !== 'classic') conflict(`unmarked sidecar, PLY ${plyMode}`);
    plan = [];
    if (plyInfo || existsSync(plyFile)) plan.push(planPly(plyFile, m.kind));
    if (spzAntialiased !== null) plan.push(planSpz(spzFile));
    const out = markJsonText(text);
    plan.push({
      file: metaFile,
      action: 'meta',
      bytes: out,
      detail: 'adds "rasterize_mode": "classic" (written last)',
      before: { bytes: Buffer.byteLength(text), sha: sha(text) },
      after: { bytes: Buffer.byteLength(out), sha: sha(out) },
    });
  } catch (e) {
    refused++;
    return note({
      file: metaFile,
      action: 'REFUSED',
      detail: `${e.message}; nothing of this run changed`,
    });
  }
  for (const p of plan) {
    if (p.action !== 'skip') replace(p.file, p.bytes);
    note(p);
  }
}

function migratePly(file) {
  let p;
  try {
    p = planPly(file, null);
  } catch (e) {
    refused++;
    return note({ file, action: 'REFUSED', detail: e.message });
  }
  if (p.action !== 'skip') replace(p.file, p.bytes);
  note(p);
}

function dirs(d) {
  try {
    return readdirSync(d).filter((x) => statSync(join(d, x)).isDirectory());
  } catch {
    return [];
  }
}

for (const p of paths) {
  if (statSync(p).isDirectory()) {
    const metas = [];
    for (const a of dirs(p))
      for (const b of dirs(join(p, a)))
        for (const c of dirs(join(p, a, b)))
          if (existsSync(join(p, a, b, c, 'meta.json'))) metas.push(join(p, a, b, c, 'meta.json'));
    if (!metas.length) console.log(`${p}: no DATE/FLIGHT/RUN/meta.json`);
    metas.sort().forEach(migrateRun);
  } else if (p.endsWith('.json')) migrateRun(p);
  else if (p.endsWith('.ply')) migratePly(p);
  else throw new Error(`${p}: give a site directory, a meta.json or a PLY`);
}
const changed = report.filter((r) => !['skip', 'REFUSED'].includes(r.action));
const count = (a) => changed.filter((r) => r.action === a).length;
console.log(
  `${write ? 'changed' : 'would change'} ${changed.length} file(s) (${count('meta')} meta.json, ` +
    `${count('ply')} PLY, ${count('spz')} SPZ)${refused ? `; refused ${refused}` : ''}`,
);
process.exit(refused ? 1 : 0);
