/** Stage 2 worker for RGB recordings (workers/runtime.js protocol). msg: {file, outDir, select
 * (RGB_DEFAULTS names), ingestDir, homeAltitude (ingest's), coreURL, workers?, decoder?}. Writes outDir/frames.json
 * (websplat.frames v1, kind rgb), features.u8 (grey [N, H, W], what SIFT reads) and rgb.u8
 * ([N, H, W, 3]); progress in selected frames. The source is checked against stage 1's index
 * and indexed again for the demuxer's packet state.
 */
import { serveStage } from '../../workers/runtime.js';
import { readJson } from '../../storage/opfs.js';
import { indexVideo, verifyRgbSource } from '../ingest/rgb.js';
import { RgbDecoder, splitRgba } from './rgb-decoder.js';
import { RGB_SELECT_VERSION, selectRgb } from './rgb-select.js';
import { OpfsStore } from './opfs-store.js';

serveStage(async (msg, api) => {
  const controller = new AbortController();
  const stopCheck = setInterval(() => api.stopped && controller.abort(), 100);
  const signal = controller.signal;
  let media, decoder;
  const t0 = performance.now();
  const files = ['features.u8', 'rgb.u8', 'frames.json'];
  let store = null;
  let complete = false;
  try {
    api.phase('load');
    const index = await readJson(`${msg.ingestDir}/index.json`);
    media = await indexVideo(msg.file, { signal });
    await verifyRgbSource(msg.file, index, media, { signal });
    const sel = selectRgb(index.frames, msg.homeAltitude, index.track, msg.select);
    for (const line of sel.log) api.log(line);
    const { width: W, height: H } = sel;
    const n = sel.entries.length;
    decoder = new RgbDecoder(
      media,
      { width: W, height: H },
      {
        coreURL: msg.coreURL,
        workers: msg.workers ?? Math.max(1, Math.min(4, (navigator.hardwareConcurrency ?? 4) - 1)),
        mode: msg.decoder ?? 'auto',
      },
    );
    store = await OpfsStore.open(msg.outDir);
    const grey = await store.open('features.u8');
    const rgb = await store.open('rgb.u8');
    const tDecode = performance.now();
    try {
      api.phase('decode');
      let k = 0;
      for await (const { index: i, rgba } of decoder.decode(sel.selected, { signal })) {
        if (i !== sel.selected[k])
          throw new Error(`decoded frame ${i}, expected ${sel.selected[k]}`);
        if (rgba.length !== W * H * 4) throw new Error(`frame ${i}: ${rgba.length} bytes decoded`);
        const s = splitRgba(rgba, W * H);
        grey.write(s.grey, k * W * H);
        rgb.write(s.rgb, k * W * H * 3);
        api.progress(++k, n);
      }
      grey.truncate(n * W * H);
      rgb.truncate(n * W * H * 3);
    } finally {
      grey.close();
      rgb.close();
    }
    if (decoder.fallback)
      api.log(
        `WebCodecs failed at keyframe group ${decoder.fallback.group} (${decoder.fallback.reason}); ` +
          'the rest was decoded with FFmpeg',
        'warn',
      );
    else if (decoder.used !== 'webcodecs')
      api.log(`decoded with FFmpeg (WebCodecs has no ${index.track.codec} decoder here)`);
    const decodeMs = performance.now() - tDecode;
    const info = {
      format: 'websplat.frames',
      version: 1,
      kind: 'rgb',
      source: msg.file.name ?? null,
      width: W,
      height: H,
      hfov_deg: sel.hfov,
      K: sel.K,
      rotation_deg: 0,
      origin: sel.origin,
      target_enu: sel.target_enu,
      frames: sel.entries,
      selection: {
        version: RGB_SELECT_VERSION,
        options: sel.params,
        source_size: [index.track.width, index.track.height],
        counts: sel.counts,
        step: sel.step,
        selected: n,
        decoder: decoder.used,
      },
    };
    await store.write('frames.json', JSON.stringify(info, null, 1));
    complete = true;
    return {
      frames: n,
      width: W,
      height: H,
      step: sel.step,
      counts: sel.counts,
      decoder: decoder.used,
      log: sel.log,
      timings: { decodeMs, totalMs: performance.now() - t0 },
      decoderStats: decoder.stats,
    };
  } finally {
    clearInterval(stopCheck);
    decoder?.close();
    media?.source.close();
    if (store && !complete) for (const f of files) await store.remove(f).catch(() => {});
  }
});
