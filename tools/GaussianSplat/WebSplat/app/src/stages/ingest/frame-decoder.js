/** Bounded batch FFV1 decoding of indexed frames, for extraction rather than seeking.
 * Reuses the viewer's Matroska group builder; each batch becomes one small Matroska Blob
 * decoded by one FFmpeg exec in a pool of classic workers.
 */
import { createMiniMatroska } from '../../../../../../viewer/src/media/software-decoder.js';
import { FFMPEG_VERSION } from '../../../../../../viewer/src/versions.js';

const MIB = 1024 * 1024;
const abortError = () => new DOMException('Decode cancelled', 'AbortError');

export function defaultCoreURL(base = globalThis.location?.href) {
  return new URL(`vendor/ffmpeg/${FFMPEG_VERSION}/ffmpeg-core.js`, base).href;
}

export class BatchLimitError extends Error {}

/** Split wanted frame indices into decode batches. Every batch starts at a keyframe; a
 * non-keyframe pulls in its predecessors back to the keyframe (decoded and dropped). Hard bounds,
 * checked before anything is read: at most batchFrames packets, except that a batch holding a
 * dependency chain may grow to maxChainFrames; at most batchBytes compressed and maxOutputBytes
 * decoded. A frame whose chain alone exceeds them is rejected (BatchLimitError). */
export function planBatches(
  frames,
  wanted,
  {
    batchFrames = 16,
    maxChainFrames = 64,
    batchBytes = 24 * MIB,
    frameBytes = 0,
    maxOutputBytes = 128 * MIB,
  } = {},
) {
  const chainLimit = Math.min(
    maxChainFrames,
    frameBytes ? Math.floor(maxOutputBytes / frameBytes) : Infinity,
  );
  if (!(chainLimit >= 1))
    throw new BatchLimitError(
      `a decoded frame (${frameBytes} bytes) exceeds the ${maxOutputBytes} byte output limit`,
    );
  const frameLimit = Math.min(batchFrames, chainLimit);
  const sizeOf = (from, to) => {
    let n = 0;
    for (let i = from; i <= to; i++) n += frames[i].size;
    return n;
  };
  const batches = [];
  let current = null;
  for (const index of [...new Set(wanted)].sort((a, b) => a - b)) {
    if (!Number.isInteger(index) || index < 0 || index >= frames.length)
      throw new Error(`no frame ${index}`);
    let first = index;
    while (first >= 0 && !frames[first].keyframe) first--;
    if (first < 0) throw new Error(`frame ${index} has no preceding keyframe`);
    const chain = index - first + 1;
    if (chain > chainLimit || sizeOf(first, index) > batchBytes)
      throw new BatchLimitError(
        `frame ${index} depends on ${chain} packets from keyframe ${first}, beyond the decode ` +
          `limit of ${chainLimit} packets / ${batchBytes} bytes`,
      );
    if (current) {
      const continues = current.last >= first; // same decode chain
      const from = continues ? current.last + 1 : first;
      const bytes = sizeOf(from, index);
      const n = current.packets.length + index - from + 1;
      const chained = current.chained || chain > 1;
      if (n <= (chained ? chainLimit : frameLimit) && current.bytes + bytes <= batchBytes) {
        for (let i = from; i <= index; i++) current.packets.push(i);
        current.bytes += bytes;
        current.wanted.push(index);
        current.last = index;
        current.chained = chained;
        continue;
      }
    }
    current = {
      packets: [],
      wanted: [index],
      bytes: sizeOf(first, index),
      last: index,
      chained: chain > 1,
    };
    for (let i = first; i <= index; i++) current.packets.push(i);
    batches.push(current);
  }
  return batches;
}

export class FrameDecoder {
  /** ingest: indexMkv result. workers: decode workers in parallel. */
  constructor(
    ingest,
    {
      coreURL = defaultCoreURL(),
      workerFactory,
      workers = 3,
      batchFrames = 16,
      maxChainFrames = 64,
      batchBytes = 24 * MIB,
    } = {},
  ) {
    this.ingest = ingest;
    this.coreURL = coreURL;
    // literal new Worker(new URL(...)) so bundlers emit the worker
    this.workerFactory =
      workerFactory ?? (() => new Worker(new URL('./ffv1-batch.worker.js', import.meta.url)));
    this.poolSize = Math.max(1, workers);
    this.limits = { batchFrames, maxChainFrames, batchBytes, maxOutputBytes: 128 * MIB };
    this.pool = [];
    this.sequence = 0;
    this.stats = { frames: 0, packets: 0, decodeMs: 0, readMs: 0 };
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
    entry.ready = entry.call('load', { coreURL: this.coreURL });
    await entry.ready;
    return entry;
  }

  async _decodeBatch(slot, batch, signal) {
    const { frames, source, track } = this.ingest;
    const t0 = performance.now();
    const packets = [];
    for (const i of batch.packets) {
      if (signal?.aborted) throw abortError();
      const p = await source.readPacket(
        { offset: frames[i].offset, size: frames[i].size },
        { signal },
      );
      if (p.byteLength !== frames[i].size) throw new Error('truncated compressed packet');
      packets.push(p.slice());
    }
    const group = batch.packets.map((i) => frames[i]);
    const srcTrack = source.tracks.find((t) => t.id === track.id);
    const { blob, origin } = createMiniMatroska(srcTrack, group, packets);
    this.stats.readMs += performance.now() - t0;
    const entry = await this._worker(slot);
    if (signal?.aborted) throw abortError();
    const result = await entry.call('decode', {
      blob,
      times: group.map((f) => f.pts - origin),
      width: track.width,
      height: track.height,
    });
    this.stats.decodeMs += result.decodeMs;
    this.stats.packets += group.length;
    const frameBytes = track.width * track.height * 2;
    const out = new Map();
    batch.packets.forEach((index, k) => {
      if (batch.wanted.includes(index))
        out.set(index, new Uint16Array(result.buffer.slice(k * frameBytes, (k + 1) * frameBytes)));
    });
    return out;
  }

  /** Yield {index, meta, data: Uint16Array (row-major gray16, host order)} for each wanted index,
   * ascending. At most poolSize + 1 batches are in flight. */
  async *decode(wanted, { signal } = {}) {
    if (new Uint8Array(new Uint16Array([1]).buffer)[0] !== 1) throw new Error('big-endian host');
    const { width, height } = this.ingest.track;
    const batches = planBatches(this.ingest.frames, wanted, {
      ...this.limits,
      frameBytes: width * height * 2,
    });
    // cancellation stops running FFmpeg commands too: their workers are terminated
    const onAbort = () => this.close();
    signal?.addEventListener('abort', onAbort, { once: true });
    try {
      yield* this._run(batches, signal);
    } finally {
      signal?.removeEventListener('abort', onAbort);
    }
  }

  async *_run(batches, signal) {
    const inflight = [];
    let next = 0;
    const launch = () => {
      const slot = next % this.poolSize;
      const batch = batches[next++];
      const prev = inflight.filter((p) => p.slot === slot).at(-1)?.promise ?? Promise.resolve();
      const promise = prev.catch(() => {}).then(() => this._decodeBatch(slot, batch, signal));
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
        if (signal?.aborted) throw abortError();
        this.stats.frames++;
        yield { index, meta: this.ingest.frames[index].meta, data: out.get(index) };
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
