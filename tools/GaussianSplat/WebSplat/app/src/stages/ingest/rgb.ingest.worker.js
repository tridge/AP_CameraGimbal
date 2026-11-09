/** Stage 1 worker for RGB recordings (workers/runtime.js protocol). msg: {file, outDir,
 * proposal: {range?}}. Indexes the recording (progress in bytes: index, then hash), resolves a
 * request's range, records the takeoff altitude and writes outDir/index.json
 * (websplat.ingest-rgb v1) and outDir/proposal.json (a target for the form: where the camera
 * looked at the takeoff-height ground).
 */
import { serveStage } from '../../workers/runtime.js';
import { writeJson } from '../../storage/opfs.js';
import { sourceIdentity } from './ingest.js';
import { indexVideo, rgbHomeAltitude, rgbIngestRecord, rgbTelemetrySummary } from './rgb.js';
import { proposeRgbTarget } from '../select/rgb-select.js';
import { resolveRange } from '../../request.js';

serveStage(async (msg, api) => {
  const controller = new AbortController();
  const stopCheck = setInterval(() => api.stopped && controller.abort(), 100);
  const signal = controller.signal;
  const timings = {};
  let media;
  try {
    let t = performance.now();
    api.phase('index');
    const total = msg.file.size * 2;
    media = await indexVideo(msg.file, {
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
    const index = rgbIngestRecord(media, source);
    const telemetry = rgbTelemetrySummary(index.frames);
    for (const p of telemetry.problems) api.log(p, 'warn');
    if (!telemetry.position || !telemetry.attitude)
      throw new Error(
        `this recording has no pose telemetry (position, platform and sensor attitude) in its ` +
          `${telemetry.frames} frames, so the cameras cannot be placed`,
      );
    let range = null;
    if (msg.proposal?.range) {
      const r = resolveRange(index.frames, msg.proposal.range, media.track.timescale);
      range = { ...r, frames: r.end - r.start + 1 };
      api.log(`request range: frames ${r.start}..${r.end} (${range.frames})`);
    }
    const home = rgbHomeAltitude(index.frames);
    api.log(`takeoff altitude ${home.alt.toFixed(1)} m AMSL (${home.rule})`);
    await writeJson(`${msg.outDir}/index.json`, index);
    let proposal;
    try {
      const hfov = index.frames.find((f) => f.telemetry?.hfov_deg)?.telemetry.hfov_deg ?? null;
      const p = proposeRgbTarget(index.frames, home.alt, range?.start, range?.end);
      proposal = {
        format: 'websplat.proposal',
        version: 1,
        kind: 'rgb',
        proposal: { lat: p.lat, lon: p.lon, target_alt: p.target_alt, hfov },
        homeAltitude: home.alt,
        notes: [
          `target: ${p.rule} (${p.views} views); takeoff altitude: ${home.rule}. Check the target ` +
            'height: the splat is centred on it',
        ],
      };
    } catch (e) {
      proposal = { format: 'websplat.proposal', version: 1, kind: 'rgb', error: e.message };
      api.log(`no proposal: ${e.message}`, 'warn');
    }
    await writeJson(`${msg.outDir}/proposal.json`, proposal);
    return {
      kind: 'rgb',
      frames: index.frames.length,
      container: media.format,
      track: index.track,
      source,
      telemetry,
      proposal,
      range,
      homeAltitude: home.alt,
      homeAltitudeRule: home.rule,
      timings,
    };
  } finally {
    clearInterval(stopCheck);
    media?.source.close();
  }
});
