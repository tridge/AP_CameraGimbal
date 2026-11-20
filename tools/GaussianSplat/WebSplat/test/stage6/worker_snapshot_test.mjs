// brush-worker.js snapshot labels (codex stage 6 round 1, finding 2): a checkpoint or preview
// requested while a trainSteps batch is running must hold the parameters of the iteration it is
// labelled with. The mocked trainer advances its step one at a time with awaits in between and
// its exportCheckpoint/renderEval return the step they saw.
// usage: node --experimental-vm-modules worker_snapshot_test.mjs
import vm from 'node:vm';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

// WORKER: another brush-worker.js (negative control: the version before the fix)
const file = process.env.WORKER ?? join(dirname(fileURLToPath(import.meta.url)), '../../app/src/stages/train/brush-worker.js');
const source = fs.readFileSync(file, 'utf8');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function run({ total = 500, batch = 50, requests = 6 } = {}) {
  const posted = [];
  let step = 0;
  class GPUDevice { createShaderModule() { return { getCompilationInfo: async () => ({ messages: [] }) }; } }
  const dev = new GPUDevice();
  Object.assign(dev, {
    queue: { onSubmittedWorkDone: async () => {} }, addEventListener: () => {}, lost: new Promise(() => {}), destroy() {},
  });
  class BrushApp {
    initExisting() {}
    startTrainingFromDirectory() {
      return {
        trainSteps: async (n) => {
          const out = [];
          for (let i = 0; i < n && step < total; i++) {
            await sleep(0); // the splat slot advances while this call is in flight
            step++;
            out.push({ kind: 1, iter: step, elapsedMs: step, free() {} });
          }
          return out;
        },
        currentSplats: () => null,
        exportCheckpoint: async () => { await sleep(1); return new Uint8Array(new Uint32Array([step]).buffer); },
        renderEval: async () => { await sleep(1); return [{ name: 'v', data: new Float32Array([step]) }]; },
        exportPly: async () => new Uint8Array([1]),
        free() {},
      };
    }
    free() {}
  }
  const brush = { default: async () => ({ _initialize() {} }), BrushApp, BrushMessageKind: { TrainStep: 1 } };
  const context = vm.createContext({
    console, GPUDevice, Uint8Array, Uint32Array, Float32Array, setTimeout, clearTimeout, Promise,
    navigator: {
      gpu: { requestAdapter: async () => ({ limits: {}, features: [], info: {}, requestDevice: async () => dev }) },
      storage: { getDirectory: async () => ({}) },
    },
    self: { postMessage: (m) => posted.push(m), close() {} },
  });
  const dep = new vm.SyntheticModule(Object.keys(brush), function () {
    for (const k of Object.keys(brush)) this.setExport(k, brush[k]);
  }, { context });
  await dep.link(() => {});
  await dep.evaluate();
  const mod = new vm.SourceTextModule(source, { context, importModuleDynamically: () => dep });
  await mod.link(() => {});
  await mod.evaluate();
  const send = (data) => context.self.onmessage({ data });
  const started = send({ type: 'start', pkg: 'mock', dataset: [], config: { 'total-train-iters': total }, stepsPerMessage: batch });
  // requests while batches are in flight, at varying points inside them
  for (let k = 0; k < requests; k++) {
    await sleep(3 + 7 * k);
    send({ type: k % 2 ? 'previewRender' : 'checkpoint' });
  }
  await started;
  await sleep(50);
  return posted;
}

let failures = 0;
const check = (name, ok, detail = '') => {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${ok ? '' : ': ' + detail}`);
  if (!ok) failures++;
};
const posted = await run();
const snaps = posted.filter((m) => m.type === 'checkpoint' || m.type === 'previewRender');
const seen = (m) => (m.type === 'checkpoint' ? new Uint32Array(m.bytes.buffer)[0] : m.views[0].data[0]);
check('every request answered', snaps.length === 6, `${snaps.length} snapshots: ${posted.map((m) => m.type).join(',')}`);
const bad = snaps.filter((m) => seen(m) !== m.iter);
check('labels equal the parameters\' iteration', bad.length === 0, bad.map((m) => `${m.type} label ${m.iter}, parameters ${seen(m)}`).join('; '));
check('snapshots taken at batch boundaries', snaps.every((m) => m.iter % 50 === 0), snaps.map((m) => m.iter).join(','));
check('training completed', posted.some((m) => m.type === 'done'), posted.map((m) => m.type).join(','));
console.log(failures ? `${failures} check(s) failed` : 'worker snapshots: PASS');
process.exit(failures ? 1 : 0);
