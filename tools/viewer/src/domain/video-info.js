import { finite, utcText } from './time.js';
import { validPose } from './geometry.js';
import { calibration } from './thermal.js';
import { frameMaximumC } from './video-track.js';

const unknown = 'Unavailable';
const count = (n) => n.toLocaleString('en-AU');
const numeric = (n, unit = '', digits = 3) => (finite(n) ? `${n.toFixed(digits)}${unit}` : unknown);
export function metadataJSON(value) {
  return JSON.stringify(
    value,
    (_key, item) => {
      if (typeof item === 'bigint') return item.toString();
      if (item instanceof Map) return { type: 'Map', entries: [...item] };
      if (item instanceof Set) return { type: 'Set', values: [...item] };
      if (ArrayBuffer.isView(item)) {
        const bytes = new Uint8Array(item.buffer, item.byteOffset, item.byteLength);
        return {
          type: item.constructor.name,
          byteLength: item.byteLength,
          hex: Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join(''),
        };
      }
      return item;
    },
    2,
  );
}
function bytes(n) {
  if (!finite(n) || n < 0) return unknown;
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
  const index = Math.min(units.length - 1, n ? Math.floor(Math.log(n) / Math.log(1024)) : 0);
  return `${(n / 1024 ** index).toFixed(index ? 2 : 0)} ${units[index]} (${count(n)} bytes)`;
}
function aspect(width, height) {
  if (![width, height].every((n) => Number.isInteger(n) && n > 0)) return unknown;
  const gcd = (a, b) => (b ? gcd(b, a % b) : a);
  const divisor = gcd(width, height);
  return `${width / divisor}:${height / divisor}`;
}
function telemetryType(frame) {
  const record = frame.telemetry;
  if (!record) return null;
  if (record.sourceSchema === 'misb.st0601') return 'MISB ST0601 KLV';
  if (record.provenance?.format === 'apcg-sei') return 'APCG JSON SEI';
  return record.sourceSchema || record.schema || 'Embedded telemetry';
}

/** Only parsed metadata is inspected: no payload reads, decoding or pixel copies.
 * Rates and coverage describe the indexed, presented video frames, not audio
 * streams or an assumed constant frame rate.
 */
export function videoInformation(state, alignedTelemetry = state.frame?.telemetry) {
  const frames = state.frames || [],
    track = state.source?.tracks?.find(
      (t) => t.id === (state.frame?.trackId ?? state.source?.selectedTrackId ?? frames[0]?.trackId),
    ),
    file = state.file,
    first = frames.find((f) => finite(f.pts)),
    last = frames.findLast((f) => finite(f.pts)),
    span = first && last ? last.pts - first.pts : null,
    duration =
      last?.duration > 0
        ? last.duration
        : track?.defaultDuration > 0
          ? track.defaultDuration
          : span > 0 && frames.length > 1
            ? span / (frames.length - 1)
            : null,
    origin = finite(state.origin) ? state.origin : first?.pts,
    playbackDuration =
      last && finite(origin) && finite(duration) ? last.pts + duration - origin : null,
    videoDuration = finite(span) && finite(duration) ? span + duration : null,
    averageRate = span > 0 ? (frames.length - 1) / span : duration > 0 ? 1 / duration : null,
    kinds = new Set();
  let telemetry = 0,
    klv = 0,
    poses = 0,
    positions = 0,
    raw = 0,
    calibrated = 0,
    keyframes = 0,
    payload = 0,
    reordered = 0,
    firstUtc = Infinity,
    lastUtc = -Infinity,
    minInterval = Infinity,
    maxInterval = -Infinity,
    previousPts = null,
    minMaximum = Infinity,
    maxMaximum = -Infinity,
    approximations = 0;
  for (const frame of frames) {
    const record = frame.telemetry,
      type = telemetryType(frame);
    if (type) kinds.add(type);
    if (record) telemetry++;
    if (frame.klvTelemetry || record?.sourceSchema === 'misb.st0601') {
      klv++;
      kinds.add('MISB ST0601 KLV');
    }
    if (record?.position) positions++;
    if (validPose(record)) poses++;
    if (frame.thermalMetadata) {
      raw++;
      kinds.add('APCG raw thermal (apcg.thermal.v1)');
    }
    if (calibration(frame.thermalMetadata)) calibrated++;
    if (frame.keyframe) keyframes++;
    if (finite(frame.size)) payload += frame.size;
    if (finite(frame.dts) && finite(frame.pts) && Math.abs(frame.pts - frame.dts) > 1e-6)
      reordered++;
    if (finite(record?.utc_us) && record.utc_us > 0) {
      firstUtc = Math.min(firstUtc, record.utc_us);
      lastUtc = Math.max(lastUtc, record.utc_us);
    }
    if (record?.provenance?.association === 'nearest-pts') approximations++;
    if (finite(frame.pts)) {
      if (previousPts !== null && frame.pts > previousPts) {
        minInterval = Math.min(minInterval, frame.pts - previousPts);
        maxInterval = Math.max(maxInterval, frame.pts - previousPts);
      }
      previousPts = frame.pts;
    }
    const maximum = frameMaximumC(frame);
    if (finite(maximum)) {
      minMaximum = Math.min(minMaximum, maximum);
      maxMaximum = Math.max(maxMaximum, maximum);
    }
  }
  const width = track?.width ?? state.decoded?.width,
    height = track?.height ?? state.decoded?.height,
    coverage = (n) => `${count(n)} / ${count(frames.length)} frames`,
    tolerance =
      (track?.timescale > 0 ? 2 / track.timescale : 0) +
      (averageRate > 0 ? 0.001 / averageRate : 0),
    hasIntervals = finite(minInterval) && finite(maxInterval),
    codec =
      { avc: 'H.264 / AVC', hevc: 'H.265 / HEVC', ffv1: 'FFV1' }[track?.codec] ||
      track?.codec ||
      unknown,
    container =
      { mp4: 'MP4 / MOV', matroska: 'Matroska', mpegts: 'MPEG-TS' }[state.source?.format] ||
      state.source?.format ||
      unknown,
    fileDate =
      finite(file?.lastModified) && file.lastModified > 0
        ? new Date(file.lastModified).toISOString()
        : unknown;
  const sections = [
    {
      title: 'File',
      rows: [
        ['Filename', file?.name || unknown],
        ['File size', bytes(file?.size)],
        ['Reported file type', file?.type || unknown],
        ['File modified', fileDate],
        ['Container', container],
        [
          'Indexing',
          state.indexFailed
            ? 'Incomplete — playable prefix only'
            : state.indexed
              ? 'Complete'
              : `In progress · ${state.indexPercent || 0}%`,
        ],
      ],
    },
    {
      title: 'Video',
      rows: [
        ['Codec', codec],
        ['Codec identifier', track?.codecString || track?.codecId || unknown],
        ['Encoded dimensions', width > 0 && height > 0 ? `${width} × ${height}` : unknown],
        ['Aspect ratio', aspect(width, height)],
        ['Presented frames', count(frames.length)],
        ['Playback duration', numeric(playbackDuration, ' s')],
        ['Frame rate (average)', numeric(averageRate, ' fps')],
        [
          'Nominal frame rate',
          track?.defaultDuration > 0 ? numeric(1 / track.defaultDuration, ' fps') : unknown,
        ],
        [
          'Frame timing',
          hasIntervals
            ? maxInterval - minInterval <= tolerance
              ? 'Regular (within timestamp precision)'
              : 'Variable intervals / gaps'
            : unknown,
        ],
        [
          'Frame intervals',
          hasIntervals
            ? `${(minInterval * 1000).toFixed(3)}–${(maxInterval * 1000).toFixed(3)} ms`
            : unknown,
        ],
        ['Keyframes', count(keyframes)],
        ['Frames with differing PTS / DTS', count(reordered)],
        ['Timebase', track?.timescale > 0 ? `${track.timescale} ticks/s` : unknown],
        ['Video track ID', track ? String(track.id) : unknown],
        ['Indexed video tracks', count(state.source?.tracks?.length || 0)],
        ['Indexed video payload', bytes(payload)],
        [
          'Average video payload rate',
          videoDuration > 0 ? numeric((payload * 8) / videoDuration / 1e6, ' Mbit/s') : unknown,
        ],
        ['Decoder', state.decoder?.mode || unknown],
        [
          'Decoded pixels',
          state.decoded
            ? `${state.decoded.width} × ${state.decoded.height} · ${state.decoded.kind === 'gray16' ? '16-bit unsigned grayscale' : state.decoded.kind === 'rgba' ? '8-bit RGBA' : state.decoded.kind}`
            : unknown,
        ],
      ],
    },
    {
      title: 'Telemetry',
      rows: [
        ['Telemetry type', kinds.size ? [...kinds].join(' · ') : 'None found in indexed frames'],
        ['Frames with telemetry', coverage(telemetry)],
        ['Frames with position', coverage(positions)],
        ['Frames with KLV', coverage(klv)],
        ['Frames with fresh camera pose', coverage(poses)],
        ['Approximate timestamp associations', coverage(approximations)],
        ['First recorded UTC', utcText(firstUtc)],
        ['Last recorded UTC', utcText(lastUtc)],
        ['Raw thermal metadata', coverage(raw)],
        ['Calibrated raw thermal frames', coverage(calibrated)],
        [
          'Frame-maximum temperature range',
          minMaximum <= maxMaximum
            ? `${minMaximum.toFixed(2)}–${maxMaximum.toFixed(2)} °C`
            : unknown,
        ],
      ],
    },
    {
      title: 'Current frame',
      rows: [
        ['Frame', state.frame ? `${count(state.number + 1)} / ${count(frames.length)}` : unknown],
        ['Stable frame ID', state.frame ? String(state.frame.id) : unknown],
        [
          'Playback time',
          state.frame && finite(origin) ? numeric(state.frame.pts - origin, ' s') : unknown,
        ],
        ['Source PTS', numeric(state.frame?.pts, ' s', 6)],
        ['Source DTS', numeric(state.frame?.dts, ' s', 6)],
        ['Recorded UTC', utcText(state.frame?.telemetry?.utc_us)],
        ['Aligned pose UTC', utcText(alignedTelemetry?.pose_utc_us ?? alignedTelemetry?.utc_us)],
        ['Aligned camera pose', validPose(alignedTelemetry) ? 'Available' : 'Unavailable'],
        ['Horizontal FOV', numeric(alignedTelemetry?.hfov_deg, '°')],
        ['Telemetry offset', numeric(state.offset, ' s')],
        ['Frame payload', bytes(state.frame?.size)],
      ],
    },
  ];
  if (state.frame?.thermalMetadata) {
    const meta = state.frame.thermalMetadata;
    sections.push({
      title: 'Raw thermal calibration',
      rows: [
        ['Schema', meta.schema || unknown],
        ['Pixel format', meta.pixel_format || unknown],
        ['Bits per sample', finite(meta.bits_per_sample) ? String(meta.bits_per_sample) : unknown],
        ['Temperature scale', numeric(meta.temperature_scale_k, ' K/count', 8)],
        ['Temperature offset', numeric(meta.temperature_offset_k, ' K', 6)],
        ['Recorded frame minimum', numeric(meta.minimum_c, ' °C', 2)],
        ['Recorded frame maximum', numeric(meta.maximum_c, ' °C', 2)],
        ['Recorded rotation', numeric(meta.rotation_deg, '°', 0)],
        ['Original filename', meta.original_filename || unknown],
        ['Source SHA-256', meta.source_sha256 || unknown],
      ],
    });
  }
  const diagnostics = [...new Set([...(state.warnings || []), state.indexError].filter(Boolean))];
  if (diagnostics.length)
    sections.push({
      title: 'Diagnostics',
      rows: diagnostics.map((text, i) => [`Notice ${i + 1}`, text]),
    });
  return {
    sections,
    raw: {
      file: file
        ? { name: file.name, size: file.size, type: file.type, lastModified: file.lastModified }
        : null,
      container: state.source?.format ?? null,
      statistics: {
        playbackDurationSeconds: playbackDuration,
        averageFrameRate: averageRate,
        nominalFrameRate: track?.defaultDuration > 0 ? 1 / track.defaultDuration : null,
        minimumFrameIntervalSeconds: hasIntervals ? minInterval : null,
        maximumFrameIntervalSeconds: hasIntervals ? maxInterval : null,
        videoPayloadBytes: payload,
        averageVideoPayloadBitsPerSecond: videoDuration > 0 ? (payload * 8) / videoDuration : null,
        framesWithTelemetry: telemetry,
        framesWithKLV: klv,
        framesWithPosition: positions,
        framesWithFreshPose: poses,
        telemetryTypes: [...kinds],
        rawThermalFrames: raw,
        calibratedRawFrames: calibrated,
        minimumFrameMaximumC: minMaximum <= maxMaximum ? minMaximum : null,
        maximumFrameMaximumC: minMaximum <= maxMaximum ? maxMaximum : null,
      },
      indexing: {
        complete: !!state.indexed,
        incomplete: !!state.indexFailed,
        progressPercent: state.indexPercent,
        presentedFrames: frames.length,
        indexedFrames: state.source?.frames?.length ?? frames.length,
      },
      tracks: state.source?.tracks || [],
      currentFrame: state.frame || null,
      alignedTelemetry: alignedTelemetry || null,
      decoded: state.decoded
        ? {
            width: state.decoded.width,
            height: state.decoded.height,
            kind: state.decoded.kind,
            decoder: state.decoded.decoder,
          }
        : null,
      alignment: {
        offsetSeconds: state.offset,
        anchors: state.anchors || [],
        fovOverrideDegrees: state.fovOverride ?? null,
        displayRotationDegrees: state.rotation,
      },
      diagnostics,
    },
  };
}
