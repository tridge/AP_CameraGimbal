// viewer/migrate-classic.mjs on local copies of legacy exports (made from export_web.py output by
// removing the marker and setting the SPZ antialiased flag, as exports before the marker were):
// a site (run13 and the 7a tiny model), a samples-style NAME.json + NAME.spz pair and a
// standalone PLY. Checks that only the marker changes, that the migrated SPZ equals a fresh
// export's, that a second run changes nothing, and lists what a deployment would change.
//   node migrate_check.mjs SITE_RUN13 SITE_TINY OUT      (exit 1 on failure)
import { execFileSync } from 'node:child_process';
import { cpSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { gunzipSync, gzipSync } from 'node:zlib';
import { setAntialiased } from '../../../viewer/spz-flags.mjs';

const tool = fileURLToPath(new URL('../../../viewer/migrate-classic.mjs', import.meta.url));
const [run13, tiny, out] = process.argv.slice(2);
if (!out) throw new Error('usage: node migrate_check.mjs SITE_RUN13 SITE_TINY OUT');
let failures = 0;
const check = (name, ok, detail = '') => {
  console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? `: ${detail}` : ''}`);
  if (!ok) failures++;
};

// legacy copies
rmSync(out, { recursive: true, force: true });
const site = `${out}/site`;
const fresh = `${out}/fresh`;
for (const [src, name] of [
  [run13, 'run13'],
  [tiny, 'tiny'],
]) {
  cpSync(src, `${fresh}/${name}`, { recursive: true });
  mkdirSync(`${site}/2026-11-08/legacy/${name}`, { recursive: true });
}
const unmarkJson = (t) => {
  const o = t.replace(/\n "rasterize_mode": "classic",/, '');
  if (o === t) throw new Error('no marker to remove');
  return o;
};
function unmarkPly(b) {
  const end = b.indexOf('\nend_header');
  const head = b.subarray(0, end).toString('latin1');
  const h = head.replace(',"rasterize_mode":"classic"', '');
  if (h === head) throw new Error('no PLY marker to remove');
  return Buffer.concat([Buffer.from(h, 'latin1'), b.subarray(end)]);
}
for (const name of ['run13', 'tiny']) {
  const d = `${site}/2026-11-08/legacy/${name}`;
  writeFileSync(`${d}/meta.json`, unmarkJson(readFileSync(`${fresh}/${name}/meta.json`, 'utf8')));
  writeFileSync(`${d}/splat.ply`, unmarkPly(readFileSync(`${fresh}/${name}/splat.ply`)));
  writeFileSync(`${d}/splat.spz`, setAntialiased(readFileSync(`${fresh}/${name}/splat.spz`), true));
}
mkdirSync(`${out}/samples`, { recursive: true });
cpSync(`${site}/2026-11-08/legacy/tiny/meta.json`, `${out}/samples/tiny.json`);
cpSync(`${site}/2026-11-08/legacy/tiny/splat.spz`, `${out}/samples/tiny.spz`);
cpSync(`${site}/2026-11-08/legacy/run13/splat.ply`, `${out}/standalone.ply`);
const legacy = `${out}/legacy`;
cpSync(site, `${legacy}/site`, { recursive: true });
cpSync(`${out}/samples`, `${legacy}/samples`, { recursive: true });
cpSync(`${out}/standalone.ply`, `${legacy}/standalone.ply`);

const targets = [site, `${out}/samples/tiny.json`, `${out}/standalone.ply`];
const runTool = (...a) => execFileSync('node', [tool, ...a, ...targets], { encoding: 'utf8' });
const dry = runTool();
console.log(dry);
check(
  'dry run lists 3 meta.json, 3 PLY, 3 SPZ',
  /would change 9 file\(s\) \(3 meta\.json, 3 PLY, 3 SPZ\)/.test(dry),
);
check(
  'dry run changes nothing',
  readFileSync(`${site}/2026-11-08/legacy/run13/meta.json`).equals(
    readFileSync(`${legacy}/site/2026-11-08/legacy/run13/meta.json`),
  ),
);
const wet = runTool('--write');
writeFileSync(`${out}/report.txt`, wet);
check('migration changes 9 files', /changed 9 file\(s\)/.test(wet));

const files = [
  ['site/2026-11-08/legacy/run13', 'run13'],
  ['site/2026-11-08/legacy/tiny', 'tiny'],
];
for (const [rel, name] of files) {
  const a = JSON.parse(readFileSync(`${legacy}/${rel}/meta.json`, 'utf8'));
  const b = JSON.parse(readFileSync(`${out}/${rel}/meta.json`, 'utf8'));
  check(
    `${name}: meta.json = legacy + rasterize_mode`,
    JSON.stringify({ rasterize_mode: 'classic', ...a }) === JSON.stringify(b),
  );
  const pa = readFileSync(`${legacy}/${rel}/splat.ply`);
  const pb = readFileSync(`${out}/${rel}/splat.ply`);
  const ea = pa.indexOf('\nend_header');
  const eb = pb.indexOf('\nend_header');
  check(
    `${name}: PLY body unchanged`,
    pa.subarray(ea).equals(pb.subarray(eb)),
    `${pa.length - ea} bytes`,
  );
  const ha = JSON.parse(
    pa
      .subarray(0, ea)
      .toString('latin1')
      .match(/comment apcg_splat (.*)/)[1],
  );
  const hb = JSON.parse(
    pb
      .subarray(0, eb)
      .toString('latin1')
      .match(/comment apcg_splat (.*)/)[1],
  );
  check(
    `${name}: PLY header info = legacy + rasterize_mode`,
    JSON.stringify({ rasterize_mode: 'classic', ...ha }) === JSON.stringify(hb),
  );
  const sa = gunzipSync(readFileSync(`${legacy}/${rel}/splat.spz`));
  const sb = gunzipSync(readFileSync(`${out}/${rel}/splat.spz`));
  sa[14] &= ~1;
  check(
    `${name}: SPZ decodes to the same bytes but the antialiased flag`,
    sa.equals(sb) && !(sb[14] & 1),
  );
  check(
    `${name}: migrated SPZ = a fresh export's`,
    readFileSync(`${out}/${rel}/splat.spz`).equals(readFileSync(`${fresh}/${name}/splat.spz`)),
  );
}
const again = runTool('--write');
check('a second run changes nothing', /changed 0 file\(s\)/.test(again));

// an interrupted run (failure before the SPZ) is completed by the next one
{
  const r = `${out}/resume`;
  cpSync(`${legacy}/site/2026-11-08/legacy/run13`, r, { recursive: true });
  let failed = false;
  try {
    execFileSync('node', [tool, '--write', `${r}/meta.json`], {
      encoding: 'utf8',
      env: { ...process.env, MIGRATE_FAIL_BEFORE: 'splat.spz' },
      stdio: 'pipe',
    });
  } catch {
    failed = true;
  }
  const metaAfterFail = JSON.parse(readFileSync(`${r}/meta.json`, 'utf8'));
  check(
    'interrupted run: fails, PLY marked, SPZ and meta.json not yet',
    failed &&
      !metaAfterFail.rasterize_mode &&
      readFileSync(`${r}/splat.spz`).equals(
        readFileSync(`${legacy}/site/2026-11-08/legacy/run13/splat.spz`),
      ) &&
      !readFileSync(`${r}/splat.ply`).equals(
        readFileSync(`${legacy}/site/2026-11-08/legacy/run13/splat.ply`),
      ),
  );
  execFileSync('node', [tool, '--write', `${r}/meta.json`], { encoding: 'utf8' });
  const done = ['meta.json', 'splat.ply', 'splat.spz'].every((f) =>
    readFileSync(`${r}/${f}`).equals(readFileSync(`${out}/site/2026-11-08/legacy/run13/${f}`)),
  );
  check('interrupted run: the next run completes it, same files as an uninterrupted one', done);
}

// pairing: NAME.json goes with NAME.spz only; an unrelated splat.spz beside it is not touched
{
  const d = `${out}/pairs`;
  mkdirSync(d, { recursive: true });
  cpSync(`${legacy}/samples/tiny.json`, `${d}/tiny.json`);
  cpSync(`${legacy}/samples/tiny.spz`, `${d}/tiny.spz`);
  cpSync(`${legacy}/samples/tiny.spz`, `${d}/splat.spz`); // flag set: would be cleared if taken
  const before = readFileSync(`${d}/splat.spz`);
  execFileSync('node', [tool, '--write', `${d}/tiny.json`], { encoding: 'utf8' });
  check(
    'pairing: tiny.json migrates tiny.spz, leaves splat.spz alone',
    readFileSync(`${d}/splat.spz`).equals(before) &&
      !(gunzipSync(readFileSync(`${d}/tiny.spz`))[14] & 1),
  );
}

// invalid files: the run is refused and nothing of it changes
{
  const good = gunzipSync(readFileSync(`${legacy}/samples/tiny.spz`));
  const goodPly = readFileSync(`${legacy}/site/2026-11-08/legacy/tiny/splat.ply`);
  const end = goodPly.indexOf('end_header\n') + 'end_header\n'.length;
  const spzWith = (f) => {
    const u = Buffer.from(good);
    f(u);
    return gzipSync(u);
  };
  const metaFile = `${legacy}/site/2026-11-08/legacy/tiny/meta.json`;
  const metaMode = (mode) =>
    readFileSync(metaFile, 'utf8').replace(/^\{\n/, `{\n "rasterize_mode": "${mode}",\n`);
  const plyMode = (mode) =>
    Buffer.from(
      goodPly.toString('latin1').replace('apcg_splat {', `apcg_splat {"rasterize_mode":"${mode}",`),
      'latin1',
    );
  const cases = {
    'SPZ not gzip': { spz: Buffer.from('not a gzip file at all') },
    'SPZ bad magic': { spz: spzWith((u) => u.writeUInt32LE(0x12345678, 0)) },
    'SPZ bad version': { spz: spzWith((u) => u.writeUInt32LE(9, 4)) },
    'SPZ short header': { spz: gzipSync(good.subarray(0, 10)) },
    'SPZ truncated': { spz: gzipSync(good.subarray(0, good.length - 7)) },
    'PLY bad preamble': { ply: Buffer.concat([Buffer.from('plx'), goodPly.subarray(3)]) },
    'PLY ascii format': {
      ply: Buffer.from(
        goodPly.toString('latin1').replace('binary_little_endian', 'ascii'),
        'latin1',
      ),
    },
    'PLY truncated': { ply: goodPly.subarray(0, goodPly.length - 10) },
    'PLY no end_header': { ply: goodPly.subarray(0, end - 12) },
    'conflict: unmarked sidecar, PLY antialiased': { ply: plyMode('antialiased') },
    'conflict: sidecar antialiased, SPZ flag cleared': {
      meta: metaMode('antialiased'),
      ply: plyMode('antialiased'),
      spz: setAntialiased(readFileSync(`${legacy}/samples/tiny.spz`), false),
    },
    'conflict: sidecar classic, SPZ flag antialiased': {
      meta: metaMode('classic'),
      ply: plyMode('classic'),
    },
    'conflict: sidecar antialiased, PLY classic': {
      meta: metaMode('antialiased'),
      ply: plyMode('classic'),
    },
    'PLY kind differs from its sidecar': {
      ply: Buffer.from(
        goodPly.toString('latin1').replace('"kind":"thermal"', '"kind":"rgb"'),
        'latin1',
      ),
    },
  };
  for (const [name, c] of Object.entries(cases)) {
    const d = `${out}/bad/${name.replace(/\W+/g, '_')}`;
    mkdirSync(d, { recursive: true });
    writeFileSync(`${d}/meta.json`, c.meta ?? readFileSync(metaFile));
    writeFileSync(`${d}/splat.ply`, c.ply ?? goodPly);
    writeFileSync(`${d}/splat.spz`, c.spz ?? readFileSync(`${legacy}/samples/tiny.spz`));
    const snap = () =>
      ['meta.json', 'splat.ply', 'splat.spz'].map((f) => readFileSync(`${d}/${f}`));
    const before = snap();
    let code = 0;
    let text = '';
    try {
      text = execFileSync('node', [tool, '--write', `${d}/meta.json`], {
        encoding: 'utf8',
        stdio: 'pipe',
      });
    } catch (e) {
      code = e.status;
      text = e.stdout;
    }
    const same = snap().every((b, i) => b.equals(before[i]));
    check(
      `invalid input refused, nothing changed: ${name}`,
      code === 1 && /REFUSED/.test(text) && same,
      text.split('\n')[0],
    );
  }
}
// consistent explicit markers of another mode: left alone, nothing refused
{
  const d = `${out}/antialiased`;
  mkdirSync(d, { recursive: true });
  const metaFile = `${legacy}/site/2026-11-08/legacy/tiny/meta.json`;
  const goodPly = readFileSync(`${legacy}/site/2026-11-08/legacy/tiny/splat.ply`);
  writeFileSync(
    `${d}/meta.json`,
    readFileSync(metaFile, 'utf8').replace(/^\{\n/, '{\n "rasterize_mode": "antialiased",\n'),
  );
  writeFileSync(
    `${d}/splat.ply`,
    Buffer.from(
      goodPly
        .toString('latin1')
        .replace('apcg_splat {', 'apcg_splat {"rasterize_mode":"antialiased",'),
      'latin1',
    ),
  );
  cpSync(`${legacy}/samples/tiny.spz`, `${d}/splat.spz`);
  const snap = () => ['meta.json', 'splat.ply', 'splat.spz'].map((f) => readFileSync(`${d}/${f}`));
  const before = snap();
  const text = execFileSync('node', [tool, '--write', `${d}/meta.json`], { encoding: 'utf8' });
  check(
    'consistently antialiased run: skipped, nothing changed',
    /skip.*already antialiased/.test(text) && snap().every((b, i) => b.equals(before[i])),
  );
}
process.exit(failures ? 1 : 0);
