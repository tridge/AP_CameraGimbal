// Embed the splat metadata in existing SPZ files (spz-meta.mjs), so a downloaded SPZ opens in the
// viewer as its site page does. Only the gzip header is rewritten: the deflate data and trailer are
// copied unchanged, and the decoded SPZ is checked to be identical.
//   node embed-meta.mjs [--write] PATH ...
// PATH: an export_web.py site (every DATE/FLIGHT/RUN/meta.json with that run's splat.spz) or a
// NAME.json with NAME.spz beside it (meta.json pairs with splat.spz). The metadata is the
// apcg_splat JSON of the PLY beside it (as ply2spz.mjs embeds it) when that says the same as the
// sidecar, else the sidecar without its site-only fields (downloads, video). Without --write it
// only reports what would change.
import { createHash, randomBytes } from 'node:crypto';
import {
  closeSync,
  existsSync,
  fchmodSync,
  fsyncSync,
  openSync,
  readdirSync,
  readFileSync,
  readSync,
  renameSync,
  statSync,
  unlinkSync,
  writeSync,
} from 'node:fs';
import { basename, dirname, join } from 'node:path';
import { gunzipSync } from 'node:zlib';
import { spzDecoded } from './spz-flags.mjs';
import { gzipHeaderInfo, spzMeta, spzMetaText, withSpzMeta } from './spz-meta.mjs';

const argv = process.argv.slice(2);
const write = argv.includes('--write');
const paths = argv.filter((a) => !a.startsWith('--'));
if (!paths.length) {
  console.error('usage: node embed-meta.mjs [--write] PATH ...');
  process.exit(2);
}
const sha = (b) => createHash('sha256').update(b).digest('hex').slice(0, 16);
// JSON with sorted keys, to compare objects
const canon = (v) =>
  v && typeof v === 'object' && !Array.isArray(v)
    ? `{${Object.keys(v)
        .sort()
        .map((k) => `${JSON.stringify(k)}:${canon(v[k])}`)
        .join(',')}}`
    : Array.isArray(v)
      ? `[${v.map(canon).join(',')}]`
      : JSON.stringify(v);
let changed = 0;
let refused = 0;

// file replaced by bytes: a new temporary file beside it (exclusive create, so an existing name or
// symlink is never followed), given the original's mode, then renamed over it
function replace(file, bytes) {
  const tmp = join(dirname(file), `.${basename(file)}.${randomBytes(6).toString('hex')}.tmp`);
  const fd = openSync(tmp, 'wx', 0o600);
  try {
    for (let at = 0; at < bytes.length; ) at += writeSync(fd, bytes, at);
    fchmodSync(fd, statSync(file).mode & 0o7777);
    fsyncSync(fd);
    closeSync(fd);
    renameSync(tmp, file);
  } catch (e) {
    try {
      closeSync(fd);
    } catch {
      // already closed
    }
    unlinkSync(tmp);
    throw e;
  }
}

// the apcg_splat JSON text of a PLY's header (the first MiB), or null
function plyMetaText(file) {
  const fd = openSync(file, 'r');
  const b = Buffer.alloc(Math.min(statSync(file).size, 1 << 20));
  readSync(fd, b, 0, b.length, 0);
  closeSync(fd);
  const head = b.toString('latin1');
  const end = head.search(/\nend_header\r?\n/);
  if (!head.startsWith('ply') || end < 0) return null;
  const line = head
    .slice(0, end)
    .split('\n')
    .find((l) => l.startsWith('comment apcg_splat '));
  return line ? line.slice('comment apcg_splat '.length).replace(/\r$/, '') : null;
}

function embedRun(metaFile) {
  const dir = dirname(metaFile);
  const stem = basename(metaFile, '.json') === 'meta' ? 'splat' : basename(metaFile, '.json');
  const spzFile = join(dir, `${stem}.spz`);
  const plyFile = join(dir, `${stem}.ply`);
  try {
    if (!existsSync(spzFile)) return console.log(`skip      ${metaFile}: no ${stem}.spz`);
    const m = JSON.parse(readFileSync(metaFile, 'utf8'));
    if (!m || typeof m !== 'object' || Array.isArray(m)) throw new Error('not a JSON object');
    const { downloads, video, ...want } = m;
    let text = existsSync(plyFile) ? plyMetaText(plyFile) : null;
    let from = `${stem}.ply header`;
    try {
      if (text != null && canon(JSON.parse(text)) !== canon(want)) text = null;
    } catch {
      text = null;
    }
    if (text == null) {
      text = JSON.stringify(want);
      from = basename(metaFile);
    }
    const b = readFileSync(spzFile);
    const u = spzDecoded(b);
    const old = spzMetaText(b);
    if (old === text) return console.log(`skip      ${spzFile}: metadata already embedded`);
    const out = Buffer.from(withSpzMeta(b, text));
    // the decoded SPZ, the deflate data and trailer are unchanged; the metadata reads back
    const body = (x) => x.subarray(gzipHeaderInfo(x).body);
    if (!gunzipSync(out).equals(u)) throw new Error('decoded SPZ would change');
    if (!body(out).equals(body(b))) throw new Error('deflate data would change');
    if (canon(spzMeta(out)) !== canon(JSON.parse(text)))
      throw new Error('metadata reads back wrong');
    if (write) {
      replace(spzFile, out);
    }
    changed++;
    console.log(
      `${old == null ? 'embed' : 'replace'}   ${spzFile}: ${Buffer.byteLength(text)} bytes from ` +
        `${from} (${b.length} -> ${out.length} bytes, ${sha(b)} -> ${sha(out)}; decoded ` +
        `${u.length} bytes identical)`,
    );
  } catch (e) {
    refused++;
    console.log(`REFUSED   ${metaFile}: ${e.message}; nothing changed`);
  }
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
    metas.sort().forEach(embedRun);
  } else if (p.endsWith('.json')) embedRun(p);
  else throw new Error(`${p}: give a site directory or a NAME.json`);
}
console.log(
  `${write ? 'changed' : 'would change'} ${changed} SPZ file(s)${refused ? `; refused ${refused}` : ''}`,
);
process.exit(refused ? 1 : 0);
