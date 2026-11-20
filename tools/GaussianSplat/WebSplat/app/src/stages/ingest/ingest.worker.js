/** Stage 1 worker (workers/runtime.js protocol). msg: {file, outDir, coreURL, verify?, workers?,
 * proposal?}. Indexes the MKV (progress in bytes), records the source identity, proposes the
 * selection options (decoding the hot frames) and writes outDir/index.json (websplat.ingest v1)
 * and outDir/proposal.json. proposal.range (a request's range) limits the proposal to those
 * frames. verify decodes every frame and returns its SHA-256 (gray16le).
 */
import { serveStage } from '../../workers/runtime.js';
import { writeJson } from '../../storage/opfs.js';
import { indexMkv, ingestRecord, sourceIdentity, telemetrySummary } from './ingest.js';
import { FrameDecoder } from './frame-decoder.js';
import { homeAltitude, propose, PROPOSAL_KEYS } from '../select/select.js';
import { resolveRange } from '../../request.js';

// only the settings the stage is keyed on may influence the proposal
const pick = (o = {}) =>
  Object.fromEntries(PROPOSAL_KEYS.filter((k) => o[k] !== undefined).map((k) => [k, o[k]]));

const hex = (buf) => [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, '0')).join('');

serveStage(async (msg, api) => {
  const controller = new AbortController();
  const stopCheck = setInterval(() => api.stopped && controller.abort(), 100);
  const signal = controller.signal;
  const timings = {};
  let ingest, decoder;
  try {
    let t = performance.now();
    api.phase('index');
    // bytes: indexing, then hashing the source (each counted once)
    const total = msg.file.size * 2;
    ingest = await indexMkv(msg.file, {
      signal,
      onProgress: ({ bytes }) => api.progress(bytes, total),
    });
    timings.indexMs = performance.now() - t;
    t = performance.now();
    api.phase('hash');
    const source = await sourceIdentity(msg.file, {
      signal,
      onProgress: ({ bytes }) => api.progress(msg.file.size + bytes, total),
    });
    timings.hashMs = performance.now() - t;
    const index = ingestRecord(ingest, source);
    const telemetry = telemetrySummary(ingest.frames);
    for (const p of telemetry.problems) api.log(p, 'warn');
    // mapping is seeded by the vehicle telemetry: without any there is nothing to place the cameras
    if (!telemetry.position || !telemetry.attitude)
      throw new Error(
        `this recording has no vehicle telemetry (position and attitude) in its ${telemetry.frames} ` +
          'frames, so the cameras cannot be placed; record with the autopilot connected to the gimbal',
      );
    let range = null;
    if (msg.proposal?.range) {
      const r = resolveRange(index.frames, msg.proposal.range, ingest.track.timescale);
      range = { ...r, frames: r.end - r.start + 1 };
      api.log(`request range: frames ${r.start}..${r.end} (${range.frames})`);
    }
    let home = null;
    try {
      home = homeAltitude(ingest.frames.map((f) => f.meta));
    } catch (e) {
      api.log(`no takeoff altitude: ${e.message}`, 'warn');
    }
    await writeJson(`${msg.outDir}/index.json`, index);

    decoder = new FrameDecoder(ingest, { coreURL: msg.coreURL, workers: msg.workers });
    t = performance.now();
    api.phase('propose');
    let proposal = null;
    try {
      const { samples, ...rest } = await propose(ingest, decoder, {
        params: range
          ? { ...pick(msg.proposal), start: range.start, end: range.end }
          : pick(msg.proposal),
        signal,
      });
      proposal = { format: 'websplat.proposal', version: 1, ...rest };
    } catch (e) {
      if (e.name === 'AbortError') throw e;
      proposal = { format: 'websplat.proposal', version: 1, error: e.message };
      api.log(`no proposal: ${e.message}`, 'warn');
    }
    timings.proposeMs = performance.now() - t;
    await writeJson(`${msg.outDir}/proposal.json`, proposal);

    let hashes = null;
    if (msg.verify) {
      t = performance.now();
      api.phase('verify');
      hashes = [];
      for await (const f of decoder.decode(
        ingest.frames.map((f) => f.index),
        { signal },
      )) {
        hashes[f.index] = hex(await crypto.subtle.digest('SHA-256', f.data));
        api.progress(hashes.length, ingest.frames.length);
      }
      timings.decodeMs = performance.now() - t;
    }
    return {
      frames: ingest.frames.length,
      source,
      telemetry,
      proposal,
      range,
      homeAltitude: home,
      hashes,
      timings,
      decoderStats: decoder.stats,
    };
  } finally {
    clearInterval(stopCheck);
    decoder?.close();
    ingest?.source.close();
  }
});
