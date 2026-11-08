// Stop points of the fork in Node: verification between pairs, mapping inside triangulation
// and inside bundle adjustment (Ceres iteration callback), and reuse of the instance after a stop.
//   node test_cancel.mjs FRAMES.json FEATURES.bin MATCHES.bin GEOMETRY.bin
import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
import { loadColmap } from '../../app/src/stages/map/colmap.js';
import { decodeFeatures } from '../../app/src/stages/map/features.js';
import { verifyPairs } from '../../app/src/stages/map/verify.js';
import { priorMap, cameraFromFrames } from '../../app/src/stages/map/priormap.js';
import { decodeMatches, decodeGeometry } from '../../app/src/stages/match/format.js';

const [framesPath, featPath, matchPath, geomPath] = process.argv.slice(2);
const wasm = readFileSync(
  new URL('../../app/src/stages/map/wasm/websplat-colmap.wasm', import.meta.url),
);
const cw = await loadColmap({ wasmBinary: wasm });
const info = JSON.parse(readFileSync(framesPath, 'utf8'));
const features = decodeFeatures(readFileSync(featPath));
const camera = cameraFromFrames(info);
const pairs = decodeMatches(readFileSync(matchPath)).slice(0, 40);
const geometry = decodeGeometry(readFileSync(geomPath));

const stopAt = (fn) => {
  try {
    fn();
  } catch (e) {
    return e;
  }
  return null;
};
let e = stopAt(() =>
  verifyPairs(cw, camera, features, pairs, {
    onProgress: (done) => done >= 16,
  }),
);
assert.equal(e?.name, 'AbortError');
assert.equal(e.done, 16);
const full = verifyPairs(cw, camera, features, pairs.slice(0, 8));
assert.equal(full.length, 8);
console.log('verification: stopped at pair 16, instance reusable');

for (const [phase, at] of [
  ['triangulate', 50],
  ['ba', 3],
]) {
  const seen = [];
  e = stopAt(() =>
    priorMap(cw, camera, info.frames, features, geometry, {
      onProgress: (p, done, total, round) => {
        seen.push(p);
        return round === 0 && p === phase && done >= at;
      },
    }),
  );
  assert.equal(e?.name, 'AbortError', `stop in ${phase}`);
  assert.equal(e.round, 0);
  console.log(`mapping: stopped in round 0 ${phase} after ${seen.length} progress calls`);
}
console.log('PASS');
