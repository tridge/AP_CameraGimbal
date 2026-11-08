// Fake stage work: phases, progress, checkpoints and output files with deterministic content.
import { serveStage } from '../../workers/runtime.js';
import { readJson, writeFile, writeJson } from '../../storage/opfs.js';
import { DESKTOP_FACTOR, PROFILES, cumCostOf, gaussians } from './profile.js';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function* bytes(seed, n, chunk = 1 << 20) {
  let x = seed >>> 0 || 1;
  for (let o = 0; o < n; o += chunk) {
    const u = new Uint32Array(Math.min(chunk, n - o) / 4);
    for (let i = 0; i < u.length; i++) {
      x ^= x << 13;
      x ^= x >>> 17;
      x ^= x << 5;
      u[i] = x;
    }
    yield u;
  }
}

const seedOf = (s) => [...s].reduce((h, c) => Math.imul(h ^ c.charCodeAt(0), 16777619), 2166136261);

// a small real GPU load for the harness to measure: a buffer sized like the Gaussians' state
async function gpuLoad(env) {
  if (!navigator.gpu || env.noGpu) return null;
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: env.powerPreference });
  if (!adapter) return null;
  const device = await adapter.requestDevice({
    requiredLimits: {
      maxBufferSize: Math.min(adapter.limits.maxBufferSize, 1 << 30),
      maxStorageBufferBindingSize: Math.min(adapter.limits.maxStorageBufferBindingSize, 1 << 30),
    },
  });
  const module = device.createShaderModule({
    code: `@group(0) @binding(0) var<storage, read_write> b: array<f32>;
      @compute @workgroup_size(64) fn main(@builtin(global_invocation_id) g: vec3u, @builtin(num_workgroups) n: vec3u) {
        let i = g.x + g.y * n.x * 64u; if (i < arrayLength(&b)) { b[i] = b[i] * 0.999 + 0.001; } }`,
  });
  const pipeline = device.createComputePipeline({
    layout: 'auto',
    compute: { module, entryPoint: 'main' },
  });
  let buf = null;
  let size = 0;
  return {
    step(g) {
      const want = Math.min(256 << 20, Math.ceil((g * 59 * 4 * 2) / (16 << 20)) * (16 << 20)); // params + grads
      if (want !== size) {
        buf?.destroy();
        size = want;
        buf = device.createBuffer({ size, usage: GPUBufferUsage.STORAGE });
      }
      const enc = device.createCommandEncoder();
      const pass = enc.beginComputePass();
      pass.setPipeline(pipeline);
      pass.setBindGroup(
        0,
        device.createBindGroup({
          layout: pipeline.getBindGroupLayout(0),
          entries: [{ binding: 0, resource: { buffer: buf } }],
        }),
      );
      const groups = Math.ceil(size / 4 / 64);
      pass.dispatchWorkgroups(Math.min(groups, 65535), Math.ceil(groups / 65535));
      pass.end();
      device.queue.submit([enc.finish()]);
      return device.queue.onSubmittedWorkDone();
    },
    bytes: () => size,
    destroy: () => device.destroy(),
  };
}

serveStage(async (msg, api) => {
  const p = PROFILES[msg.stage];
  const speed = msg.config.speed ?? 1;
  const factor = (msg.env.gpuClass === 'desktop' ? DESKTOP_FACTOR : 1) / speed;
  const cumCost = cumCostOf(p);
  const msPerCost = (p.seconds * factor * 1000) / cumCost(p.total);
  const seed = seedOf(`${msg.stage}:${msg.config.dummySeed ?? 0}`);
  let jitter = seed;
  const rand = () => (jitter = Math.imul(jitter ^ (jitter >>> 15), 2246822519) >>> 0) / 2 ** 32;

  let done = 0;
  let ckptGen = msg.firstCheckpointGeneration;
  if (msg.checkpoint) {
    const state = await readJson(`${msg.checkpoint.dir}/state.json`);
    done = state.done;
    api.log(`resuming from checkpoint ${msg.checkpoint.generation} at ${done} ${p.unit}`);
  }
  for (const [phase, sec] of Object.entries(p.overhead)) {
    if (phase === 'eval') continue;
    api.phase(phase);
    await sleep((sec * 1000) / speed);
    api.throwIfStopped();
  }
  api.phase('work');
  const gpu = p.gpu
    ? await gpuLoad(msg.env).catch((e) => (api.log(`no GPU load: ${e.message}`, 'warn'), null))
    : null;
  let peakGpuBytes = 0;
  const tick = 50;
  let t = performance.now();
  let owed = 0; // ms of simulated work not yet done
  api.progress(done, p.total, p.unit === 'iterations' ? { gaussians: gaussians(done) } : undefined);
  try {
    while (done < p.total) {
      await sleep(tick);
      api.throwIfStopped();
      const now = performance.now();
      owed += (now - t) * (0.9 + 0.2 * rand());
      t = now;
      while (done < p.total) {
        const next = Math.min(p.total, done + p.step);
        const cost = (cumCost(next) - cumCost(done)) * msPerCost;
        if (cost > owed) break;
        owed -= cost;
        done = next;
        if (
          p.checkpointEvery &&
          done < p.total &&
          Math.floor(done / p.checkpointEvery) > Math.floor((done - p.step) / p.checkpointEvery)
        ) {
          const g = ckptGen++;
          const dir = `${msg.ckptBase}${g}`;
          const at = done;
          await api.checkpoint(
            g,
            async () => {
              // slow, chunked write so a reload can land in the middle of it
              const ckptMs = msg.config.ckptMs ?? 1500;
              const n = 16;
              let k = 0;
              await writeFile(
                `${dir}/params.bin`,
                (async function* () {
                  for (const chunk of bytes(seed + at, n << 20)) {
                    yield chunk;
                    if (++k < n) await sleep(ckptMs / n);
                  }
                })(),
              );
              await writeJson(`${dir}/state.json`, { done: at, gaussians: gaussians(at) });
            },
            { done: at, unit: p.unit },
          );
          t = performance.now();
        }
      }
      const extra =
        p.unit === 'iterations'
          ? { gaussians: gaussians(done) }
          : p.uncertain
            ? { iterations: Math.round((done % 1) * 50) }
            : undefined;
      if (gpu) {
        await gpu.step(gaussians(done));
        peakGpuBytes = Math.max(peakGpuBytes, gpu.bytes());
      }
      api.progress(p.uncertain ? Math.floor(done * 20) / 20 : done, p.total, extra);
    }
  } finally {
    gpu?.destroy();
  }
  if (p.overhead.eval) {
    api.phase('eval');
    await sleep((p.overhead.eval * 1000) / speed);
  }
  api.phase('work');
  await writeFile(`${msg.outDir}/data.bin`, bytes(seed, p.outBytes));
  await writeJson(`${msg.outDir}/out.json`, {
    stage: msg.stage,
    inputs: msg.inputHashes,
    units: done,
  });
  return { units: done, peakGpuBytes };
});
