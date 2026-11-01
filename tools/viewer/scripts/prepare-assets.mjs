import { mkdir, cp, rm, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { FFMPEG_VERSION, CESIUM_VERSION } from '../src/versions.js';
const root = resolve(import.meta.dirname, '..');
// This ignored directory contains only generated dependency assets.
await rm(resolve(root, 'public/vendor'), { recursive: true, force: true });
const assets = [
  [
    'node_modules/@ffmpeg/core/dist/umd/ffmpeg-core.js',
    `public/vendor/ffmpeg/${FFMPEG_VERSION}/ffmpeg-core.js`,
  ],
  [
    'node_modules/@ffmpeg/core/dist/umd/ffmpeg-core.wasm',
    `public/vendor/ffmpeg/${FFMPEG_VERSION}/ffmpeg-core.wasm`,
  ],
  ['node_modules/cesium/Build/Cesium/Workers', `public/vendor/cesium/${CESIUM_VERSION}/Workers`],
  ['node_modules/cesium/Build/Cesium/Assets', `public/vendor/cesium/${CESIUM_VERSION}/Assets`],
  ['node_modules/cesium/Build/Cesium/Widgets', `public/vendor/cesium/${CESIUM_VERSION}/Widgets`],
  [
    'node_modules/cesium/Build/Cesium/ThirdParty',
    `public/vendor/cesium/${CESIUM_VERSION}/ThirdParty`,
  ],
];
for (const [source, destination] of assets) {
  await mkdir(resolve(root, destination, '..'), { recursive: true });
  await cp(resolve(root, source), resolve(root, destination), { recursive: true });
}
console.log('Prepared local decoder and Cesium assets.');

await mkdir(resolve(root, 'public/licenses'), { recursive: true });
await cp(resolve(root, 'THIRD_PARTY.md'), resolve(root, 'public/licenses/THIRD_PARTY.md'));
await cp(resolve(root, '../../COPYING.txt'), resolve(root, 'public/licenses/GPL-3.0.txt'));
await cp(
  resolve(root, 'node_modules/cesium/LICENSE.md'),
  resolve(root, 'public/licenses/Cesium-LICENSE.md'),
);
const lock = JSON.parse(await readFile(resolve(root, 'package-lock.json'), 'utf8'));
const dependencies = [];
for (const name of ['@ffmpeg/core', 'cesium']) {
  const metadata = JSON.parse(
    await readFile(resolve(root, `node_modules/${name}/package.json`), 'utf8'),
  );
  dependencies.push({
    name,
    version: metadata.version,
    license: metadata.license,
    repository: metadata.repository,
    integrity: lock.packages[`node_modules/${name}`]?.integrity,
  });
}
await writeFile(
  resolve(root, 'public/licenses/dependency-manifest.json'),
  JSON.stringify({ dependencies }, null, 2) + '\n',
);
