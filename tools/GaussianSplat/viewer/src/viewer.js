// Static Gaussian splat viewer: renders on the visitor's GPU with Spark (three.js).
//
// Site pages (export_web.py) set window.SPLAT = {meta, splat, colormaps}. Without it the page is
// the open-a-file viewer: a local file (never uploaded), or ?url=FILE[&meta=META.json], or (in a
// same-origin iframe, ?embed=1) bytes posted by the parent page. Without a meta.json, PLY and SPZ
// files carry their own (PLY header, SPZ gzip header: spz-meta.mjs), else a sidecar .json is used.
// meta.rasterize_mode 'classic' selects gsplat classic rendering (no mip opacity compensation).
// Thermal splats hold the scaled temperature in every channel; it is blended over black into a
// float (or half-float) target, then decoded to degrees C and colormapped per pixel, as view_splat.py does.
// A ceiling slider hides Gaussians above a height (floaters) with a Spark SplatEdit plane, display
// only; a PLY can be downloaded without them.
// View settings (gear button, kept in localStorage): a horizon-level orbit that stays above the
// ground, a circular trim (Spark SplatEdit cylinder) about trim_centre / the target, and for
// georeferenced splats (meta.origin) Esri satellite imagery on a ground disc below the splat.
import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import {
  SparkRenderer,
  SplatEdit,
  SplatEditRgbaBlendMode,
  SplatEditSdf,
  SplatEditSdfType,
  SplatMesh,
} from '@sparkjsdev/spark';
import { embeddedMeta } from './splat-meta.js';
import { clipPly } from '../ply-clip.mjs';
import projectIconUrl from '../../../../assets/camera-gimbal.svg?url';
import {
  MIN_PITCH_DEG,
  clampCamera,
  dollyDistance,
  estimateTrim,
  groundAt,
  groundGrid,
  loadSettings,
  median,
  saveSettings,
  tileLayout,
  trimFromMeta,
} from './view-math.js';

// render size caps (pixels): integrated GPUs struggle at full HiDPI resolution
const MAX_PIXELS = 2.0e6;
const MOVING_SCALE = 0.45; // linear scale while the view moves, refined when it settles
const FORMATS = ['ply', 'spz', 'splat', 'ksplat', 'sog'];
const UP_AXES = { z: [0, 0, 1], y: [0, 1, 0], '-y': [0, -1, 0] };
const SAMPLES = 300000; // Gaussians sampled for the trim and ground estimates
// the video viewer's satellite imagery (tools/viewer map-view.js), with its credit
const SAT_URL =
  'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}';
const SAT_CREDIT = 'Esri, Maxar, Earthstar Geographics';
const GEAR_SVG =
  '<svg viewBox="-12 -12 24 24" width="20" height="20" aria-hidden="true"><mask id="gear-hole">' +
  '<rect x="-12" y="-12" width="24" height="24" fill="#fff"/><circle r="3.2" fill="#000"/></mask>' +
  '<g fill="currentColor" mask="url(#gear-hole)">' +
  [0, 45, 90, 135]
    .map((a) => `<rect x="-2" y="-10.5" width="4" height="21" rx="1" transform="rotate(${a})"/>`)
    .join('') +
  '<circle r="7.5"/></g></svg>';

const cfg = window.SPLAT;
window.splatViewer = {}; // state for tests (ceiling)
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

// the metadata beside a splat that does not carry its own (not a PLY): NAME.json, then the
// directory's meta.json (export_web.py's layout); null if neither is there
async function sidecarMeta(url) {
  if (extension(url.pathname) === 'ply') return null;
  const base = url.pathname.replace(/\.[^./]*$/, '');
  for (const path of [`${base}.json`, url.pathname.replace(/[^/]*$/, 'meta.json')]) {
    try {
      const r = await fetch(new URL(path, url));
      if (r.ok) return await r.json();
    } catch {
      // not there, or not readable cross-origin
    }
  }
  return null;
}

async function getJson(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
  return r.json();
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
  controls.minDistance = 0.5;

  let store = null;
  try {
    store = window.localStorage;
  } catch {
    // storage blocked
  }
  const settings = loadSettings(store);
  const view = { settings }; // state for tests
  window.splatViewer.view = view;
  window.splatViewer.camera = () => ({
    position: camera.position.toArray(),
    target: controls.target.toArray(),
  });

  // panel collapse: a triangle beside the title; narrow screens start collapsed
  const panel = $('panel');
  const panelHead = document.createElement('div');
  panelHead.className = 'panel-head';
  const panelToggle = document.createElement('button');
  panelToggle.id = 'panel-toggle';
  panelToggle.type = 'button';
  panelHead.append(panelToggle, $('title'));
  panel.prepend(panelHead);
  function setCollapsed(collapsed) {
    panel.classList.toggle('collapsed', collapsed);
    panelToggle.textContent = collapsed ? '▸' : '▾';
    panelToggle.title = collapsed ? 'Show the panel' : 'Hide the panel';
    panelToggle.setAttribute('aria-expanded', String(!collapsed));
  }
  setCollapsed(
    settings.panel ? settings.panel === 'collapsed' : matchMedia('(max-width: 600px)').matches,
  );
  panelToggle.onclick = () => {
    setCollapsed(!panel.classList.contains('collapsed'));
    settings.panel = panel.classList.contains('collapsed') ? 'collapsed' : 'open';
    saveSettings(store, settings);
  };

  // zoom buttons and the settings gear, bottom right
  const tools = document.createElement('div');
  tools.id = 'view-tools';
  tools.hidden = true;
  tools.innerHTML =
    '<div class="zoom" role="group" aria-label="Zoom">' +
    '<button id="zoom-in" type="button" aria-label="Zoom in" title="Zoom in (+)">+</button>' +
    '<button id="zoom-out" type="button" aria-label="Zoom out" title="Zoom out (−)">−</button>' +
    '</div><button id="settings-button" type="button" aria-label="View settings" ' +
    `title="View settings" aria-expanded="false">${GEAR_SVG}</button>`;
  const settingsUi = document.createElement('div');
  settingsUi.id = 'settings';
  settingsUi.hidden = true;
  settingsUi.innerHTML =
    '<h2>View settings</h2>' +
    '<label><input id="set-level" type="checkbox" /> Keep the horizon level</label>' +
    '<p class="note">No roll; the camera stays above the horizon and the ground.</p>' +
    '<label><input id="set-trim" type="checkbox" /> Trim to a circle</label>' +
    '<label id="trim-radius-label">Radius <input id="trim-radius" type="range" min="0" ' +
    'max="1000" step="1" /></label><p id="trim-note" class="note"></p>' +
    '<div id="satellite-controls" hidden>' +
    '<label><input id="set-satellite" type="checkbox" /> Satellite map below</label>' +
    '<label id="splat-opacity-label">Splat opacity <input id="splat-opacity" type="range" ' +
    'min="0" max="100" step="1" value="100" /></label><p id="satellite-note" class="note"></p>' +
    '</div>';
  document.body.append(tools, settingsUi);
  const satCredit = document.createElement('span');
  satCredit.id = 'sat-credit';
  satCredit.hidden = true;
  satCredit.textContent = `Imagery: ${SAT_CREDIT} · `;
  document.querySelector('.credits')?.prepend(satCredit);
  $('settings-button').onclick = () => {
    settingsUi.hidden = !settingsUi.hidden;
    $('settings-button').setAttribute('aria-expanded', String(!settingsUi.hidden));
  };

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

  // ceiling: Gaussians above it get opacity 0 (a plane edit, inverted: it acts above the plane);
  // moving the plane re-runs Spark's generator on the GPU, the splat is not rebuilt
  const ceilingSdf = new SplatEditSdf({
    type: SplatEditSdfType.PLANE,
    invert: true,
    opacity: 0,
    color: new THREE.Color(1, 1, 1),
  });
  const ceilingEdit = new SplatEdit({ rgbaBlendMode: SplatEditRgbaBlendMode.MULTIPLY });
  ceilingEdit.add(ceilingSdf);
  const STEPS = 1000;
  // sorted Gaussian heights (world z) as Spark holds the centres (float16 for colour splats, so
  // the count can differ slightly from the exact PLY's at the ceiling)
  let heights = null;
  let ceilingRange = null; // [target height (or the lowest 1% above it), highest Gaussian]
  let targetZ = null;
  let ceilingH = null; // the ceiling, null when off
  let source = null; // the opened PLY (Blob or bytes), for the clipped download
  let sourceName = '';
  let upAxis = [0, 0, 1];
  const ceilingUi = document.createElement('div');
  ceilingUi.id = 'ceiling-controls';
  ceilingUi.hidden = true;
  ceilingUi.innerHTML =
    `<label>Ceiling (hide floaters above) <input id="ceiling" type="range" min="0" ` +
    `max="${STEPS}" step="1" value="${STEPS}" /></label><p id="ceiling-value" class="note"></p>` +
    `<button id="ceiling-download" type="button" hidden>Download PLY without them</button>`;
  $('reset').before(ceilingUi);

  const above = (h) => {
    // binary search: the Gaussians above h
    let a = 0;
    let b = heights.length;
    while (a < b) {
      const m = (a + b) >> 1;
      if (heights[m] <= h) a = m + 1;
      else b = m;
    }
    return heights.length - a;
  };

  function setCeiling(step) {
    const [lo, hi] = ceilingRange;
    const off = step >= STEPS;
    ceilingH = off ? null : lo + ((hi - lo) * step) / STEPS;
    if (off) scene.remove(ceilingEdit);
    else {
      ceilingSdf.position.set(0, 0, ceilingH);
      if (!ceilingEdit.parent) scene.add(ceilingEdit);
    }
    const h = off ? hi : ceilingH;
    const rel = meta.target
      ? `${(h - targetZ).toFixed(1)} m above the target`
      : `height ${h.toFixed(1)} m`;
    const amsl =
      meta.origin?.alt_amsl != null && meta.up
        ? `, ${(meta.origin.alt_amsl + h).toFixed(1)} m AMSL`
        : '';
    $('ceiling-value').textContent = off
      ? `Off: highest Gaussian ${rel}${amsl}.`
      : `${rel}${amsl}: ${above(h).toLocaleString('en')} Gaussians hidden (view only).`;
    $('ceiling-download').hidden = off || !source;
    window.splatViewer.ceiling = {
      height: ceilingH,
      hidden: off ? 0 : above(h),
      range: ceilingRange,
    };
    needSplat = true;
  }

  // world positions: every height (the ceiling) and a sample of the opaque ones (trim, ground)
  function sampleSplat() {
    const zs = new Float32Array(mesh.numSplats);
    const step = Math.max(1, Math.floor(mesh.numSplats / SAMPLES));
    const s = { x: [], y: [], z: [] };
    const v = new THREE.Vector3();
    mesh.forEachSplat((i, centre, _scales, _q, opacity) => {
      v.copy(centre).applyQuaternion(holder.quaternion);
      zs[i] = v.z;
      if (i % step === 0 && opacity > 0.2) {
        s.x.push(v.x);
        s.y.push(v.y);
        s.z.push(v.z);
      }
    });
    zs.sort();
    return { zs, sample: s };
  }

  function setupCeiling(zs) {
    scene.remove(ceilingEdit);
    ceilingH = null;
    heights = zs;
    const top = zs[zs.length - 1];
    targetZ = meta.target
      ? new THREE.Vector3(...meta.target).applyQuaternion(holder.quaternion).z
      : null;
    // from the target, or from the lowest 1% when the target is below the scene
    const lo = Math.max(targetZ ?? -Infinity, zs[Math.floor(0.01 * (zs.length - 1))]);
    ceilingUi.hidden = !(top > lo);
    if (ceilingUi.hidden) return;
    ceilingRange = [lo, top];
    $('ceiling').value = STEPS;
    setCeiling(STEPS);
  }

  $('ceiling').oninput = () => setCeiling(Number($('ceiling').value));
  $('ceiling-download').onclick = async () => {
    if (!source || ceilingH == null) return;
    const bytes =
      source instanceof Blob
        ? new Uint8Array(await source.arrayBuffer())
        : source instanceof Uint8Array
          ? source
          : new Uint8Array(source);
    // null too when the bytes went to Spark (detached)
    const r = bytes.length ? clipPly(bytes, upAxis, ceilingH) : null;
    if (!r) {
      setStatus('This PLY cannot be clipped here.');
      return;
    }
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([r.bytes]));
    a.download = `${sourceName.replace(/\.ply$/i, '')}-ceiling-${ceilingH.toFixed(1)}m.ply`;
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 60000);
  };

  // circular trim: Gaussians outside a vertical cylinder (Spark's is along its local y) get
  // opacity 0, display only
  const trimSdf = new SplatEditSdf({
    type: SplatEditSdfType.CYLINDER,
    invert: true,
    opacity: 0,
    color: new THREE.Color(1, 1, 1),
    radius: 1,
  });
  trimSdf.rotation.x = Math.PI / 2;
  trimSdf.scale.set(1, 1e6, 1); // half height
  const trimEdit = new SplatEdit({ rgbaBlendMode: SplatEditRgbaBlendMode.MULTIPLY });
  trimEdit.add(trimSdf);
  let trim = null; // {centre: [x, y] world, radius, initial, max, source}
  let ground = null; // groundGrid() in world coordinates
  const metresText = (m) => `${m.toFixed(m < 100 ? 1 : 0)} m`;

  function applyTrim() {
    const on = settings.trim && trim;
    if (on) {
      trimSdf.radius = trim.radius;
      trimSdf.position.set(trim.centre[0], trim.centre[1], 0);
      if (!trimEdit.parent) scene.add(trimEdit);
    } else scene.remove(trimEdit);
    $('set-trim').checked = settings.trim;
    $('set-trim').disabled = !trim;
    $('trim-radius-label').hidden = !on;
    $('trim-note').textContent = trim
      ? `${metresText(trim.radius)} about the ${trim.about}` +
        (trim.radius === trim.initial
          ? trim.source === 'file'
            ? ' (from the file).'
            : ' (estimated).'
          : ` (${trim.source === 'file' ? 'the file says' : 'estimate'} ` +
            `${metresText(trim.initial)}).`)
      : '';
    view.trim = trim && { ...trim, on: !!on };
    needSplat = true;
  }

  // world [x, y] of a splat-frame point [x, y(, z)]
  const worldXY = (p) => {
    const v = new THREE.Vector3(p[0], p[1], p[2] ?? 0).applyQuaternion(holder.quaternion);
    return [v.x, v.y];
  };

  function setupTrim(sample) {
    const fromFile = trimFromMeta(meta);
    const about =
      fromFile && meta.trim_centre ? 'scene centre' : meta.target ? 'target' : 'median centre';
    const centre = fromFile
      ? worldXY(fromFile.centre)
      : meta.target
        ? worldXY(meta.target)
        : [median(sample.x), median(sample.y)];
    const est = estimateTrim(sample.x, sample.y, centre);
    const radius = fromFile?.radius ?? est?.radius;
    trim =
      radius > 0
        ? {
            centre,
            radius,
            initial: radius,
            max: Math.max(radius * 1.5, est?.edge ?? 0),
            source: fromFile ? 'file' : 'estimate',
            about,
          }
        : null;
    if (trim) {
      $('trim-radius').max = String(Math.ceil(trim.max));
      $('trim-radius').min = String(Math.min(10, Math.floor(trim.radius)));
      $('trim-radius').value = String(Math.round(trim.radius));
    }
    // ground over the trim circle (or the estimate's) for the camera limit and the map
    const r = trim?.radius ?? est?.edge;
    ground = r > 0 ? groundGrid(sample.x, sample.y, sample.z, centre, r) : null;
    if (ground) {
      // the map's height: low among the cells, so hills stand on it rather than cut through
      const hs = Array.from(ground.h).filter(Number.isFinite);
      hs.sort((a, b) => a - b);
      ground.map = hs[Math.floor(0.2 * (hs.length - 1))];
    }
    view.ground = ground && { ground: ground.ground, map: ground.map, cell: ground.cell };
    applyTrim();
  }

  $('set-trim').onchange = () => {
    settings.trim = $('set-trim').checked;
    saveSettings(store, settings);
    applyTrim();
    updateSatellite();
  };
  $('trim-radius').oninput = () => {
    if (!trim) return;
    trim.radius = Number($('trim-radius').value);
    applyTrim();
  };

  // horizon-level orbit (as MAVProxy's map3d): pitch at least MIN_PITCH_DEG, pans horizontal,
  // camera above the ground estimate; off: OrbitControls' free orbit
  function applyLevel() {
    controls.screenSpacePanning = !settings.level;
    controls.maxPolarAngle = settings.level
      ? THREE.MathUtils.degToRad(90 - MIN_PITCH_DEG)
      : Math.PI;
    $('set-level').checked = settings.level;
    controls.update();
  }
  function limitCamera() {
    if (!settings.level) return false;
    const p = camera.position;
    const t = controls.target;
    const dist = p.distanceTo(t);
    const out = clampCamera([p.x, p.y, p.z], [t.x, t.y, t.z], {
      ground: ground ? (x, y) => groundAt(ground, x, y) : null,
      clearance: Math.min(2, Math.max(0.2, dist * 0.02)),
    });
    if (out[0] === p.x && out[1] === p.y && out[2] === p.z) return false;
    p.set(...out);
    return true;
  }
  $('set-level').onchange = () => {
    settings.level = $('set-level').checked;
    saveSettings(store, settings);
    applyLevel();
  };

  function zoom(inward) {
    const offset = camera.position.clone().sub(controls.target);
    const d = offset.length();
    if (!(d > 0)) return;
    offset.setLength(dollyDistance(d, inward, controls.minDistance, controls.maxDistance));
    camera.position.copy(controls.target).add(offset);
    controls.update();
  }
  $('zoom-in').onclick = () => zoom(true);
  $('zoom-out').onclick = () => zoom(false);
  window.addEventListener('keydown', (e) => {
    if (tools.hidden || e.ctrlKey || e.metaKey || e.altKey) return;
    if (e.target.closest?.('input, select, textarea')) return;
    if (e.key === '+' || e.key === '=') zoom(true);
    else if (e.key === '-' || e.key === '_') zoom(false);
  });

  // satellite map: Esri World Imagery tiles drawn into one texture on a horizontal disc at the
  // ground estimate (no terrain), about the splat's ENU origin, its rim fading to black; tiles
  // that fail are left grey
  const groundScene = new THREE.Scene();
  let sat = null; // {mesh, state, loaded, failed, total, zoom}
  let satGen = 0;
  let splatOpacity = 1;
  const satAvailable = () =>
    !!(
      meta?.origin &&
      Number.isFinite(meta.origin.lat) &&
      Number.isFinite(meta.origin.lon) &&
      upAxis[2] === 1 &&
      ground
    );
  const satVisible = () => !!(settings.satellite && sat?.mesh.visible && satAvailable());

  function disposeSatellite() {
    satGen++;
    if (!sat) return;
    groundScene.remove(sat.mesh);
    sat.mesh.geometry.dispose();
    sat.mesh.material.map?.dispose();
    sat.mesh.material.dispose();
    sat = null;
  }

  function buildSatellite() {
    const gen = ++satGen;
    const half = Math.min(3000, Math.max(600, 5 * (trim?.initial ?? 200)));
    const layout = tileLayout(meta.origin, half);
    const mosaic = document.createElement('canvas');
    mosaic.width = layout.width;
    mosaic.height = layout.height;
    const ctx = mosaic.getContext('2d');
    ctx.fillStyle = '#3a3a3a';
    ctx.fillRect(0, 0, layout.width, layout.height);
    const tex = new THREE.CanvasTexture(mosaic);
    tex.colorSpace = THREE.SRGBColorSpace;
    tex.anisotropy = renderer.capabilities.getMaxAnisotropy();
    const geom = new THREE.RingGeometry(0, half, 128, 16);
    const pos = geom.attributes.position;
    const uv = geom.attributes.uv;
    const rgb = new Float32Array(pos.count * 3);
    for (let i = 0; i < pos.count; i++) {
      uv.setXY(i, ...layout.uv(pos.getX(i), pos.getY(i)));
      const r = Math.hypot(pos.getX(i), pos.getY(i)) / half;
      rgb.fill(Math.min(1, Math.max(0, (1 - r) / 0.25)), 3 * i, 3 * i + 3);
    }
    geom.setAttribute('color', new THREE.BufferAttribute(rgb, 3));
    const mesh = new THREE.Mesh(
      geom,
      new THREE.MeshBasicMaterial({ map: tex, vertexColors: true }),
    );
    mesh.position.z = ground.map;
    mesh.visible = false; // until a tile arrives
    groundScene.add(mesh);
    sat = {
      mesh,
      state: 'loading',
      loaded: 0,
      failed: 0,
      total: layout.tiles.length,
      zoom: layout.z,
    };
    const s = sat;
    const queue = [...layout.tiles.entries()];
    const one = async ([k, [x, y]]) => {
      const img = new Image();
      img.crossOrigin = 'anonymous';
      img.src = SAT_URL.replace('{z}', layout.z).replace('{y}', y).replace('{x}', x);
      let timer = null;
      try {
        await Promise.race([
          img.decode(),
          new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error('timeout')), 20000);
          }),
        ]);
        if (gen !== satGen) return;
        const nx = layout.width / 256;
        ctx.drawImage(img, (k % nx) * 256, Math.floor(k / nx) * 256);
        tex.needsUpdate = true;
        s.loaded++;
        mesh.visible = true;
      } catch {
        s.failed++;
      } finally {
        clearTimeout(timer);
      }
      if (gen === satGen) updateSatellite();
    };
    const worker = async () => {
      while (queue.length && gen === satGen) await one(queue.shift());
    };
    Promise.all(Array.from({ length: 6 }, worker)).then(() => {
      if (gen !== satGen) return;
      s.state = !s.loaded ? 'failed' : s.failed ? 'partial' : 'done';
      updateSatellite();
    });
  }

  function updateSatellite() {
    const avail = satAvailable();
    $('satellite-controls').hidden = !avail;
    $('set-satellite').checked = settings.satellite;
    if (avail && settings.satellite && !sat) buildSatellite();
    const on = avail && settings.satellite && sat?.state !== 'failed';
    $('splat-opacity-label').hidden = !on;
    let note = '';
    if (avail && settings.satellite && sat) {
      if (sat.state === 'failed')
        note = 'Satellite imagery could not be loaded (offline, or blocked by the network).';
      else if (sat.state === 'loading')
        note = `Loading imagery ${sat.loaded} of ${sat.total} tiles…`;
      else
        note =
          `Map plane at ${metresText(ground.map)}` +
          (meta.origin.alt_amsl != null
            ? ` (${(meta.origin.alt_amsl + ground.map).toFixed(0)} m AMSL)`
            : '') +
          `, a ground estimate from the splat (no terrain).` +
          (sat.failed ? ` ${sat.failed} of ${sat.total} tiles failed.` : '');
    }
    $('satellite-note').textContent = note;
    satCredit.hidden = !satVisible();
    const op = on ? splatOpacity : 1;
    if (mesh) mesh.opacity = thermal ? 1 : op; // thermal: faded in the post pass, values kept
    if (post) post.material.uniforms.op.value = op;
    view.satellite = avail
      ? {
          on,
          visible: satVisible(),
          state: sat?.state ?? null,
          loaded: sat?.loaded ?? 0,
          failed: sat?.failed ?? 0,
          total: sat?.total ?? 0,
          zoom: sat?.zoom ?? null,
          height: ground.map,
          opacity: op,
        }
      : null;
    needSplat = needPost = true;
  }

  $('set-satellite').onchange = () => {
    settings.satellite = $('set-satellite').checked;
    saveSettings(store, settings);
    if (sat?.state === 'failed') disposeSatellite(); // try again next time
    // on: the splat starts opaque
    splatOpacity = 1;
    $('splat-opacity').value = '100';
    updateSatellite();
  };
  $('splat-opacity').oninput = () => {
    splatOpacity = Number($('splat-opacity').value) / 100;
    updateSatellite();
  };

  function setupView(sample) {
    disposeSatellite();
    setupTrim(sample);
    applyLevel();
    updateSatellite();
  }

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
      const knee = bg !== null && bg > lo && bg < u.hi.value;
      u.bg.value = knee ? bg : 0;
      u.knee.value = knee ? 1 : 0;
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
          op: { value: 1 },
        },
        vertexShader: `in vec3 position; out vec2 uv;
          void main() { uv = position.xy * 0.5 + 0.5; gl_Position = vec4(position.xy, 0.0, 1.0); }`,
        fragmentShader: `precision highp float; uniform sampler2D src; uniform sampler2D lut;
          uniform float t0, k, norm, lo, hi, bg, knee, glory, gmin, op; in vec2 uv; out vec4 colour;
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
            float a = clamp(s.a, 0.0, 1.0) * op;
            colour = vec4(c * a, a); // premultiplied, over black or the map
          }`,
        depthTest: false,
        depthWrite: false,
        blending: THREE.CustomBlending,
        blendSrc: THREE.OneFactor,
        blendDst: THREE.OneMinusSrcAlphaFactor,
        blendSrcAlpha: THREE.OneFactor,
        blendDstAlpha: THREE.OneMinusSrcAlphaFactor,
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
    const step = Math.max(1, Math.floor(mesh.numSplats / 50000));
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
    let size = Math.max(...xs.map((a) => q(a, 0.98) - q(a, 0.02)), 1e-3);
    if (trim && settings.trim) size = Math.min(size, 2 * trim.radius);
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
    controls.maxDistance = camera.far / 4;
    controls.update();
  }

  function setUp(axis) {
    upAxis = axis;
    holder.quaternion.setFromUnitVectors(
      new THREE.Vector3(...axis).normalize(),
      new THREE.Vector3(0, 0, 1),
    );
    holder.updateMatrixWorld();
    needSplat = true;
  }

  // src: the file as opened (a Blob, or bytes), kept for the clipped PLY download
  async function show(bytes, name, m, src = bytes) {
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
    source = extension(name) === 'ply' ? src : null;
    sourceName = String(name).split('/').pop();
    const { zs, sample } = sampleSplat();
    setupCeiling(zs);

    if (thermal) {
      if (!colormaps) colormaps = await getJson(cfg ? cfg.colormaps : 'colormaps.json');
      setupThermal();
      const s = meta.temperature_scale;
      const norm = Math.log1p((s.t1_c - s.t0_c) / s.k_c);
      toC = (v) => s.t0_c + s.k_c * Math.expm1(v * norm);
      post.material.uniforms.t0.value = s.t0_c;
      post.material.uniforms.k.value = s.k_c;
      post.material.uniforms.norm.value = norm;
      // thermal splats open in whitehot; meta.colormap ('ironbow' from every exporter so far) is
      // not a per-splat choice
      ui.cmap.value = 'whitehot';
      setLut(ui.cmap.value);
      const range = meta.display_range_c || [Math.floor(s.t0_c), Math.ceil(s.t1_c)];
      ui.lo.value = range[0];
      ui.hi.value = range[1];
      ui.auto.checked = true;
      applyRange();
    }
    setupView(sample);
    tools.hidden = false;

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
      const { zs, sample } = sampleSplat();
      setupCeiling(zs);
      setupView(sample);
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
    if (limitCamera()) {
      lastMove = performance.now();
      needSplat = true;
    }
    if (!mesh || (thermal && !post)) return; // thermal set-up awaits the colormaps
    const isMoving = performance.now() - lastMove < 200;
    if (thermal && (isMoving ? moving : full) !== target) {
      target = isMoving ? moving : full;
      needSplat = true; // refine at full size once the view settles
    }
    if (!needSplat && !needPost) return;
    // the map is drawn first and the splat over it: it hides nothing of the splat (Gaussians
    // below the estimated ground included, e.g. heat seen under canopy)
    const map = satVisible();
    if (!thermal) {
      if (map) {
        renderer.render(groundScene, camera);
        renderer.clearDepth();
      }
      renderer.autoClear = !map;
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
      if (map) renderer.render(groundScene, camera);
      renderer.autoClear = !map;
      renderer.render(post.userData.scene, post.userData.camera);
    }
    renderer.autoClear = true;
    needSplat = needPost = false;
  });

  // metadata: m (given, or the site's meta.json), else the file's own, else sidecar() (a .json
  // beside it or chosen with it)
  async function openBytes(bytes, name, m, sidecar = null, src = bytes) {
    if ($('open-panel')) $('open-panel').hidden = true;
    $('panel').hidden = false;
    await show(bytes, name, m || embeddedMeta(bytes) || (sidecar && (await sidecar())), src);
  }

  if (cfg) {
    // a site page from export_web.py
    const m = await getJson(cfg.meta);
    await openBytes(await fetchBytes(cfg.splat, 'Loading splat…'), cfg.splat, m);
    return;
  }

  // open-a-file viewer: files are read in the browser, nothing is uploaded. A .json chosen with
  // the splat is its metadata (export_web.py's meta.json) if the file carries none
  const openFiles = async (files) => {
    const list = [...files];
    const file = list.find((f) => FORMATS.includes(extension(f.name)));
    if (!file) {
      setStatus(`${list.map((f) => f.name).join(', ')}: not a splat file (${FORMATS.join(', ')})`);
      return;
    }
    const json = list.find((f) => extension(f.name) === 'json');
    setStatus(`Reading ${file.name}…`);
    const sidecar = json ? async () => JSON.parse(await json.text()) : null;
    await openBytes(new Uint8Array(await file.arrayBuffer()), file.name, null, sidecar, file);
  };
  for (const input of document.querySelectorAll('input[type=file]')) {
    input.onchange = (e) => e.target.files.length && openFiles(e.target.files).catch(fail);
  }
  window.addEventListener('dragover', (e) => e.preventDefault());
  window.addEventListener('drop', (e) => {
    e.preventDefault();
    if (e.dataTransfer.files.length) openFiles(e.dataTransfer.files).catch(fail);
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
      .then((b) => openBytes(new Uint8Array(b), name, meta, null, bytes))
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
    await openBytes(await fetchBytes(u, 'Downloading splat…'), u.pathname, m, () => sidecarMeta(u));
  } else {
    $('open-panel').hidden = false;
  }
}

function fail(e) {
  console.error(e);
  setStatus(`Could not load the splat: ${e.message}`);
}

main().catch(fail);
