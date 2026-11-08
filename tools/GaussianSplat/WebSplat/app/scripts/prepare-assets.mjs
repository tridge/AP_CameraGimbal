// Copies the viewer's pinned FFmpeg core (FFV1 decoding) to public/vendor/ffmpeg/<version>/,
// the URL the ingest and select stages load it from. The app pins the same version.
// Also the built splat viewer (tools/GaussianSplat/viewer/dist) to public/vendor/splat-viewer/,
// the result view's iframe.
import { existsSync } from 'node:fs';
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
await rm(resolve(root, 'public/vendor'), { recursive: true, force: true });
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
