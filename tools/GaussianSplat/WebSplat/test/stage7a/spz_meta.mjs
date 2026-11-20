// Splat metadata in the SPZ gzip header (viewer/spz-meta.mjs): the reader on good and malformed
// headers, the JS and native exports carrying their PLY's apcg_splat JSON, Spark (Node) decoding an
// SPZ with the metadata as the same SPZ without, gunzip and Python reading it, the flag rewrite
// keeping it (native and JS alike), and viewer/embed-meta.mjs on a copy of the native site.
//   node spz_meta.mjs [NATIVE JS OUT]      (exit 1 on failure)
// Default: $WEBSPLAT_DATA/stage7a/{native,js,spz-meta}, as run_tests.sh leaves them.
import { execFileSync } from 'node:child_process';
import {
  chmodSync,
  cpSync,
  mkdirSync,
  readdirSync,
  readFileSync,
  rmSync,
  statSync,
  symlinkSync,
  writeFileSync,
} from 'node:fs';
import { fileURLToPath } from 'node:url';
import { gunzipSync } from 'node:zlib';
import { setAntialiased } from '../../../viewer/spz-flags.mjs';
import { setSpzFlags } from '../../app/src/stages/export/spz.js';
import { env } from '../harness/lib/env.mjs';
import {
  gzipHeader,
  setMetaMode,
  spzMeta,
  spzMetaText,
  withSpzMeta,
} from '../../../viewer/spz-meta.mjs';

const viewer = fileURLToPath(new URL('../../../viewer/', import.meta.url));
const { transcodeSpz, getSplatFileType } = await import(
  `${viewer}node_modules/@sparkjsdev/spark/dist/spark.module.js`
);
const PY = fileURLToPath(new URL('../../../.venv/bin/python', import.meta.url));

const argv = process.argv.slice(2);
if (argv.length && argv.length !== 3) throw new Error('usage: node spz_meta.mjs [NATIVE JS OUT]');
const O = argv.length ? null : `${env('WEBSPLAT_DATA')}/stage7a`;
const [native, js, out] = argv.length ? argv : [`${O}/native`, `${O}/js`, `${O}/spz-meta`];
rmSync(out, { recursive: true, force: true });
mkdirSync(out, { recursive: true });
let failures = 0;
const check = (name, ok, detail = '') => {
  console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? `: ${detail}` : ''}`);
  if (!ok) failures++;
};
const read = (p) => readFileSync(p);
const site = `${native}/site/2026-11-08/tiny/run1`;
const plyText = (p) => {
  const b = read(p);
  const head = b.subarray(0, b.indexOf('\nend_header\n')).toString('latin1');
  return head
    .split('\n')
    .find((l) => l.startsWith('comment apcg_splat '))
    .slice('comment apcg_splat '.length);
};
const eq = (a, b) => Buffer.from(a).equals(Buffer.from(b));

// the header layout
{
  const h = gzipHeader('{"a":1}');
  check(
    'header: FLG FEXTRA, XLEN, subfield AP, LEN, JSON',
    eq(h, [31, 139, 8, 4, 0, 0, 0, 0, 0, 255, 11, 0, 0x41, 0x50, 7, 0, ...Buffer.from('{"a":1}')]),
  );
  check(
    'no metadata: the fixed 10-byte header',
    eq(gzipHeader(null), [31, 139, 8, 0, 0, 0, 0, 0, 0, 255]),
  );
  let threw = false;
  try {
    gzipHeader('x'.repeat(65532));
  } catch {
    threw = true;
  }
  check('metadata over 65531 bytes is refused', threw);
}

const spz = read(`${js}/splat.spz`);
const plain = withSpzMeta(spz, null);
const text = spzMetaText(spz);

// exports: the PLY's apcg_splat JSON, exactly
check('JS SPZ embeds its PLY apcg_splat JSON', text === plyText(`${js}/splat.ply`));
check(
  'native SPZ (ply2spz.mjs) embeds its PLY apcg_splat JSON',
  spzMetaText(read(`${site}/splat.spz`)) === plyText(`${site}/splat.ply`),
);
check(
  'native SPZ metadata = meta.json without downloads',
  (() => {
    const { downloads, ...m } = JSON.parse(read(`${site}/meta.json`));
    return JSON.stringify(spzMeta(read(`${site}/splat.spz`))) === JSON.stringify(m);
  })(),
);
check('stripped SPZ has no metadata, 10-byte header', spzMeta(plain) === null && plain[3] === 0);

// the reader fails safe
{
  const body = plain.subarray(10);
  const raw = (extra, flg = 4) => {
    const h = Buffer.from([31, 139, 8, flg, 0, 0, 0, 0, 0, 255]);
    const x = Buffer.alloc(2);
    x.writeUInt16LE(extra.length);
    return Buffer.concat([h, x, extra, body]);
  };
  const sub = (id, data) => {
    const l = Buffer.alloc(2);
    l.writeUInt16LE(data.length);
    return Buffer.concat([Buffer.from(id), l, Buffer.from(data)]);
  };
  const cases = {
    'no FEXTRA': [plain, null],
    'other subfield only': [raw(sub('RA', 'xyz')), null],
    'AP after another subfield': [
      raw(Buffer.concat([sub('RA', 'xyz'), sub('AP', '{"k":1}')])),
      { k: 1 },
    ],
    'subfield LEN past XLEN': [raw(Buffer.from([0x41, 0x50, 200, 0, 0x7b, 0x7d])), null],
    'AP, then a stray byte': [raw(Buffer.concat([sub('AP', '{"k":1}'), Buffer.from([0])])), null],
    'AP, then a subfield past XLEN': [
      raw(Buffer.concat([sub('AP', '{"k":1}'), Buffer.from([0x52, 0x41, 9, 0, 1])])),
      null,
    ],
    'AP, then a 3-byte partial subfield header': [
      raw(Buffer.concat([sub('AP', '{"k":1}'), Buffer.from([0x52, 0x41, 0])])),
      null,
    ],
    'AP, then a valid subfield': [
      raw(Buffer.concat([sub('AP', '{"k":1}'), sub('RA', 'xyz')])),
      { k: 1 },
    ],
    'XLEN past the file': [
      Buffer.from([31, 139, 8, 4, 0, 0, 0, 0, 0, 255, 0xff, 0xff, 0x41]),
      null,
    ],
    'not UTF-8': [raw(sub('AP', Buffer.from([0x7b, 0xff, 0x7d]))), null],
    'not JSON': [raw(sub('AP', '{"k":')), null],
    'a JSON array': [raw(sub('AP', '[1,2]')), null],
    'JSON null': [raw(sub('AP', 'null')), null],
    'reserved FLG bits': [raw(sub('AP', '{"k":1}'), 4 | 0x20), null],
    'not gzip': [Buffer.from('ply\nformat binary_little_endian 1.0\n'), null],
    empty: [Buffer.alloc(0), null],
  };
  for (const [name, [b, want]] of Object.entries(cases)) {
    let got;
    try {
      got = spzMeta(b);
    } catch (e) {
      got = `threw ${e.message}`;
    }
    check(`reader: ${name}`, JSON.stringify(got) === JSON.stringify(want), JSON.stringify(got));
  }
  // FNAME and FCOMMENT after FEXTRA (as gzip -N might write): still found, body offset right
  const h = Buffer.concat([
    Buffer.from([31, 139, 8, 4 | 8 | 16, 0, 0, 0, 0, 0, 255, 11, 0]),
    sub('AP', '{"k":2}'),
    Buffer.from('name\0comment\0'),
  ]);
  const b = Buffer.concat([h, body]);
  check(
    'reader: FEXTRA with FNAME and FCOMMENT',
    spzMeta(b)?.k === 2 && eq(gunzipSync(b), gunzipSync(plain)) && eq(withSpzMeta(b, null), plain),
  );
}

// Spark 2.4.0 (Node, its WASM decoder): the SPZ with metadata decodes as the SPZ without; also with
// metadata far longer than its 1 KiB file-type probe chunks
{
  const big = withSpzMeta(
    spz,
    JSON.stringify({ ...JSON.parse(text), description: 'd'.repeat(60000) }),
  );
  const enc = async (b) =>
    (
      await transcodeSpz({
        inputs: [{ fileBytes: new Uint8Array(b), pathOrUrl: 'x.spz' }],
        maxSh: 3,
      })
    ).fileBytes;
  const ref = await enc(plain);
  for (const [name, b] of [
    ['metadata', spz],
    ['60 kB metadata', big],
  ]) {
    check(`Spark detects the SPZ with ${name}`, getSplatFileType(new Uint8Array(b)) === 'spz');
    check(`Spark decodes the SPZ with ${name} as the SPZ without`, eq(await enc(b), ref));
    // no file name: the type comes from the content
    const t = await transcodeSpz({ inputs: [{ fileBytes: new Uint8Array(b) }], maxSh: 3 });
    check(`Spark reads the SPZ with ${name} without a file name`, eq(t.fileBytes, ref));
  }
  writeFileSync(`${out}/big.spz`, big);
  writeFileSync(`${out}/plain.spz`, plain);
  // gzip tools
  for (const f of ['big.spz', 'plain.spz']) {
    let ok = true;
    try {
      execFileSync('gunzip', ['-t', `${out}/${f}`]);
    } catch {
      ok = false;
    }
    check(`gunzip -t ${f}`, ok);
  }
  const py = execFileSync(
    PY,
    [
      '-I',
      '-c',
      'import gzip, hashlib, sys\nfor f in sys.argv[1:]: print(hashlib.sha256(gzip.open(f).read()).hexdigest())',
      `${js}/splat.spz`,
      `${out}/big.spz`,
      `${out}/plain.spz`,
    ],
    { encoding: 'utf8' },
  )
    .trim()
    .split('\n');
  check('Python gzip reads them, same data', py.length === 3 && py.every((h) => h === py[0]));
}

// the flag rewrite keeps the metadata and sets its rasterize_mode; a round trip is the export
{
  const aa = setAntialiased(spz, true);
  check(
    'setAntialiased(on): metadata kept, rasterize_mode antialiased',
    spzMeta(aa)?.rasterize_mode === 'antialiased' &&
      JSON.stringify({ ...spzMeta(aa), rasterize_mode: 'classic' }) ===
        JSON.stringify(spzMeta(spz)),
  );
  check(
    'setAntialiased round trip = the export, byte for byte',
    eq(setAntialiased(aa, false), spz),
  );
  check('setAntialiased without metadata adds none', spzMeta(setAntialiased(plain, true)) === null);
  // the JS flag rewrite (WebSplat export) gives the native bytes, both directions
  const on = (f) => f | 1;
  const off = (f) => f & ~1;
  for (const [name, src, fn, flag] of [
    ['classic -> antialiased', spz, on, true],
    ['antialiased -> classic', aa, off, false],
    ['no metadata, set', plain, on, true],
    ['no metadata, clear', setAntialiased(plain, true), off, false],
  ]) {
    const n = setAntialiased(src, flag);
    const j = await setSpzFlags(new Uint8Array(src), fn);
    check(
      `setSpzFlags (JS) = setAntialiased (native), ${name}`,
      eq(j, n),
      `${spzMeta(j)?.rasterize_mode ?? 'no metadata'}, ${j.length} bytes`,
    );
  }
  const t1 = '{"title":"says \\"rasterize_mode\\":\\"x\\"","rasterize_mode":"classic"}';
  const t2 = '{\n "kind": "thermal",\n "gaussians": 8.0\n}';
  check(
    'setMetaMode: replaces the key, not text in a string',
    setMetaMode(t1, 'antialiased') ===
      '{"title":"says \\"rasterize_mode\\":\\"x\\"","rasterize_mode":"antialiased"}',
  );
  check(
    "setMetaMode: inserts in the text's layout, rest unchanged",
    setMetaMode(t2, 'classic') ===
      '{\n "rasterize_mode": "classic",\n "kind": "thermal",\n "gaussians": 8.0\n}',
  );
}

// embed-meta.mjs on a copy of the native site whose SPZ lost its metadata
{
  const tool = `${viewer}embed-meta.mjs`;
  const s = `${out}/site`;
  const run = `${s}/2026-11-08/tiny/run1`;
  cpSync(site, run, { recursive: true });
  const stripped = withSpzMeta(read(`${site}/splat.spz`), null);
  writeFileSync(`${run}/splat.spz`, stripped);
  mkdirSync(`${out}/samples`);
  writeFileSync(`${out}/samples/tiny.spz`, stripped);
  cpSync(`${site}/meta.json`, `${out}/samples/tiny.json`);
  const go = (...a) =>
    execFileSync('node', [tool, ...a, s, `${out}/samples/tiny.json`], { encoding: 'utf8' });
  const dry = go();
  console.log(dry.trimEnd());
  check(
    'embed-meta dry run: 2 SPZ, nothing written',
    /would change 2 SPZ/.test(dry) && eq(read(`${run}/splat.spz`), stripped),
  );
  go('--write');
  check(
    'embed-meta: site SPZ = the native export, byte for byte',
    eq(read(`${run}/splat.spz`), read(`${site}/splat.spz`)),
  );
  const sample = read(`${out}/samples/tiny.spz`);
  const { downloads, ...want } = JSON.parse(read(`${site}/meta.json`));
  check(
    'embed-meta: NAME.json pair gets the sidecar less downloads, decoded identical',
    JSON.stringify(spzMeta(sample)) === JSON.stringify(want) &&
      eq(gunzipSync(sample), gunzipSync(stripped)),
  );
  check('embed-meta: a second run changes nothing', /changed 0 SPZ/.test(go('--write')));
  // the file mode is kept; a planted name beside it is not followed; no temporary file is left
  {
    const d = `${out}/perm`;
    cpSync(site, d, { recursive: true });
    writeFileSync(`${d}/splat.spz`, stripped);
    chmodSync(`${d}/splat.spz`, 0o600);
    writeFileSync(`${out}/victim`, 'victim');
    symlinkSync(`${out}/victim`, `${d}/splat.spz.embedding`);
    const before = readdirSync(d).sort();
    execFileSync('node', [tool, '--write', `${d}/meta.json`], { encoding: 'utf8' });
    const mode = statSync(`${d}/splat.spz`).mode & 0o7777;
    check(
      'embed-meta: a 0600 SPZ stays 0600',
      mode === 0o600 && spzMeta(read(`${d}/splat.spz`)) !== null,
      mode.toString(8),
    );
    check(
      'embed-meta: planted symlink not followed, no temporary file left',
      read(`${out}/victim`).toString() === 'victim' &&
        JSON.stringify(readdirSync(d).sort()) === JSON.stringify(before),
      readdirSync(d).join(' '),
    );
  }
  // a broken SPZ is refused and left alone
  const bad = `${out}/bad`;
  cpSync(site, bad, { recursive: true });
  const broken = Buffer.from(stripped.subarray(0, stripped.length - 100));
  writeFileSync(`${bad}/splat.spz`, broken);
  let code = 0;
  let txt = '';
  try {
    txt = execFileSync('node', [tool, '--write', `${bad}/meta.json`], {
      encoding: 'utf8',
      stdio: 'pipe',
    });
  } catch (e) {
    code = e.status;
    txt = e.stdout;
  }
  check(
    'embed-meta: truncated SPZ refused, unchanged',
    code === 1 && /REFUSED/.test(txt) && eq(read(`${bad}/splat.spz`), broken),
  );
}
process.exit(failures ? 1 : 0);
