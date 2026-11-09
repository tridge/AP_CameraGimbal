// websplat.request v1: a job request from the video viewer (CONTRACTS.md). Pure (no DOM), so it
// runs in the page, in workers and in node tests.
import { hashJson } from './storage/hash.js';

export const REQUEST_FORMAT = 'websplat.request';
export const REQUEST_VERSION = 1;
export const MAX_TITLE = 120;
// candidate frames a request job passes to selection at most (step = ceil(frames / this))
export const FRAME_BUDGET = 1000;
// RGB frames bring far more matches than thermal ones: mapping (4 GB wasm heap, about 1.6 GB for
// 168 frames at 8192 features) bounds them
export const RGB_FRAME_BUDGET = 500;
// selection margin for a picked (unrefined) target: telemetry poses can be tens of metres off,
// so the target may project up to a quarter frame outside the image (select margin default 0.1)
export const PICKED_MARGIN = -0.25;
export const KINDS = ['thermal', 'rgb'];
export const MIN_RGB_WIDTH = 64;
export const MAX_RGB_WIDTH = 16384;

export class RequestError extends Error {
  name = 'RequestError';
}

const fail = (field, why) => {
  throw new RequestError(`request ${field}: ${why}`);
};
const isObject = (v) => v !== null && typeof v === 'object' && !Array.isArray(v);

function object(v, field, required, optional = []) {
  if (!isObject(v)) fail(field, 'must be an object');
  for (const k of Object.keys(v))
    if (!required.includes(k) && !optional.includes(k)) fail(field, `unknown key "${k}"`);
  for (const k of required) if (!(k in v)) fail(field, `missing "${k}"`);
  return v;
}

function number(v, field, lo = -Infinity, hi = Infinity) {
  if (typeof v !== 'number' || !Number.isFinite(v)) fail(field, 'must be a finite number');
  if (v < lo || v > hi) fail(field, `${v} is outside [${lo}, ${hi}]`);
  return v;
}

function integer(v, field, lo = Number.MIN_SAFE_INTEGER) {
  if (!Number.isSafeInteger(v)) fail(field, 'must be an integer');
  if (v < lo) fail(field, `must be at least ${lo}`);
  return v;
}

const lat = (v, f) => number(v, f, -90, 90);
const lon = (v, f) => number(v, f, -180, 180);
const height = (v, f) => number(v, f, -500, 9000);
const fov = (v, f) => number(v, f, 10, 120);

function endpoint(v, field) {
  object(v, field, ['pts', 'offset', 'size'], ['index']);
  return {
    pts: integer(v.pts, `${field}.pts`),
    offset: integer(v.offset, `${field}.offset`, 0),
    size: integer(v.size, `${field}.size`, 1),
    index: v.index == null ? null : integer(v.index, `${field}.index`, 0),
  };
}

/** Check a request (strict: known keys only) and return it normalised (optional fields null). */
export function validateRequest(obj) {
  object(
    obj,
    'request',
    ['format', 'version', 'source', 'range', 'target'],
    ['kind', 'rgb', 'hfov', 'view', 'title'],
  );
  if (obj.format !== REQUEST_FORMAT) fail('format', `"${obj.format}" is not ${REQUEST_FORMAT}`);
  if (obj.version !== REQUEST_VERSION)
    fail('version', `${obj.version} is not supported (only ${REQUEST_VERSION})`);
  const kind = obj.kind ?? 'thermal';
  if (!KINDS.includes(kind)) fail('kind', `"${kind}" is not one of ${KINDS.join(', ')}`);
  let rgb = null;
  if (kind === 'rgb') {
    if (obj.rgb == null) fail('rgb', 'is required for an rgb request');
    object(obj.rgb, 'rgb', ['width']);
    rgb = { width: integer(obj.rgb.width, 'rgb.width', MIN_RGB_WIDTH) };
    if (rgb.width > MAX_RGB_WIDTH) fail('rgb.width', `${rgb.width} is above ${MAX_RGB_WIDTH}`);
  } else if (obj.rgb != null) fail('rgb', 'is only allowed for an rgb request');

  const s = object(obj.source, 'source', ['name', 'size'], ['lastModified']);
  if (typeof s.name !== 'string' || !s.name) fail('source.name', 'must be a non-empty string');
  const source = {
    name: s.name,
    size: integer(s.size, 'source.size', 0),
    lastModified: s.lastModified == null ? null : number(s.lastModified, 'source.lastModified'),
  };

  const r = object(obj.range, 'range', ['timescale', 'start', 'end']);
  const range = {
    timescale: number(r.timescale, 'range.timescale', Number.MIN_VALUE),
    start: endpoint(r.start, 'range.start'),
    end: endpoint(r.end, 'range.end'),
  };
  if (range.start.pts > range.end.pts) fail('range', 'start is after end');

  const t = object(obj.target, 'target', ['lat', 'lon', 'alt_amsl', 'refine']);
  if (typeof t.refine !== 'boolean') fail('target.refine', 'must be true or false');
  if (kind === 'rgb' && t.refine) fail('target.refine', 'must be false for rgb (no hot spot)');
  const target = {
    lat: lat(t.lat, 'target.lat'),
    lon: lon(t.lon, 'target.lon'),
    alt_amsl: height(t.alt_amsl, 'target.alt_amsl'),
    refine: t.refine,
  };

  const hfov = obj.hfov == null ? null : fov(obj.hfov, 'hfov');

  let view = null;
  if (obj.view != null) {
    const v = object(obj.view, 'view', ['camera', 'fov_deg']);
    const c = object(v.camera, 'view.camera', ['lat', 'lon', 'alt_amsl']);
    view = {
      camera: {
        lat: lat(c.lat, 'view.camera.lat'),
        lon: lon(c.lon, 'view.camera.lon'),
        alt_amsl: height(c.alt_amsl, 'view.camera.alt_amsl'),
      },
      fov_deg: fov(v.fov_deg, 'view.fov_deg'),
    };
  }

  let title = null;
  if (obj.title != null) {
    if (typeof obj.title !== 'string') fail('title', 'must be a string');
    if (obj.title.length > MAX_TITLE) fail('title', `longer than ${MAX_TITLE} characters`);
    title = obj.title.trim() || null;
  }
  return {
    format: REQUEST_FORMAT,
    version: REQUEST_VERSION,
    kind,
    rgb,
    source,
    range,
    target,
    hfov,
    view,
    title,
  };
}

/** The fields that affect a job's outputs (endpoint `index` is only a hint, so not included).
 * kind and rgb only for rgb, so thermal identities (and job ids) keep the v1 form. */
export function requestIdentity(req) {
  const end = ({ pts, offset, size }) => ({ pts, offset, size });
  return {
    ...(req.kind === 'rgb' && { kind: 'rgb', rgb: { width: req.rgb.width } }),
    range: { timescale: req.range.timescale, start: end(req.range.start), end: end(req.range.end) },
    target: { ...req.target },
    hfov: req.hfov,
    view: req.view && { camera: { ...req.view.camera }, fov_deg: req.view.fov_deg },
  };
}

/** First 16 hex digits of the identity's canonical-JSON hash (part of the job id). */
export async function requestKey(req) {
  return (await hashJson(requestIdentity(req))).split(':')[1].slice(0, 16);
}

/** The packet-order index interval [start, end] of `range` in an ingest frame list ({offset,
 * size, pts} as index.json, pts in ticks of `timescale`). Throws RequestError if an endpoint
 * is not a frame of the recording or the pts in the range are not strictly increasing. */
export function resolveRange(frames, range, timescale) {
  if (timescale !== range.timescale)
    throw new RequestError(
      `the request's timescale ${range.timescale} is not the recording's (${timescale})`,
    );
  const find = (e, name) => {
    const i = frames.findIndex((f) => f.offset === e.offset);
    const f = frames[i];
    if (!f) throw new RequestError(`the range ${name} (offset ${e.offset}) is not a video frame`);
    if (f.size !== e.size || f.pts !== e.pts)
      throw new RequestError(
        `the range ${name} does not match frame ${i} (size ${f.size}, pts ${f.pts}; request ` +
          `size ${e.size}, pts ${e.pts}): is this the same recording?`,
      );
    return i;
  };
  const start = find(range.start, 'start');
  const end = find(range.end, 'end');
  if (start > end)
    throw new RequestError(`the range start (frame ${start}) is after its end (${end})`);
  for (let i = start; i <= end; i++) {
    if (!Number.isSafeInteger(frames[i].pts))
      throw new RequestError(`frame ${i} in the range has no timestamp`);
    if (i > start && frames[i].pts <= frames[i - 1].pts)
      throw new RequestError(
        `frame timestamps in the range are not increasing (frame ${i}: ${frames[i].pts} after ` +
          `${frames[i - 1].pts}), so the viewer's range cannot be mapped to frames`,
      );
  }
  return { start, end };
}

/** config.select for a request job, from ingest's result (range, homeAltitude, proposal). */
export function selectConfig(identity, ingest) {
  const range = ingest?.range;
  if (!range) throw new RequestError('ingest did not resolve the request range');
  if (!Number.isFinite(ingest.homeAltitude))
    throw new RequestError('the recording gives no takeoff altitude (no relative altitude)');
  if (identity.kind === 'rgb')
    // rgb select samples the range at its fps, then thins to the budget (no hot spot, no rotation)
    return {
      lat: identity.target.lat,
      lon: identity.target.lon,
      target_alt: identity.target.alt_amsl - ingest.homeAltitude,
      hfov: identity.hfov,
      width: identity.rgb.width,
      start: range.start,
      end: range.end,
      budget: RGB_FRAME_BUDGET,
    };
  const p = ingest.proposal?.proposal ?? {};
  return {
    lat: identity.target.lat,
    lon: identity.target.lon,
    target_alt: identity.target.alt_amsl - ingest.homeAltitude,
    locate_hotspot: identity.target.refine,
    ...(identity.target.refine ? {} : { margin: PICKED_MARGIN }),
    rotation: p.rotation ?? null,
    hfov: identity.hfov ?? p.hfov ?? null,
    start: range.start,
    end: range.end,
    step: Math.max(1, Math.ceil(range.frames / FRAME_BUDGET)),
  };
}

const LOCAL = /^http:\/\/(localhost|127\.0\.0\.1)(:\d+)?$/;

/** Whether a message from `origin` may hand a request to a page served from `pageOrigin`: the
 * same origin, or any local development origin when the page is itself local. */
export function trustedOrigin(origin, pageOrigin) {
  return origin === pageOrigin || (LOCAL.test(pageOrigin) && LOCAL.test(origin));
}

export const isLocalOrigin = (origin) => LOCAL.test(origin);
export const validNonce = (n) => typeof n === 'string' && /^[0-9a-f]{32}$/i.test(n);
