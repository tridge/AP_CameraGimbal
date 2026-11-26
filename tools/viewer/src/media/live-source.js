/** A SupportProxy live stream as a source: the LocalSource surface plus connection state. */
import { transportPayload } from './containers.js';

export const MAX_BACKLOG = 64 * 1024 * 1024;
const RETRY_MS = [500, 1000, 2000, 5000];

/** wss://HOST:PORT/ of a SupportProxy video port; ws: only from an http page (local tests). */
export function parseLiveUrl(text, base = globalThis.location?.href) {
  // messages do not repeat the input: it may hold a password
  let url;
  try {
    url = new URL(String(text).trim());
  } catch {
    throw new Error('Invalid live stream address; it looks like wss://HOST:PORT/');
  }
  if (url.protocol === 'https:') url.protocol = 'wss:';
  if (url.protocol === 'http:') url.protocol = 'ws:';
  if (!['ws:', 'wss:'].includes(url.protocol))
    throw new Error('A live stream address starts with wss://');
  if (url.protocol === 'ws:' && base && new URL(base).protocol === 'https:')
    throw new Error('An https page can only open a wss:// live stream.');
  // a password in the address would reach this site's server and the browser history
  if (url.username || url.password || url.searchParams.has('pw') || url.searchParams.has('t'))
    throw new Error(
      'Put the viewer password in the password box, or after #pw= in a link, not in the address.',
    );
  url.search = '';
  url.hash = '';
  return { url };
}

/** Name for the recording: live-HOST-PORT-YYYYMMDDTHHMMSSZ.mkv, or .ts for MPEG-TS video */
export function liveName(url, started, format = 'matroska') {
  const stamp = new Date(started).toISOString().replace(/[-:]|\.\d+/g, '');
  return `live-${url.hostname}${url.port ? '-' + url.port : ''}-${stamp}.${format === 'mpegts' ? 'ts' : 'mkv'}`;
}

export class LiveSource {
  /**
   * onLive({state, detail}) with state connecting | waiting | live | reconnecting | ended |
   * failed. Frames arrive in presentation order with id equal to their index.
   */
  constructor(
    url,
    {
      password = null,
      onBatch,
      onStatus,
      onError,
      onLive,
      WebSocket = globalThis.WebSocket,
      workerFactory = () =>
        new Worker(new URL('./live.worker.js', import.meta.url), { type: 'module' }),
      now = () => Date.now(),
    } = {},
  ) {
    this.url = url;
    this.password = password;
    this.callbacks = { onBatch, onStatus, onError, onLive };
    this.WebSocket = WebSocket;
    this.frames = [];
    this.tracks = [];
    this.format = 'matroska';
    this.live = true;
    this.started = now();
    this.name = liveName(url, this.started);
    this.size = 0;
    this.state = 'connecting';
    this.closed = this.ended = false;
    this.attempt = 0;
    this.backlog = 0;
    this.connection = 0;
    this.socket = null;
    this.retry = null;
    this.requests = new Map();
    this.sequence = 0;
    this.worker = workerFactory();
    this.worker.onmessage = ({ data }) => this.message(data);
    this.worker.onerror = (event) => {
      // nothing will answer a request now
      this.dead = true;
      for (const request of this.requests.values())
        request.reject(new Error('Live stream worker failed'));
      this.requests.clear();
      this.fail(new Error(event.message || 'Live stream worker failed'));
    };
    this.worker.postMessage({
      type: 'open',
      session: `${this.started.toString(36)}-${Math.random().toString(36).slice(2, 8)}`,
    });
    this.connect();
  }
  set(state, detail = '') {
    this.state = state;
    this.detail = detail;
    this.callbacks.onLive?.({ state, detail });
  }
  message(data) {
    if (this.closed) return;
    if (data.type === 'opened') this.persistent = data.persistent;
    else if (data.type === 'ack') this.backlog -= data.bytes;
    else if (data.type === 'batch') {
      if (data.tracks) {
        this.tracks = data.tracks;
        this.selectedTrackId = data.tracks[0]?.id;
      }
      this.frames.push(...data.frames);
      if (data.progress.format !== this.format) {
        // known from the first bytes: raw thermal Matroska, or H.264/HEVC in MPEG-TS
        this.format = data.progress.format;
        this.name = liveName(this.url, this.started, this.format);
      }
      this.size = data.progress.bytes;
      this.epochs = data.progress.epochs;
      // frames of a connection that has since closed do not make this one live
      if (
        data.frames.length &&
        this.state !== 'live' &&
        !this.ended &&
        this.socket &&
        data.connection === this.connection
      )
        this.set('live');
      this.callbacks.onBatch?.({ ...data, tracks: this.tracks, warnings: [] });
    } else if (data.type === 'error') {
      if (data.terminal) this.fail(new Error(data.message));
      else if (data.connection === this.connection) {
        // malformed input: drop this connection and join again at a fresh Cluster
        this.detail = data.message;
        this.socket?.close();
      }
    } else if (data.type === 'read' || data.type === 'snapshot') {
      const request = this.requests.get(data.id);
      this.requests.delete(data.id);
      if (data.error) request?.reject(new Error(data.error));
      else request?.resolve(data);
    }
  }
  // capture is over; what was received stays readable
  fail(error) {
    if (this.closed || this.ended) return;
    this.stop();
    this.set('failed', error.message);
    this.callbacks.onError?.(error);
  }
  stop() {
    this.ended = true;
    clearTimeout(this.retry);
    const socket = this.socket;
    this.socket = null;
    if (socket) {
      socket.onopen = socket.onmessage = socket.onclose = socket.onerror = null;
      socket.close();
    }
  }
  connect() {
    if (this.closed || this.ended) return;
    const target = new URL(this.url);
    if (this.password) target.searchParams.set('pw', this.password);
    const connection = ++this.connection;
    let socket,
      opened = false,
      received = 0;
    // a refusal by this page's own policy is permanent; say so rather than retry
    const onViolation = (e) => {
      if (e.effectiveDirective === 'connect-src' && e.blockedURI?.startsWith(this.url.origin))
        this.fail(
          new Error(
            `This viewer's Content-Security-Policy (connect-src) does not allow ${this.url.origin}.`,
          ),
        );
    };
    globalThis.document?.addEventListener('securitypolicyviolation', onViolation);
    setTimeout(
      () => globalThis.document?.removeEventListener('securitypolicyviolation', onViolation),
      2000,
    );
    try {
      socket = new this.WebSocket(target.href);
    } catch (e) {
      this.fail(new Error(`Cannot open ${this.url.host}: ${e.message}`));
      return;
    }
    socket.binaryType = 'arraybuffer';
    this.socket = socket;
    if (!this.frames.length) this.set('connecting');
    socket.onopen = () => {
      opened = true;
      this.worker.postMessage({ type: 'connect', connection });
      this.set(
        this.frames.length ? 'reconnecting' : 'waiting',
        'Connected; waiting for the camera to send video',
      );
    };
    socket.onmessage = ({ data }) => {
      if (this.socket !== socket || socket.readyState !== 1) return;
      if (!(data instanceof ArrayBuffer) || !data.byteLength) return;
      received += data.byteLength;
      this.attempt = 0;
      if (this.backlog + data.byteLength > MAX_BACKLOG) {
        // the page cannot keep up: rejoin at the newest frame
        this.detail = 'Fell behind the stream';
        socket.close();
        return;
      }
      this.backlog += data.byteLength;
      this.worker.postMessage({ type: 'data', buffer: data }, [data]);
    };
    socket.onclose = () => {
      if (this.socket !== socket) return;
      this.socket = null;
      const delay = RETRY_MS[Math.min(this.attempt++, RETRY_MS.length - 1)];
      // only what the browser lets us observe: it does not say why a socket closed
      const detail = received
        ? this.detail || 'The stream stopped'
        : opened
          ? 'The proxy closed the connection before sending video (viewer password wrong or needed, or the camera stopped)'
          : `Could not connect to ${this.url.host} (wrong address or port, or not allowed from this page)`;
      this.detail = '';
      this.set('reconnecting', detail);
      this.retry = setTimeout(() => this.connect(), delay);
    };
  }
  /** A new password, or none, used from the next connection (which starts now). */
  setPassword(password) {
    this.password = password || null;
    if (this.closed || this.ended) return;
    clearTimeout(this.retry);
    const socket = this.socket;
    this.socket = null;
    if (socket) {
      socket.onopen = socket.onmessage = socket.onclose = socket.onerror = null;
      socket.close();
    }
    this.attempt = 0;
    this.connect();
  }
  request(message, transfer = []) {
    if (this.closed) return Promise.reject(new DOMException('Source closed', 'AbortError'));
    if (this.dead) return Promise.reject(new Error('Live stream worker failed'));
    const id = ++this.sequence;
    return new Promise((resolve, reject) => {
      this.requests.set(id, { resolve, reject });
      this.worker.postMessage({ ...message, id }, transfer);
    });
  }
  async readPacket(frame, { signal } = {}) {
    signal?.throwIfAborted();
    // an MPEG-TS frame is spread over the transport packets of its span
    const t = frame.transport,
      offset = t ? t.packetStart : frame.offset,
      size = t ? t.packetEnd - t.packetStart : frame.size;
    const { bytes } = await this.request({ type: 'read', offset, size });
    signal?.throwIfAborted();
    if (this.closed) throw new DOMException('Source closed', 'AbortError');
    return t ? transportPayload(bytes, offset, frame) : bytes;
  }
  /** The recording so far as a File with the frame index's offsets. */
  async snapshot() {
    const { parts } = await this.request({ type: 'snapshot' });
    return new File(parts, this.name, {
      type: this.format === 'mpegts' ? 'video/mp2t' : 'video/x-matroska',
      lastModified: this.started,
    });
  }
  /** Stops receiving; frames and packets stay available until close(). */
  async end() {
    if (this.closed) return null;
    if (!this.ended) {
      this.stop();
      this.set('ended');
    }
    return this.snapshot();
  }
  close() {
    if (this.closed) return;
    this.stop();
    this.closed = true;
    for (const request of this.requests.values())
      request.reject(new DOMException('Source closed', 'AbortError'));
    this.requests.clear();
    // releases the recording's storage lock; the next live stream deletes it
    this.worker.terminate();
    this.frames.length = 0;
  }
}
