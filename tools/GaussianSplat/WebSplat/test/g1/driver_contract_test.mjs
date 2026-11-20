// Driver contract test (after codex's G1 round 2 reproduction): run_browser.mjs with a mocked
// browser. It must refuse a reused output directory, ignore verdicts of other attempts, reject
// empty, misnamed, wrong-sized or non-finite renders and an inconsistent PLY, and accept a
// complete attempt.
// usage: node --experimental-vm-modules driver_contract_test.mjs DATASET SCRATCH_DIR
import vm from 'node:vm';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const src = join(dirname(fileURLToPath(import.meta.url)), 'run_browser.mjs');
const [dataset, scratch] = process.argv.slice(2);
const meta = JSON.parse(fs.readFileSync(join(dataset, 'g1.json'), 'utf8'));
const names = fs.readFileSync(join(dataset, 'eval_views.txt'), 'utf8').split('\n').filter(Boolean);

function ply(n, apcg) {
  const props = ['x', 'y', 'z'];
  const head = `ply\nformat binary_little_endian 1.0\ncomment apcg_splat ${JSON.stringify(apcg)}\nelement vertex ${n}\n`
    + props.map((p) => `property float ${p}\n`).join('') + 'end_header\n';
  return Buffer.concat([Buffer.from(head, 'latin1'), Buffer.alloc(n * props.length * 4)]);
}
const goodPly = ply(10, { kind: 'thermal', temperature_scale: meta.temperature_scale });
const render = (fill) => Buffer.from(new Float32Array(meta.width * meta.height).fill(fill).buffer);

// write = (out, attempt) => artifacts this attempt produces; disconnect: the CDP link drops
async function run(name, { prefill, write, disconnect, verdictAttempt }) {
  const out = join(scratch, name);
  fs.rmSync(out, { recursive: true, force: true });
  if (prefill) { fs.mkdirSync(out, { recursive: true }); prefill(out); }
  const handlers = {};
  let code = null;
  class Exit extends Error {}
  const page = {
    on: (n, fn) => { handlers[n] = fn; },
    setViewportSize: async () => {},
    goto: async (url) => {
      const attempt = new URL(url).searchParams.get('attempt');
      write?.(out, attempt);
      const v = { type: 'disposed', ok: true, gpuErrors: 0, shaderErrors: 0, attempt: verdictAttempt === undefined ? attempt : verdictAttempt };
      if (v.attempt === null) delete v.attempt;
      fs.writeFileSync(join(out, 'verdict.json'), JSON.stringify(v));
      if (!disconnect) for (const m of [v, { stage: 'finished' }]) handlers.console({ text: () => `G1 ${JSON.stringify(m)}`, type: () => 'log' });
    },
    waitForFunction: async () => { if (disconnect) throw Error('Target page, context or browser has been closed'); },
    close: async () => {},
  };
  const chromium = { connectOverCDP: async () => ({ contexts: () => [{ newPage: async () => page }], close: async () => {} }) };
  const context = vm.createContext({
    console: { log() {}, error() {} }, URL, Date, Float32Array, JSON, Number, Buffer,
    process: { argv: ['node', src, '--dataset', dataset, '--pkg', out, '--out', out, '--port', '0', '--cdp', 'mock'], env: {},
      exit: (c) => { code = c; throw new Exit(); } },
    setInterval: () => 0, clearInterval: () => {}, setTimeout: (f) => f(),
  });
  const synthetic = async (ns) => {
    const keys = Object.keys(ns);
    const mod = new vm.SyntheticModule(keys, function () { for (const k of keys) this.setExport(k, ns[k]); }, { context });
    await mod.link(() => {});
    await mod.evaluate();
    return mod;
  };
  const mod = new vm.SourceTextModule(fs.readFileSync(src, 'utf8'), {
    context, initializeImportMeta: (m) => { m.url = 'file://' + src; }, importModuleDynamically: () => synthetic({ chromium }),
  });
  await mod.link(async (spec) => synthetic(spec === 'node:child_process'
    ? { execFile: (...args) => args.at(-1)(Error('no GPU')) } : await import(spec)));
  try {
    await mod.evaluate();
  } catch (e) {
    if (!(e instanceof Exit)) throw e;
  }
  return code;
}

const writeGood = (out, { skip, rename, size, fill = 0.5, plyBuf = goodPly } = {}) => {
  fs.mkdirSync(join(out, 'eval'), { recursive: true });
  names.forEach((n, i) => {
    if (i === skip) return;
    const buf = i === 0 && size ? Buffer.alloc(size) : render(i === 0 ? fill : 0.5);
    fs.writeFileSync(join(out, 'eval', (i === 0 && rename ? rename : n) + '.f32'), buf);
  });
  fs.writeFileSync(join(out, 'export.ply'), plyBuf);
};
const cases = [
  ['reused output directory', 2, { prefill: (o) => fs.writeFileSync(join(o, 'verdict.json'), '{"ok":true}'), write: (o) => writeGood(o) }],
  ['complete attempt', 0, { write: (o) => writeGood(o) }],
  ['complete attempt, link lost', 0, { write: (o) => writeGood(o), disconnect: true }],
  ['verdict of another attempt, link lost', 1, { write: (o) => writeGood(o), disconnect: true, verdictAttempt: 'other' }],
  ['verdict without attempt, link lost', 1, { write: (o) => writeGood(o), disconnect: true, verdictAttempt: null }],
  ['missing render', 1, { write: (o) => writeGood(o, { skip: 3 }) }],
  ['misnamed render', 1, { write: (o) => writeGood(o, { rename: 'wrong' }) }],
  ['empty render', 1, { write: (o) => writeGood(o, { size: 1 }) }],
  ['NaN render', 1, { write: (o) => writeGood(o, { fill: NaN }) }],
  ['empty PLY', 1, { write: (o) => writeGood(o, { plyBuf: Buffer.alloc(0) }) }],
  ['truncated PLY', 1, { write: (o) => writeGood(o, { plyBuf: goodPly.subarray(0, goodPly.length - 4) }) }],
  ['PLY without thermal metadata', 1, { write: (o) => writeGood(o, { plyBuf: ply(10, { kind: 'rgb' }) }) }],
];
let failures = 0;
for (const [name, want, c] of cases) {
  const code = await run(name.replace(/[^a-z]+/gi, '-'), c);
  const ok = code === want;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}: exit ${code}${ok ? '' : `, expected ${want}`}`);
  if (!ok) failures++;
}
console.log(failures ? `${failures} case(s) failed` : 'driver contract: PASS');
process.exit(failures ? 1 : 0);
