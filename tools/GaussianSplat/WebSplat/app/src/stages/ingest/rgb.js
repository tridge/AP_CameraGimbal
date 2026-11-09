/** Stage 1 for RGB recordings (H.264/HEVC in MPEG-TS or MP4 with MISB ST 0601 KLV or APCG SEI):
 * index with the viewer's demuxer; per-frame pose telemetry as the viewer normalises it
 * (apcg.telemetry.v1, the fields selection reads). Pixels are decoded by stage 2.
 */
import { openMedia } from '../../../../../../viewer/src/media/containers.js';
import { hashBlob } from './ingest.js';
import { homeAltitude, SelectError } from '../select/select.js';

export const RGB_INGEST_VERSION = 1;
export const RGB_INGEST_FORMAT = 'websplat.ingest-rgb';

export class RgbIngestError extends Error {}

// the pose fields stage 2 reads (CONTRACTS.md 1-rgb)
export function poseTelemetry(t) {
  if (!t) return null;
  const p = t.position;
  return {
    utc_us: typeof t.utc_us === 'number' ? t.utc_us : null,
    position: p
      ? {
          lat_e7: p.lat_e7,
          lon_e7: p.lon_e7,
          alt_amsl_m: p.alt_amsl_m ?? null,
          alt_relative_m: p.alt_relative_m ?? null,
        }
      : null,
    vehicle_attitude: t.vehicle_attitude
      ? {
          roll_rad: t.vehicle_attitude.roll_rad,
          pitch_rad: t.vehicle_attitude.pitch_rad,
          yaw_rad: t.vehicle_attitude.yaw_rad,
        }
      : null,
    gimbal_attitude: t.gimbal_attitude
      ? {
          roll_rad: t.gimbal_attitude.roll_rad,
          pitch_rad: t.gimbal_attitude.pitch_rad,
          yaw_rad: t.gimbal_attitude.yaw_rad,
        }
      : null,
    hfov_deg: Number.isFinite(t.hfov_deg) ? t.hfov_deg : null,
    // a KLV sample associated by nearest timestamp: its distance from the frame
    offset_ms: t.provenance?.metadata_offset_ms ?? 0,
    source: t.sourceSchema ?? t.schema ?? null,
  };
}

/** Index `file`. Returns {source (demuxer, keeps packet state), format, track, frames: [{index,
 * pts (ticks), timescale, keyframe, offset, size, telemetry}], demuxed (the demuxer's frames, for
 * decoding), warnings}. */
export async function indexVideo(file, { signal, onProgress = () => {}, batchSize = 256 } = {}) {
  const source = await openMedia(file, { batchSize });
  if (!['mpegts', 'mp4'].includes(source.format)) {
    source.close();
    throw new RgbIngestError(
      `not an RGB recording: ${source.format} (an MPEG-TS or MP4 with H.264/HEVC is needed; ` +
        'thermal MKVs make thermal splats)',
    );
  }
  let track = null;
  const frames = [];
  const demuxed = [];
  try {
    for await (const batch of source.index({ signal, batchSize })) {
      track ??= batch.tracks.find((t) => ['avc', 'hevc'].includes(t.codec)) ?? null;
      if (batch.tracks.length && !track) throw new RgbIngestError('no H.264 or HEVC video track');
      for (const f of batch.frames) {
        if (!track || f.trackId !== track.id) continue;
        demuxed.push(f);
        frames.push({
          index: frames.length,
          pts: f.timestampTicks,
          timescale: f.timescale,
          keyframe: f.keyframe,
          offset: f.offset,
          size: f.size,
          telemetry: poseTelemetry(f.telemetry),
        });
      }
      onProgress({ bytes: batch.progress.position, total: file.size, frames: frames.length });
      if (batch.error) throw new RgbIngestError(`index stopped early: ${batch.error}`);
    }
  } catch (e) {
    source.close();
    throw e;
  }
  if (!frames.length) {
    source.close();
    throw new RgbIngestError('no video frames');
  }
  return {
    source,
    format: source.format,
    track: {
      id: track.id,
      codec: track.codec,
      width: track.width,
      height: track.height,
      timescale: frames[0].timescale,
    },
    frames,
    demuxed,
    warnings: source.warnings.slice(),
  };
}

/** Takeoff altitude: APCG SEI carries relative altitude (the thermal rule: median of
 * alt_amsl - alt_relative); KLV does not, so extract_ts_frames.py's rule applies: the first
 * record's GPS altitude. Returns {alt, rule} or throws. */
export function rgbHomeAltitude(frames) {
  const metas = frames.map((f) => ({ telemetry: f.telemetry }));
  if (frames.some((f) => f.telemetry?.position?.alt_relative_m != null)) {
    try {
      return { alt: homeAltitude(metas), rule: 'median of alt_amsl - alt_relative' };
    } catch (e) {
      if (!(e instanceof SelectError)) throw e;
    }
  }
  const first = frames.find((f) => Number.isFinite(f.telemetry?.position?.alt_amsl_m));
  if (!first) throw new RgbIngestError('no frame carries a position');
  return {
    alt: first.telemetry.position.alt_amsl_m,
    rule: 'first record (KLV has no relative altitude)',
  };
}

export function rgbTelemetrySummary(frames) {
  let position = 0,
    attitude = 0,
    hfov = 0;
  for (const f of frames) {
    const t = f.telemetry;
    if (t?.position && t.position.alt_amsl_m != null) position++;
    if (t?.vehicle_attitude && t?.gimbal_attitude) attitude++;
    if (t?.hfov_deg) hfov++;
  }
  const problems = [];
  if (position < frames.length) problems.push(`${frames.length - position} frames lack position`);
  if (attitude < frames.length)
    problems.push(`${frames.length - attitude} frames lack platform or sensor attitude`);
  return { frames: frames.length, position, attitude, hfov, problems };
}

/** websplat.ingest-rgb v1 (ingest/index.json) */
export function rgbIngestRecord(result, source) {
  return {
    format: RGB_INGEST_FORMAT,
    version: RGB_INGEST_VERSION,
    source,
    container: result.format,
    track: {
      codec: result.track.codec,
      width: result.track.width,
      height: result.track.height,
      timescale: result.track.timescale,
    },
    frames: result.frames.map(({ timescale, ...f }) => f),
    warnings: result.warnings,
  };
}

/** The source must be the one a websplat.ingest-rgb index was made from: size, content hash and
 * (given a fresh indexVideo result) the packet layout. */
export async function verifyRgbSource(file, index, fresh = null, { signal } = {}) {
  if (index?.format !== RGB_INGEST_FORMAT || index.version !== RGB_INGEST_VERSION)
    throw new RgbIngestError(
      `ingest index is ${index?.format ?? 'missing'} v${index?.version}, not ${RGB_INGEST_FORMAT} v${RGB_INGEST_VERSION}`,
    );
  if (index.source?.size !== file.size)
    throw new RgbIngestError('the source file is not the indexed one (size)');
  if ((await hashBlob(file, { signal })) !== index.source.hash)
    throw new RgbIngestError('the source file content differs from the indexed one');
  if (
    fresh &&
    (index.frames.length !== fresh.frames.length ||
      index.frames.some(
        (f, i) =>
          f.offset !== fresh.frames[i].offset ||
          f.size !== fresh.frames[i].size ||
          f.pts !== fresh.frames[i].pts,
      ))
  )
    throw new RgbIngestError('the source file does not match the ingest index');
}
