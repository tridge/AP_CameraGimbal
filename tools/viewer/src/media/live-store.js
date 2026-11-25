/** Append-only bytes of a live recording with random reads: sealed segments plus a tail. */
export const SEGMENT_BYTES = 8 * 1024 * 1024,
  MEMORY_LIMIT = 512 * 1024 * 1024;

export class StoreFullError extends Error {
  constructor(message) {
    super(message);
    this.name = 'StoreFullError';
  }
}

export class LiveStore {
  /**
   * writeSegment(index, bytes) resolves to a Blob of the stored segment; without it segments
   * are Blobs held by the browser, up to memoryLimit in all.
   */
  constructor({ writeSegment = null, segmentBytes = SEGMENT_BYTES, memoryLimit = MEMORY_LIMIT }) {
    this.writeSegment = writeSegment;
    this.segmentBytes = segmentBytes;
    this.memoryLimit = memoryLimit;
    this.segments = []; // {start, size, blob | bytes}
    this.tail = [];
    this.tailStart = 0;
    this.size = 0;
    this.memory = 0;
    this.full = null;
    this.sealing = Promise.resolve();
  }
  /** Resolves to the offset the bytes were stored at; rejects once storage is exhausted. */
  async append(bytes) {
    await this.sealing;
    if (this.full) throw this.full;
    if (!this.writeSegment && this.memory + bytes.length > this.memoryLimit)
      throw (this.full = new StoreFullError(
        `Browser file storage is unavailable and the ${Math.round(this.memoryLimit / 2 ** 20)} MiB memory limit for a live recording is reached.`,
      ));
    const offset = this.size;
    this.tail.push(bytes);
    this.size += bytes.length;
    this.memory += bytes.length;
    if (this.size - this.tailStart >= this.segmentBytes) this.sealing = this.seal();
    return offset;
  }
  async seal() {
    const bytes = join(this.tail, this.size - this.tailStart),
      segment = { start: this.tailStart, size: bytes.length, bytes };
    this.segments.push(segment);
    this.tail = [];
    this.tailStart = this.size;
    if (!this.writeSegment) {
      // a Blob can be handed to the page and to a saved File without another copy
      segment.blob = new Blob([bytes]);
      segment.bytes = null;
      return;
    }
    try {
      const blob = await this.writeSegment(this.segments.length - 1, bytes);
      if (blob.size !== bytes.length) throw new Error('Short segment write');
      segment.blob = blob;
      segment.bytes = null;
      this.memory -= bytes.length;
    } catch (e) {
      // what was received stays readable from memory
      this.full = new StoreFullError(
        e?.name === 'QuotaExceededError'
          ? 'Browser storage is full; the live recording has stopped growing.'
          : `Browser storage failed (${e?.message || e}); the live recording has stopped growing.`,
      );
    }
  }
  async read(offset, size) {
    if (!Number.isSafeInteger(offset) || !Number.isSafeInteger(size) || offset < 0 || size < 0)
      throw new Error('Invalid live read');
    if (offset + size > this.size) throw new Error('Read past the end of the live recording');
    const parts = [];
    let at = offset,
      left = size,
      lo = 0,
      hi = this.segments.length;
    while (lo < hi) {
      const m = (lo + hi) >>> 1;
      if (this.segments[m].start + this.segments[m].size <= at) lo = m + 1;
      else hi = m;
    }
    for (let i = lo; left && i < this.segments.length; i++) {
      const s = this.segments[i],
        from = at - s.start,
        n = Math.min(left, s.size - from);
      parts.push(
        s.bytes
          ? s.bytes.subarray(from, from + n)
          : new Uint8Array(await s.blob.slice(from, from + n).arrayBuffer()),
      );
      at += n;
      left -= n;
    }
    let start = this.tailStart;
    for (const chunk of this.tail) {
      if (!left) break;
      if (at < start + chunk.length) {
        const from = at - start,
          n = Math.min(left, chunk.length - from);
        parts.push(chunk.subarray(from, from + n));
        at += n;
        left -= n;
      }
      start += chunk.length;
    }
    if (left) throw new Error('Short live read');
    return parts.length === 1 ? parts[0].slice() : join(parts, size);
  }
  /** Blob parts of everything stored so far; segments are immutable, so nothing is copied. */
  async snapshot() {
    await this.sealing;
    return {
      size: this.size,
      parts: [...this.segments.map((s) => s.blob || s.bytes), ...this.tail],
    };
  }
}

function join(parts, size) {
  const out = new Uint8Array(size);
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}
