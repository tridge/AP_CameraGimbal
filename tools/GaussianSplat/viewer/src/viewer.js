// Static Gaussian splat viewer: renders on the visitor's GPU with Spark (three.js).
// A page sets window.SPLAT = {meta: 'meta.json', splat: 'splat.spz', colormaps: '../../../colormaps.json'}.
// Thermal splats hold the scaled temperature in every channel; it is blended over black into a
// half-float target, then decoded to degrees C and colormapped per pixel, as view_splat.py does.
import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { SparkRenderer, SplatMesh } from '@sparkjsdev/spark';

// render size caps (pixels): integrated GPUs struggle at full HiDPI resolution
const MAX_PIXELS = 2.0e6;
const MOVING_SCALE = 0.45; // linear scale while the view moves, refined when it settles

const cfg = window.SPLAT;
const $ = (id) => document.getElementById(id);

async function getJson(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${url}: ${r.status}`);
  return r.json();
}

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

async function main() {
  const meta = await getJson(cfg.meta);
  const thermal = meta.kind === 'thermal';
  document.title = meta.title;
  $('title').textContent = meta.title;
  $('description').textContent = meta.description || '';
  $('thermal-controls').hidden = !thermal;

  // thermal values must reach the target unconverted (Spark linearises colours for a linear target)
  if (thermal) THREE.ColorManagement.workingColorSpace = THREE.SRGBColorSpace;

  const canvas = $('canvas');
  const renderer = new THREE.WebGLRenderer({
    canvas,
    antialias: false,
    alpha: false,
    powerPreference: 'high-performance',
  });
  renderer.setClearColor(0x000000, thermal ? 0 : 1);

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

  const cam = meta.camera;
  const camera = new THREE.PerspectiveCamera(cam.fov_deg, 1, 0.5, 20000);
  camera.up.set(...meta.up);
  camera.position.set(...cam.position);
  const controls = new OrbitControls(camera, canvas);
  controls.target.set(...cam.target);
  controls.enableDamping = true;
  controls.update();
  const home = {
    position: camera.position.clone(),
    target: controls.target.clone(),
    fov: cam.fov_deg,
  };

  setStatus('Loading splat…');
  const mesh = new SplatMesh({
    url: cfg.splat,
    extSplats: thermal, // full-precision colour for the temperature
    onProgress: (e) => {
      if (e.lengthComputable)
        setStatus(`Loading splat… ${Math.round((100 * e.loaded) / e.total)}%`);
    },
  });
  scene.add(mesh);
  await mesh.initialized;
  setStatus('');

  let target = null;
  let full = null;
  let moving = null;
  let post = null;
  let lut = null;
  const ui = Object.fromEntries(
    ['cmap', 'lo', 'hi', 'auto', 'stats', 'hover'].map((id) => [id, $(id)]),
  );
  const scale = meta.temperature_scale;
  const norm = thermal ? Math.log1p((scale.t1_c - scale.t0_c) / scale.k_c) : 1;
  const toC = (v) => scale.t0_c + scale.k_c * Math.expm1(v * norm);

  if (thermal) {
    const colormaps = await getJson(cfg.colormaps);
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
    const setLut = (name) => {
      const rows = colormaps[name];
      for (let i = 0; i < 256; i++) {
        for (let c = 0; c < 3; c++) lut.image.data[4 * i + c] = Math.round(255 * rows[i][c]);
        lut.image.data[4 * i + 3] = 255;
      }
      lut.needsUpdate = true;
      needPost = true;
    };
    post = new THREE.Mesh(
      new THREE.PlaneGeometry(2, 2),
      new THREE.RawShaderMaterial({
        glslVersion: THREE.GLSL3,
        uniforms: {
          src: { value: target.texture },
          lut: { value: lut },
          t0: { value: scale.t0_c },
          k: { value: scale.k_c },
          norm: { value: norm },
          lo: { value: meta.display_range_c[0] },
          hi: { value: meta.display_range_c[1] },
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
    const postScene = new THREE.Scene();
    postScene.add(post);
    const postCamera = new THREE.Camera();
    post.userData = { scene: postScene, camera: postCamera };

    for (const name of Object.keys(colormaps))
      ui.cmap.add(new Option(name, name, false, name === meta.colormap));
    setLut(ui.cmap.value);
    ui.cmap.onchange = () => setLut(ui.cmap.value);
    ui.lo.value = meta.display_range_c[0];
    ui.hi.value = meta.display_range_c[1];
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

  // temperatures of the current frame, as view_splat.py: auto range is the 1%..99.9% of covered pixels
  function readFrame() {
    const w = full.width;
    const h = full.height;
    const buf = new Uint16Array(w * h * 4);
    renderer.readRenderTargetPixels(full, 0, 0, w, h, buf);
    return { w, h, buf };
  }

  function autoRange() {
    const { buf } = readFrame();
    const temps = [];
    const step = Math.max(1, Math.floor(buf.length / 4 / 100000));
    let peak = -Infinity;
    for (let i = 0; i < buf.length; i += 4 * step) {
      if (halfToFloat(buf[i + 3]) <= 0.5) continue;
      const v = Math.min(
        Math.max((halfToFloat(buf[i]) + halfToFloat(buf[i + 1]) + halfToFloat(buf[i + 2])) / 3, 0),
        1,
      );
      const t = toC(v);
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

  if (thermal) {
    canvas.addEventListener('pointermove', (e) => {
      const r = canvas.getBoundingClientRect();
      const x = Math.floor(((e.clientX - r.left) / r.width) * target.width);
      const y = Math.floor((1 - (e.clientY - r.top) / r.height) * target.height);
      if (x < 0 || y < 0 || x >= target.width || y >= target.height) return;
      const px = new Uint16Array(4);
      renderer.readRenderTargetPixels(target, x, y, 1, 1, px);
      const a = halfToFloat(px[3]);
      const v = Math.min(
        Math.max((halfToFloat(px[0]) + halfToFloat(px[1]) + halfToFloat(px[2])) / 3, 0),
        1,
      );
      ui.hover.textContent = a > 0.5 ? `${toC(v).toFixed(1)} °C under pointer` : '';
    });
  }

  function resize() {
    const w = canvas.clientWidth;
    const h = canvas.clientHeight;
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, Math.sqrt(MAX_PIXELS / (w * h))));
    renderer.setSize(w, h, false);
    camera.aspect = w / h;
    camera.updateProjectionMatrix();
    if (thermal) {
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

  $('reset').onclick = () => {
    camera.position.copy(home.position);
    controls.target.copy(home.target);
    camera.fov = home.fov;
    camera.updateProjectionMatrix();
    controls.update();
  };

  let lastMove = -Infinity;
  controls.addEventListener('change', () => {
    lastMove = performance.now();
    needSplat = true;
  });

  renderer.setAnimationLoop(() => {
    controls.update(); // damping keeps emitting change events after release
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
}

main().catch((e) => {
  console.error(e);
  setStatus(`Could not load the splat: ${e.message}`);
});
