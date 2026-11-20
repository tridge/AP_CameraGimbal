import { SoftwareDecoder } from './software-decoder.js';

const abortError = () => new DOMException('Decode cancelled', 'AbortError');
const MIB = 1024 * 1024;
function configFor(track) {
  const codec = track.codecString || (track.codec === 'avc' ? 'avc1.420029' : 'hvc1.1.6.L120.B0');
  const config = { codec, optimizeForLatency: true, hardwareAcceleration: 'no-preference' };
  const description = track.description || track.codecPrivate;
  if (description?.length && track.nalLengthSize) config.description = description;
  return config;
}
function configKey(track) {
  // Tracks can acquire new parameter sets during indexing. Detect changes while
  // caching unsupported configurations instead of probing them on every frame.
  let hash = 2166136261;
  for (const byte of track.description || track.codecPrivate || [])
    hash = Math.imul(hash ^ byte, 16777619) >>> 0;
  return `${track.id}:${track.codecString}:${track.width}x${track.height}:${track.nalLengthSize}:${hash}`;
}

/** Native codec adapter with incremental decode and bounded future-frame cache.
 * VideoFrames remain owned here and are closed on eviction, abort and close.
 */
export class FrameDecoder {
  constructor(
    file,
    source,
    onStatus = () => {},
    { maxCacheBytes = 32 * MIB, maxGopFrames = 180, maxGopBytes = 32 * MIB } = {},
  ) {
    this.file = file;
    this.source = source;
    this.onStatus = onStatus;
    this.software = null;
    this.closed = false;
    this.mode = 'Detecting decoder';
    this.maxCacheBytes = maxCacheBytes;
    this.maxGopFrames = maxGopFrames;
    this.maxGopBytes = maxGopBytes;
    this._state = null;
    this._operation = null;
    this._generation = 0;
    this._probes = new Map();
    this._unsupported = new Set();
    this._canvas = null;
    this.stats = {
      nativeConfigurations: 0,
      submittedPackets: 0,
      cacheHits: 0,
      cacheBytes: 0,
      maxCacheBytes: 0,
    };
  }
  _status(message) {
    try {
      this.onStatus(message);
    } catch {
      /* UI observers cannot break cleanup. */
    }
  }
  _check(operation) {
    if (
      this.closed ||
      operation.controller.signal.aborted ||
      operation.generation !== this._generation
    )
      throw abortError();
  }
  _dropState() {
    const state = this._state;
    this._state = null;
    if (!state) return;
    if (state.decoder.state !== 'closed') state.decoder.close();
    for (const entry of state.outputs.values()) entry.frame.close();
    state.outputs.clear();
    state.cacheBytes = 0;
    this.stats.cacheBytes = 0;
    for (const notify of state.waiters) notify();
    state.waiters.clear();
  }
  async _probe(track) {
    const key = configKey(track);
    if (!this._probes.has(key))
      this._probes.set(
        key,
        (async () => {
          if (!globalThis.VideoDecoder) return false;
          try {
            return (await VideoDecoder.isConfigSupported(configFor(track))).supported;
          } catch {
            return false;
          }
        })(),
      );
    return this._probes.get(key);
  }
  async decode(frame, { signal } = {}) {
    if (this.closed || signal?.aborted) throw abortError();
    this._operation?.controller.abort();
    const operation = { controller: new AbortController(), generation: ++this._generation };
    this._operation = operation;
    const externalAbort = () => operation.controller.abort();
    const interrupt = () => {
      if (this._operation === operation) this._dropState();
    };
    operation.controller.signal.addEventListener('abort', interrupt, { once: true });
    signal?.addEventListener('abort', externalAbort, { once: true });
    const track = this.source.tracks.find((candidate) => candidate.id === frame.trackId);
    try {
      if (!track) throw new Error('Frame video track is unavailable');
      if (track.codec === 'avc' || track.codec === 'hevc') {
        const key = configKey(track);
        if (!this._unsupported.has(key)) {
          if (!(await this._probe(track))) {
            this._unsupported.add(key);
            this._status(
              `Software decode: ${configFor(track).codec} is not supported by this browser`,
            );
          } else {
            this._check(operation);
            try {
              return await this.native(frame, track, operation);
            } catch (error) {
              if (this._operation === operation) this._dropState();
              if (operation.controller.signal.aborted || error.name === 'AbortError')
                throw abortError();
              this._status(`Software decode: ${error.message}`);
            }
          }
        }
      }
      this._check(operation);
      if (!this.software) {
        this.software = new SoftwareDecoder({ onStatus: this.onStatus });
        await this.software.open(this.file, this.source);
      }
      this._check(operation);
      this.mode = 'Software · local bounded packet decode';
      return await this.software.decode(frame, { signal: operation.controller.signal });
    } finally {
      signal?.removeEventListener('abort', externalAbort);
      operation.controller.signal.removeEventListener('abort', interrupt);
      if (this._operation === operation) this._operation = null;
    }
  }
  _cacheOutput(state, output) {
    state.pendingTimestamps.delete(output.timestamp);
    if (state !== this._state || this.closed || output.timestamp < state.targetTimestamp) {
      output.close();
      return;
    }
    const bytes = output.codedWidth * output.codedHeight * 4; // conservative RGBA-sized accounting, even for native YUV
    if (!Number.isSafeInteger(bytes) || bytes <= 0 || bytes > this.maxCacheBytes) {
      output.close();
      state.failure = new Error('Native decoded frame exceeds the 32 MiB frame-cache budget');
    } else {
      const previous = state.outputs.get(output.timestamp);
      if (previous) {
        previous.frame.close();
        state.cacheBytes -= previous.bytes;
      }
      state.outputs.set(output.timestamp, { frame: output, bytes });
      state.cacheBytes += bytes;
      while (state.cacheBytes > this.maxCacheBytes) {
        // Keep the requested frame and its nearest future neighbours. Closing a
        // farther prefetched frame may require a bounded restart when revisited.
        const timestamps = [...state.outputs.keys()]
          .filter((time) => time !== state.targetTimestamp)
          .sort((a, b) => b - a);
        const victim = timestamps[0];
        if (victim === undefined) break;
        const entry = state.outputs.get(victim);
        state.outputs.delete(victim);
        entry.frame.close();
        state.cacheBytes -= entry.bytes;
      }
      this.stats.cacheBytes = state.cacheBytes;
      this.stats.maxCacheBytes = Math.max(this.stats.maxCacheBytes, state.cacheBytes);
    }
    for (const notify of state.waiters) notify();
  }
  _createState(track, first, timestamp) {
    this._dropState();
    const state = {
      key: configKey(track),
      first,
      next: first,
      targetTimestamp: timestamp,
      outputs: new Map(),
      pendingTimestamps: new Set(),
      cacheBytes: 0,
      waiters: new Set(),
      failure: null,
      flushed: false,
    };
    state.decoder = new VideoDecoder({
      output: (output) => this._cacheOutput(state, output),
      error: (error) => {
        state.failure = error;
        for (const notify of state.waiters) notify();
      },
    });
    this._state = state;
    try {
      state.decoder.configure(configFor(track));
    } catch (error) {
      this._dropState();
      throw error;
    }
    this.stats.nativeConfigurations++;
    return state;
  }
  async _progress(state, operation) {
    this._check(operation);
    if (!state.decoder.decodeQueueSize || state.outputs.has(state.targetTimestamp) || state.failure)
      return;
    await new Promise((resolve, reject) => {
      let timer;
      const cleanup = () => {
        clearTimeout(timer);
        state.waiters.delete(done);
        state.decoder.removeEventListener('dequeue', done);
        operation.controller.signal.removeEventListener('abort', cancel);
      };
      const done = () => {
        cleanup();
        resolve();
      };
      const cancel = () => {
        cleanup();
        reject(abortError());
      };
      state.waiters.add(done);
      state.decoder.addEventListener('dequeue', done, { once: true });
      operation.controller.signal.addEventListener('abort', cancel, { once: true });
      timer = setTimeout(() => {
        cleanup();
        reject(new Error('Native decoder made no progress within 2 seconds'));
      }, 2000);
    });
    this._check(operation);
  }
  async native(target, track, operation) {
    const frames = this.source.frames.filter((frame) => frame.trackId === target.trackId);
    const index = frames.findIndex((frame) => frame.id === target.id);
    if (index < 0) throw new Error('Frame no longer available');
    const timestamp = Math.round(target.pts * 1e6);
    let first = index;
    while (first >= 0 && !frames[first].keyframe && index - first < this.maxGopFrames) first--;
    let state = this._state;
    if (
      state?.key !== configKey(track) ||
      state.failure ||
      (frames[first]?.keyframe &&
        first > state.next &&
        !state.outputs.has(timestamp) &&
        !state.pendingTimestamps.has(timestamp)) ||
      (state.flushed && !state.outputs.has(timestamp)) ||
      (!state.outputs.has(timestamp) &&
        index < state.next &&
        !state.pendingTimestamps.has(timestamp))
    ) {
      if (first < 0 || !frames[first]?.keyframe)
        throw new Error(
          `No preceding keyframe within ${this.maxGopFrames} packets; software fallback uses the same bounded-group limit`,
        );
      state = this._createState(track, first, timestamp);
    }
    state.targetTimestamp = timestamp;
    // Drop now-obsolete cached frames without keeping an entire decoded GOP.
    for (const [time, entry] of state.outputs)
      if (time < timestamp) {
        entry.frame.close();
        state.outputs.delete(time);
        state.cacheBytes -= entry.bytes;
      }
    this.stats.cacheBytes = state.cacheBytes;
    if (state.outputs.has(timestamp)) this.stats.cacheHits++;
    let submitted = 0;
    let compressedBytes = 0;
    while (!state.outputs.has(timestamp)) {
      this._check(operation);
      if (state.failure) throw state.failure;
      if (state.next >= frames.length) {
        // Flush only at an index boundary/end: WebCodecs requires a new keyframe
        // after flush. A later request therefore starts a new bounded group.
        await state.decoder.flush();
        state.flushed = true;
        this._check(operation);
        if (!state.outputs.has(timestamp))
          throw new Error('Decoder did not return the requested presentation timestamp');
        break;
      }
      const frame = frames[state.next];
      if (
        ++submitted > this.maxGopFrames ||
        !Number.isSafeInteger(frame.size) ||
        (compressedBytes += frame.size) > this.maxGopBytes
      )
        throw new Error('Native keyframe group exceeds its 180-packet / 32 MiB budget');
      const bytes = await this.source.readPacket(frame, { signal: operation.controller.signal });
      this._check(operation);
      state.pendingTimestamps.add(Math.round(frame.pts * 1e6));
      state.decoder.decode(
        new EncodedVideoChunk({
          type: frame.keyframe ? 'key' : 'delta',
          timestamp: Math.round(frame.pts * 1e6),
          data: bytes,
        }),
      );
      state.next++;
      this.stats.submittedPackets++;
      await this._progress(state, operation);
    }
    this._check(operation);
    const selected = state.outputs.get(timestamp);
    state.outputs.delete(timestamp);
    state.cacheBytes -= selected.bytes;
    this.stats.cacheBytes = state.cacheBytes;
    try {
      const width = selected.frame.displayWidth;
      const height = selected.frame.displayHeight;
      if (!this._canvas || this._canvas.width !== width || this._canvas.height !== height)
        this._canvas = new OffscreenCanvas(width, height);
      const context = this._canvas.getContext('2d', { willReadFrequently: true });
      if (!context) throw new Error('Browser cannot create a frame canvas');
      context.drawImage(selected.frame, 0, 0);
      const data = context.getImageData(0, 0, width, height).data;
      this.mode = 'WebCodecs · native browser';
      return {
        kind: 'rgba',
        width,
        height,
        data,
        pts: target.pts,
        frameId: target.id,
        decoder: 'webcodecs',
      };
    } finally {
      selected.frame.close();
    }
  }
  close() {
    if (this.closed) return;
    this.closed = true;
    this._generation++;
    this._operation?.controller.abort();
    this._operation = null;
    this._dropState();
    this.software?.close();
    this.software = null;
    this._canvas = null;
    this._probes.clear();
    this._unsupported.clear();
  }
}
