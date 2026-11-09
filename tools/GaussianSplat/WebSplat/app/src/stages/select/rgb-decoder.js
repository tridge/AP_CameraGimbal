/** RGB frames at the processing size for extraction (ascending indices, each decode chain once).
 * WebCodecs where the browser supports the codec; else FFmpeg (the viewer's pinned core) on
 * keyframe groups built with the viewer's Matroska group builder, in a pool of workers. The
 * viewer's own FrameDecoder seeks one frame at a time (its software path decodes each frame's
 * chain again), which suits review, not extraction.
 */
import {
  annexBToLengthPrefixed,
  createMiniMatroska,
} from '../../../../../../viewer/src/media/software-decoder.js';
import { packetNals } from '../../../../../../viewer/src/media/telemetry.js';

const MIB = 1024 * 1024;
export const MAX_GROUP_PACKETS = 300;
const abortError = () => new DOMException('Decode cancelled', 'AbortError');

const webCodecsConfig = (track) => {
  const config = {
    codec: track.codecString || (track.codec === 'avc' ? 'avc1.420029' : 'hvc1.1.6.L120.B0'),
    optimizeForLatency: true,
    hardwareAcceleration: 'no-preference',
  };
  const description = track.description || track.codecPrivate;
  if (description?.length && track.nalLengthSize) config.description = description;
  return config;
};

export class RgbDecoder {
  /** media: {source (demuxer), demuxed (its frames of the video track), track}; size: processing
   * {width, height}; mode: 'auto' | 'webcodecs' | 'software' */
  constructor(media, size, { coreURL, workers = 3, mode = 'auto', workerFactory } = {}) {
    this.media = media;
    this.size = size;
    this.coreURL = coreURL;
    this.poolSize = Math.max(1, workers);
    this.mode = mode;
    this.workerFactory =
      workerFactory ?? (() => new Worker(new URL('./rgb-batch.worker.js', import.meta.url)));
    this.pool = [];
    this.sequence = 0;
    this.used = null;
    this.stats = { frames: 0, packets: 0, decodeMs: 0 };
  }

  get srcTrack() {
    return this.media.source.tracks.find((t) => t.id === this.media.track.id);
  }

  async choose() {
    if (this.mode !== 'auto') return this.mode;
    if (!globalThis.VideoDecoder) return 'software';
    try {
      const r = await VideoDecoder.isConfigSupported(webCodecsConfig(this.srcTrack));
      return r.supported ? 'webcodecs' : 'software';
    } catch {
      return 'software';
    }
  }

  /** Yield {index, rgba: Uint8ClampedArray [height * width * 4]} for each wanted index, ascending.
   * WebCodecs decodes one keyframe group at a time (submitted once, flushed, its selections
   * drained); a WebCodecs failure moves that group and the rest to FFmpeg. */
  async *decode(wanted, { signal } = {}) {
    const order = [...new Set(wanted)].sort((a, b) => a - b);
    const groups = groupsByKeyframe(this.media.demuxed, order);
    this.used = await this.choose();
    const onAbort = () => this.close();
    signal?.addEventListener('abort', onAbort, { once: true });
    try {
      let g = 0;
      if (this.used === 'webcodecs') {
        let native = null;
        try {
          native = this._nativeGroups(signal); // a configure() failure falls back as well
          for (; g < groups.length; g++) {
            const out = await native.group(groups[g]);
            for (const index of groups[g].wanted) {
              this.stats.frames++;
              yield { index, rgba: out.get(index) };
            }
          }
        } catch (e) {
          if (signal?.aborted || e?.name === 'AbortError') throw e;
          this.fallback = { group: g, reason: String(e?.message ?? e) };
          this.used = 'webcodecs+software';
        } finally {
          native?.close();
        }
      }
      yield* this._software(groups.slice(g), signal);
    } finally {
      signal?.removeEventListener('abort', onAbort);
    }
  }

  // WebCodecs over whole keyframe groups: group(batch) -> Map(index -> rgba)
  _nativeGroups(signal) {
    const frames = this.media.demuxed;
    const track = this.srcTrack;
    const { width, height } = this.size;
    const canvas = new OffscreenCanvas(width, height);
    const ctx = canvas.getContext('2d', { willReadFrequently: true });
    const sets = track.annexB ? parameterSetsAnnexB(track) : null;
    let want = new Map(); // timestamp -> index, the current group's selections
    let ready = new Map(); // index -> Promise<rgba>
    let failure = null;
    let wake = null;
    const decoder = new VideoDecoder({
      output: (frame) => {
        const i = want.get(frame.timestamp);
        if (i === undefined || ready.has(i)) return frame.close();
        let p;
        try {
          p = createImageBitmap(frame, {
            resizeWidth: width,
            resizeHeight: height,
            resizeQuality: 'high',
          }).then((bmp) => {
            try {
              ctx.drawImage(bmp, 0, 0);
              return ctx.getImageData(0, 0, width, height).data;
            } finally {
              bmp.close();
            }
          });
        } catch (e) {
          p = Promise.reject(e);
        } finally {
          frame.close(); // the bitmap holds its own copy
        }
        p.catch(() => {});
        ready.set(i, p);
      },
      error: (e) => {
        failure = e;
        wake?.();
      },
    });
    try {
      decoder.configure(webCodecsConfig(track));
    } catch (e) {
      decoder.close();
      throw e;
    }
    const pause = (ms) =>
      new Promise((r) => {
        const t = setTimeout(r, ms);
        wake = () => {
          clearTimeout(t);
          r();
        };
      });
    return {
      group: async (batch) => {
        const t0 = performance.now();
        want = new Map(batch.wanted.map((i) => [Math.round(frames[i].pts * 1e6), i]));
        ready = new Map();
        for (const i of batch.packets) {
          if (signal?.aborted) throw abortError();
          if (failure) throw failure;
          const f = frames[i];
          let data = await this.media.source.readPacket(f, { signal });
          if (f.keyframe && sets) data = withParameterSets(data, sets, track.codec);
          decoder.decode(
            new EncodedVideoChunk({
              type: f.keyframe ? 'key' : 'delta',
              timestamp: Math.round(f.pts * 1e6),
              data,
            }),
          );
          this.stats.packets++;
          while (decoder.decodeQueueSize > 8 && !failure) await pause(20);
        }
        await decoder.flush(); // every output of the group; the next group starts at a keyframe
        if (failure) throw failure;
        const out = new Map();
        for (const i of batch.wanted) {
          if (!ready.has(i)) throw new Error(`WebCodecs returned no frame ${i}`);
          out.set(i, await ready.get(i));
        }
        ready = new Map();
        this.stats.decodeMs += performance.now() - t0;
        return out;
      },
      close: () => {
        if (decoder.state !== 'closed') decoder.close();
      },
    };
  }

  async _worker(slot) {
    if (this.pool[slot]) return this.pool[slot];
    const worker = this.workerFactory();
    const pending = new Map();
    const entry = {
      worker,
      pending,
      call: (type, data, transfer = []) => {
        const id = ++this.sequence;
        return new Promise((resolve, reject) => {
          pending.set(id, { resolve, reject });
          worker.postMessage({ id, type, ...data }, transfer);
        });
      },
    };
    worker.onmessage = ({ data }) => {
      const p = pending.get(data.id);
      if (!p) return;
      pending.delete(data.id);
      data.error ? p.reject(new Error(data.error)) : p.resolve(data.result);
    };
    worker.onerror = (event) => {
      for (const p of pending.values())
        p.reject(new Error(event.message || 'decoder worker failed'));
      pending.clear();
      if (this.pool[slot] === entry) this.pool[slot] = null;
    };
    this.pool[slot] = entry;
    await entry.call('load', { coreURL: this.coreURL });
    return entry;
  }

  async _batch(slot, batch, signal) {
    const frames = this.media.demuxed;
    const track = this.srcTrack;
    const group = batch.packets.map((i) => frames[i]);
    const packets = [];
    for (const f of group) {
      if (signal?.aborted) throw abortError();
      const p = await this.media.source.readPacket(f, { signal });
      packets.push(track.annexB ? annexBToLengthPrefixed(p) : p.slice());
    }
    const { blob, origin } = createMiniMatroska(track, group, packets);
    const entry = await this._worker(slot);
    if (signal?.aborted) throw abortError();
    const r = await entry.call('decode', {
      blob,
      times: batch.wanted.map((i) => frames[i].pts - origin),
      width: this.size.width,
      height: this.size.height,
    });
    this.stats.decodeMs += r.decodeMs;
    this.stats.packets += group.length;
    const n = this.size.width * this.size.height * 4;
    return new Map(batch.wanted.map((i, k) => [i, new Uint8ClampedArray(r.buffer, k * n, n)]));
  }

  async *_software(batches, signal) {
    const inflight = [];
    let next = 0;
    const launch = () => {
      const slot = next % this.poolSize;
      const batch = batches[next++];
      const prev = inflight.filter((x) => x.slot === slot).at(-1)?.promise ?? Promise.resolve();
      const promise = prev.catch(() => {}).then(() => this._batch(slot, batch, signal));
      promise.catch(() => {});
      inflight.push({ slot, batch, promise });
    };
    while (next < batches.length && inflight.length < this.poolSize + 1) launch();
    while (inflight.length) {
      const { batch, promise } = inflight.shift();
      const out = await promise;
      if (signal?.aborted) throw abortError();
      if (next < batches.length) launch();
      for (const index of batch.wanted) {
        this.stats.frames++;
        yield { index, rgba: out.get(index) };
      }
    }
  }

  close() {
    for (const entry of this.pool) {
      if (!entry) continue;
      entry.worker.terminate();
      for (const p of entry.pending.values()) p.reject(abortError());
    }
    this.pool = [];
  }
}

/** One decode batch per keyframe group holding wanted frames: packets from the keyframe to the
 * group's last wanted frame, so each group is decoded once. */
export function groupsByKeyframe(
  frames,
  order,
  { maxPackets = MAX_GROUP_PACKETS, maxBytes = 64 * MIB } = {},
) {
  const batches = [];
  for (const index of order) {
    let key = index;
    while (key > 0 && !frames[key].keyframe) key--;
    if (!frames[key].keyframe) throw new Error(`frame ${index} has no preceding keyframe`);
    const last = batches.at(-1);
    if (last && last.key === key) {
      for (let i = last.packets.at(-1) + 1; i <= index; i++) last.packets.push(i);
      last.wanted.push(index);
    } else
      batches.push({
        key,
        packets: Array.from({ length: index - key + 1 }, (_, k) => key + k),
        wanted: [index],
      });
    const b = batches.at(-1);
    if (b.packets.length > maxPackets)
      throw new Error(
        `frame ${index} is ${b.packets.length - 1} packets after its keyframe (limit ${maxPackets})`,
      );
    if (b.packets.reduce((n, i) => n + frames[i].size, 0) > maxBytes)
      throw new Error(`the keyframe group of frame ${index} exceeds ${maxBytes / MIB} MiB`);
  }
  return batches;
}

/** The track's parameter sets (from its avcC / hvcC record) as Annex B NAL units. */
export function parameterSetsAnnexB(track) {
  const c = track.codecPrivate ?? track.description;
  if (!c?.length) return null;
  const nals = [];
  const u16 = (o) => (c[o] << 8) | c[o + 1];
  let o;
  const take = (count) => {
    for (let n = 0; n < count; n++) {
      const len = u16(o);
      nals.push(c.subarray(o + 2, o + 2 + len));
      o += 2 + len;
    }
  };
  if (track.codec === 'avc') {
    o = 6;
    take(c[5] & 31); // SPS
    o++;
    take(c[o - 1]); // PPS
  } else if (track.codec === 'hevc') {
    o = 23;
    for (let a = 0; a < c[22]; a++) {
      const count = u16(o + 1);
      o += 3;
      take(count);
    }
  } else return null;
  return nals;
}

/** An Annex B keyframe with the track's parameter sets it lacks in front (H.264: SPS, PPS; HEVC:
 * VPS, SPS, PPS). WebCodecs has no description for Annex B, so a range starting mid-file
 * needs them in band. */
export function withParameterSets(packet, sets, codec) {
  const type = (nal) => (codec === 'hevc' ? (nal[0] >> 1) & 63 : nal[0] & 31);
  const required = codec === 'hevc' ? [32, 33, 34] : [7, 8];
  const have = new Set([...packetNals(packet)].map(type));
  const missing = sets.filter((n) => required.includes(type(n)) && !have.has(type(n)));
  if (!missing.length) return packet;
  const parts = missing.flatMap((n) => [START, n]);
  const out = new Uint8Array(parts.reduce((k, p) => k + p.length, 0) + packet.length);
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  out.set(packet, at);
  return out;
}
const START = Uint8Array.of(0, 0, 0, 1);

/** RGBA -> packed RGB8 and the grey image SIFT uses (cv2 BGR2GRAY fixed point, Rec. 601). */
export function splitRgba(rgba, n) {
  const rgb = new Uint8Array(3 * n);
  const grey = new Uint8Array(n);
  for (let i = 0; i < n; i++) {
    const r = rgba[4 * i],
      g = rgba[4 * i + 1],
      b = rgba[4 * i + 2];
    rgb[3 * i] = r;
    rgb[3 * i + 1] = g;
    rgb[3 * i + 2] = b;
    grey[i] = (r * 4899 + g * 9617 + b * 1868 + 8192) >> 14;
  }
  return { rgb, grey };
}
