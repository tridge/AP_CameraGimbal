// Stage 3 in Node: COLMAP SIFT (the fork) on stage 2's CLAHE images -> WSFT.
//   node run_sift_node.mjs FEATURES.u8 FRAMES.json OUT.bin [--limit N] [--max 8192]
import { readFileSync, writeFileSync, openSync, readSync } from 'node:fs';
import { performance } from 'node:perf_hooks';
import { loadColmap } from '../../app/src/stages/map/colmap.js';
import { siftImage } from '../../app/src/stages/features/sift.js';
import { encodeFeatures } from '../../app/src/stages/features/format.js';

const args = process.argv.slice(2);
const flag = (k, d) => {
  const i = args.indexOf(k);
  return i < 0 ? d : args.splice(i, 2)[1];
};
const limit = Number(flag('--limit', 0));
const max = Number(flag('--max', 8192));
const wasm = flag('--wasm', null);
const [u8Path, framesPath, out] = args;
const info = JSON.parse(readFileSync(framesPath, 'utf8'));
const { width: W, height: H } = info;
const n = limit || info.frames.length;
const wasmUrl =
  wasm ?? new URL('../../app/src/stages/map/wasm/websplat-colmap.wasm', import.meta.url);
const t0 = performance.now();
const cw = await loadColmap({ wasmBinary: readFileSync(wasmUrl) });
const fd = openSync(u8Path, 'r');
const images = [];
const ms = [];
for (let k = 0; k < n; k++) {
  const grey = new Uint8Array(W * H);
  readSync(fd, grey, 0, W * H, k * W * H);
  const t = performance.now();
  images.push(siftImage(cw, grey, W, H, { maxNumFeatures: max }));
  ms.push(performance.now() - t);
  if (k % 20 === 0)
    console.error(
      `sift ${k}/${n} ${images[k].keypoints.length / 6} features ${ms[k].toFixed(0)} ms`,
    );
}
writeFileSync(out, encodeFeatures(images));
const stats = {
  images: n,
  total_ms: performance.now() - t0,
  sift_ms_mean: ms.reduce((a, b) => a + b, 0) / n,
  counts: images.map((im) => im.keypoints.length / 6),
  wasm_heap_bytes: cw.heapBytes,
};
writeFileSync(out.replace(/\.bin$/, '') + '.json', JSON.stringify(stats, null, 1));
console.log(JSON.stringify({ ...stats, counts: undefined }));
