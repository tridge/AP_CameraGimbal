import { FFMPEG_VERSION } from '../versions.js';
/** Bounded, worker-owned software decode. The source File is never copied whole.
 * Container indexing chooses a keyframe group; only that group becomes a small
 * seekable Matroska Blob, avoiding FFmpeg's linear scan of uncued recordings.
 */
const MIB = 1024 * 1024;
// The pinned FFmpeg runtime retains allocations between exec calls, and can
// fail after repeated decodes even when the JS frame cache is bounded.
const MAX_WORKER_DECODES = 32;
const MAX_WORKER_BYTES = 64 * MIB;
const abortError = (message = 'Decode cancelled') => new DOMException(message, 'AbortError');
const asBytes = (value) => (value instanceof Uint8Array ? value : new Uint8Array(value ?? 0));
const concat = (...parts) => {
  const result = new Uint8Array(parts.reduce((size, part) => size + part.length, 0));
  let offset = 0;
  for (const part of parts) {
    result.set(part, offset);
    offset += part.length;
  }
  return result;
};
function sizeVint(value) {
  if (!Number.isSafeInteger(value) || value < 0) throw new Error('Invalid EBML size');
  for (let width = 1; width <= 7; width++) {
    if (value < 2 ** (7 * width) - 1) {
      const bytes = new Uint8Array(width);
      for (let i = width - 1; i >= 0; i--) {
        bytes[i] = value % 256;
        value = Math.floor(value / 256);
      }
      bytes[0] |= 1 << (8 - width);
      return bytes;
    }
  }
  throw new Error('EBML size exceeds supported range');
}
function uint(value) {
  if (!Number.isSafeInteger(value) || value < 0) throw new Error('Invalid EBML integer');
  let width = 1;
  while (value >= 256 ** width) width++;
  const bytes = new Uint8Array(width);
  for (let i = width - 1; i >= 0; i--) {
    bytes[i] = value % 256;
    value = Math.floor(value / 256);
  }
  return bytes;
}
const idBytes = (id) => {
  const values = [];
  do {
    values.unshift(id % 256);
    id = Math.floor(id / 256);
  } while (id);
  return Uint8Array.from(values);
};
const element = (id, value) => concat(idBytes(id), sizeVint(value.length), value);
const numberElement = (id, value) => element(id, uint(value));
const textElement = (id, value) => element(id, new TextEncoder().encode(value));

/** Matroska AVC/HEVC with avcC/hvcC initialization uses length-prefixed NALs. */
export function annexBToLengthPrefixed(packet) {
  const bytes = asBytes(packet);
  const starts = [];
  for (let i = 0; i + 2 < bytes.length; i++) {
    if (bytes[i] !== 0 || bytes[i + 1] !== 0) continue;
    if (bytes[i + 2] === 1) {
      starts.push([i, i + 3]);
      i += 2;
    } else if (i + 3 < bytes.length && bytes[i + 2] === 0 && bytes[i + 3] === 1) {
      starts.push([i, i + 4]);
      i += 3;
    }
  }
  if (!starts.length) throw new Error('Annex-B video packet has no NAL start code');
  const parts = [];
  for (let i = 0; i < starts.length; i++) {
    const start = starts[i][1];
    const end = i + 1 < starts.length ? starts[i + 1][0] : bytes.length;
    if (end <= start) continue;
    const length = new Uint8Array(4);
    new DataView(length.buffer).setUint32(0, end - start, false);
    parts.push(length, bytes.subarray(start, end));
  }
  if (!parts.length) throw new Error('Annex-B video packet has no NAL payload');
  return concat(...parts);
}

/** Small independently seekable Matroska. packets are already in decode order.
 * Each compressed packet keeps its PTS at microsecond precision. One Cluster per
 * packet avoids signed 16-bit Block timestamp overflow on sparse thermal data.
 */
export function createMiniMatroska(track, frames, packets) {
  if (!frames.length || frames.length !== packets.length)
    throw new Error('Empty or inconsistent decode group');
  const codecId = { ffv1: 'V_FFV1', avc: 'V_MPEG4/ISO/AVC', hevc: 'V_MPEGH/ISO/HEVC' }[track.codec];
  if (!codecId) throw new Error(`Unsupported software codec: ${track.codec}`);
  if (![track.width, track.height].every((n) => Number.isSafeInteger(n) && n > 0))
    throw new Error('Invalid video dimensions');
  const privateData = asBytes(track.codecPrivate ?? track.description);
  if (!privateData.length) throw new Error(`Missing ${track.codec} codec initialization data`);
  if (privateData.length > MIB) throw new Error('Codec initialization exceeds its 1 MiB budget');
  if (frames.some((frame) => !Number.isFinite(frame.pts))) throw new Error('Missing packet PTS');
  const origin = Math.min(...frames.map((frame) => frame.pts));
  const ebml = element(
    0x1a45dfa3,
    concat(
      numberElement(0x4286, 1),
      numberElement(0x42f7, 1),
      numberElement(0x42f2, 4),
      numberElement(0x42f3, 8),
      textElement(0x4282, 'matroska'),
      numberElement(0x4287, 4),
      numberElement(0x4285, 2),
    ),
  );
  const info = element(
    0x1549a966,
    concat(
      numberElement(0x2ad7b1, 1000),
      textElement(0x4d80, 'Fire viewer'),
      textElement(0x5741, 'Fire viewer'),
    ),
  );
  const video = element(
    0xe0,
    concat(numberElement(0xb0, track.width), numberElement(0xba, track.height)),
  );
  const duration =
    Number.isFinite(track.defaultDuration) && track.defaultDuration > 0
      ? numberElement(0x23e383, Math.round(track.defaultDuration * 1e9))
      : new Uint8Array();
  const tracks = element(
    0x1654ae6b,
    element(
      0xae,
      concat(
        numberElement(0xd7, 1),
        numberElement(0x73c5, 1),
        numberElement(0x83, 1),
        numberElement(0x9c, 0),
        textElement(0x86, codecId),
        element(0x63a2, privateData),
        duration,
        video,
      ),
    ),
  );
  const parts = [
    ebml,
    idBytes(0x18538067),
    Uint8Array.of(0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff),
    info,
    tracks,
  ];
  for (let i = 0; i < frames.length; i++) {
    const frame = frames[i];
    const packet = asBytes(packets[i]);
    if (!packet.length) throw new Error('Empty compressed video packet');
    const timestamp = numberElement(0xe7, Math.round((frame.pts - origin) * 1e6));
    const blockHeader = concat(
      idBytes(0xa3),
      sizeVint(packet.length + 4),
      Uint8Array.of(0x81, 0, 0, frame.keyframe ? 0x80 : 0),
    );
    const clusterSize = timestamp.length + blockHeader.length + packet.length;
    parts.push(idBytes(0x1f43b675), sizeVint(clusterSize), timestamp, blockHeader, packet);
  }
  return { blob: new Blob(parts, { type: 'video/x-matroska' }), origin };
}

/** Return the minimal available decode-order group from a keyframe to target. */
export function selectDecodeGroup(source, target, { maxFrames = 180, maxBytes = 32 * MIB } = {}) {
  const frames = source.frames.filter((frame) => frame.trackId === target.trackId);
  const last = frames.findIndex((frame) => frame === target || frame.id === target.id);
  if (last < 0) throw new Error('Frame is not in the current media index');
  let first = last;
  while (first >= 0 && !frames[first].keyframe) first--;
  if (first < 0) throw new Error('No indexed keyframe precedes this frame');
  const group = frames.slice(first, last + 1);
  if (group.length > maxFrames)
    throw new Error(
      `Decode group exceeds ${maxFrames} frames; select another frame or use native decoding`,
    );
  const bytes = group.reduce((sum, frame) => sum + frame.size, 0);
  if (!Number.isSafeInteger(bytes) || bytes <= 0 || bytes > maxBytes)
    throw new Error(`Decode group exceeds ${maxBytes / MIB} MiB compressed-data budget`);
  return group;
}

export class SoftwareDecoder {
  constructor({
    onStatus = () => {},
    baseURL,
    coreURL,
    workerFactory,
    maxFrames = 180,
    maxBytes = 32 * MIB,
  } = {}) {
    this.onStatus = onStatus;
    const base = new URL(
      baseURL ?? import.meta.env?.BASE_URL ?? '/',
      globalThis.location?.href ?? 'http://localhost/',
    );
    this.coreURL = coreURL
      ? new URL(coreURL, base).href
      : new URL(`vendor/ffmpeg/${FFMPEG_VERSION}/ffmpeg-core.js`, base).href;
    this.workerFactory =
      workerFactory ?? (() => new Worker(new URL('./software-decoder.worker.js', import.meta.url)));
    this.limits = { maxFrames, maxBytes };
    this._pending = new Map();
    this._sequence = 0;
    this._generation = 0;
    this._worker = null;
    this._loaded = false;
    this._workerDecodes = this._workerBytes = this._workerOutputBytes = 0;
    this._closed = true;
    this._active = null;
  }
  _status(message) {
    try {
      this.onStatus(message);
    } catch {
      /* UI callbacks cannot break decoding. */
    }
  }
  async open(file, source) {
    this.close();
    if (!file || !source?.tracks || !source?.frames || typeof source.readPacket !== 'function')
      throw new Error('Missing local media source');
    this.file = file;
    this.source = source;
    this._closed = false;
    this._status('Software decoder ready; local indexed packets only');
  }
  _terminate(error = abortError()) {
    const worker = this._worker;
    this._worker = null;
    this._loaded = false;
    this._workerDecodes = this._workerBytes = this._workerOutputBytes = 0;
    worker?.terminate();
    for (const pending of this._pending.values()) pending.reject(error);
    this._pending.clear();
  }
  _check(job) {
    if (job.controller.signal.aborted || job.generation !== this._generation || this._closed)
      throw abortError();
  }
  _call(type, data = {}) {
    const id = ++this._sequence;
    return new Promise((resolve, reject) => {
      this._pending.set(id, { resolve, reject });
      try {
        this._worker.postMessage({ id, type, ...data });
      } catch (error) {
        this._pending.delete(id);
        reject(error);
      }
    });
  }
  async _load(job) {
    if (this._loaded) return;
    this._check(job);
    const worker = this.workerFactory();
    this._worker = worker;
    worker.onmessage = ({ data }) => {
      if (worker !== this._worker) return;
      const pending = this._pending.get(data.id);
      if (!pending) return;
      this._pending.delete(data.id);
      data.error ? pending.reject(new Error(data.error)) : pending.resolve(data.result);
    };
    worker.onerror = (event) => {
      if (worker === this._worker)
        this._terminate(new Error(event.message || 'Software decoder worker failed'));
    };
    this._status('Loading local software video decoder');
    await this._call('load', { coreURL: this.coreURL });
    this._check(job);
    this._loaded = true;
  }
  async decode(frame, { signal } = {}) {
    if (this._closed) throw new Error('Software decoder is closed');
    if (signal?.aborted) throw abortError();
    this._active?.controller.abort();
    const job = { generation: ++this._generation, controller: new AbortController() };
    this._active = job;
    const externalAbort = () => job.controller.abort();
    const interrupt = () => {
      if (this._active === job && job.usingWorker) this._terminate(abortError());
    };
    job.controller.signal.addEventListener('abort', interrupt, { once: true });
    signal?.addEventListener('abort', externalAbort, { once: true });
    try {
      const track = this.source.tracks.find((candidate) => candidate.id === frame.trackId);
      if (!track || !['ffv1', 'avc', 'hevc'].includes(track.codec))
        throw new Error(`Unsupported software video codec: ${track?.codec}`);
      if (track.width * track.height > 4096 * 4096)
        throw new Error('Software frame exceeds the 16 megapixel decode budget');
      const group = selectDecodeGroup(this.source, frame, this.limits);
      const packets = [];
      for (const candidate of group) {
        this._check(job);
        const packet = await this.source.readPacket(candidate, { signal: job.controller.signal });
        this._check(job);
        if (packet.byteLength !== candidate.size)
          throw new Error('Truncated compressed video packet');
        packets.push(
          track.annexB && track.codec !== 'ffv1' ? annexBToLengthPrefixed(packet) : packet,
        );
      }
      const { blob, origin } = createMiniMatroska(track, group, packets);
      if (blob.size > this.limits.maxBytes + MIB)
        throw new Error('Software input exceeds its memory budget');
      this._check(job);
      // Dispose the whole WASM runtime, including its virtual filesystem and
      // retained allocations. Charge cumulative input plus
      // the largest output, since frame.raw is unlinked and its copy transferred
      // after each decode. Summing every RGBA output would reload 4K each frame.
      const outputBytes = track.width * track.height * (track.codec === 'ffv1' ? 2 : 4);
      const workBytes =
        this._workerBytes + blob.size + Math.max(this._workerOutputBytes, outputBytes);
      if (
        this._workerDecodes >= MAX_WORKER_DECODES ||
        (this._workerDecodes && workBytes > MAX_WORKER_BYTES)
      )
        this._terminate();
      job.usingWorker = true;
      await this._load(job);
      this._check(job);
      this._status(
        `Decoding ${track.codec.toUpperCase()} locally (${group.length} packet${group.length === 1 ? '' : 's'})`,
      );
      this._workerDecodes++;
      this._workerBytes += blob.size;
      this._workerOutputBytes = Math.max(this._workerOutputBytes, outputBytes);
      const result = await this._call('decode', {
        blob,
        targetTime: frame.pts - origin,
        width: track.width,
        height: track.height,
        kind: track.codec === 'ffv1' ? 'gray16' : 'rgba',
      });
      this._check(job);
      let data;
      if (result.kind === 'gray16') {
        const littleEndian = new Uint8Array(new Uint16Array([1]).buffer)[0] === 1;
        if (littleEndian) data = new Uint16Array(result.buffer);
        else {
          const view = new DataView(result.buffer);
          data = Uint16Array.from({ length: view.byteLength / 2 }, (_, i) =>
            view.getUint16(i * 2, true),
          );
        }
      } else data = new Uint8ClampedArray(result.buffer);
      this._status(`Software frame decoded in ${Math.round(result.decodeMs)} ms`);
      return {
        kind: result.kind,
        width: track.width,
        height: track.height,
        data,
        pts: frame.pts,
        frameId: frame.id,
        decoder: 'ffmpeg-wasm',
      };
    } catch (error) {
      // Decoder failures can leave FFmpeg runtime state unusable; recreate the
      // worker on the next request instead of retaining a poisoned instance.
      if (this._active === job && job.usingWorker) this._terminate(error);
      throw error;
    } finally {
      signal?.removeEventListener('abort', externalAbort);
      job.controller.signal.removeEventListener('abort', interrupt);
      if (this._active === job) this._active = null;
    }
  }
  close() {
    this._generation++;
    this._closed = true;
    this._active?.controller.abort();
    this._active = null;
    this._terminate();
    this.file = null;
    this.source = null;
  }
}
