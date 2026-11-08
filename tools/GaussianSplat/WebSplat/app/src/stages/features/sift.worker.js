// Stage 3 pool worker: one colmap-wasm fork instance running SIFT on one image per message.
// in:  {type: 'sift', id, k, grey: Uint8Array (transferred), width, height, maxNumFeatures}
// out: {type: 'done', id, k, keypoints, descriptors, ms, heap}  {type: 'error', id, name, message}
import { loadColmap } from '../map/colmap.js';
import { siftImage } from './sift.js';

let cw = null;
self.onmessage = async ({ data: m }) => {
  try {
    cw ??= await loadColmap({ onLog: (text, level) => level === 'warn' && console.warn(text) });
    const t = performance.now();
    const r = siftImage(cw, m.grey, m.width, m.height, { maxNumFeatures: m.maxNumFeatures });
    self.postMessage(
      { type: 'done', id: m.id, k: m.k, ...r, ms: performance.now() - t, heap: cw.heapBytes },
      [r.keypoints.buffer, r.descriptors.buffer],
    );
  } catch (e) {
    self.postMessage({ type: 'error', id: m.id, name: e.name, message: e.message });
  }
};
