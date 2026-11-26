/** Live APCG Matroska as SupportProxy relays it: a prefix, then one complete Cluster per frame. */
import { parseMatroskaHead, parseMatroskaCluster } from './containers.js';

const EBML = 0x1a45dfa3,
  SEGMENT = 0x18538067,
  INFO = 0x1549a966,
  TRACKS = 0x1654ae6b,
  CLUSTER = 0x1f43b675,
  TIMESTAMP = 0xe7,
  DURATION = 0x4489;
// the relay's own limits (videomkv.h)
export const MAX_HEAD_ELEMENT = 65536,
  MAX_PREFIX = 131072,
  MAX_CLUSTER = 4 * 1024 * 1024;
const SAME_EPOCH_SLACK_MS = 10000,
  MIN_EPOCH_GAP_MS = 40;

export class LiveError extends Error {
  /** terminal: reconnecting cannot help (wrong format, changed tracks, storage full). */
  constructor(code, message, terminal = false) {
    super(message);
    this.name = 'LiveError';
    this.code = code;
    this.terminal = terminal;
  }
}

const concat = (parts) => {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
};
const same = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);

// {id, header, size, unknown} or null when more bytes are needed
function elementHeader(bytes, at = 0, end = bytes.length) {
  if (at >= end) return null;
  if (!bytes[at]) throw new LiveError('format', 'Invalid EBML ID');
  let idSize = 1,
    mask = 0x80;
  while (!(bytes[at] & mask)) {
    idSize++;
    mask >>= 1;
  }
  if (idSize > 4) throw new LiveError('format', 'Invalid EBML ID');
  if (at + idSize >= end) return null;
  let id = 0;
  for (let i = 0; i < idSize; i++) id = id * 256 + bytes[at + i];
  const first = bytes[at + idSize];
  if (!first) throw new LiveError('format', 'Invalid EBML size');
  let sizeSize = 1;
  mask = 0x80;
  while (!(first & mask)) {
    sizeSize++;
    mask >>= 1;
  }
  if (at + idSize + sizeSize > end) return null;
  let size = first & (mask - 1),
    ones = size === mask - 1;
  for (let i = 1; i < sizeSize; i++) {
    const b = bytes[at + idSize + i];
    size = size * 256 + b;
    ones &&= b === 0xff;
  }
  return { id, header: idSize + sizeSize, size, unknown: ones };
}

function sizeVint(n) {
  let length = 1;
  while (n >= 2 ** (7 * length) - 1) length++;
  const out = new Uint8Array(length);
  for (let i = length - 1; i >= 0; i--) {
    out[i] = n % 256;
    n = Math.floor(n / 256);
  }
  out[0] |= 0x80 >> (length - 1);
  return out;
}
function uintBytes(n) {
  const out = [];
  do {
    out.unshift(n % 256);
    n = Math.floor(n / 256);
  } while (n);
  return Uint8Array.from(out);
}
const idBytes = (id) => uintBytes(id);
const element = (id, payload) => concat([idBytes(id), sizeVint(payload.length), payload]);

function* children(bytes) {
  const h = elementHeader(bytes);
  let at = h.header;
  while (at < bytes.length) {
    const child = elementHeader(bytes, at);
    if (!child || child.unknown || at + child.header + child.size > bytes.length)
      throw new LiveError('format', 'Malformed Matroska element');
    yield { id: child.id, at, data: at + child.header, end: at + child.header + child.size };
    at += child.header + child.size;
  }
}

/** The Cluster's Timestamp in ticks. */
export function clusterTicks(cluster) {
  for (const child of children(cluster))
    if (child.id === TIMESTAMP) {
      if (child.end - child.data > 6) throw new LiveError('format', 'Oversized Cluster timestamp');
      let ticks = 0;
      for (let i = child.data; i < child.end; i++) ticks = ticks * 256 + cluster[i];
      return ticks;
    }
  throw new LiveError('format', 'Cluster has no timestamp');
}

/** The same Cluster with another Timestamp; every other child is copied unchanged. */
export function retimeCluster(cluster, ticks) {
  const parts = [];
  for (const child of children(cluster))
    parts.push(
      child.id === TIMESTAMP
        ? element(TIMESTAMP, uintBytes(ticks))
        : cluster.subarray(child.at, child.end),
    );
  return element(CLUSTER, concat(parts));
}

// A saved or late-joined stream is not the length the publisher's file was.
export function withoutDuration(info) {
  const parts = [];
  for (const child of children(info))
    if (child.id !== DURATION) parts.push(info.subarray(child.at, child.end));
  return element(INFO, concat(parts));
}

/** Frames top-level elements from arbitrary chunks of one connection. */
export class MatroskaLiveDemuxer {
  constructor() {
    this.chunks = [];
    this.length = 0;
    this.need = 1;
    this.stage = 0;
    this.head = [];
  }
  /** Returns [{type: 'head', ebml, segment, info, tracks} | {type: 'cluster', bytes}]. */
  push(chunk) {
    // join only once the element being waited for can be complete: the relay sends
    // 16 KiB messages, and a large Cluster would otherwise be copied for each
    this.chunks.push(chunk);
    this.length += chunk.length;
    if (this.length < this.need) return [];
    const out = [],
      bytes = this.chunks.length === 1 ? chunk : concat(this.chunks);
    let at = 0;
    this.need = 1;
    for (;;) {
      if (!this.stage && at < bytes.length && bytes[at] === 0x47)
        throw new LiveError(
          'mpegts',
          'The stream changed from raw thermal to MPEG-TS video; open it again.',
          true,
        );
      const h = elementHeader(bytes, at);
      if (!h) break;
      if (this.stage === 1) {
        if (h.id !== SEGMENT || !h.unknown)
          throw new LiveError('format', 'Expected an unknown-size Matroska Segment');
        this.head.push(bytes.slice(at, at + h.header));
        at += h.header;
        this.stage = 2;
        continue;
      }
      const expected = [EBML, SEGMENT, INFO, TRACKS][this.stage] ?? CLUSTER;
      if (h.id !== expected || h.unknown)
        throw new LiveError('format', `Unexpected Matroska element ${h.id.toString(16)}`);
      if (h.size > (this.stage < 4 ? MAX_HEAD_ELEMENT : MAX_CLUSTER))
        throw new LiveError('format', 'Oversized Matroska element');
      const total = h.header + h.size;
      if (bytes.length - at < total) {
        this.need = total;
        break;
      }
      const item = bytes.slice(at, at + total);
      at += total;
      if (this.stage < 4) {
        this.head.push(item);
        if (this.head.reduce((n, p) => n + p.length, 0) > MAX_PREFIX)
          throw new LiveError('format', 'Oversized Matroska prefix');
        if (++this.stage === 4) {
          const [ebml, segment, info, tracks] = this.head;
          out.push({ type: 'head', ebml, segment, info, tracks });
        }
      } else out.push({ type: 'cluster', bytes: item });
    }
    const rest = at ? bytes.slice(at) : bytes;
    this.chunks = rest.length ? [rest] : [];
    this.length = rest.length;
    this.need = Math.max(this.need, rest.length + 1);
    return out;
  }
}

/**
 * Turns connections into one stored recording and its frame index. A publisher that restarts
 * its timestamps starts a new epoch, stored after the frames already held.
 */
export class LiveRecorder {
  constructor({ store, now = () => Date.now() }) {
    this.store = store;
    this.now = now;
    this.tracks = null;
    this.tracksBytes = null;
    this.frames = 0;
    this.epoch = 0;
    this.offsetTicks = 0;
    this.lastRaw = null;
    this.lastStored = null;
    this.lastWall = null;
    this.lastCluster = null;
    this.demuxer = null;
    this.joining = false;
  }
  /** Call for each new connection; bytes of an unfinished Cluster are dropped. */
  beginConnection() {
    this.demuxer = new MatroskaLiveDemuxer();
    this.joining = true;
  }
  /**
   * Resolves to out = {frames, tracks?}; frames are in presentation order with id = index.
   * If it rejects, a caller's out still holds what was stored before the failure.
   */
  async feed(chunk, out = { frames: [] }) {
    for (const item of this.demuxer.push(chunk)) {
      if (item.type === 'head') {
        if (await this.head(item)) out.tracks = this.tracks;
      } else out.frames.push(...(await this.cluster(item.bytes)));
    }
    return out;
  }
  async head({ ebml, segment, info, tracks }) {
    const parsed = await parseMatroskaHead(info, tracks);
    if (this.tracks) {
      if (!same(tracks, this.tracksBytes) || parsed.scale !== this.scale)
        throw new LiveError(
          'tracks',
          'The stream restarted with a different video format; open it again.',
          true,
        );
      return false;
    }
    if (parsed.tracks[0].codec !== 'ffv1')
      throw new LiveError('codec', 'Only raw thermal (FFV1) live streams are supported.', true);
    this.tracks = [parsed.tracks[0]];
    this.tracksBytes = tracks;
    this.scale = parsed.scale;
    this.msPerTick = parsed.scale / 1e6;
    await this.store.append(concat([ebml, segment, withoutDuration(info), tracks]));
    return true;
  }
  async cluster(bytes) {
    const raw = clusterTicks(bytes),
      wall = this.now();
    const joined = this.joining;
    // the relay starts a viewer at its newest complete Cluster, which may be one we hold;
    // the next one is then the first of this connection
    if (joined && this.lastCluster && same(bytes, this.lastCluster)) return [];
    this.joining = false;
    let epoch = this.epoch,
      offsetTicks = this.offsetTicks;
    if (this.lastRaw !== null) {
      const advance = (raw - this.lastRaw) * this.msPerTick,
        elapsed = wall - this.lastWall;
      // a later timestamp on a new connection is the same publisher only if its clock
      // advanced about as far as ours
      if (raw <= this.lastRaw || (joined && Math.abs(advance - elapsed) > SAME_EPOCH_SLACK_MS)) {
        epoch++;
        const gap = Math.max(MIN_EPOCH_GAP_MS, elapsed) / this.msPerTick;
        offsetTicks = this.lastStored + Math.max(1, Math.round(gap)) - raw;
      }
    }
    // parsed before it is stored: a Cluster that does not parse must not enter the recording
    const ticks = raw + offsetTicks,
      stored = offsetTicks ? retimeCluster(bytes, ticks) : bytes,
      base = this.store.size,
      track = this.tracks[0],
      frames = (await parseMatroskaCluster(this.tracks, stored, base)).filter(
        (frame) => frame.trackId === track.id,
      );
    if ((await this.store.append(stored)) !== base)
      throw new LiveError('store', 'Live recording offsets are inconsistent', true);
    this.epoch = epoch;
    this.offsetTicks = offsetTicks;
    for (const frame of frames) {
      frame.id = this.frames++;
      frame.epoch = this.epoch;
      frame.sourceTimestampTicks = frame.timestampTicks - this.offsetTicks;
      frame.received = wall;
      frame.decodable = frame.presentable = true;
      frame.telemetry ??= null;
      frame.thermalMetadata ??= null;
    }
    this.lastRaw = raw;
    this.lastStored = ticks;
    this.lastWall = wall;
    this.lastCluster = bytes;
    return frames;
  }
}
