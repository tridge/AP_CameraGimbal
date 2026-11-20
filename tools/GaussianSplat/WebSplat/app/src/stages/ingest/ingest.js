/** Stage 1 ingest: index an APCG radiometric thermal MKV with the viewer's demuxer.
 * Frame records hold packet locations and the per-frame APCG metadata (the same dict the
 * native tools read from the Matroska block addition); pixels are decoded on demand.
 */
import { openMedia } from '../../../../../../viewer/src/media/containers.js';

export const INGEST_VERSION = 1;

export class IngestError extends Error {}

/** Index `file` (File/Blob). onProgress({bytes, total, frames}). Returns the ingest result:
 * { source, track, frames: [{index, pts, timestampTicks, keyframe, offset, size, meta}], warnings }.
 * `source` keeps the demuxer for packet reads (not serialisable).
 */
export async function indexMkv(file, { signal, onProgress = () => {}, batchSize = 32 } = {}) {
  const source = await openMedia(file, { batchSize });
  if (source.format !== 'matroska') {
    source.close();
    throw new IngestError(`not a Matroska file (${source.format})`);
  }
  let track = null;
  const frames = [];
  try {
    for await (const batch of source.index({ signal, batchSize })) {
      track ??= batch.tracks.find((t) => t.codec === 'ffv1') ?? null;
      if (batch.tracks.length && !track) throw new IngestError('no FFV1 video track');
      for (const f of batch.frames) {
        if (f.trackId !== track.id) continue;
        frames.push({
          index: frames.length,
          id: f.id,
          pts: f.pts,
          timestampTicks: f.timestampTicks,
          timescale: f.timescale,
          keyframe: f.keyframe,
          offset: f.offset,
          size: f.size,
          meta: f.thermalMetadata,
        });
      }
      onProgress({ bytes: batch.progress.position, total: file.size, frames: frames.length });
      if (batch.error) throw new IngestError(`index stopped early: ${batch.error}`);
    }
  } catch (error) {
    source.close();
    throw error;
  }
  if (!frames.length) throw new IngestError('no frames');
  return {
    source,
    track: {
      id: track.id,
      codec: track.codec,
      width: track.width,
      height: track.height,
      timescale: track.timescale,
    },
    frames,
    warnings: source.warnings.slice(),
  };
}

/** What the page shows about the telemetry before selection. */
export function telemetrySummary(frames) {
  let meta = 0,
    position = 0,
    attitude = 0,
    gimbal = 0;
  for (const f of frames) {
    const t = f.meta?.telemetry;
    if (f.meta) meta++;
    if (t?.position && t.position.alt_amsl_m != null) position++;
    if (t?.vehicle_attitude) attitude++;
    if (t?.gimbal_attitude) gimbal++;
  }
  const problems = [];
  if (meta < frames.length) problems.push(`${frames.length - meta} frames have no APCG metadata`);
  if (position < frames.length) problems.push(`${frames.length - position} frames lack position`);
  if (attitude < frames.length) problems.push(`${frames.length - attitude} frames lack attitude`);
  if (gimbal < frames.length)
    problems.push(`${frames.length - gimbal} frames lack gimbal attitude`);
  return { frames: frames.length, meta, position, attitude, gimbal, problems };
}

const hex = (buf) => [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, '0')).join('');
const CHUNK = 4 * 1024 * 1024;

/** CONTRACTS.md hash of a Blob: sha256c (sha256 of the 4 MiB chunks' sha256s) */
export async function hashBlob(blob, { signal, onProgress = () => {} } = {}) {
  const digests = new Uint8Array(Math.ceil(blob.size / CHUNK) * 32);
  for (let at = 0, k = 0; at < blob.size; at += CHUNK, k++) {
    if (signal?.aborted) throw new DOMException('Aborted', 'AbortError');
    const chunk = await blob.slice(at, Math.min(at + CHUNK, blob.size)).arrayBuffer();
    digests.set(new Uint8Array(await crypto.subtle.digest('SHA-256', chunk)), k * 32);
    onProgress({ bytes: Math.min(at + CHUNK, blob.size), total: blob.size });
  }
  return 'sha256c:' + hex(await crypto.subtle.digest('SHA-256', digests));
}

/** websplat.source v1 */
export async function sourceIdentity(file, { copied = false, ...options } = {}) {
  return {
    format: 'websplat.source',
    version: 1,
    name: file.name ?? null,
    size: file.size,
    lastModified: file.lastModified ?? null,
    hash: await hashBlob(file, options),
    copied,
  };
}

/** websplat.ingest v1 record (ingest/index.json) */
export function ingestRecord(result, source) {
  return {
    format: 'websplat.ingest',
    version: INGEST_VERSION,
    source,
    track: {
      codec: result.track.codec,
      width: result.track.width,
      height: result.track.height,
      pixel_format: 'gray16le',
      timescale_ns: Math.round(1e9 / result.track.timescale),
    },
    frames: result.frames.map((f) => ({
      index: f.index,
      pts: f.timestampTicks,
      keyframe: f.keyframe,
      offset: f.offset,
      size: f.size,
      meta: f.meta,
    })),
    warnings: result.warnings,
  };
}

/** Check that `file` is the source a websplat.ingest index was made from: format and version,
 * size and content hash, then (given `ingest`, a fresh indexMkv result) the packet layout. */
export async function verifySource(file, index, ingest = null, { signal } = {}) {
  if (index?.format !== 'websplat.ingest' || index.version !== INGEST_VERSION)
    throw new IngestError(
      `ingest index is ${index?.format ?? 'missing'} v${index?.version}, not websplat.ingest v${INGEST_VERSION}`,
    );
  const want = index.source;
  if (!want?.hash || want.size !== file.size)
    throw new IngestError(
      `the source file (${file.size} bytes) is not the indexed one (${want?.size})`,
    );
  const hash = await hashBlob(file, { signal });
  if (hash !== want.hash)
    throw new IngestError('the source file content differs from the indexed one');
  if (
    ingest &&
    (index.frames.length !== ingest.frames.length ||
      index.frames.some(
        (f, i) =>
          f.offset !== ingest.frames[i].offset ||
          f.size !== ingest.frames[i].size ||
          f.pts !== ingest.frames[i].timestampTicks,
      ))
  )
    throw new IngestError('the source file does not match the ingest index');
  return hash;
}
