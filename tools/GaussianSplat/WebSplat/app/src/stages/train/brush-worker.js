// Thermal training with the Brush fork (brush-js wasm) in a module worker. No canvas: the
// laptop's NVIDIA/Vulkan Chrome fails WebGPU canvas presentation, compute is unaffected.
//
// Contract: one worker per run. 'start' once; after 'done', any of 'renderEval' and 'exportPly';
// then 'dispose' (also valid at any time to cancel), which releases everything and closes the
// worker. Any GPU validation/OOM/internal error, shader compilation error, device loss or
// exception (start-up, training or a result operation) fails the run: training stops, 'failed'
// is posted, resources are released once no wasm call is outstanding, and no result is served.
//
// in:  {type: 'start', pkg, dataset: [OPFS directory names], config: {kebab-case Brush options},
//       stepsPerMessage, captureShaders, traceBuffers, stressCleanup, tasksPerSubmit}
//      {type: 'renderEval'}  {type: 'exportPly', comments: [..]}  {type: 'dispose'}
//      while training or trained: {type: 'checkpoint'} (raw parameters, PLY bytes) and
//      {type: 'previewRender'} (eval-view renders), served at the next batch boundary; iter is the
//      step whose parameters they hold
// out: {type: 'device', adapter, features}  {type: 'dataset', train, eval}
//      {type: 'progress', iter, total, elapsedMs, splats, buffers (traceBuffers)}
//      {type: 'refine', iter, splats}  {type: 'eval', iter, psnr}  {type: 'warning', text}
//      {type: 'gpuerror', count, iter, message}  {type: 'shaderError', label, messages}
//      {type: 'shader', label, code} (captureShaders)
//      {type: 'done', iter, elapsedMs, splats}  {type: 'evalRenders', views}  {type: 'ply', bytes}
//      {type: 'checkpoint', iter, bytes}  {type: 'previewRender', iter, views}
//      {type: 'failure', reason}: the run has failed (sent at once; 'failed' follows when idle)
//      {type: 'failed', reason, gpuErrors, shaderErrors}  {type: 'error', message}
//      {type: 'disposed', ok, reason, gpuErrors, shaderErrors}

let state = 'idle'; // idle, running, trained, failed, disposing, disposed
let failure = null;
let brush = null;
let app = null;
let training = null;
let device = null;
let gpuErrors = 0;
let shaderErrors = 0;
let currentIter = 0;
const post = (m, transfer) => self.postMessage(m, transfer || []);

Error.stackTraceLimit = 100; // full Rust panic stacks

function release() {
  try {
    training?.free();
  } catch {
    /* already freed */
  }
  try {
    app?.free();
  } catch {
    /* already freed */
  }
  training = null;
  app = null;
  device?.destroy();
  device = null;
}

// Outstanding wasm calls: releasing the wasm objects under one of them would trap, so failure
// cleanup and disposal wait until none is in flight (settle).
let busy = 0;
let failureReported = false;
let disposeOk = null; // set when disposal is requested
async function wasmCall(fn) {
  busy++;
  try {
    return await fn();
  } finally {
    busy--;
    settle();
  }
}

function settle() {
  if (busy > 0) return;
  if (state === 'failed' && !failureReported) {
    failureReported = true;
    release();
    post({ type: 'failed', reason: failure, gpuErrors, shaderErrors });
  }
  if (disposeOk !== null && state !== 'disposed') {
    state = 'disposed';
    release();
    post({ type: 'disposed', ok: disposeOk, reason: failure, gpuErrors, shaderErrors });
    self.close();
  }
}

// Terminal: the first failure wins, from any state but disposed. The run is invalid, no result
// is served afterwards, and resources are released as soon as no wasm call is outstanding.
function fail(reason) {
  if (failure || state === 'disposed') return;
  failure = reason;
  state = 'failed';
  // the cause at once: 'failed' waits for wasm calls to settle, and a call that traps never does
  post({ type: 'failure', reason });
  settle();
}

// wgpu's naga WGSL output uses subgroup builtins without `enable subgroups;`; Chrome rejects it
// wherever the adapter offers subgroups (CubeCL then takes the subgroup path), as on the laptop.
const SUBGROUP_USE = /\bsubgroup[A-Z]\w*\s*\(|@builtin\(subgroup_(size|invocation_id)\)/;
export function fixWgsl(code) {
  if (SUBGROUP_USE.test(code) && !/^\s*enable\s+[^;]*\bsubgroups\b/m.test(code))
    return 'enable subgroups;\n' + code;
  return code;
}

// apply the WGSL fix; report compilation errors (Dawn otherwise only says "invalid due to a
// previous error") and fail the run on them
function watchShaders(capture) {
  const create = GPUDevice.prototype.createShaderModule;
  GPUDevice.prototype.createShaderModule = function (desc) {
    desc = { ...desc, code: fixWgsl(desc.code) };
    const m = create.call(this, desc);
    if (capture) post({ type: 'shader', label: desc.label, code: desc.code });
    m.getCompilationInfo().then((info) => {
      const messages = info.messages
        .filter((x) => x.type === 'error')
        .map((x) => `${x.lineNum}:${x.linePos} ${x.message}`);
      if (messages.length) {
        shaderErrors++;
        post({ type: 'shaderError', label: desc.label, messages });
        fail(`shader ${desc.label}: ${messages[0]}`);
      }
    });
    return m;
  };
}

// diagnostic: GPU buffer bytes created, destroyed, and garbage collected without destroy()
// (wgpu's web backend never destroys; the CubeCL fork does for its pools)
const bufferStats = { created: 0, destroyed: 0, collected: 0, big: {}, byUsage: {} };
function traceBuffers() {
  const registry = new FinalizationRegistry((info) => {
    if (info.live) bufferStats.collected += info.size;
  });
  const create = GPUDevice.prototype.createBuffer;
  GPUDevice.prototype.createBuffer = function (desc) {
    const b = create.call(this, desc);
    bufferStats.created += desc.size;
    const u = 'usage ' + desc.usage + (desc.mappedAtCreation ? ' mapped' : '');
    bufferStats.byUsage[u] = (bufferStats.byUsage[u] || 0) + desc.size;
    if (desc.size >= 32 << 20) {
      const k = Math.round(desc.size / (1 << 20)) + ' MiB ' + u;
      bufferStats.big[k] = (bufferStats.big[k] || 0) + 1;
    }
    const info = { size: desc.size, live: true };
    registry.register(b, info);
    const destroy = b.destroy.bind(b);
    b.destroy = () => {
      if (info.live) {
        info.live = false;
        bufferStats.destroyed += desc.size;
      }
      destroy();
    };
    return b;
  };
}

async function createDevice() {
  const adapter = await navigator.gpu?.requestAdapter({ powerPreference: 'high-performance' });
  if (!adapter) throw new Error('no WebGPU adapter');
  if (!adapter.features.has('subgroups')) throw new Error('WebGPU adapter lacks subgroups');
  // what cubecl asks for on its own device: every adapter feature and its limits
  const requiredLimits = {};
  for (const k in adapter.limits) requiredLimits[k] = adapter.limits[k];
  const dev = await adapter.requestDevice({
    requiredFeatures: [...adapter.features],
    requiredLimits,
  });
  dev.addEventListener('uncapturederror', (e) => {
    gpuErrors++;
    if (gpuErrors <= 20 || gpuErrors % 100 === 0) {
      post({
        type: 'gpuerror',
        count: gpuErrors,
        iter: currentIter,
        message: e.error.message.slice(0, 300),
      });
    }
    fail(`GPU error at iteration ${currentIter}: ${e.error.message.slice(0, 200)}`);
  });
  dev.lost.then((i) => {
    if (state !== 'disposed') fail(`device lost (${i.reason}): ${i.message}`);
  });
  post({
    type: 'device',
    adapter: `${adapter.info.vendor} ${adapter.info.architecture} ${adapter.info.description}`,
    features: [...adapter.features],
  });
  return { adapter, dev };
}

async function openDir(path) {
  let dir = await navigator.storage.getDirectory();
  for (const p of path) dir = await dir.getDirectoryHandle(p);
  return dir;
}

async function start(msg) {
  state = 'running';
  brush = await import(msg.pkg);
  const exports = await brush.default();
  // static constructors, exactly once per instance (the fork's _initialize; see brush-js lib.rs)
  exports._initialize();
  const kinds = Object.fromEntries(
    Object.entries(brush.BrushMessageKind)
      .filter(([, v]) => typeof v === 'number')
      .map(([k, v]) => [v, k]),
  );
  watchShaders(!!msg.captureShaders);
  if (msg.traceBuffers) traceBuffers();
  app = new brush.BrushApp();
  if (msg.tasksPerSubmit) app.setTasksPerSubmit(msg.tasksPerSubmit); // buffer-lifetime stress test
  const { adapter, dev } = await createDevice();
  device = dev;
  app.initExisting(adapter, dev, dev.queue);
  const dir = await openDir(msg.dataset);
  const total = msg.config['total-train-iters'];
  training = app.startTrainingFromDirectory(dir, async (cfg) => ({ ...cfg, ...msg.config }));
  let iter = 0;
  let elapsedMs = 0;
  const splatCount = () => {
    // a snapshot holds the splat tensors' GPU buffers until freed
    const s = training.currentSplats();
    const n = s?.numSplats;
    s?.free();
    return n;
  };
  while (state === 'running') {
    const msgs = await wasmCall(() => training.trainSteps(msg.stepsPerMessage || 100));
    if (!msgs.length) break;
    for (const m of msgs) {
      const kind = kinds[m.kind];
      if (kind === 'TrainStep') {
        iter = m.iter;
        currentIter = iter;
        elapsedMs = m.elapsedMs;
      } else if (kind === 'RefineStep') {
        post({ type: 'refine', iter: m.iter, splats: m.numSplats });
      } else if (kind === 'EvalResult') {
        post({ type: 'eval', iter: m.iter, psnr: m.psnr });
      } else if (kind === 'Warning') {
        post({ type: 'warning', text: m.text });
      } else if (kind === 'DatasetLoaded') {
        post({ type: 'dataset', train: m.trainViews, eval: m.evalViews });
      }
      m.free();
    }
    if (state !== 'running') break;
    await drainSnapshots(); // at this batch boundary: the splats are those of `iter`
    if (state !== 'running') break;
    injectFault(msg.testFault, iter);
    if (msg.stressCleanup) app.memoryCleanup(); // buffer-lifetime stress test
    post({
      type: 'progress',
      iter,
      total,
      elapsedMs,
      splats: splatCount(),
      ...(msg.traceBuffers
        ? {
            buffers: {
              ...bufferStats,
              big: { ...bufferStats.big },
              byUsage: { ...bufferStats.byUsage },
            },
          }
        : {}),
    });
    if (iter >= total) break;
  }
  if (state === 'running' && iter < total)
    fail(`training stream ended at iteration ${iter} of ${total}`);
  if (state === 'running') {
    // let errors from the last submitted work arrive before declaring success
    await device.queue.onSubmittedWorkDone();
  }
  if (state !== 'running') {
    await drainSnapshots(); // rejects what is still queued
    return; // fail() has reported (or will, once no wasm call is busy)
  }
  state = 'trained';
  await drainSnapshots();
  post({ type: 'done', iter, elapsedMs, splats: splatCount() });
}

// recovery tests only (testFault {deviceLostAt, allocAt}): lose the device, or ask for an
// allocation the device must refuse, once training reaches the iteration
let faulted = false;
function injectFault(f, iter) {
  if (!f || faulted) return;
  if (f.deviceLostAt != null && iter >= f.deviceLostAt) {
    faulted = true;
    device.destroy();
  } else if (f.allocAt != null && iter >= f.allocAt) {
    faulted = true;
    device.createBuffer({ size: 2 ** 40, usage: GPUBufferUsage.STORAGE });
  }
}

// a snapshot of the splats while training continues (checkpoints, previews): taken between two
// trainSteps batches, so its parameters are exactly those of the iteration it is labelled with
const pendingSnapshots = [];
function snapshot(fn) {
  if (state !== 'running' && state !== 'trained')
    return Promise.reject(new Error(`no snapshot: run is ${state}${failure ? ` (${failure})` : ''}`));
  return new Promise((resolve, reject) => {
    pendingSnapshots.push({ fn, resolve, reject });
    if (state === 'trained') drainSnapshots();
  });
}

async function drainSnapshots() {
  while (pendingSnapshots.length) {
    const { fn, resolve, reject } = pendingSnapshots.shift();
    if (state !== 'running' && state !== 'trained') {
      reject(new Error(`run failed before a snapshot: ${failure}`));
      continue;
    }
    const iter = currentIter;
    try {
      const out = await wasmCall(fn);
      if (state !== 'running' && state !== 'trained')
        reject(new Error(`run failed during a snapshot: ${failure}`));
      else resolve({ out, iter });
    } catch (e) {
      reject(e);
    }
  }
}

async function result(fn) {
  if (state !== 'trained')
    throw new Error(`no result: run is ${state}${failure ? ` (${failure})` : ''}`);
  const out = await wasmCall(fn);
  await device.queue.onSubmittedWorkDone();
  if (state !== 'trained') throw new Error(`run failed while producing a result: ${failure}`);
  return out;
}

self.onmessage = async (e) => {
  const msg = e.data;
  if (msg.type === 'start' && state !== 'idle') {
    post({
      type: 'error',
      message: `start: worker already used (${state}); use a new worker per run`,
    });
    return;
  }
  try {
    if (msg.type === 'start') {
      await start(msg);
    } else if (msg.type === 'renderEval') {
      const views = await result(() => training.renderEval());
      post(
        { type: 'evalRenders', views },
        views.map((v) => v.data.buffer),
      );
    } else if (msg.type === 'exportPly') {
      const bytes = await result(() => training.exportPly(msg.comments || []));
      post({ type: 'ply', bytes }, [bytes.buffer]);
    } else if (msg.type === 'checkpoint') {
      const { out, iter } = await snapshot(() => training.exportCheckpoint());
      post({ type: 'checkpoint', iter, bytes: out }, [out.buffer]);
    } else if (msg.type === 'previewRender') {
      const { out, iter } = await snapshot(() => training.renderEval());
      post(
        { type: 'previewRender', iter, views: out },
        out.map((v) => v.data.buffer),
      );
    } else if (msg.type === 'dispose') {
      if (disposeOk === null) disposeOk = state === 'trained' && !failure;
      if (state === 'running') fail('disposed while training');
      else if (state === 'trained') state = 'disposing'; // no more results
      settle();
      setTimeout(() => self.close(), 10000); // a wasm call that never settles
    }
  } catch (err) {
    // any exception, in start-up, training or a result operation, invalidates the run
    fail(String(err?.message || err));
    post({ type: 'error', message: String(err?.stack || err) });
  }
};
