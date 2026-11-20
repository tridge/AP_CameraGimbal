/** Shared stage 2 Node runner: the port on native-decoded frames vs native extract_frames.py with
 * the same options. */
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { dataDir, TOOLS } from '../stage1/harness.mjs';
import { indexMkv } from '../../app/src/stages/ingest/ingest.js';
import { propose, selectFrames, SelectError } from '../../app/src/stages/select/select.js';
import { RawDirDecoder, FsStore } from './node-io.mjs';

export const python = path.join(TOOLS, 'GaussianSplat/.venv/bin/python');
const here = path.dirname(new URL(import.meta.url).pathname);

// selection option -> extract_frames.py flag
const FLAGS = {
  lat: '--lat',
  lon: '--lon',
  target_alt: '--target-alt',
  hotspot_min_c: '--hotspot-min-c',
  hotspot_max_error: '--hotspot-max-error',
  rotation: '--rotation',
  hfov: '--hfov',
  margin: '--margin',
  radius: '--radius',
  start: '--start',
  end: '--end',
  step: '--step',
  max_range: '--max-range',
  max_median_dev: '--max-median-dev',
};

export function nativeArgs(params) {
  const args = [];
  for (const [k, v] of Object.entries(params)) {
    if (k === 'locate_hotspot') {
      if (!v) args.push('--no-locate-hotspot');
    } else if (v != null) args.push(FLAGS[k], String(v)); // String(): shortest round-trip repr
  }
  return args;
}

const fileHash = (f) => crypto.createHash('sha256').update(fs.readFileSync(f)).digest('hex');
const inputHashes = new Map();
// native outputs are cached per input file content, native code revision, options and injection
const nativeRevision = () =>
  [
    path.join(TOOLS, 'GaussianSplat/extract_frames.py'),
    path.join(TOOLS, 'GaussianSplat/splat_common.py'),
    path.join(here, 'native_injected.py'),
  ]
    .map(fileHash)
    .join(',');

/** Run native extract_frames.py once per (input, native code, options, injection); returns
 * {dir, ok, out}. inject: see native_injected.py. */
export function runNative(mkv, base, params, inject = null) {
  const args = nativeArgs(params);
  if (!inputHashes.has(mkv)) inputHashes.set(mkv, fileHash(mkv));
  const key = crypto
    .createHash('sha256')
    .update(
      JSON.stringify({ args, inject, input: inputHashes.get(mkv), revision: nativeRevision() }),
    )
    .digest('hex')
    .slice(0, 16);
  const dir = path.join(base, 'native-extract', key);
  const status = path.join(dir, 'status.json');
  if (!fs.existsSync(status)) {
    fs.rmSync(dir, { recursive: true, force: true });
    fs.mkdirSync(dir, { recursive: true });
    let ok = true,
      out = '';
    const cmd = inject
      ? [path.join(here, 'native_injected.py'), JSON.stringify(inject), mkv, dir, ...args]
      : [path.join(TOOLS, 'GaussianSplat/extract_frames.py'), mkv, dir, ...args];
    try {
      out = execFileSync(python, cmd, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
    } catch (e) {
      ok = false;
      out = (e.stdout ?? '') + (e.stderr ?? '');
    }
    fs.writeFileSync(status, JSON.stringify({ args, inject, ok, out }));
  }
  return { dir, ...JSON.parse(fs.readFileSync(status, 'utf8')) };
}

/** The port's view of an injection: patched metadata and a decoder that blanks frames. */
export function injected({ ingest, decoder }, inject) {
  if (!inject) return { ingest, decoder };
  const blank = new Set(inject.blank ?? []);
  const frames = ingest.frames.map((f) => {
    const patch = inject.meta?.[f.index];
    if (!patch) return f;
    const meta = structuredClone(f.meta);
    for (const [key, value] of Object.entries(patch)) {
      const parts = key.split('.');
      const last = parts.pop();
      const d = parts.reduce((o, k) => o[k], meta);
      if (value === '__delete__') delete d[last];
      else d[last] = value;
    }
    return { ...f, meta };
  });
  const patched = { ...ingest, frames };
  return {
    ingest: patched,
    decoder: {
      async *decode(wanted, o) {
        for await (const f of decoder.decode(wanted, o)) {
          let data = f.data;
          if (blank.has(f.index)) {
            data = new Uint16Array(f.data.length).fill(18000);
            data[777] = 30000;
          }
          yield { ...f, data, meta: frames[f.index].meta };
        }
      },
    },
  };
}

export async function prepare(mkv) {
  const base = path.join(dataDir(), 'stage12', path.basename(mkv, '.mkv'));
  if (!fs.existsSync(path.join(base, 'native/raw')))
    execFileSync(
      python,
      [path.join(here, '../stage1/native_dump.py'), mkv, path.join(base, 'native'), '--raw'],
      { stdio: 'inherit' },
    );
  const ingest = await indexMkv(await fs.openAsBlob(mkv));
  const decoder = new RawDirDecoder(ingest, path.join(base, 'native/raw'));
  return { base, ingest, decoder };
}

export { propose };

/** One case: port + native + compare. Returns {ok, port, native, compare} */
export async function runCase(
  ctx,
  name,
  params,
  { mode = 'single', samples, inject = null, nativeError = null, portError = null } = {},
) {
  const { mkv, base } = ctx;
  const { ingest, decoder } = injected(ctx, inject);
  if (inject) samples = null; // the hot frames must be decoded through the injection
  const out = path.join(base, 'select-node', name);
  const store = new FsStore(out);
  let port,
    error = null;
  const t = performance.now();
  try {
    port = await selectFrames(ingest, decoder, store, params, {
      mode,
      samples,
      sourceName: path.basename(mkv),
    });
  } catch (e) {
    if (!(e instanceof SelectError)) throw e;
    error = e.message;
  }
  const ms = performance.now() - t;
  const native = runNative(mkv, base, params, inject);
  let ok,
    compare = '';
  if (error || !native.ok) {
    // both must fail: with the same message, or the expected native exception and port message
    ok =
      !!error &&
      !native.ok &&
      (native.out.includes(error) ||
        (!!nativeError &&
          new RegExp(nativeError).test(native.out) &&
          new RegExp(portError).test(error)));
    compare = `port: ${error ?? 'ok'}; native: ${native.ok ? 'ok' : native.out.trim().split('\n').at(-1)}`;
  } else {
    try {
      compare = execFileSync(python, [path.join(here, 'compare_select.py'), native.dir, out], {
        encoding: 'utf8',
      });
      ok = true;
    } catch (e) {
      compare = e.stdout + e.stderr;
      ok = false;
    }
    // the port's log must match the native messages that carry decisions
    for (const line of port.log.filter((l) =>
      /^(skipped|\d+ of \d+ frames|hotspot (rays|is far))/.test(l),
    ))
      if (!native.out.includes(line.replace(/: check .*$/, ''))) {
        ok = false;
        compare += `\nlog line not in native output: ${line}`;
      }
  }
  return { ok, ms, port, error, native, compare, store };
}
