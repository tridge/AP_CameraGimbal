/** Stage 2 worker (workers/runtime.js protocol). msg: {file, outDir, select (selection options,
 * CONTRACTS.md names), ingestDir, coreURL, mode?, workers?}. Writes outDir/frames.json,
 * features.u8 and thermal.u16 (websplat.frames v1); progress in selected frames.
 * The source is verified against stage 1's index (size, sha256c, packet layout) and indexed
 * again (tens of ms) because packet decoding needs the demuxer's track state.
 */
import { serveStage } from '../../workers/runtime.js';
import { readJson } from '../../storage/opfs.js';
import { indexMkv, verifySource } from '../ingest/ingest.js';
import { FrameDecoder } from '../ingest/frame-decoder.js';
import { selectFrames } from './select.js';
import { OpfsStore } from './opfs-store.js';

serveStage(async (msg, api) => {
  const controller = new AbortController();
  const stopCheck = setInterval(() => api.stopped && controller.abort(), 100);
  const signal = controller.signal;
  let ingest, decoder;
  try {
    const t0 = performance.now();
    // the source must be the one stage 1 indexed: format/version, size, hash, packet layout
    if (!msg.ingestDir) throw new Error('select needs the ingest stage output (ingestDir)');
    api.phase('load'); // re-index and source check: setup, not throughput
    const index = await readJson(`${msg.ingestDir}/index.json`);
    ingest = await indexMkv(msg.file, { signal });
    await verifySource(msg.file, index, ingest, { signal });
    decoder = new FrameDecoder(ingest, { coreURL: msg.coreURL, workers: msg.workers });
    const store = await OpfsStore.open(msg.outDir, { faultAfterBytes: msg.testFault?.writeBytes });
    let phase = null;
    let scanned = 0;
    const twoPass = (msg.mode ?? 'single') !== 'single';
    const r = await selectFrames(ingest, decoder, store, msg.select, {
      mode: msg.mode ?? 'single',
      signal,
      sourceName: msg.file.name ?? null,
      // one monotonic count for the stage: single mode makes one pass; redecode and cache add an
      // output pass over about as many frames again until its size is known
      onProgress: (p) => {
        if (p.phase !== phase) api.phase((phase = p.phase));
        if (!twoPass) api.progress(p.done, p.total);
        else if (p.phase === 'select') {
          scanned = p.total;
          api.progress(p.done, 2 * p.total);
        } else api.progress(scanned + p.done, scanned + p.total);
      },
    });
    for (const line of r.log) api.log(line);
    return {
      log: r.log,
      selected: r.selected.length,
      frames: r.info.frames.length,
      blank: r.blank,
      drifted: r.drifted,
      hotspot: r.hotspot,
      timings: { ...r.timings, totalMs: performance.now() - t0 },
      framesDecoded: decoder.stats.frames,
      bytesWritten: store.bytesWritten,
    };
  } finally {
    clearInterval(stopCheck);
    decoder?.close();
    ingest?.source.close();
  }
});
