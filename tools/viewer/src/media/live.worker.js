// Stores and indexes a live stream. The page owns the socket and posts its bytes here.
import { LiveRecorder } from './matroska-live.js';
import { TsLiveRecorder } from './mpegts-live.js';
import { LiveStore } from './live-store.js';

const DIRECTORY = 'live-streams',
  LOCK_PREFIX = 'apcg-live:',
  BATCH_MS = 100;
let store = null,
  recorder = null,
  queue = Promise.resolve(),
  failed = false,
  latest = 0,
  current = 0,
  dead = 0,
  pending = { frames: [], tracks: null, connection: 0 },
  timer = null;

// Deletes recordings whose page has gone; each holds its lock while open.
async function prune(root) {
  const names = [];
  for await (const [name, handle] of root.entries())
    if (handle.kind === 'directory') names.push(name);
  for (const name of names)
    await navigator.locks.request(LOCK_PREFIX + name, { ifAvailable: true }, async (lock) => {
      if (lock) await root.removeEntry(name, { recursive: true }).catch(() => {});
    });
}

// null without OPFS (private browsing): segments then stay in memory
async function segmentWriter(session) {
  try {
    if (!navigator.storage?.getDirectory || !navigator.locks) return null;
    const root = await (
      await navigator.storage.getDirectory()
    ).getDirectoryHandle(DIRECTORY, { create: true });
    // held until this worker is terminated
    await new Promise((locked) => {
      navigator.locks.request(LOCK_PREFIX + session, () => {
        locked();
        return new Promise(() => {});
      });
    });
    await prune(root).catch(() => {});
    const dir = await root.getDirectoryHandle(session, { create: true });
    return async (index, bytes) => {
      const handle = await dir.getFileHandle(String(index).padStart(6, '0'), { create: true });
      if (handle.createSyncAccessHandle) {
        const access = await handle.createSyncAccessHandle();
        try {
          if (access.write(bytes, { at: 0 }) !== bytes.length) throw new Error('Short write');
          access.flush();
        } finally {
          access.close();
        }
      } else {
        const writable = await handle.createWritable();
        await writable.write(bytes);
        await writable.close();
      }
      return handle.getFile();
    };
  } catch {
    return null;
  }
}

function flush() {
  timer = null;
  if (!recorder || (!pending.frames.length && !pending.tracks)) return;
  self.postMessage({
    type: 'batch',
    frames: pending.frames,
    tracks: pending.tracks,
    connection: pending.connection,
    progress: {
      live: true,
      bytes: store.size,
      epochs: recorder.epoch + 1,
      format: recorder.format || 'matroska',
    },
  });
  pending = { frames: [], tracks: null, connection: pending.connection };
}

async function data(own, buffer) {
  try {
    if (failed || own !== current || own === dead) return;
    if (!recorder) {
      // the first byte tells the two stream kinds apart: MPEG-TS sync, or the EBML header
      const first = new Uint8Array(buffer, 0, Math.min(1, buffer.byteLength))[0];
      if (first === undefined) return;
      recorder = first === 0x47 ? new TsLiveRecorder({ store }) : new LiveRecorder({ store });
      recorder.beginConnection();
    }
    // what was stored before a failure part way through a message is still published
    const out = { frames: [] };
    try {
      await recorder.feed(new Uint8Array(buffer), out);
    } finally {
      if (out.tracks) pending.tracks = out.tracks;
      pending.frames.push(...out.frames);
      pending.connection = own;
      if ((out.tracks || out.frames.length) && timer === null) timer = setTimeout(flush, BATCH_MS);
    }
  } catch (e) {
    const terminal = !!e.terminal || e.name === 'StoreFullError';
    if (terminal) failed = true;
    dead = own; // the rest of this connection is ignored
    self.postMessage({
      type: 'error',
      code: e.code || e.name,
      message: e.message,
      terminal,
      connection: own,
    });
  } finally {
    self.postMessage({ type: 'ack', bytes: buffer.byteLength });
  }
}

self.onmessage = ({ data: message }) => {
  if (message.type === 'open') {
    queue = queue.then(async () => {
      const writeSegment = await segmentWriter(message.session);
      store = new LiveStore({ writeSegment });
      self.postMessage({ type: 'opened', persistent: !!writeSegment });
    });
  } else if (message.type === 'connect') {
    const id = (latest = message.connection);
    queue = queue.then(() => {
      current = id;
      recorder?.beginConnection();
    });
  } else if (message.type === 'data') {
    const id = latest;
    queue = queue.then(() => data(id, message.buffer));
  } else if (message.type === 'read') {
    // not queued behind input: a seek must not wait for the stream
    store
      .read(message.offset, message.size)
      .then((bytes) => self.postMessage({ type: 'read', id: message.id, bytes }, [bytes.buffer]))
      .catch((e) => self.postMessage({ type: 'read', id: message.id, error: e.message }));
  } else if (message.type === 'snapshot') {
    queue = queue.then(async () => {
      if (timer !== null) clearTimeout(timer);
      flush();
      const snapshot = store ? await store.snapshot() : { parts: [], size: 0 };
      self.postMessage({ type: 'snapshot', id: message.id, ...snapshot });
    });
  }
};
