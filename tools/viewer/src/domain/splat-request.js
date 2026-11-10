// websplat.request v1: the viewer's half of the WebSplat v2 contract (PLAN-v2.md).
import { finite } from './time.js';
import { validPose } from './geometry.js';

export const REQUEST_FORMAT = 'websplat.request';
export const REQUEST_VERSION = 1;
export const MAX_LOOK_POINTS = 500;
export const STEP_FRAMES = 1000;
export const LONG_RANGE_FRAMES = 3000;
export const READY_TIMEOUT_MS = 30000;
export const MAX_TITLE = 120;
export const RGB_WIDTHS = [1920, 1280, 960, 640];
export const RGB_DEFAULT_WIDTH = 1280;
export const RGB_FPS = 3;
// WebSplat's RGB_FRAME_BUDGET
export const RGB_FRAME_BUDGET = 500;
// prepare's radius about the target (m): WebSplat accepts [10, 10000]; the default is clamped
export const MIN_RADIUS = 10;
export const MAX_RADIUS = 10000;
export const DEFAULT_RADIUS_RANGE = [100, 2000];
// cap on the kept height, metres above the target: WebSplat accepts [1, 10000]
export const MIN_MAX_HEIGHT = 1;
export const MAX_MAX_HEIGHT = 10000;
const RAD = Math.PI / 180,
  DEG = 180 / Math.PI;

export class RequestError extends Error {
  constructor(field, message) {
    super(`${field}: ${message}`);
    this.name = 'RequestError';
    this.field = field;
  }
}

/** 'thermal' (FFV1 MKV), 'rgb' (H.264/HEVC in MP4 or MPEG-TS) or null. */
export function sourceKind(format, codec) {
  if (format === 'matroska' && codec === 'ffv1') return 'thermal';
  if (['mp4', 'mpegts'].includes(format) && ['avc', 'hevc'].includes(codec)) return 'rgb';
  return null;
}

const integer = (v) => Number.isSafeInteger(v);
const validFov = (fov) => finite(fov) && fov > 0 && fov < 179;

/** Why Create splat is unavailable, or null when the recording can be splatted. */
export function splatUnavailable({
  frames,
  indexed,
  indexFailed,
  format,
  codec,
  decodedKind,
  fovOverride = null,
}) {
  if (!frames?.length) return 'Open a raw thermal MKV or an RGB recording to create a splat.';
  if (!indexed) return 'Waiting for the recording to finish indexing.';
  if (indexFailed) return 'The recording did not index completely.';
  const kind = sourceKind(format, codec);
  if (!kind)
    return 'Needs an APCG raw thermal (FFV1 gray16) MKV, or an H.264/HEVC MPEG-TS or MP4 recording.';
  if (kind === 'thermal') {
    if (decodedKind && decodedKind !== 'gray16') return 'Needs 16-bit (gray16) thermal frames.';
    if (!frames.some((f) => f.thermalMetadata))
      return 'The recording has no APCG thermal metadata.';
  }
  if (!frames.some((f) => validPose(f.telemetry)))
    return 'The recording has no camera pose telemetry.';
  if (
    kind === 'rgb' &&
    !validFov(fovOverride) &&
    !frames.some((f) => validPose(f.telemetry) && validFov(f.telemetry.hfov_deg))
  )
    return 'The recording has no horizontal FOV; set a FOV override in Settings.';
  return null;
}

/** Processing widths for an RGB source: Full, then the standard widths below it. */
export function rgbWidthOptions(sourceWidth) {
  if (!integer(sourceWidth) || sourceWidth <= 0) return [];
  return [
    { width: sourceWidth, label: `Full (${sourceWidth})` },
    ...RGB_WIDTHS.filter((w) => w < sourceWidth).map((width) => ({ width, label: String(width) })),
  ];
}

export function defaultRgbWidth(sourceWidth) {
  return Math.min(RGB_DEFAULT_WIDTH, sourceWidth);
}

/** Rough RGB workload: frames sampled at RGB_FPS, thinned to RGB_FRAME_BUDGET, at width x height. */
export function rgbWorkload({ count, durationS, width, sourceWidth, sourceHeight }) {
  let frames = Math.min(count, Math.max(1, Math.ceil(durationS * RGB_FPS)));
  frames = Math.ceil(frames / Math.ceil(frames / RGB_FRAME_BUDGET));
  const height = Math.round((sourceHeight * width) / sourceWidth);
  return { frames, width, height, megapixels: (frames * width * height) / 1e6 };
}

/** Inclusive indices of the frames (sorted by pts) with startPts <= pts <= endPts, or null. */
export function rangeFrames(frames, startPts, endPts) {
  if (!finite(startPts) || !finite(endPts) || startPts > endPts) return null;
  let start = -1,
    end = -1;
  for (let i = 0; i < frames.length; i++) {
    const pts = frames[i].pts;
    if (!finite(pts) || pts < startPts - 1e-9) continue;
    if (pts > endPts + 1e-9) break;
    if (start < 0) start = i;
    end = i;
  }
  return start < 0 ? null : { start, end, count: end - start + 1 };
}

/** Packet-order index of `frame` within its track (ingest's frame list order). */
export function packetIndex(frames, frame) {
  let index = 0;
  for (const f of frames) if (f.trackId === frame.trackId && f.offset < frame.offset) index++;
  return index;
}

export function packetIdentity(frame, index) {
  const identity = { pts: frame.timestampTicks, offset: frame.offset, size: frame.size, index };
  for (const key of ['pts', 'offset', 'size', 'index'])
    if (!integer(identity[key]))
      throw new RequestError(`range.${key}`, 'frame has no packet identity');
  return identity;
}

/** Evenly spaced inclusive indices from start to end, at most max. */
export function sampleIndices(start, end, max = MAX_LOOK_POINTS) {
  const count = end - start + 1;
  if (count <= 0) return [];
  if (count <= max) return Array.from({ length: count }, (_, i) => start + i);
  return Array.from({ length: max }, (_, i) => start + Math.round((i * (count - 1)) / (max - 1)));
}

/** Where the camera's optical axis meets the terrain: {point, status}. */
export function opticalAxisGround(record, elevation) {
  if (!validPose(record)) return { point: null, status: 'unavailable' };
  // the centre pixel's ray is independent of the FOV
  const result = elevation.project({ ...record, hfov_deg: 60 }, 0.5, 0.5, 2, 2);
  return {
    point: result.point
      ? { lat: result.point.lat, lon: result.point.lon, altMsl: result.point.altMsl }
      : null,
    status: result.status,
  };
}

/** Optical-axis ground points of up to max evenly sampled frames in [start, end]. */
export function lookPoints(frames, range, recordForFrame, elevation, max = MAX_LOOK_POINTS) {
  const points = [];
  let loading = false;
  if (!range) return { points, loading };
  for (const i of sampleIndices(range.start, range.end, max)) {
    const { point, status } = opticalAxisGround(recordForFrame(frames[i]), elevation);
    if (point) points.push(point);
    else if (status === 'loading') loading = true;
  }
  return { points, loading };
}

const median = (values) => {
  const sorted = values.slice().sort((a, b) => a - b),
    middle = sorted.length >> 1;
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
};

/** Longitude difference a - b in [-180, 180). */
export function longitudeDelta(a, b) {
  return ((((a - b + 180) % 360) + 360) % 360) - 180;
}

/** Per-axis median of {lat, lon, altMsl} points, or null; longitudes unwrapped across 180°. */
export function medianPoint(points) {
  const valid = points.filter((p) => [p?.lat, p?.lon, p?.altMsl].every(finite));
  if (!valid.length) return null;
  const reference = valid[0].lon,
    lon = reference + median(valid.map((p) => longitudeDelta(p.lon, reference)));
  return {
    lat: median(valid.map((p) => p.lat)),
    lon: longitudeDelta(lon, 0),
    altMsl: median(valid.map((p) => p.altMsl)),
  };
}

/** Default splat radius: the ground width the camera sees across its view (2 d tan(HFOV / 2))
 * at the median camera-to-target distance d and median HFOV of up to max sampled frames in the
 * range. Seen from all round an orbit, frames reach about that far from the target. Rounded to
 * 10 m and clamped to DEFAULT_RADIUS_RANGE; null without poses or a FOV. */
export function footprintRadius(frames, range, recordForFrame, target, max = MAX_LOOK_POINTS) {
  if (!range || ![target?.lat, target?.lon, target?.alt_amsl].every(finite)) return null;
  const distances = [],
    fovs = [];
  for (const i of sampleIndices(range.start, range.end, max)) {
    const record = recordForFrame(frames[i]),
      pose = validPose(record);
    if (!pose || !validFov(record.hfov_deg)) continue;
    distances.push(
      viewFromCamera({ lat: pose.lat, lon: pose.lon, alt_amsl: pose.altitude }, target).distanceM,
    );
    fovs.push(record.hfov_deg);
  }
  if (!distances.length) return null;
  const width = 2 * median(distances) * Math.tan((median(fovs) * RAD) / 2);
  const [lo, hi] = DEFAULT_RADIUS_RANGE;
  return Math.min(hi, Math.max(lo, Math.round(width / 10) * 10));
}

/** Camera position seen from the target: bearing (deg from north), elevation (deg), distance (m). */
export function viewFromCamera(camera, target) {
  const a = 6378137,
    e2 = 0.00669437999014,
    s = Math.sin(target.lat * RAD),
    w = Math.sqrt(1 - e2 * s * s),
    m = (a * (1 - e2)) / (w * w * w),
    n = a / w;
  const north = (camera.lat - target.lat) * RAD * m,
    east = longitudeDelta(camera.lon, target.lon) * RAD * n * Math.cos(target.lat * RAD),
    up = camera.alt_amsl - target.alt_amsl,
    horizontal = Math.hypot(north, east);
  return {
    bearingDeg: (Math.atan2(east, north) * DEG + 360) % 360,
    elevationDeg: Math.atan2(up, horizontal) * DEG,
    distanceM: Math.hypot(horizontal, up),
  };
}

/** Inverse of ReviewMap.position(): Cesium cartographic height to metres AMSL. */
export function amslFromCartographic(height, offset) {
  return height - offset;
}

export function requestStep(frameCount) {
  return Math.max(1, Math.ceil(frameCount / STEP_FRAMES));
}

export function defaultTitle(name) {
  return String(name || '')
    .replace(/\.[^./\\]+$/, '')
    .slice(0, MAX_TITLE);
}

export function timingCorrected(offset, anchors) {
  return (finite(offset) && offset !== 0) || !!anchors?.length;
}

const round = (value, digits) => Number(value.toFixed(digits));

/** The request for frames[range.start..range.end] of `source`. Throws RequestError. */
export function buildRequest({
  kind = 'thermal',
  rgbWidth,
  source,
  frames,
  range,
  target,
  refine = true,
  hfov,
  radius,
  maxHeight,
  view,
  title,
}) {
  if (!range || range.start > range.end) throw new RequestError('range', 'no frames in range');
  const first = frames[range.start],
    last = frames[range.end];
  for (let i = range.start + 1; i <= range.end; i++)
    if (
      !(frames[i].timestampTicks > frames[i - 1].timestampTicks) ||
      !(frames[i].offset > frames[i - 1].offset)
    )
      throw new RequestError(
        'range',
        'frame timestamps are not increasing in file order (B-frames, as in a re-encoded ' +
          "file?): use the camera's own recording",
      );
  const request = {
    format: REQUEST_FORMAT,
    version: REQUEST_VERSION,
    kind,
    source: { name: source.name, size: source.size, lastModified: source.lastModified },
    range: {
      timescale: first.timescale,
      start: packetIdentity(first, packetIndex(frames, first)),
      end: packetIdentity(last, packetIndex(frames, last)),
    },
    target: {
      lat: round(target.lat, 7),
      lon: round(target.lon, 7),
      alt_amsl: round(target.alt_amsl, 2),
      refine: kind === 'rgb' ? false : !!refine,
    },
    hfov: hfov ?? null,
  };
  if (kind === 'rgb') request.rgb = { width: rgbWidth };
  if (finite(radius)) request.radius = Math.round(radius);
  if (finite(maxHeight)) request.max_height = maxHeight;
  if (view)
    request.view = {
      camera: {
        lat: round(view.camera.lat, 7),
        lon: round(view.camera.lon, 7),
        alt_amsl: round(view.camera.alt_amsl, 2),
      },
      fov_deg: round(view.fov_deg, 3),
    };
  const text = String(title ?? '').trim();
  if (text) request.title = text;
  return validateRequest(request);
}

function object(value, field, keys, required = keys) {
  if (!value || typeof value !== 'object' || Array.isArray(value))
    throw new RequestError(field, 'must be an object');
  for (const key of Object.keys(value))
    if (!keys.includes(key)) throw new RequestError(`${field}.${key}`, 'unknown key');
  for (const key of required)
    if (!(key in value)) throw new RequestError(`${field}.${key}`, 'is required');
  return value;
}

function number(value, field, min, max) {
  if (!finite(value) || value < min || value > max)
    throw new RequestError(field, `must be a number in [${min}, ${max}]`);
  return value;
}

function count(value, field, min = 0) {
  if (!integer(value) || value < min) throw new RequestError(field, `must be an integer >= ${min}`);
  return value;
}

function position(value, field) {
  object(value, field, ['lat', 'lon', 'alt_amsl']);
  return {
    lat: number(value.lat, `${field}.lat`, -90, 90),
    lon: number(value.lon, `${field}.lon`, -180, 180),
    alt_amsl: number(value.alt_amsl, `${field}.alt_amsl`, -500, 9000),
  };
}

function endpoint(value, field) {
  object(value, field, ['pts', 'offset', 'size', 'index']);
  if (!integer(value.pts)) throw new RequestError(`${field}.pts`, 'must be an integer');
  return {
    pts: value.pts,
    offset: count(value.offset, `${field}.offset`),
    size: count(value.size, `${field}.size`, 1),
    index: count(value.index, `${field}.index`),
  };
}

/** Strict websplat.request v1 check; returns a normalised copy or throws RequestError. */
export function validateRequest(value) {
  object(
    value,
    'request',
    [
      'format',
      'version',
      'kind',
      'source',
      'range',
      'target',
      'hfov',
      'rgb',
      'radius',
      'max_height',
      'view',
      'title',
    ],
    ['format', 'version', 'source', 'range', 'target'],
  );
  if (value.format !== REQUEST_FORMAT)
    throw new RequestError('request.format', `must be ${REQUEST_FORMAT}`);
  if (value.version !== REQUEST_VERSION)
    throw new RequestError('request.version', `must be ${REQUEST_VERSION}`);
  const kind = value.kind ?? 'thermal';
  if (!['thermal', 'rgb'].includes(kind))
    throw new RequestError('request.kind', 'must be thermal or rgb');
  const source = object(value.source, 'request.source', ['name', 'size', 'lastModified']);
  if (typeof source.name !== 'string' || !source.name)
    throw new RequestError('request.source.name', 'must be a non-empty string');
  if (!finite(source.lastModified))
    throw new RequestError('request.source.lastModified', 'must be a number');
  const range = object(value.range, 'request.range', ['timescale', 'start', 'end']);
  if (!finite(range.timescale) || range.timescale <= 0)
    throw new RequestError('request.range.timescale', 'must be a positive number');
  const start = endpoint(range.start, 'request.range.start'),
    end = endpoint(range.end, 'request.range.end');
  if (start.pts > end.pts || start.offset > end.offset || start.index > end.index)
    throw new RequestError('request.range', 'start must not be after end');
  const target = object(value.target, 'request.target', ['lat', 'lon', 'alt_amsl', 'refine']);
  if (typeof target.refine !== 'boolean')
    throw new RequestError('request.target.refine', 'must be true or false');
  if (kind === 'rgb' && target.refine !== false)
    throw new RequestError('request.target.refine', 'must be false for rgb');
  const result = {
    format: REQUEST_FORMAT,
    version: REQUEST_VERSION,
    kind,
    source: {
      name: source.name,
      size: count(source.size, 'request.source.size'),
      lastModified: source.lastModified,
    },
    range: { timescale: range.timescale, start, end },
    target: {
      ...position(
        { lat: target.lat, lon: target.lon, alt_amsl: target.alt_amsl },
        'request.target',
      ),
      refine: target.refine,
    },
    hfov:
      value.hfov === undefined || value.hfov === null
        ? null
        : number(value.hfov, 'request.hfov', 10, 120),
  };
  if (kind === 'rgb') {
    if (value.rgb === undefined) throw new RequestError('request.rgb', 'is required for rgb');
    const rgb = object(value.rgb, 'request.rgb', ['width']);
    if (!integer(rgb.width) || rgb.width < 64 || rgb.width > 16384)
      throw new RequestError('request.rgb.width', 'must be an integer in [64, 16384]');
    result.rgb = { width: rgb.width };
  } else if (value.rgb !== undefined) throw new RequestError('request.rgb', 'only allowed for rgb');
  if (value.radius !== undefined)
    result.radius = number(value.radius, 'request.radius', MIN_RADIUS, MAX_RADIUS);
  if (value.max_height !== undefined)
    result.max_height = number(
      value.max_height,
      'request.max_height',
      MIN_MAX_HEIGHT,
      MAX_MAX_HEIGHT,
    );
  if (value.view !== undefined) {
    const view = object(value.view, 'request.view', ['camera', 'fov_deg']);
    result.view = {
      camera: position(view.camera, 'request.view.camera'),
      fov_deg: number(view.fov_deg, 'request.view.fov_deg', 10, 120),
    };
  }
  if (value.title !== undefined) {
    if (typeof value.title !== 'string' || value.title.length > MAX_TITLE)
      throw new RequestError(
        'request.title',
        `must be a string of at most ${MAX_TITLE} characters`,
      );
    result.title = value.title;
  }
  return result;
}

/** 128 random bits in hex. */
export function newNonce(random = (bytes) => crypto.getRandomValues(bytes)) {
  return [...random(new Uint8Array(16))].map((b) => b.toString(16).padStart(2, '0')).join('');
}

export function handoverUrl(websplatUrl, base, nonce) {
  const url = new URL(websplatUrl || '../websplat/', base);
  url.search = '';
  url.hash = '';
  url.searchParams.set('request', nonce);
  return url;
}

/** Only the opened window, from the WebSplat origin, with this handover's nonce. */
export function handoverMessage(event, { popup, origin, nonce }) {
  const data = event?.data;
  if (event?.source !== popup || event.origin !== origin) return null;
  if (!data || typeof data !== 'object' || data.nonce !== nonce || typeof data.type !== 'string')
    return null;
  return data;
}

export function deepFreeze(value) {
  if (value && typeof value === 'object') {
    for (const item of Object.values(value)) deepFreeze(item);
    Object.freeze(value);
  }
  return value;
}
