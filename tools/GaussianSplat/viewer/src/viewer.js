// Static Gaussian splat viewer: renders on the visitor's GPU with Spark (three.js).
//
// Site pages (export_web.py) set window.SPLAT = {meta, splat, colormaps}. Without it the page is
// the open-a-file viewer: a local file (never uploaded), or ?url=FILE[&meta=META.json].
// Thermal splats hold the scaled temperature in every channel; it is blended over black into a
// half-float target, then decoded to degrees C and colormapped per pixel, as view_splat.py does.
import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { SparkRenderer, SplatMesh } from '@sparkjsdev/spark';

// render size caps (pixels): integrated GPUs struggle at full HiDPI resolution
const MAX_PIXELS = 2.0e6;
const MOVING_SCALE = 0.45; // linear scale while the view moves, refined when it settles
const FORMATS = ['ply', 'spz', 'splat', 'ksplat', 'sog'];
const UP_AXES = { z: [0, 0, 1], y: [0, 1, 0], '-y': [0, -1, 0] };

const cfg = window.SPLAT;
const $ = (id) => document.getElementById(id);

function setStatus(text) {
  $('status').textContent = text;
  $('status').hidden = !text;
}

function halfToFloat(h) {
  const s = h & 0x8000 ? -1 : 1;
  const e = (h >> 10) & 0x1f;
  const f = h & 0x3ff;
  if (e === 0) return s * 2 ** -14 * (f / 1024);
  if (e === 31) return f ? NaN : s * Infinity;
  return s * 2 ** (e - 15) * (1 + f / 1024);
}

// fetch with a progress display
async function fetchBytes(url, label) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
  const total = Number(r.headers.get('content-length')) || 0;
  const reader = r.body.getReader();
  const chunks = [];
  let got = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    got += value.length;
    setStatus(
      total
        ? `${label} ${Math.round((100 * got) / total)}%`
        : `${label} ${(got / 1e6).toFixed(1)} MB`,
    );
  }
  const out = new Uint8Array(got);
  let at = 0;
  for (const c of chunks) {
    out.set(c, at);
    at += c.length;
  }
  return out;
}

async function getJson(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
  return r.json();
}

// metadata written by train.py, train_rgb.py and export_web.py: "comment apcg_splat {json}"
function plyMeta(bytes) {
  const head = new TextDecoder('latin1').decode(bytes.subarray(0, Math.min(bytes.length, 1 << 20)));
  const end = head.indexOf('end_header');
  if (!head.startsWith('ply') || end < 0) return null;
  for (const line of head.slice(0, end).split('\n')) {
    if (line.startsWith('comment apcg_splat ')) {
      try {
        return JSON.parse(line.slice('comment apcg_splat '.length));
      } catch {
        return null;
      }
    }
  }
  return null;
}

function extension(name) {
  return (String(name).split('?')[0].split('.').pop() || '').toLowerCase();
}

async function main() {
  const canvas = $('canvas');
  const renderer = new THREE.WebGLRenderer({
    canvas,
    antialias: false,
    alpha: false,
    powerPreference: 'high-performance',
  });

  // draw only when something changed: an idle page leaves the GPU alone
  let needSplat = true;
  let needPost = true;
  const scene = new THREE.Scene();
  const spark = new SparkRenderer({
    renderer,
    premultipliedAlpha: true,
    onDirty: () => {
      needSplat = true;
    },
  });
  scene.add(spark);
  const holder = new THREE.Group(); // turns the splat's up axis to +z
  scene.add(holder);

  const camera = new THREE.PerspectiveCamera(50, 1, 0.5, 20000);
  camera.up.set(0, 0, 1);
  const controls = new OrbitControls(camera, canvas);
  controls.enableDamping = true;

  const ui = Object.fromEntries(
    ['cmap', 'lo', 'hi', 'auto', 'stats', 'hover', 'up'].map((id) => [id, $(id)]),
  );
  let colormaps = null;
  let lut = null;
  let post = null;
  let full = null;
  let moving = null;
  let target = null;
  let mesh = null;
  let meta = null;
  let thermal = false;
  let toC = (v) => v;
  let home = null;

  function setLut(name) {
    const rows = colormaps[name];
    for (let i = 0; i < 256; i++) {
      for (let c = 0; c < 3; c++) lut.image.data[4 * i + c] = Math.round(255 * rows[i][c]);
      lut.image.data[4 * i + 3] = 255;
    }
    lut.needsUpdate = true;
    needPost = true;
  }

  function applyRange() {
    const lo = parseFloat(ui.lo.value);
    const hi = parseFloat(ui.hi.value);
    if (Number.isFinite(lo) && Number.isFinite(hi)) {
      post.material.uniforms.lo.value = lo;
      post.material.uniforms.hi.value = Math.max(hi, lo + 0.1);
      needPost = true;
    }
  }

  function pixelValue(buf, i) {
    const v = (halfToFloat(buf[i]) + halfToFloat(buf[i + 1]) + halfToFloat(buf[i + 2])) / 3;
    return Math.min(Math.max(v, 0), 1);
  }

  // as view_splat.py: auto range is the 1%..99.9% of covered pixels
  function autoRange() {
    const w = full.width;
    const h = full.height;
    const buf = new Uint16Array(w * h * 4);
    renderer.readRenderTargetPixels(full, 0, 0, w, h, buf);
    const temps = [];
    const step = Math.max(1, Math.floor(buf.length / 4 / 100000));
    let peak = -Infinity;
    for (let i = 0; i < buf.length; i += 4 * step) {
      if (halfToFloat(buf[i + 3]) <= 0.5) continue;
      const t = toC(pixelValue(buf, i));
      temps.push(t);
      if (t > peak) peak = t;
    }
    if (!temps.length) return;
    temps.sort((a, b) => a - b);
    const lo = temps[Math.floor(0.01 * (temps.length - 1))];
    const p999 = temps[Math.floor(0.999 * (temps.length - 1))];
    ui.lo.value = lo.toFixed(1);
    ui.hi.value = Math.max(p999, lo + 1).toFixed(1);
    applyRange();
    ui.stats.textContent = `peak ${peak.toFixed(1)} °C in view`;
  }

  function setupThermal() {
    if (post) return;
    const makeTarget = () => {
      const t = new THREE.WebGLRenderTarget(1, 1, { type: THREE.HalfFloatType, depthBuffer: true });
      t.texture.colorSpace = THREE.SRGBColorSpace;
      return t;
    };
    full = makeTarget();
    moving = makeTarget();
    target = full;
    lut = new THREE.DataTexture(new Uint8Array(256 * 4), 256, 1, THREE.RGBAFormat);
    lut.magFilter = lut.minFilter = THREE.LinearFilter;
    post = new THREE.Mesh(
      new THREE.PlaneGeometry(2, 2),
      new THREE.RawShaderMaterial({
        glslVersion: THREE.GLSL3,
        uniforms: {
          src: { value: full.texture },
          lut: { value: lut },
          t0: { value: 0 },
          k: { value: 1 },
          norm: { value: 1 },
          lo: { value: 0 },
          hi: { value: 1 },
        },
        vertexShader: `in vec3 position; out vec2 uv;
          void main() { uv = position.xy * 0.5 + 0.5; gl_Position = vec4(position.xy, 0.0, 1.0); }`,
        fragmentShader: `precision highp float; uniform sampler2D src; uniform sampler2D lut;
          uniform float t0, k, norm, lo, hi; in vec2 uv; out vec4 colour;
          void main() {
            vec4 s = texture(src, uv);
            float v = clamp((s.r + s.g + s.b) / 3.0, 0.0, 1.0);
            float t = t0 + k * (exp(v * norm) - 1.0);
            float x = clamp((t - lo) / max(hi - lo, 0.1), 0.0, 1.0);
            colour = vec4(texture(lut, vec2((x * 255.0 + 0.5) / 256.0, 0.5)).rgb * clamp(s.a, 0.0, 1.0), 1.0);
          }`,
        depthTest: false,
        depthWrite: false,
      }),
    );
    post.frustumCulled = false;
    post.userData = { scene: new THREE.Scene(), camera: new THREE.Camera() };
    post.userData.scene.add(post);

    for (const name of Object.keys(colormaps)) ui.cmap.add(new Option(name, name));
    ui.cmap.onchange = () => setLut(ui.cmap.value);
    const manual = () => {
      ui.auto.checked = false;
      applyRange();
    };
    ui.lo.oninput = manual;
    ui.hi.oninput = manual;
    ui.auto.onchange = () => {
      if (ui.auto.checked) autoRange();
      else applyRange();
    };
    canvas.addEventListener('pointermove', (e) => {
      if (!thermal) return;
      const r = canvas.getBoundingClientRect();
      const x = Math.floor(((e.clientX - r.left) / r.width) * target.width);
      const y = Math.floor((1 - (e.clientY - r.top) / r.height) * target.height);
      if (x < 0 || y < 0 || x >= target.width || y >= target.height) return;
      const px = new Uint16Array(4);
      renderer.readRenderTargetPixels(target, x, y, 1, 1, px);
      ui.hover.textContent =
        halfToFloat(px[3]) > 0.5 ? `${toC(pixelValue(px, 0)).toFixed(1)} °C under pointer` : '';
    });
  }

  // a view of the whole splat from the south, 50 degrees up, robust to outlying Gaussians
  function fitView() {
    const xs = [[], [], []];
    const step = Math.max(1, Math.floor(mesh.packedSplats.numSplats / 50000));
    const v = new THREE.Vector3();
    mesh.forEachSplat((i, centre) => {
      if (i % step) return;
      v.copy(centre).applyQuaternion(holder.quaternion);
      xs[0].push(v.x);
      xs[1].push(v.y);
      xs[2].push(v.z);
    });
    const q = (a, f) => a[Math.floor(f * (a.length - 1))];
    for (const a of xs) a.sort((p, r) => p - r);
    const size = Math.max(...xs.map((a) => q(a, 0.98) - q(a, 0.02)), 1e-3);
    const look = meta.target
      ? new THREE.Vector3(...meta.target).applyQuaternion(holder.quaternion)
      : new THREE.Vector3(...xs.map((a) => q(a, 0.5)));
    const fov = 50;
    const dist = (0.6 * size) / Math.tan(THREE.MathUtils.degToRad(fov / 2));
    const el = THREE.MathUtils.degToRad(50);
    const pos = look
      .clone()
      .add(new THREE.Vector3(0, -Math.cos(el), Math.sin(el)).multiplyScalar(dist));
    return { position: pos, target: look, fov };
  }

  function goHome() {
    camera.position.copy(home.position);
    controls.target.copy(home.target);
    camera.fov = home.fov;
    const dist = home.position.distanceTo(home.target);
    camera.near = Math.max(0.01, Math.min(0.5, dist / 1000));
    camera.far = Math.max(20000, dist * 100);
    camera.updateProjectionMatrix();
    controls.update();
  }

  function setUp(axis) {
    holder.quaternion.setFromUnitVectors(
      new THREE.Vector3(...axis).normalize(),
      new THREE.Vector3(0, 0, 1),
    );
    holder.updateMatrixWorld();
    needSplat = true;
  }

  async function show(bytes, name, m) {
    meta = m || { kind: 'rgb', title: name };
    thermal = meta.kind === 'thermal' && !!meta.temperature_scale;
    const title = meta.title || name.split('/').pop();
    document.title = title;
    $('title').textContent = title;
    $('description').textContent = meta.description || '';
    $('thermal-controls').hidden = !thermal;
    // thermal values must reach the target unconverted: Spark linearises colours for a linear target
    THREE.ColorManagement.workingColorSpace = thermal
      ? THREE.SRGBColorSpace
      : THREE.LinearSRGBColorSpace;
    renderer.setClearColor(0x000000, thermal ? 0 : 1);

    if (mesh) {
      holder.remove(mesh);
      mesh.dispose();
      mesh = null;
    }
    setStatus('Preparing splat…');
    const next = new SplatMesh({ fileBytes: bytes, fileName: name, extSplats: thermal });
    await next.initialized;
    mesh = next;
    holder.add(mesh);
    setStatus('');

    if (ui.up) ui.up.closest('label').hidden = !!meta.up;
    setUp(meta.up || UP_AXES[ui.up ? ui.up.value : 'z']);

    if (thermal) {
      if (!colormaps) colormaps = await getJson(cfg ? cfg.colormaps : 'colormaps.json');
      setupThermal();
      const s = meta.temperature_scale;
      const norm = Math.log1p((s.t1_c - s.t0_c) / s.k_c);
      toC = (v) => s.t0_c + s.k_c * Math.expm1(v * norm);
      post.material.uniforms.t0.value = s.t0_c;
      post.material.uniforms.k.value = s.k_c;
      post.material.uniforms.norm.value = norm;
      const names = [...ui.cmap.options].map((o) => o.value);
      ui.cmap.value = names.includes(meta.colormap) ? meta.colormap : 'ironbow';
      setLut(ui.cmap.value);
      const range = meta.display_range_c || [Math.floor(s.t0_c), Math.ceil(s.t1_c)];
      ui.lo.value = range[0];
      ui.hi.value = range[1];
      ui.auto.checked = true;
      applyRange();
    }

    home = meta.camera
      ? {
          position: new THREE.Vector3(...meta.camera.position).applyQuaternion(holder.quaternion),
          target: new THREE.Vector3(...meta.camera.target).applyQuaternion(holder.quaternion),
          fov: meta.camera.fov_deg,
        }
      : fitView();
    goHome();
    resize();
  }

  function resize() {
    const w = canvas.clientWidth;
    const h = canvas.clientHeight;
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, Math.sqrt(MAX_PIXELS / (w * h))));
    renderer.setSize(w, h, false);
    camera.aspect = w / h;
    camera.updateProjectionMatrix();
    if (full) {
      const size = renderer.getDrawingBufferSize(new THREE.Vector2());
      full.setSize(size.x, size.y);
      moving.setSize(
        Math.max(1, Math.round(size.x * MOVING_SCALE)),
        Math.max(1, Math.round(size.y * MOVING_SCALE)),
      );
    }
    needSplat = true;
  }
  window.addEventListener('resize', resize);
  resize();

  $('reset').onclick = () => home && goHome();
  if (ui.up)
    ui.up.onchange = () => {
      if (!mesh || meta.up) return;
      setUp(UP_AXES[ui.up.value]);
      home = fitView();
      goHome();
    };

  let lastMove = -Infinity;
  controls.addEventListener('change', () => {
    lastMove = performance.now();
    needSplat = true;
  });

  renderer.setAnimationLoop(() => {
    controls.update(); // damping keeps emitting change events after release
    if (!mesh) return;
    const isMoving = performance.now() - lastMove < 200;
    if (thermal && (isMoving ? moving : full) !== target) {
      target = isMoving ? moving : full;
      needSplat = true; // refine at full size once the view settles
    }
    if (!needSplat && !needPost) return;
    if (!thermal) {
      renderer.render(scene, camera);
    } else {
      if (needSplat) {
        renderer.setRenderTarget(target);
        renderer.clear();
        renderer.render(scene, camera);
        renderer.setRenderTarget(null);
        if (target === full && ui.auto.checked) autoRange();
      }
      post.material.uniforms.src.value = target.texture;
      renderer.render(post.userData.scene, post.userData.camera);
    }
    needSplat = needPost = false;
  });

  async function openBytes(bytes, name, m) {
    if ($('open-panel')) $('open-panel').hidden = true;
    $('panel').hidden = false;
    await show(bytes, name, m || (extension(name) === 'ply' ? plyMeta(bytes) : null));
  }

  if (cfg) {
    // a site page from export_web.py
    const m = await getJson(cfg.meta);
    await openBytes(await fetchBytes(cfg.splat, 'Loading splat…'), cfg.splat, m);
    return;
  }

  // open-a-file viewer: files are read in the browser, nothing is uploaded
  const openFile = async (file) => {
    if (!FORMATS.includes(extension(file.name))) {
      setStatus(`${file.name}: not a splat file (${FORMATS.join(', ')})`);
      return;
    }
    setStatus(`Reading ${file.name}…`);
    await openBytes(new Uint8Array(await file.arrayBuffer()), file.name);
  };
  for (const input of document.querySelectorAll('input[type=file]')) {
    input.onchange = (e) => e.target.files[0] && openFile(e.target.files[0]).catch(fail);
  }
  window.addEventListener('dragover', (e) => e.preventDefault());
  window.addEventListener('drop', (e) => {
    e.preventDefault();
    const f = e.dataTransfer.files[0];
    if (f) openFile(f).catch(fail);
  });
  const params = new URLSearchParams(location.search);
  if (params.get('url')) {
    const u = new URL(params.get('url'), location.href);
    if (!['https:', 'http:'].includes(u.protocol)) throw new Error('url must be http or https');
    const m = params.get('meta') ? await getJson(new URL(params.get('meta'), location.href)) : null;
    await openBytes(await fetchBytes(u, 'Downloading splat…'), u.pathname, m);
  } else {
    $('open-panel').hidden = false;
  }
}

function fail(e) {
  console.error(e);
  setStatus(`Could not load the splat: ${e.message}`);
}

main().catch(fail);
