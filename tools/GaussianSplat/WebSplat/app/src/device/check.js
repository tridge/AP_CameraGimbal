// WebGPU availability, adapter info and limits, and a small real compute + texture smoke test.
// powerPreference: 'high-performance' (default), 'low-power' (e.g. the Intel GPU of a laptop).
const LINUX_HINT =
  'Chrome on Linux: start Chrome with --enable-unsafe-webgpu --use-angle=gl --enable-features=Vulkan ' +
  '(or enable chrome://flags/#enable-unsafe-webgpu and #enable-vulkan), then reload.';

const isLinux = () => /Linux/.test(navigator.userAgent) && !/Android/.test(navigator.userAgent);

export const LIMIT_KEYS = [
  'maxBufferSize',
  'maxStorageBufferBindingSize',
  'maxComputeWorkgroupStorageSize',
  'maxComputeInvocationsPerWorkgroup',
  'maxComputeWorkgroupsPerDimension',
  'maxStorageBuffersPerShaderStage',
  'maxTextureDimension2D',
];

// GPU class used by the stage time priors
export function gpuClass(info) {
  const d = `${info?.description ?? ''} ${info?.device ?? ''}`;
  if (/4070|4080|4090|50[789]0/i.test(d)) return 'desktop';
  if (/RTX PRO 1000|RTX A1000|laptop/i.test(d)) return 'laptop';
  if (info?.vendor === 'intel') return 'integrated';
  // Chrome on Linux leaves description empty; the two release-1 machines differ by architecture
  if (info?.vendor === 'nvidia' && /^(ada|lovelace)/i.test(info.architecture ?? ''))
    return 'desktop';
  if (info?.vendor === 'nvidia' && /^blackwell/i.test(info.architecture ?? '')) return 'laptop';
  return 'unknown';
}

export async function checkDevice({ powerPreference = 'high-performance', smoke = true } = {}) {
  const out = { ok: false, powerPreference, userAgent: navigator.userAgent };
  if (!navigator.gpu) {
    out.reason = 'This browser does not offer WebGPU.';
    out.hint = isLinux() ? LINUX_HINT : 'Use a current Chrome or Edge.';
    return out;
  }
  let adapter;
  try {
    adapter = await navigator.gpu.requestAdapter({ powerPreference });
  } catch (e) {
    out.reason = `requestAdapter failed: ${e.message}`;
  }
  if (!adapter) {
    out.reason ??= 'WebGPU is present but no GPU adapter is available.';
    out.hint = isLinux()
      ? LINUX_HINT
      : 'Check that hardware acceleration is enabled in the browser settings.';
    return out;
  }
  const i = adapter.info ?? {};
  out.adapter = {
    vendor: i.vendor,
    architecture: i.architecture,
    device: i.device,
    description: i.description,
    isFallbackAdapter: !!i.isFallbackAdapter,
  };
  out.gpuClass = gpuClass(i);
  out.features = [...adapter.features].sort();
  out.limits = Object.fromEntries(LIMIT_KEYS.map((k) => [k, adapter.limits[k]]));
  if (i.isFallbackAdapter) {
    out.reason = 'Only a software (fallback) WebGPU adapter is available; it is far too slow.';
    out.hint = isLinux() ? LINUX_HINT : 'Check that hardware acceleration is enabled.';
    return out;
  }
  // Brush's sort and scan kernels always use subgroup operations
  if (!adapter.features.has('subgroups')) {
    out.reason = "This browser's WebGPU lacks subgroups, which training needs.";
    out.hint = 'Use a current Chrome or Edge, or Firefox Nightly (release Firefox lacks them).';
    return out;
  }
  out.ok = true;
  if (smoke) {
    out.smoke = await smokeTest(adapter);
    if (!out.smoke.ok) {
      out.ok = false;
      out.reason = `This GPU adapter failed the WebGPU smoke test: ${out.smoke.errors[0]}`;
      out.hint =
        powerPreference === 'low-power'
          ? 'Try the high-performance adapter (?adapter=high-performance).'
          : 'Try the low-power adapter (?adapter=low-power), or another browser.';
    }
  }
  return out;
}

const COMPUTE = /* wgsl */ `
@group(0) @binding(0) var<storage, read_write> buf: array<u32>;
@group(0) @binding(1) var tex: texture_storage_2d<rgba32float, write>;
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) g: vec3u, @builtin(num_workgroups) nw: vec3u) {
  let i = g.x + g.y * nw.x * 64u;
  if (i < arrayLength(&buf)) { buf[i] = i * 3u + 1u; }
  let w = textureDimensions(tex).x;
  if (i < w * textureDimensions(tex).y) {
    textureStore(tex, vec2u(i % w, i / w), vec4f(f32(i % w), f32(i / w), 0.5, 1.0));
  }
}
`;

// WGSL zero-initialises a function-scope var at each declaration; Chrome 155 on Linux/Vulkan
// does not inside loops (found by stage 4): 12 instead of 6 here
const LOOP_ZERO = /* wgsl */ `
@group(0) @binding(0) var<storage, read_write> o: array<f32>;
@compute @workgroup_size(1) fn main() {
  var total = 0.0;
  for (var t = 0; t < 3; t++) {
    var acc: array<vec4f, 4>;
    acc[0] += vec4f(1.0);
    acc[0] += vec4f(1.0);
    total += acc[0].x;
  }
  o[0] = total;
}
`;

async function loopZeroInit(device) {
  const module = device.createShaderModule({ code: LOOP_ZERO });
  const pipeline = await device.createComputePipelineAsync({
    layout: 'auto',
    compute: { module, entryPoint: 'main' },
  });
  const buf = device.createBuffer({
    size: 16,
    usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC,
  });
  const read = device.createBuffer({
    size: 16,
    usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
  });
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
  pass.dispatchWorkgroups(1);
  pass.end();
  enc.copyBufferToBuffer(buf, 0, read, 0, 16);
  device.queue.submit([enc.finish()]);
  await read.mapAsync(GPUMapMode.READ);
  const v = new Float32Array(read.getMappedRange())[0];
  buf.destroy();
  read.destroy();
  return v;
}

// a real allocation, compute dispatch, storage texture write, render target clear and readback,
// with every error scope and device loss reported (some drivers fail only on texture creation)
export async function smokeTest(adapter) {
  const t0 = performance.now();
  const errors = [];
  const warnings = [];
  const want = (k, v) => Math.min(adapter.limits[k], v);
  let device;
  try {
    device = await adapter.requestDevice({
      requiredLimits: {
        maxBufferSize: want('maxBufferSize', 1 << 30),
        maxStorageBufferBindingSize: want('maxStorageBufferBindingSize', 1 << 30),
      },
    });
  } catch (e) {
    return { ok: false, ms: performance.now() - t0, errors: [`requestDevice: ${e.message}`] };
  }
  let lost = null;
  device.lost.then((l) => (lost = l));
  device.addEventListener('uncapturederror', (e) => errors.push(`uncaptured: ${e.error.message}`));
  const scopes = ['validation', 'out-of-memory', 'internal'];
  scopes.forEach((s) => device.pushErrorScope(s));
  const W = 1024;
  const H = 1024;
  const N = 4 << 20; // 16 MiB of u32
  const checks = {};
  try {
    const buf = device.createBuffer({
      size: N * 4,
      usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC,
    });
    const tex = device.createTexture({
      size: [W, H],
      format: 'rgba32float',
      usage:
        GPUTextureUsage.STORAGE_BINDING |
        GPUTextureUsage.COPY_SRC |
        GPUTextureUsage.TEXTURE_BINDING,
    });
    // the size of Brush's failing canvas on the laptop
    const rt = device.createTexture({
      size: [1600, 1000],
      format: 'rgba16float',
      usage:
        GPUTextureUsage.RENDER_ATTACHMENT |
        GPUTextureUsage.COPY_SRC |
        GPUTextureUsage.TEXTURE_BINDING,
    });
    const module = device.createShaderModule({ code: COMPUTE });
    const info = await module.getCompilationInfo();
    for (const m of info.messages) if (m.type === 'error') errors.push(`shader: ${m.message}`);
    const pipeline = await device.createComputePipelineAsync({
      layout: 'auto',
      compute: { module, entryPoint: 'main' },
    });
    const bind = device.createBindGroup({
      layout: pipeline.getBindGroupLayout(0),
      entries: [
        { binding: 0, resource: { buffer: buf } },
        { binding: 1, resource: tex.createView() },
      ],
    });
    const mapUsage = GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ;
    const readBuf = device.createBuffer({ size: 4096, usage: mapUsage });
    const readTex = device.createBuffer({ size: W * 16 * 2, usage: mapUsage });
    const readRt = device.createBuffer({ size: 256, usage: mapUsage });
    const enc = device.createCommandEncoder();
    const pass = enc.beginComputePass();
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, bind);
    pass.dispatchWorkgroups(Math.ceil(Math.max(N, W * H) / 64 / 16), 16);
    pass.end();
    const rp = enc.beginRenderPass({
      colorAttachments: [
        { view: rt.createView(), loadOp: 'clear', storeOp: 'store', clearValue: [0.25, 0.5, 1, 1] },
      ],
    });
    rp.end();
    enc.copyBufferToBuffer(buf, (N - 1024) * 4, readBuf, 0, 4096);
    enc.copyTextureToBuffer(
      { texture: tex, origin: [0, H - 2] },
      { buffer: readTex, bytesPerRow: W * 16 },
      [W, 2],
    );
    enc.copyTextureToBuffer(
      { texture: rt, origin: [1599, 999] },
      { buffer: readRt, bytesPerRow: 256 },
      [1, 1],
    );
    device.queue.submit([enc.finish()]);
    await Promise.all([readBuf, readTex, readRt].map((b) => b.mapAsync(GPUMapMode.READ)));
    const u = new Uint32Array(readBuf.getMappedRange());
    checks.buffer = u.every((v, k) => v === (N - 1024 + k) * 3 + 1);
    const f = new Float32Array(readTex.getMappedRange());
    const last = (W * 2 - 1) * 4;
    checks.texture = f[last] === W - 1 && f[last + 1] === H - 1 && f[last + 2] === 0.5;
    const h = new Uint16Array(readRt.getMappedRange(0, 8));
    checks.renderTarget = h[0] === 0x3400 && h[1] === 0x3800 && h[2] === 0x3c00; // 0.25, 0.5, 1 in f16
    for (const c of Object.keys(checks)) if (!checks[c]) errors.push(`${c} readback was wrong`);
    const z = await loopZeroInit(device);
    checks.loopZeroInit = z === 6;
    if (z !== 6)
      warnings.push(
        `loop variables are not zero-initialised (got ${z}, want 6): shaders must initialise them`,
      );
    [buf, tex, rt, readBuf, readTex, readRt].forEach((r) => r.destroy());
  } catch (e) {
    errors.push(`${e.name}: ${e.message}`);
  }
  for (const s of scopes.reverse()) {
    const e = await device.popErrorScope().catch((x) => x);
    if (e) errors.push(`${s}: ${e.message}`);
  }
  await device.queue.onSubmittedWorkDone().catch(() => {});
  if (lost) errors.push(`device lost: ${lost.reason} ${lost.message}`);
  device.destroy();
  return {
    ok: errors.length === 0,
    ms: Math.round(performance.now() - t0),
    checks,
    errors,
    warnings,
  };
}
