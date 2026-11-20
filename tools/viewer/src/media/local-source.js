import { openMedia } from './containers.js';
/** Local adapter. A live adapter can emit the same batches without File objects. */
export class LocalSource {
  constructor(file, { onBatch, onStatus, onError, onDone }) {
    this.file = file;
    this.frames = [];
    this.tracks = [];
    this.closed = false;
    this.reader = null;
    this.callbacks = { onBatch, onStatus, onError, onDone };
    this.worker = new Worker(new URL('./index.worker.js', import.meta.url), { type: 'module' });
    this.worker.onmessage = ({ data }) => {
      if (this.closed) return;
      if (data.type === 'opened') {
        this.tracks = data.tracks;
        this.selectedTrackId = data.tracks[0]?.id;
        this.format = data.format;
        onStatus?.(`Indexing ${this.format}`);
      }
      if (data.type === 'batch') {
        if (data.tracks) this.tracks = data.tracks;
        this.selectedTrackId ??= this.tracks[0]?.id;
        const frames = data.frames.filter((frame) => frame.trackId === this.selectedTrackId);
        this.frames.push(...frames);
        onBatch?.({ ...data, frames: frames.filter((frame) => frame.presentable !== false) });
      }
      if (data.type === 'done') {
        if (data.tracks) this.tracks = data.tracks;
        onDone?.();
      }
      if (data.type === 'error') onError?.(new Error(data.message));
    };
    this.worker.onerror = (event) => onError?.(new Error(event.message || 'Index worker failed'));
    this.worker.postMessage({ type: 'open', file });
  }
  async readPacket(frame, options = {}) {
    if (this.closed) throw new DOMException('Source closed', 'AbortError');
    // The independent reader is initialized lazily and never scans the file.
    if (!this.readerPromise)
      this.readerPromise = openMedia(this.file).then((reader) => {
        if (this.closed) {
          reader.close();
          throw new DOMException('Source closed', 'AbortError');
        }
        this.reader = reader;
        return reader;
      });
    const reader = await this.readerPromise;
    if (this.closed) throw new DOMException('Source closed', 'AbortError');
    return reader.readPacket(frame, options);
  }
  close() {
    this.closed = true;
    this.worker.terminate();
    this.reader?.close();
    this.frames.length = 0;
  }
}
