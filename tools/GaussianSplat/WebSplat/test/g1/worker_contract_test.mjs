// Worker contract test (after codex's G1 round 2 reproduction): brush-worker.js against a mocked
// brush-js and WebGPU. Any failure (dataset open, training step, result readback) must post
// 'failed', release everything and refuse later results; a clean run must dispose with ok.
// usage: node --experimental-vm-modules worker_contract_test.mjs
import vm from 'node:vm';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const file = join(dirname(fileURLToPath(import.meta.url)), '../../app/src/stages/train/brush-worker.js');
const source = fs.readFileSync(file, 'utf8');
const tick = () => new Promise((r) => setTimeout(r, 10));

async function scenario(failurePoint) {
  const posted = [];
  const calls = [];
  class GPUDevice { createShaderModule() { return { getCompilationInfo: async () => ({ messages: [] }) }; } }
  const dev = new GPUDevice();
  Object.assign(dev, {
    queue: { onSubmittedWorkDone: async () => {} }, addEventListener: () => {}, lost: new Promise(() => {}),
    destroy: () => calls.push('device.destroy'),
  });
  class BrushApp {
    initExisting() {}
    startTrainingFromDirectory() {
      return {
        trainSteps: async () => {
          if (failurePoint === 'trainSteps') throw Error('injected trainSteps rejection');
          return [{ kind: 1, iter: 10, elapsedMs: 1, free() {} }];
        },
        currentSplats: () => null,
        renderEval: async () => {
          if (failurePoint === 'renderEval') throw Error('injected GPU readback rejection');
          return [];
        },
        exportPly: async () => new Uint8Array([1, 2]),
        free: () => calls.push('training.free'),
      };
    }
    free() { calls.push('app.free'); }
  }
  const brush = { default: async () => ({ _initialize() {} }), BrushApp, BrushMessageKind: { TrainStep: 1 } };
  const context = vm.createContext({
    console, GPUDevice, Uint8Array, setTimeout, clearTimeout,
    navigator: {
      gpu: { requestAdapter: async () => ({ limits: {}, features: [], info: {}, requestDevice: async () => dev }) },
      storage: {
        getDirectory: async () => {
          if (failurePoint === 'dataset-open') throw Error('injected missing dataset');
          return {};
        },
      },
    },
    self: { postMessage: (m) => posted.push(m), close: () => calls.push('self.close') },
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
  await send({ type: 'start', pkg: 'mock', dataset: [], config: { 'total-train-iters': 10 } });
  await send({ type: 'renderEval' });
  await send({ type: 'exportPly' });
  await send({ type: 'dispose' });
  await tick();
  return { posted, calls };
}

let failures = 0;
const check = (name, ok, detail) => {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${ok ? '' : ': ' + detail}`);
  if (!ok) failures++;
};
for (const point of ['dataset-open', 'trainSteps', 'renderEval']) {
  const { posted, calls } = await scenario(point);
  const types = posted.map((m) => m.type);
  check(`${point}: failed posted`, types.includes('failed'), types.join(','));
  check(`${point}: no PLY served`, !types.includes('ply'), types.join(','));
  check(`${point}: disposal not ok`, posted.find((m) => m.type === 'disposed')?.ok === false, JSON.stringify(posted.at(-1)));
  check(`${point}: device destroyed`, calls.includes('device.destroy'), calls.join(','));
  check(`${point}: worker closed`, calls.includes('self.close'), calls.join(','));
}
{
  const { posted, calls } = await scenario('none');
  const types = posted.map((m) => m.type);
  check('clean run: done, renders, PLY', ['done', 'evalRenders', 'ply'].every((t) => types.includes(t)), types.join(','));
  check('clean run: no failure', !types.includes('failed'), types.join(','));
  check('clean run: disposal ok', posted.find((m) => m.type === 'disposed')?.ok === true, JSON.stringify(posted.at(-1)));
  check('clean run: released and closed', ['training.free', 'app.free', 'device.destroy', 'self.close'].every((c) => calls.includes(c)), calls.join(','));
}
console.log(failures ? `${failures} check(s) failed` : 'worker contract: PASS');
process.exit(failures ? 1 : 0);
