// Static Gaussian splat viewer: renders on the visitor's GPU with Spark (three.js).
//
// Site pages (export_web.py) set window.SPLAT = {meta, splat, colormaps}. Without it the page is
// the open-a-file viewer: a local file (never uploaded), or ?url=FILE[&meta=META.json], or (in a
// same-origin iframe, ?embed=1) bytes posted by the parent page.
// meta.rasterize_mode 'classic' selects gsplat classic rendering (no mip opacity compensation).
// Thermal splats hold the scaled temperature in every channel; it is blended over black into a
// float (or half-float) target, then decoded to degrees C and colormapped per pixel, as view_splat.py does.
import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { SparkRenderer, SplatMesh } from '@sparkjsdev/spark';
import projectIconUrl from '../../../../assets/camera-gimbal.svg?url';

// render size caps (pixels): integrated GPUs struggle at full HiDPI resolution
const MAX_PIXELS = 2.0e6;
const MOVING_SCALE = 0.45; // linear scale while the view moves, refined when it settles
const FORMATS = ['ply', 'spz', 'splat', 'ksplat', 'sog'];
const UP_AXES = { z: [0, 0, 1], y: [0, 1, 0], '-y': [0, -1, 0] };

const cfg = window.SPLAT;
const $ = (id) => document.getElementById(id);

for (const img of document.querySelectorAll('img.project-icon')) img.src = projectIconUrl;
const favicon = document.createElement('link');
favicon.rel = 'icon';
favicon.type = 'image/svg+xml';
favicon.href = projectIconUrl;
document.head.append(favicon);

function megabytes(bytes) {
  return `${(bytes / 1e6).toFixed(bytes < 1e7 ? 1 : 0)} MB`;
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
  const end = head.search(/\nend_header\r?\n/); // the line, not the text inside a comment
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
  // Thermal splats render as gsplat does: Spark's extended (float) accumulator (the packed one
  // clips evaluated values before blending, which takes hot sources far down), gsplat's alpha
  // threshold (1/255), depth order and (nearly) uncapped footprints. Other splats keep Spark's defaults. Changing kind makes a
  // new renderer, inserted before the splats.
  let spark = null;
  let sparkExt = null;
  function useSpark(ext) {
    if (spark && sparkExt === ext) return;
    if (spark) {
      scene.remove(spark);
      spark.dispose();
    }
    spark = new SparkRenderer({
      renderer,
      premultipliedAlpha: true,
      // footprints up to 4096 px (Spark caps them at 512, which shrinks broad near Gaussians)
      ...(ext
        ? { accumExtSplats: true, minAlpha: 1 / 255, sortRadial: false, maxPixelRadius: 4096 }
        : {}),
      onDirty: () => {
        needSplat = true;
      },
    });
    sparkExt = ext;
    scene.add(spark);
    if (holder) {
      scene.remove(holder);
      scene.add(holder);
    }
  }
  let holder = null;
  useSpark(false);
  holder = new THREE.Group(); // turns the splat's up axis to +z
  scene.add(holder);

  const camera = new THREE.PerspectiveCamera(50, 1, 0.5, 20000);
  camera.up.set(0, 0, 1);
  const controls = new OrbitControls(camera, canvas);
  controls.enableDamping = true;

  const ui = Object.fromEntries(
    ['cmap', 'lo', 'hi', 'auto', 'stats', 'rangeinfo', 'hover', 'up'].map((id) => [id, $(id)]),
  );
  if (!ui.rangeinfo && ui.stats) {
    // pages exported before the auto range had a knee
    ui.rangeinfo = document.createElement('p');
    ui.rangeinfo.className = 'note';
    ui.stats.before(ui.rangeinfo);
  }
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
  let sceneLo = null; // the last auto range's low: GloryHot's grey floor in a manual range
  const greyBelow = (name) => name === 'gloryhot';

  function setLut(name) {
    const rows = colormaps[name];
    for (let i = 0; i < 256; i++) {
      for (let c = 0; c < 3; c++) lut.image.data[4 * i + c] = Math.round(255 * rows[i][c]);
      lut.image.data[4 * i + 3] = 255;
    }
    lut.needsUpdate = true;
    post.material.uniforms.glory.value = greyBelow(name) ? 1 : 0;
    applyRange(ui.auto.checked ? autoBg : null);
  }

  // bg > lo: the auto range's knee (lo..bg linear over the bottom 70% of the palette, bg..hi
  // log-compressed into the top 30%, as drape_thermal.py); otherwise linear lo..hi
  // GloryHot (grey below the colour range): grey lo..bg and colours bg..hi with the knee, else
  // grey below lo and colours lo..hi
  let autoBg = null;
  function applyRange(bg = null) {
    const lo = parseFloat(ui.lo.value);
    const hi = parseFloat(ui.hi.value);
    if (Number.isFinite(lo) && Number.isFinite(hi)) {
      const u = post.material.uniforms;
      u.lo.value = lo;
      u.hi.value = Math.max(hi, lo + 0.1);
      u.bg.value = bg !== null && bg > lo && bg < u.hi.value ? bg : 0;
      u.knee.value = u.bg.value ? 1 : 0;
      u.gmin.value = sceneLo !== null && sceneLo < lo ? sceneLo : lo - 10;
      const h = u.hi.value.toFixed(1);
      if (u.glory.value && u.knee.value) {
        ui.rangeinfo.textContent = `Grey ${lo.toFixed(1)} to ${bg.toFixed(1)} °C, colours hotter up to ${h} °C.`;
      } else if (u.glory.value) {
        ui.rangeinfo.textContent = `Grey below ${lo.toFixed(1)} °C, colours up to ${h} °C.`;
      } else if (u.knee.value) {
        ui.rangeinfo.textContent =
          `Colours: ${lo.toFixed(1)} to ${bg.toFixed(1)} °C over most of the scale, ` +
          `hotter up to ${h} °C compressed into the top.`;
      } else {
        ui.rangeinfo.textContent = '';
      }
      needPost = true;
    }
  }

  // the thermal target: float32 where it can be blended (blending thousands of Gaussians in
  // half floats drifts by degrees at hot points), else half float
  let floatTarget = null;
  let halfLut = null;
  // RGBA of a target region as floats
  function readTarget(t, x, y, w, h) {
    if (floatTarget) {
      const f = new Float32Array(w * h * 4);
      renderer.readRenderTargetPixels(t, x, y, w, h, f);
      return f;
    }
    if (!halfLut) {
      halfLut = new Float32Array(65536);
      for (let i = 0; i < 65536; i++) halfLut[i] = halfToFloat(i);
    }
    const b = new Uint16Array(w * h * 4);
    renderer.readRenderTargetPixels(t, x, y, w, h, b);
    const f = new Float32Array(b.length);
    for (let i = 0; i < b.length; i++) f[i] = halfLut[b[i]];
    return f;
  }

  function pixelValue(buf, i) {
    return Math.min(Math.max((buf[i] + buf[i + 1] + buf[i + 2]) / 3, 0), 1);
  }

  // auto range: low is the 1% of covered pixels (sampled), the knee their 99.5% and high the
  // hottest covered pixel (all of them: a small hot source can fall between samples), so the
  // heat sources get the top of the palette instead of saturating it
  function autoRange() {
    const buf = readTarget(full, 0, 0, full.width, full.height);
    const temps = [];
    const step = Math.max(1, Math.floor(buf.length / 4 / 100000));
    let vmax = -Infinity;
    for (let i = 0, k = 0; i < buf.length; i += 4, k++) {
      if (buf[i + 3] <= 0.5) continue;
      const v = (buf[i] + buf[i + 1] + buf[i + 2]) / 3;
      if (v > vmax) vmax = v;
      if (k % step === 0) temps.push(toC(Math.min(Math.max(v, 0), 1)));
    }
    if (vmax === -Infinity || !temps.length) return; // nothing covered
    const peak = toC(Math.min(Math.max(vmax, 0), 1));
    ui.stats.textContent = `peak ${floatTarget ? '' : '≈ '}${peak.toFixed(1)} °C in view`;
    temps.sort((a, b) => a - b);
    const lo = temps[Math.floor(0.01 * (temps.length - 1))];
    const bg = Math.max(temps[Math.floor(0.995 * (temps.length - 1))], lo + 1);
    ui.lo.value = lo.toFixed(1);
    ui.hi.value = Math.max(peak, bg + 1).toFixed(1);
    sceneLo = lo;
    autoBg = bg;
    applyRange(bg);
  }

  function setupThermal() {
    if (post) return;
    const makeTarget = () => {
      floatTarget ??=
        renderer.extensions.has('EXT_color_buffer_float') &&
        renderer.extensions.has('EXT_float_blend');
      const type = floatTarget ? THREE.FloatType : THREE.HalfFloatType;
      const t = new THREE.WebGLRenderTarget(1, 1, { type, depthBuffer: true });
      t.texture.colorSpace = THREE.SRGBColorSpace;
      // float32 textures filter linearly only with OES_texture_float_linear (else they sample
      // black); the full-size target maps pixel to pixel, so nearest is the same there
      if (floatTarget && !renderer.extensions.has('OES_texture_float_linear')) {
        t.texture.minFilter = t.texture.magFilter = THREE.NearestFilter;
      }
      return t;
    };
    full = makeTarget();
    moving = makeTarget();
    target = full;
    if (!floatTarget) {
      // degraded mode: half floats blending many Gaussians drift by degrees at hot points
      const p = document.createElement('p');
      p.id = 'precision';
      p.className = 'warn';
      p.textContent =
        'Reduced temperature precision on this GPU (no float32 blending): temperatures are ' +
        'approximate and can read several °C off at hot points.';
      $('thermal-controls').prepend(p);
    }
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
          bg: { value: 0 },
          knee: { value: 0 },
          glory: { value: 0 },
          gmin: { value: 0 },
        },
        vertexShader: `in vec3 position; out vec2 uv;
          void main() { uv = position.xy * 0.5 + 0.5; gl_Position = vec4(position.xy, 0.0, 1.0); }`,
        fragmentShader: `precision highp float; uniform sampler2D src; uniform sampler2D lut;
          uniform float t0, k, norm, lo, hi, bg, knee, glory, gmin; in vec2 uv; out vec4 colour;
          void main() {
            vec4 s = texture(src, uv);
            float v = clamp((s.r + s.g + s.b) / 3.0, 0.0, 1.0);
            float t = t0 + k * (exp(v * norm) - 1.0);
            float x = knee > 0.5
              ? (t <= bg ? 0.7 * (t - lo) / max(bg - lo, 0.1)
                         : 0.7 + 0.3 * log(1.0 + (t - bg) / 8.0) / log(1.0 + max(hi - bg, 0.1) / 8.0))
              : (t - lo) / max(hi - lo, 0.1);
            bool grey = false;
            float g = 0.0;
            if (glory > 0.5) {
              if (knee > 0.5) {
                grey = t <= bg;
                g = (t - lo) / max(bg - lo, 0.1);
                x = (x - 0.7) / 0.3;
              } else {
                grey = t <= lo;
                g = (t - gmin) / max(lo - gmin, 0.1);
              }
            }
            x = clamp(x, 0.0, 1.0);
            vec3 c = grey ? vec3(clamp(g, 0.0, 1.0) * 200.0 / 255.0)
                          : texture(lut, vec2((x * 255.0 + 0.5) / 256.0, 0.5)).rgb;
            colour = vec4(c * clamp(s.a, 0.0, 1.0), 1.0);
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
      const px = readTarget(target, x, y, 1, 1);
      ui.hover.textContent =
        px[3] > 0.5
          ? `${floatTarget ? '' : '≈ '}${toC(pixelValue(px, 0)).toFixed(1)} °C under pointer`
          : '';
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
    if ($('download')) {
      const ply = meta.downloads?.ply;
      $('download').hidden = !ply;
      if (ply) $('download').textContent = `Download PLY (${megabytes(ply)})`;
    }
    if ($('video')) {
      const v = meta.video;
      $('video').hidden = !v;
      if (v) {
        $('video').href = `../../../videos/${encodeURIComponent(v.name)}`;
        $('video').textContent = `Download source video (${megabytes(v.bytes)})`;
      }
    }
    $('thermal-controls').hidden = !thermal;
    // thermal values must reach the target unconverted: Spark linearises colours for a linear target
    THREE.ColorManagement.workingColorSpace = thermal
      ? THREE.SRGBColorSpace
      : THREE.LinearSRGBColorSpace;
    renderer.setClearColor(0x000000, thermal ? 0 : 1);
    // gsplat 'classic' training (train.py, train_rgb.py): the 0.3 px^2 blur without Spark's opacity
    // compensation; splats that do not say so keep Spark's antialiased default
    useSpark(thermal); // before its settings below
    const classic = meta.rasterize_mode === 'classic';
    spark.preBlurAmount = classic ? 0.3 : 0;
    spark.blurAmount = classic ? 0 : 0.3;

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
    if (!mesh || (thermal && !post)) return; // thermal set-up awaits the colormaps
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
  // embedding: a same-origin parent posts {type: 'apcg-splat-open', bytes, name, meta}; bytes
  // may be a Blob or File (read here, so the parent need not hold the file in memory)
  window.addEventListener('message', (e) => {
    if (e.origin !== location.origin || e.source !== window.parent) return;
    if (e.data?.type !== 'apcg-splat-open') return;
    const { bytes, name = 'splat.ply', meta = null } = e.data;
    const reply = (error) =>
      window.parent.postMessage({ type: 'apcg-splat-opened', name, error }, location.origin);
    const data = bytes instanceof Blob ? bytes.arrayBuffer() : Promise.resolve(bytes);
    data
      .then((b) => openBytes(new Uint8Array(b), name, meta))
      .then(
        () => reply(),
        (err) => (fail(err), reply(err.message)),
      );
  });
  const params = new URLSearchParams(location.search);
  if (window.parent !== window) {
    window.parent.postMessage({ type: 'apcg-splat-ready' }, location.origin);
  }
  if (params.get('embed')) {
    // the parent page opens the splat
  } else if (params.get('url')) {
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
