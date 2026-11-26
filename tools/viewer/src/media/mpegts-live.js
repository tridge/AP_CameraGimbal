/** Live MPEG-TS (H.264/HEVC with KLV) as SupportProxy relays it: program tables, then packets from a keyframe. */
import { createTsDemuxer } from './containers.js';
import { LiveError } from './matroska-live.js';

const PACKET = 188,
  WRAP = 2 ** 33,
  SAME_EPOCH_SLACK_MS = 10000,
  MIN_EPOCH_GAP_MS = 40,
  MAX_PARTIAL = PACKET * 64;

const concat = (a, b) => {
  if (!a.length) return b;
  const out = new Uint8Array(a.length + b.length);
  out.set(a);
  out.set(b, a.length);
  return out;
};
// signed difference of two 33-bit clock values
const ahead = (value, reference) => {
  const d = (((value - reference) % WRAP) + WRAP) % WRAP;
  return d > WRAP / 2 ? d - WRAP : d;
};
const readTime = (b, at) =>
  ((b[at] >> 1) & 7) * 2 ** 30 +
  b[at + 1] * 2 ** 22 +
  (b[at + 2] >> 1) * 2 ** 15 +
  b[at + 3] * 128 +
  (b[at + 4] >> 1);
function writeTime(b, at, value) {
  b[at] = (b[at] & 0xf1) | (Math.floor(value / 2 ** 30) << 1);
  b[at + 1] = Math.floor(value / 2 ** 22) & 255;
  b[at + 2] = (b[at + 2] & 1) | ((Math.floor(value / 2 ** 15) & 127) << 1);
  b[at + 3] = Math.floor(value / 128) & 255;
  b[at + 4] = (b[at + 4] & 1) | ((value & 127) << 1);
}

// {payload, pcrAt} of a packet with payload, or null
function layout(b) {
  const control = (b[3] >> 4) & 3;
  let payload = 4,
    pcrAt = -1;
  if (control & 2) {
    if (b[4] > 183) return null;
    if (b[4] >= 7 && b[5] & 0x10) pcrAt = 6;
    payload += 1 + b[4];
  }
  return { payload: control & 1 && payload < PACKET ? payload : -1, pcrAt };
}
// decode time of the PES that starts in this packet, or null
function pesClock(b, payload) {
  if (payload < 0 || payload + 14 > PACKET) return null;
  if (b[payload] || b[payload + 1] || b[payload + 2] !== 1) return null;
  if ((b[payload + 6] & 0xc0) !== 0x80 || !(b[payload + 7] & 0x80)) return null;
  const both = (b[payload + 7] & 0x40) !== 0 && payload + 19 <= PACKET;
  return { pts: payload + 9, dts: both ? payload + 14 : payload + 9 };
}
function shiftPacket(b, at, offset) {
  if (at.pcrAt >= 0) {
    // program clock reference: 33-bit base, 6 reserved bits, 9-bit extension
    const base = b[6] * 2 ** 25 + b[7] * 2 ** 17 + b[8] * 512 + b[9] * 2 + (b[10] >> 7),
      moved = (base + offset) % WRAP;
    b[6] = Math.floor(moved / 2 ** 25) & 255;
    b[7] = Math.floor(moved / 2 ** 17) & 255;
    b[8] = Math.floor(moved / 512) & 255;
    b[9] = Math.floor(moved / 2) & 255;
    b[10] = (b[10] & 0x7f) | ((moved & 1) << 7);
  }
  const clock = b[1] & 0x40 ? pesClock(b, at.payload) : null;
  if (!clock) return;
  const pts = (readTime(b, clock.pts) + offset) % WRAP;
  if (clock.dts !== clock.pts) writeTime(b, clock.dts, (readTime(b, clock.dts) + offset) % WRAP);
  writeTime(b, clock.pts, pts);
}

/** Adds ticks (90 kHz) to the clock reference and PES timestamps of one packet, in place. */
export function retimePacket(packet, ticks) {
  const where = layout(packet);
  if (where) shiftPacket(packet, where, ((ticks % WRAP) + WRAP) % WRAP);
}

/**
 * Turns connections into one stored transport stream and its frame index. A publisher that
 * restarts its clock starts a new epoch: its timestamps are moved to follow what is held, in
 * the stored bytes too, so the saved file plays as one recording.
 */
export class TsLiveRecorder {
  constructor({ store, now = () => Date.now() }) {
    this.store = store;
    this.now = now;
    this.source = { tracks: [], warnings: [], maxPacketBytes: 16777216 };
    this.demux = createTsDemuxer(this.source);
    this.format = 'mpegts';
    this.frames = 0;
    this.epoch = 0;
    this.offset = 0; // 90 kHz ticks added to this epoch's timestamps
    this.lastStart = null; // source decode time of the newest video access unit seen
    this.lastDone = null; // ... of the newest one known to be complete
    this.lastStored = null; // stored decode time of the newest video access unit
    this.lastWall = null;
    this.lastPts = -Infinity;
    this.announced = null;
    this.partial = new Uint8Array(0);
    this.tables = new Set();
    this.recent = []; // source decode times of the newest access units stored
    this.cuts = []; // stream offsets where what follows does not continue what came before
    this.keyed = false;
    this.position = 0;
    this.bounds = []; // where each later epoch starts in the stream, for frames still to come
    this.shown = { epoch: 0, offset: 0 }; // of the frame being published
    this.beginConnection();
  }
  /** Call for each new connection; an access unit cut off by the old one is dropped. */
  beginConnection() {
    this.partial = new Uint8Array(0);
    this.aligned = false;
    this.joining = true; // until this connection's first video access unit places it
    this.skipTo = null; // source time up to which this connection repeats what is held
    this.placed = false; // whether this connection continues the frames held is not known yet
    this.counters = new Map();
    this.drop = new Map(); // by PID: the access unit being received is not kept
    this.demux.reset();
  }
  // true if the packet is to be stored
  admit(b, at) {
    const pid = (b[1] & 31) * 256 + b[2],
      video = this.source.tracks[0]?.id;
    // program tables are always kept; nothing else is known before they have been seen
    if (pid === 0 || pid === 0x1fff || video === undefined || this.tables.has(pid)) return true;
    const clock = b[1] & 0x40 ? pesClock(b, at.payload) : null;
    if (pid !== video) {
      // metadata follows its video: none before the video is placed, none for frames held
      if (b[1] & 0x40)
        this.drop.set(
          pid,
          this.joining ||
            (this.skipTo !== null && !(clock && ahead(readTime(b, clock.pts), this.skipTo) > 0)),
        );
      return this.drop.get(pid) === false;
    }
    if (at.payload >= 0) {
      const counter = b[3] & 15,
        previous = this.counters.get(pid);
      if (previous !== undefined && counter !== previous && counter !== (previous + 1) % 16)
        this.cuts.push(this.position); // packets were lost: wait for a keyframe
      this.counters.set(pid, counter);
    }
    if (!clock) return this.drop.get(pid) === false;
    const raw = readTime(b, clock.dts),
      wall = this.now();
    if (this.skipTo !== null) {
      // still within what is held
      if (ahead(raw, this.skipTo) <= 0) {
        this.drop.set(pid, true);
        return false;
      }
      // the next access unit after those held: the group of pictures continues
      this.skipTo = null;
      this.placed = true;
    } else if (this.lastStart !== null) {
      const advance = ahead(raw, this.lastStart) / 90,
        elapsed = wall - this.lastWall;
      let fresh = false;
      if (this.joining) {
        // The relay starts a viewer at its newest keyframe, which may be one already held:
        // then its time is one that was stored. The access unit that was being received
        // when the old connection ended is not held, so it is taken again.
        const held = this.lastDone ?? (this.lastStart - 1 + WRAP) % WRAP;
        if (ahead(raw, held) <= 0 && this.recent.includes(raw)) {
          this.joining = false;
          this.skipTo = held;
          this.drop.set(pid, true);
          return false;
        }
        // a later time is the same publisher only if its clock advanced about as far as ours
        if (advance <= 0 || Math.abs(advance - elapsed) > SAME_EPOCH_SLACK_MS) fresh = true;
      } else if (advance <= 0 || advance - elapsed > SAME_EPOCH_SLACK_MS) fresh = true;
      if (fresh) {
        this.epoch++;
        const gap = Math.round(Math.max(MIN_EPOCH_GAP_MS, elapsed) * 90);
        this.offset = (((this.lastStored + gap - raw) % WRAP) + WRAP) % WRAP;
        this.recent.length = 0;
        this.bounds.push({ at: this.position, offset: this.offset });
      }
    }
    this.joining = false;
    // a connection that does not carry on from the frames held starts at a keyframe
    if (!this.placed) this.cuts.push(this.position);
    this.placed = true;
    this.recent.push(raw);
    if (this.recent.length > 256) this.recent.shift();
    this.drop.set(pid, false);
    this.lastDone = this.lastStart;
    this.lastStart = raw;
    this.lastStored = (raw + this.offset) % WRAP;
    this.lastWall = wall;
    return true;
  }
  /**
   * Resolves to out = {frames, tracks?}; frames are in presentation order with id = index.
   * If it rejects, a caller's out still holds what was stored before the failure.
   */
  async feed(chunk, out = { frames: [] }) {
    const bytes = concat(this.partial, chunk),
      base = this.store.size,
      kept = [],
      frames = [];
    let at = 0;
    for (;;) {
      if (!this.aligned) {
        if (bytes.length - at >= 4 && bytes[at] === 0x1a && bytes[at + 1] === 0x45)
          throw new LiveError(
            'format',
            'The stream changed from MPEG-TS video to raw thermal; open it again.',
            true,
          );
        let found = -1;
        for (let i = at; i + PACKET < bytes.length && i < at + PACKET * 4; i++)
          if (bytes[i] === 0x47 && bytes[i + PACKET] === 0x47) {
            found = i;
            break;
          }
        if (found < 0) {
          if (bytes.length - at > MAX_PARTIAL)
            throw new LiveError('format', 'No MPEG-TS packet alignment in the stream');
          break;
        }
        at = found;
        this.aligned = true;
      }
      if (at + PACKET > bytes.length) break;
      if (bytes[at] !== 0x47) {
        this.aligned = false;
        this.cuts.push(base + kept.length * PACKET);
        this.demux.reset();
        continue;
      }
      const b = bytes.subarray(at, at + PACKET),
        where = layout(b);
      at += PACKET;
      if (!where || b[1] & 0x80) continue; // damaged
      this.position = base + kept.length * PACKET; // where this packet will be stored
      if (!this.admit(b, where)) continue;
      if (this.offset) shiftPacket(b, where, this.offset);
      // parsed as it is admitted: the program tables say which packets are video
      this.demux.packet(b, base + kept.length * PACKET);
      this.noteTables(b, where);
      kept.push(b);
      frames.push(...this.demux.ready());
    }
    this.partial = bytes.slice(at);
    if (!kept.length) return out;
    const block = new Uint8Array(kept.length * PACKET);
    kept.forEach((b, i) => block.set(b, i * PACKET));
    if ((await this.store.append(block)) !== base)
      throw new LiveError('store', 'Live recording offsets are inconsistent', true);
    for (const frame of frames) this.emit(frame, out);
    return out;
  }
  // the PIDs that carry program map tables, from the program association table
  noteTables(b, at) {
    if ((b[1] & 31) * 256 + b[2] !== 0 || !(b[1] & 0x40) || at.payload < 0) return;
    const s = at.payload + 1 + b[at.payload];
    if (s + 8 > PACKET || b[s] !== 0) return;
    const end = Math.min(PACKET, s + 3 + ((b[s + 1] & 15) * 256 + b[s + 2]) - 4);
    for (let i = s + 8; i + 4 <= end; i += 4)
      if (b[i] || b[i + 1]) this.tables.add((b[i + 2] & 31) * 256 + b[i + 3]);
  }
  emit(frame, out) {
    const track = this.source.tracks[0];
    if (!track || frame.trackId !== track.id) return;
    // nothing can be shown before the first parameter sets: a viewer that joins mid-stream
    // starts at its first keyframe
    if (!track.width || !track.description) return;
    const key = `${track.codec}:${track.width}x${track.height}`;
    if (this.announced !== null && this.announced !== key)
      throw new LiveError(
        'tracks',
        'The stream restarted with a different video format; open it again.',
        true,
      );
    this.announced = key;
    // parameter sets may have been repeated or extended: the decoder reads them from the track
    out.tracks = [track];
    if (frame.pts <= this.lastPts) return; // out of order: not a stream this viewer can index
    this.lastPts = frame.pts;
    // frames after a break in the stream are not decodable until a keyframe
    while (this.cuts.length && this.cuts[0] <= frame.transport.packetStart) {
      this.cuts.shift();
      this.keyed = false;
    }
    while (this.bounds.length && this.bounds[0].at <= frame.transport.packetStart)
      this.shown = { epoch: this.shown.epoch + 1, offset: this.bounds.shift().offset };
    if (frame.keyframe) this.keyed = true;
    if (!this.keyed && !this.frames) return; // the recording starts at its first keyframe
    frame.id = this.frames++;
    frame.epoch = this.shown.epoch;
    frame.sourceTimestampTicks =
      (((frame.timestampTicks - this.shown.offset) % WRAP) + WRAP) % WRAP;
    frame.received = this.now();
    frame.decodable = this.keyed;
    frame.presentable = true;
    frame.telemetry ??= null;
    frame.thermalMetadata ??= null;
    out.frames.push(frame);
  }
}
