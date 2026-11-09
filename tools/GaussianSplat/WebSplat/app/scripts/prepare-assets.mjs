// Copies the viewer's pinned FFmpeg core (FFV1 decoding) to public/vendor/ffmpeg/<version>/,
// the URL the ingest and select stages load it from. The app pins the same version.
// Also the built splat viewer (tools/GaussianSplat/viewer/dist) to public/vendor/splat-viewer/,
// the result view's iframe.
import { existsSync, readFileSync } from 'node:fs';
import { cp, mkdir, readFile, rm } from 'node:fs/promises';
import { resolve } from 'node:path';

const root = resolve(import.meta.dirname, '..');
const viewer = JSON.parse(await readFile(resolve(root, '../../../viewer/package.json'), 'utf8'));
const app = JSON.parse(await readFile(resolve(root, 'package.json'), 'utf8'));
const version = viewer.dependencies['@ffmpeg/core'];
if (app.dependencies['@ffmpeg/core'] !== version) {
  throw new Error(
    `@ffmpeg/core ${app.dependencies['@ffmpeg/core']} differs from the viewer's ${version}`,
  );
}
// public/vendor/brush is replaced only when its package is given (below), so builds without it keep it
for (const d of ['ffmpeg', 'splat-viewer'])
  await rm(resolve(root, 'public/vendor', d), { recursive: true, force: true });
const dest = resolve(root, `public/vendor/ffmpeg/${version}`);
await mkdir(dest, { recursive: true });
for (const f of ['ffmpeg-core.js', 'ffmpeg-core.wasm']) {
  await cp(resolve(root, `node_modules/@ffmpeg/core/dist/umd/${f}`), resolve(dest, f));
}
console.log(`prepared FFmpeg core ${version}`);

const splatViewer = resolve(root, '../../viewer/dist');
if (existsSync(resolve(splatViewer, 'viewer.js'))) {
  await cp(splatViewer, resolve(root, 'public/vendor/splat-viewer'), {
    recursive: true,
    filter: (f) => !f.endsWith('.gz'),
  });
  console.log('prepared the splat viewer');
} else {
  console.warn('splat viewer not built (cd tools/GaussianSplat/viewer && npm ci && npm run build)');
}

// The Brush fork's wasm package for the training stage (vendor/brush/brush_js.js): built from
// src/stages/train/brush-patches (test/g1/README), the wasm-bindgen --target web output directory
// given by WEBSPLAT_BRUSH_PKG (environment, or WebSplat/local.env). Without it an existing copy is kept.
function localEnv(name) {
  const f = resolve(root, '../local.env');
  if (!existsSync(f)) return undefined;
  for (const line of readFileSync(f, 'utf8').split('\n')) {
    const m = line.match(/^\s*([A-Z_]+)=(.*)$/);
    if (m && m[1] === name) return m[2].trim();
  }
  return undefined;
}
const brushPkg = process.env.WEBSPLAT_BRUSH_PKG || localEnv('WEBSPLAT_BRUSH_PKG');
const brushDest = resolve(root, 'public/vendor/brush');
if (brushPkg && existsSync(resolve(brushPkg, 'brush_js_bg.wasm'))) {
  await rm(brushDest, { recursive: true, force: true });
  await mkdir(brushDest, { recursive: true });
  for (const f of ['brush_js.js', 'brush_js_bg.wasm']) await cp(resolve(brushPkg, f), resolve(brushDest, f));
  console.log('prepared the Brush training package');
} else if (existsSync(resolve(brushDest, 'brush_js_bg.wasm'))) {
  console.log('kept the existing Brush training package (WEBSPLAT_BRUSH_PKG not set)');
} else {
  console.warn('Brush package not found (set WEBSPLAT_BRUSH_PKG): the training stage will fail');
}
