// WebGPU descriptor matcher reproducing sfm.py match_pairs() on CUDA:
//   a, b = fp16(normalised descriptors); sim = fp16(a @ b.T) (fp32 accumulation)
//   forward: top-2 of each row; dist = sqrt(max(2 - 2 sim, 0)); keep if d0 < ratio * d1 (f32)
//   reverse: column argmax (first index on ties) must point back to the row
//   pairs where an image has fewer than 2 descriptors are skipped.
// One GEMM per pair: each workgroup owns 64 rows of A, walks all of B in 64-column tiles, keeps
// the rows' top-2 in registers and folds (fp16 sim, row) keys into a per-column atomicMax.
// The ratio test runs on the CPU from the fp16 top-2 so it is f32-exact. The only remaining
// difference from cuBLAS is the fp32 summation order before the fp16 rounding.
import { quantizeDescriptors, f16BitsToF32 } from './quantize.js';
import { STATUS_MATCHED, STATUS_SKIPPED } from './format.js';

const WGSL = /* wgsl */ `
@group(0) @binding(0) var<storage, read> D: array<vec2u>;          // fp16 descriptors, 32 per row
@group(0) @binding(1) var<storage, read> P: array<vec4u>;          // per pair: offA, na, offB, nb
@group(0) @binding(2) var<storage, read_write> R: array<vec2u>;    // per row: e1 | e2 << 16, j
@group(0) @binding(3) var<storage, read_write> C: array<atomic<u32>>; // per column: h16 << 16 | ~row
@group(0) @binding(4) var<uniform> U: vec4u;                       // x: per-pair stride of R and C

var<workgroup> tA: array<vec4f, 256>;
var<workgroup> tB: array<vec4f, 256>;
var<workgroup> red: array<vec2u, 512>;
var<workgroup> ck: array<atomic<u32>, 64>;

// fp16 bits of x >= 0, round to nearest even (WGSL's own f32->f16 rounding is unspecified)
fn h16(x: f32) -> u32 {
  let u = bitcast<u32>(x);
  if (u >= 0x38800000u) { return (u - 0x38000000u + 0xfffu + ((u >> 13u) & 1u)) >> 13u; }
  return u32(round(x * 16777216.0));
}

fn merge(x: vec3u, y: vec3u) -> vec3u {  // top-2 (e1, e2, idx); 0 = empty
  if (y.x > x.x) { return vec3u(y.x, max(x.x, y.y), y.z); }
  return vec3u(x.x, max(x.y, y.x), x.z);
}

fn load(off: u32, n: u32, r: u32, k: u32) -> vec4f {
  if (r >= n) { return vec4f(0.0); }
  let w = D[(off + r) * 32u + k];
  return vec4f(unpack2x16float(w.x), unpack2x16float(w.y));
}

@compute @workgroup_size(16, 16)
fn main(@builtin(workgroup_id) wid: vec3u, @builtin(local_invocation_id) lid: vec3u,
        @builtin(local_invocation_index) li: u32) {
  let pr = P[wid.y];
  let na = pr.y; let nb = pr.w; let r0 = wid.x * 64u;
  if (r0 >= na) { return; }
  let base = wid.y * U.x;
  var t = array<vec3u, 4>(vec3u(0u), vec3u(0u), vec3u(0u), vec3u(0u));
  for (var c0 = 0u; c0 < nb; c0 += 64u) {
    if (li < 64u) { atomicStore(&ck[li], 0u); }
    var acc = array<vec4f, 4>(vec4f(0.0), vec4f(0.0), vec4f(0.0), vec4f(0.0));
    for (var k = 0u; k < 32u; k += 4u) {
      workgroupBarrier();
      tA[li] = load(pr.x, na, r0 + li / 4u, k + li % 4u);
      tB[li] = load(pr.z, nb, c0 + li / 4u, k + li % 4u);
      workgroupBarrier();
      for (var kk = 0u; kk < 4u; kk++) {
        let a0 = tA[(lid.y * 4u + 0u) * 4u + kk]; let a1 = tA[(lid.y * 4u + 1u) * 4u + kk];
        let a2 = tA[(lid.y * 4u + 2u) * 4u + kk]; let a3 = tA[(lid.y * 4u + 3u) * 4u + kk];
        let b0 = tB[(lid.x * 4u + 0u) * 4u + kk]; let b1 = tB[(lid.x * 4u + 1u) * 4u + kk];
        let b2 = tB[(lid.x * 4u + 2u) * 4u + kk]; let b3 = tB[(lid.x * 4u + 3u) * 4u + kk];
        acc[0] += vec4f(dot(a0, b0), dot(a0, b1), dot(a0, b2), dot(a0, b3));
        acc[1] += vec4f(dot(a1, b0), dot(a1, b1), dot(a1, b2), dot(a1, b3));
        acc[2] += vec4f(dot(a2, b0), dot(a2, b1), dot(a2, b2), dot(a2, b3));
        acc[3] += vec4f(dot(a3, b0), dot(a3, b1), dot(a3, b2), dot(a3, b3));
      }
    }
    var colKey = vec4u(0u);
    for (var r = 0u; r < 4u; r++) {
      let row = r0 + lid.y * 4u + r;
      for (var c = 0u; c < 4u; c++) {
        let j = c0 + lid.x * 4u + c;
        if (row < na && j < nb) {
          let h = h16(acc[r][c]);
          t[r] = merge(t[r], vec3u(h + 1u, 0u, j));
          colKey[c] = max(colKey[c], (h << 16u) | (0xffffu - row));
        }
      }
    }
    for (var c = 0u; c < 4u; c++) { if (colKey[c] != 0u) { atomicMax(&ck[lid.x * 4u + c], colKey[c]); } }
    workgroupBarrier();
    if (li < 64u) {
      let v = atomicLoad(&ck[li]);
      if (c0 + li < nb && v != 0u) { atomicMax(&C[base + c0 + li], v); }
    }
  }
  // merge the 16 column lanes of each row: 16 -> 8 in red, then 8 -> 1
  for (var r = 0u; r < 4u; r++) {
    let s = (lid.y * 4u + r) * 8u + lid.x % 8u;
    if (lid.x < 8u) { red[s] = vec2u(t[r].x | (t[r].y << 16u), t[r].z); }
  }
  workgroupBarrier();
  for (var r = 0u; r < 4u; r++) {
    let s = (lid.y * 4u + r) * 8u + lid.x % 8u;
    if (lid.x >= 8u) {
      let o = red[s];
      let m = merge(vec3u(o.x & 0xffffu, o.x >> 16u, o.y), t[r]);
      red[s] = vec2u(m.x | (m.y << 16u), m.z);
    }
  }
  workgroupBarrier();
  if (li < 64u && r0 + li < na) {
    var m = vec3u(0u);
    for (var q = 0u; q < 8u; q++) { let o = red[li * 8u + q]; m = merge(m, vec3u(o.x & 0xffffu, o.x >> 16u, o.y)); }
    R[base + r0 + li] = vec2u(m.x | (m.y << 16u), m.z);
  }
}`;

export { WGSL as MATCH_WGSL };

const MiB = 1 << 20;
export const MATCH_DEFAULTS = {
  ratio: 0.8,
  poolBytes: 128 * MiB, // descriptor slots on the GPU (one storage binding)
  batchPairs: 32, // pairs per submit
  depth: 2, // submits in flight
  powerPreference: 'high-performance',
};

export class MatchError extends Error {
  constructor(name, message, done) {
    super(message);
    this.name = name;
    this.done = done;
  }
}

// dist(e) and ratio * dist(e) in f32, indexed by fp16 bits; as in torch: sqrt(clamp(2 - 2 s, 0))
function distTables(ratio) {
  const f = Math.fround,
    r = f(ratio),
    d = new Float32Array(0x7c00),
    t = new Float32Array(0x7c00);
  for (let h = 0; h < 0x7c00; h++) {
    d[h] = f(Math.sqrt(Math.max(f(2 - 2 * f16BitsToF32(h)), 0)));
    t[h] = f(r * d[h]);
  }
  return { d, t };
}

export async function createDevice(opts = {}) {
  if (!globalThis.navigator?.gpu) throw new MatchError('NoWebGPU', 'WebGPU is not available');
  const adapter = await navigator.gpu.requestAdapter({
    powerPreference: opts.powerPreference || MATCH_DEFAULTS.powerPreference,
  });
  if (!adapter) throw new MatchError('NoWebGPU', 'no WebGPU adapter');
  const want = opts.poolBytes || MATCH_DEFAULTS.poolBytes;
  const requiredLimits = {};
  // only ask beyond the defaults (128 MiB binding, 256 MiB buffer) when a bigger pool is wanted
  if (want > 128 * MiB)
    requiredLimits.maxStorageBufferBindingSize = Math.min(
      want,
      adapter.limits.maxStorageBufferBindingSize,
    );
  if (want > 256 * MiB) requiredLimits.maxBufferSize = Math.min(want, adapter.limits.maxBufferSize);
  const device = await adapter.requestDevice({ requiredLimits });
  device.addEventListener('uncapturederror', (e) =>
    console.error('WebGPU uncaptured error:', e.error.message),
  );
  return { adapter, device };
}

// descriptors(k) -> {n, data: Uint8Array(n * 128)} for frame k (may be async).
// opts: MATCH_DEFAULTS, signal (AbortSignal), onBatch, wgsl (replacement kernel, for tests).
// Errors are MatchError: NoWebGPU, Limits, TooManyFeatures, OutOfMemory, Shader, GPUError,
// MapFailed, DeviceLost, AbortError; .done is the number of pairs finished. All buffers this
// call creates are destroyed before it returns or throws.
// pairs: Uint32Array of (i, j). onBatch({done, total, results, stats}) after each batch; the
// results array of {i, j, status, matches} is in pair order.
export async function matchPairs(device, pairs, counts, descriptors, opts = {}) {
  const o = { ...MATCH_DEFAULTS, ...opts };
  const npairs = pairs.length / 2;
  const maxCount = counts.reduce((a, b) => Math.max(a, b), 0);
  if (maxCount > 0xfffe)
    throw new MatchError('TooManyFeatures', `${maxCount} descriptors in one image; at most 65534`);
  const stride = Math.max(64, Math.ceil(maxCount / 64) * 64);
  const slotBytes = stride * 256;
  const { d: dist, t: thr } = distTables(o.ratio);
  const stats = {
    pairs: npairs,
    matches: 0,
    uploads: 0,
    uploadMs: 0,
    gpuWaitMs: 0,
    finishMs: 0,
    batches: 0,
    poolSlots: 0,
  };

  let lost = null;
  device.lost.then((info) => {
    lost = info;
  });
  const owned = []; // every buffer this call creates, destroyed on any exit
  let done = 0;
  // one classification for setup and run: anything while the device is lost is DeviceLost
  const classify = (e) => {
    if (lost) return new MatchError('DeviceLost', `GPU device lost: ${lost.message}`, done);
    if (e instanceof MatchError) return e;
    return new MatchError(
      e?.name === 'GPUPipelineError' ? 'Shader' : 'GPUError',
      e?.message || String(e),
      done,
    );
  };
  // run fn inside balanced validation and out-of-memory scopes
  const scoped = async (fn) => {
    device.pushErrorScope('out-of-memory');
    device.pushErrorScope('validation');
    let value, thrown;
    try {
      value = await fn();
    } catch (e) {
      thrown = e;
    }
    const verr = await device.popErrorScope().catch(() => null);
    const oom = await device.popErrorScope().catch(() => null);
    if (thrown) throw thrown;
    return { value, verr, oom };
  };
  const create = async (desc) => {
    const { value: b, verr, oom } = await scoped(() => device.createBuffer(desc));
    owned.push(b);
    if (verr) throw new MatchError('GPUError', verr.message);
    return oom ? null : b;
  };
  const limit = Math.min(device.limits.maxStorageBufferBindingSize, device.limits.maxBufferSize);
  let slots = Math.min(Math.floor(Math.min(o.poolBytes, limit) / slotBytes), counts.length);
  if (slots < 2)
    throw new MatchError(
      'Limits',
      `storage binding limit ${limit} too small for two images of ${maxCount} descriptors`,
    );

  const inflight = [];
  const results = new Array(npairs);
  try {
    // the pool is the big allocation: halve it on allocation failure
    let pool = null;
    for (; slots >= 2; slots >>= 1) {
      pool = await create({
        size: slots * slotBytes,
        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
      });
      if (pool) break;
    }
    if (!pool) throw new MatchError('OutOfMemory', 'could not allocate the descriptor pool');
    stats.poolSlots = slots;
    const batchPairs = Math.max(1, Math.min(o.batchPairs, slots >> 1));
    const rBytes = batchPairs * stride * 8,
      cBytes = batchPairs * stride * 4;
    const S = GPUBufferUsage.STORAGE;
    const bufs = [
      await create({ size: batchPairs * 16, usage: S | GPUBufferUsage.COPY_DST }),
      await create({ size: rBytes, usage: S | GPUBufferUsage.COPY_SRC }),
      await create({ size: cBytes, usage: S | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST }),
      await create({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST }),
    ];
    const staging = [];
    for (let s = 0; s < o.depth; s++)
      staging.push(
        await create({
          size: rBytes + cBytes,
          usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST,
        }),
      );
    if (bufs.some((b) => !b) || staging.some((b) => !b))
      throw new MatchError('OutOfMemory', 'could not allocate match buffers');
    const [pbuf, rbuf, cbuf, ubuf] = bufs;
    device.queue.writeBuffer(ubuf, 0, new Uint32Array([stride, 0, 0, 0]));

    const { value: pipeline, verr } = await scoped(() =>
      device.createComputePipelineAsync({
        layout: 'auto',
        compute: {
          module: device.createShaderModule({ code: o.wgsl || WGSL }),
          entryPoint: 'main',
        },
      }),
    );
    if (verr) throw new MatchError('Shader', verr.message);
    const bind = device.createBindGroup({
      layout: pipeline.getBindGroupLayout(0),
      entries: [pool, pbuf, rbuf, cbuf, ubuf].map((buffer, binding) => ({
        binding,
        resource: { buffer },
      })),
    });
    if (lost) throw classify();

    // LRU residency of frames in pool slots
    const slotOf = new Map(),
      owner = new Array(slots).fill(-1),
      used = new Float64Array(slots);
    let tick = 0,
      qbuf = new Uint32Array(stride * 64);
    const resident = async (k, pinned) => {
      let s = slotOf.get(k);
      if (s === undefined) {
        s = -1;
        for (let q = 0; q < slots; q++)
          if (!pinned.has(owner[q]) && (s < 0 || used[q] < used[s])) s = q;
        if (owner[s] >= 0) slotOf.delete(owner[s]);
        const { n, data } = await descriptors(k);
        const t0 = performance.now();
        quantizeDescriptors(data, n, qbuf);
        device.queue.writeBuffer(pool, s * slotBytes, qbuf, 0, n * 64);
        stats.uploadMs += performance.now() - t0;
        stats.uploads++;
        owner[s] = k;
        slotOf.set(k, s);
      }
      used[s] = ++tick;
      return s;
    };

    const finish = (batch, mapped) => {
      const t0 = performance.now();
      const R = new Uint32Array(mapped, 0, rBytes / 4),
        C = new Uint32Array(mapped, rBytes, cBytes / 4);
      batch.items.forEach(({ p, i, j, na }, q) => {
        const out = new Uint32Array(2 * na);
        let m = 0;
        for (let r = 0; r < na; r++) {
          const w = R[2 * (q * stride + r)],
            col = R[2 * (q * stride + r) + 1];
          const e1 = w & 0xffff,
            e2 = w >>> 16;
          if (!(dist[e1 - 1] < thr[e2 - 1])) continue;
          if (0xffff - (C[q * stride + col] & 0xffff) !== r) continue;
          out[2 * m] = r;
          out[2 * m + 1] = col;
          m++;
        }
        results[p] = { i, j, status: STATUS_MATCHED, matches: out.slice(0, 2 * m) };
        stats.matches += m;
      });
      stats.finishMs += performance.now() - t0;
    };

    let nextStage = 0;
    const drainOne = async () => {
      const b = inflight.shift();
      const t0 = performance.now();
      try {
        await b.map;
      } catch (e) {
        throw new MatchError(
          lost ? 'DeviceLost' : 'MapFailed',
          lost ? `GPU device lost: ${lost.message}` : e.message,
          done,
        );
      }
      stats.gpuWaitMs += performance.now() - t0;
      finish(b, b.staging.getMappedRange());
      b.staging.unmap();
      done += b.count;
      o.onBatch && o.onBatch({ done, total: npairs, stats, results });
    };

    let p = 0;
    while (p < npairs) {
      if (o.signal?.aborted) break;
      if (lost) throw new MatchError('DeviceLost', `GPU device lost: ${lost.message}`, done);
      // a batch: up to batchPairs pairs whose frames fit in the pool together
      const items = [],
        pinned = new Set();
      let skipped = 0;
      for (; p < npairs && items.length < batchPairs; p++) {
        const i = pairs[2 * p],
          j = pairs[2 * p + 1];
        if (counts[i] < 2 || counts[j] < 2) {
          results[p] = { i, j, status: STATUS_SKIPPED, matches: new Uint32Array(0) };
          skipped++;
          continue;
        }
        const extra = (pinned.has(i) ? 0 : 1) + (pinned.has(j) ? 0 : 1);
        if (pinned.size + extra > slots) break;
        pinned.add(i);
        pinned.add(j);
        items.push({ p, i, j, na: counts[i], nb: counts[j] });
      }
      done += skipped;
      if (!items.length) continue;
      const table = new Uint32Array(4 * items.length);
      for (const [q, it] of items.entries()) {
        const sa = await resident(it.i, pinned),
          sb = await resident(it.j, pinned);
        table.set([sa * stride, it.na, sb * stride, it.nb], 4 * q);
      }
      if (inflight.length >= o.depth) await drainOne();
      const st = staging[nextStage];
      nextStage = (nextStage + 1) % o.depth;
      device.queue.writeBuffer(pbuf, 0, table);
      const enc = device.createCommandEncoder();
      enc.clearBuffer(cbuf);
      const pass = enc.beginComputePass();
      pass.setPipeline(pipeline);
      pass.setBindGroup(0, bind);
      pass.dispatchWorkgroups(Math.ceil(Math.max(...items.map((it) => it.na)) / 64), items.length);
      pass.end();
      enc.copyBufferToBuffer(rbuf, 0, st, 0, rBytes);
      enc.copyBufferToBuffer(cbuf, 0, st, rBytes, cBytes);
      device.queue.submit([enc.finish()]);
      inflight.push({ items, count: items.length, staging: st, map: st.mapAsync(GPUMapMode.READ) });
      stats.batches++;
    }
    while (inflight.length) await drainOne();
  } catch (e) {
    throw classify(e);
  } finally {
    // let in-flight maps settle before destroying their buffers
    await Promise.allSettled(inflight.map((b) => b.map));
    owned.forEach((b) => b && b.destroy());
  }
  if (o.signal?.aborted) throw new MatchError('AbortError', 'matching cancelled', done);
  return { results, stats };
}
