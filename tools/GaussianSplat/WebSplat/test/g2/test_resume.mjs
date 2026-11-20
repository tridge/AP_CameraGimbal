// Mapping resume from the per-round checkpoint states: map all rounds keeping the state after
// every round that gets a checkpoint (0 .. n-2, JSON round trip as stored in OPFS), then resume a
// fresh instance from each; all five model binaries must be byte-identical to the
// uninterrupted run's.
//   node test_resume.mjs FRAMES.json FEATURES.bin GEOMETRY.bin
import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
import { loadColmap } from '../../app/src/stages/map/colmap.js';
import { decodeFeatures } from '../../app/src/stages/features/format.js';
import {
  priorMap,
  cameraFromFrames,
  PRIOR_MAP_DEFAULTS,
} from '../../app/src/stages/map/priormap.js';
import { decodeGeometry } from '../../app/src/stages/match/format.js';

const [framesPath, featPath, geomPath] = process.argv.slice(2);
const wasm = readFileSync(
  new URL('../../app/src/stages/map/wasm/websplat-colmap.wasm', import.meta.url),
);
const info = JSON.parse(readFileSync(framesPath, 'utf8'));
const features = decodeFeatures(readFileSync(featPath));
const geometry = decodeGeometry(readFileSync(geomPath));
const camera = cameraFromFrames(info);
const nr = PRIOR_MAP_DEFAULTS.thresholds.length;
const saved = [];
const full = await priorMap(
  await loadColmap({ wasmBinary: wasm }),
  camera,
  info.frames,
  features,
  geometry,
  {
    onRound: (s, state) => {
      if (s.round < nr - 1)
        saved[s.round] = JSON.stringify(state(), (k, v) =>
          v instanceof Float64Array ? [...v] : v,
        );
    },
  },
);
const names = ['cameras.bin', 'images.bin', 'points3D.bin', 'rigs.bin', 'frames.bin'];
for (let r = 0; r < nr - 1; r++) {
  const st = JSON.parse(saved[r]);
  const start = { ...st, poses: new Float64Array(st.poses), params: new Float64Array(st.params) };
  const resumed = await priorMap(
    await loadColmap({ wasmBinary: wasm }),
    camera,
    info.frames,
    features,
    geometry,
    { start },
  );
  assert.equal(resumed.rounds.length, nr);
  for (const f of names)
    assert.ok(
      Buffer.from(full.files[f]).equals(Buffer.from(resumed.files[f])),
      `resume after round ${r}: ${f} differs`,
    );
  console.log(`resume after round ${r}: all ${names.length} model files byte-identical`);
}
console.log('PASS');
