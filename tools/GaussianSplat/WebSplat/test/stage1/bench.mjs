/** Decode benchmark: viewer SoftwareDecoder (one frame per request) vs FrameDecoder batches. */
import { openHarness } from './harness.mjs';

const h = await openHarness('stage12-bench');
try {
  const result = await h.page.evaluate(async () => {
    const root = '/tools/GaussianSplat/WebSplat/app/src/stages/ingest/';
    const { indexMkv } = await import(root + 'ingest.js');
    const { FrameDecoder } = await import(root + 'frame-decoder.js');
    const { SoftwareDecoder } = await import('/tools/viewer/src/media/software-decoder.js');
    const file = await (await fetch('/websplat-data/input/orbit.mkv')).blob();
    let t = performance.now();
    const ingest = await indexMkv(file);
    const out = { indexMs: performance.now() - t, frames: ingest.frames.length };
    const sd = new SoftwareDecoder({ baseURL: '/' });
    await sd.open(file, ingest.source);
    const srcFrames = ingest.source.frames;
    await sd.decode(srcFrames[0]);
    t = performance.now();
    for (let i = 1; i <= 20; i++) await sd.decode(srcFrames[i]);
    out.single = { framesPerS: 20000 / (performance.now() - t) };
    sd.close();
    out.batch = [];
    for (const [workers, batchFrames] of [
      [1, 16],
      [2, 16],
      [3, 16],
      [2, 32],
      [4, 8],
    ]) {
      const dec = new FrameDecoder(ingest, { workers, batchFrames });
      t = performance.now();
      let n = 0;
      for await (const f of dec.decode(ingest.frames.map((f) => f.index))) n++;
      out.batch.push({
        workers,
        batchFrames,
        n,
        framesPerS: (n * 1000) / (performance.now() - t),
        workerDecodeMsPerFrame: dec.stats.decodeMs / n,
      });
      dec.close();
    }
    return out;
  });
  console.log(JSON.stringify(result, null, 1));
} finally {
  await h.close();
}
