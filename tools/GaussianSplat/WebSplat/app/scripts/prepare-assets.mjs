// Copies the viewer's pinned FFmpeg core (FFV1 decoding) to public/vendor/ffmpeg/<version>/,
// the URL the ingest and select stages load it from. The app pins the same version.
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
