import test from 'node:test';
import assert from 'node:assert/strict';
import {
  parseRemoteUrl,
  remoteName,
  checkExtension,
  progressText,
  downloadFile,
  pruneDownloads,
  handOver,
  reviewKey,
  VIDEO_EXTENSIONS,
} from '../src/media/remote-file.js';

// minimal OPFS and Web Locks stand-ins; hooks let tests interleave other work
function fakeStorage(hooks = {}) {
  const files = new Map();
  const dir = {
    async getFileHandle(name, { create } = {}) {
      if (!files.has(name)) {
        if (!create) throw new DOMException('missing', 'NotFoundError');
        if (hooks.create) await hooks.create(name);
        files.set(name, new File([], name));
      }
      return {
        kind: 'file',
        getFile: async () => files.get(name),
        async createWritable() {
          if (hooks.createWritable) await hooks.createWritable();
          const parts = [];
          return {
            write: async (chunk) => parts.push(chunk),
            async close() {
              files.set(name, new File(parts, name));
              if (hooks.close) await hooks.close();
            },
            abort: async () => {},
          };
        },
      };
    },
    async removeEntry(name) {
      files.delete(name);
    },
    async *entries() {
      if (hooks.listing) await hooks.listing();
      const names = [...files.keys()];
      for (const name of names) yield [name, { kind: 'file' }];
    },
  };
  return {
    files,
    getDirectory: async () => {
      if (hooks.getDirectory) await hooks.getDirectory();
      return { getDirectoryHandle: async () => dir };
    },
    estimate: async () => ({ quota: 1e9, usage: 0 }),
  };
}
function fakeLocks() {
  const held = new Set();
  return {
    held,
    async request(name, options, callback) {
      if (!callback) [options, callback] = [{}, options];
      if (held.has(name)) {
        if (options.ifAvailable) return callback(null);
        throw new Error('fake locks only support ifAvailable contention');
      }
      held.add(name);
      try {
        return await callback({ name });
      } finally {
        held.delete(name);
      }
    },
  };
}
const tick = () => new Promise((resolve) => setTimeout(resolve));
// a body whose cancellation is observable
function trackedResponse(status, headers = {}, data = body) {
  const tracked = { cancelled: false };
  const stream = new ReadableStream({
    pull(controller) {
      controller.enqueue(data);
      controller.close();
    },
    cancel() {
      tracked.cancelled = true;
    },
  });
  tracked.fetch = async () =>
    new Response(stream, {
      status,
      headers: { 'content-length': String(data.length), ...headers },
    });
  return tracked;
}
const body = new Uint8Array(3000).map((_, i) => i & 255);
const respond =
  (status = 200, headers = {}, data = body) =>
  async () =>
    new Response(status === 200 ? data : 'nope', {
      status,
      headers: { 'content-length': String(data.length), ...headers },
    });
const url = new URL('https://example.org/data/2025-10-16_flight2_thermal.mkv');
const options = { extensions: VIDEO_EXTENSIONS, kind: 'video' };

test('URL parameters accept http(s) only and name files from the path', () => {
  assert.equal(parseRemoteUrl('https://a.b/x.mkv').href, 'https://a.b/x.mkv');
  assert.equal(parseRemoteUrl('x.mp4', 'http://a.b/video/').href, 'http://a.b/video/x.mp4');
  assert.throws(() => parseRemoteUrl('file:///etc/passwd'), /http and https/);
  assert.throws(() => parseRemoteUrl('javascript:alert(1)'), /http and https/);
  assert.throws(() => parseRemoteUrl('http://'), /Invalid URL/);
  assert.throws(() => parseRemoteUrl('http://a.b/x.mkv', 'https://c.d/video/'), /mixed content/);
  assert.equal(remoteName(new URL('https://a.b/d/my%20flight.ts?x=1')), 'my flight.ts');
  assert.equal(remoteName(new URL('https://a.b/')), 'a.b');
});

test('extensions outside the chooser list are rejected; none defers to content', () => {
  checkExtension('a.MKV', VIDEO_EXTENSIONS, 'video');
  checkExtension('download', VIDEO_EXTENSIONS, 'video');
  assert.throws(() => checkExtension('a.avi', VIDEO_EXTENSIONS, 'video'), /not a supported video/);
});

test('progress text reports percent, MB and ETA when the size is known', () => {
  assert.equal(progressText(5e6, 0, 5000), '5.0 MB');
  assert.equal(progressText(25e6, 100e6, 500), '25% · 25.0 / 100.0 MB');
  assert.equal(progressText(25e6, 100e6, 10_000), '25% · 25.0 / 100.0 MB · 30 s left');
  assert.equal(progressText(10e6, 100e6, 10_000), '10% · 10.0 / 100.0 MB · 1 min 30 s left');
});

test('downloads stream into OPFS, keep the remote name and stay locked until released', async () => {
  const storage = fakeStorage(),
    locks = fakeLocks(),
    seen = [];
  const { file, release } = await downloadFile(url, {
    ...options,
    storage,
    locks,
    fetch: respond(200, { 'last-modified': 'Thu, 16 Oct 2025 03:00:00 GMT' }),
    onProgress: (text, fraction) => seen.push(fraction),
  });
  assert.equal(file.name, '2025-10-16_flight2_thermal.mkv');
  assert.equal(file.lastModified, Date.parse('2025-10-16T03:00:00Z'));
  assert.deepEqual(new Uint8Array(await file.arrayBuffer()), body);
  assert.equal(seen.at(-1), 1);
  assert.equal(storage.files.size, 1);
  await pruneDownloads({ storage, locks });
  assert.equal(storage.files.size, 1, 'a held download survives pruning');
  release();
  await tick();
  await pruneDownloads({ storage, locks });
  assert.equal(storage.files.size, 0);
});

test('without OPFS the download is held in memory', async () => {
  const { file } = await downloadFile(url, { ...options, storage: null, fetch: respond() });
  assert.equal(file.size, body.length);
});

test('HTTP, CORS, type, truncation and abort failures are explained and cleaned up', async () => {
  const storage = fakeStorage(),
    locks = fakeLocks();
  const run = (fetch, extra = {}) =>
    downloadFile(url, { ...options, storage, locks, fetch, ...extra });
  await assert.rejects(run(respond(404)), /HTTP 404/);
  await assert.rejects(
    run(async () => {
      throw new TypeError('Failed to fetch');
    }),
    /Access-Control-Allow-Origin/,
  );
  await assert.rejects(run(respond(200, { 'content-type': 'text/html' })), /web page/);
  await assert.rejects(
    run(async () => new Response(body, { headers: { 'content-length': '9999' } })),
    /ended early/,
  );
  const controller = new AbortController();
  controller.abort();
  await assert.rejects(
    run(
      async (u, init) => {
        init.signal.throwIfAborted();
      },
      { signal: controller.signal },
    ),
    { name: 'AbortError' },
  );
  await assert.rejects(
    downloadFile(new URL('https://a.b/x.avi'), { ...options, fetch: respond() }),
    /not a supported video/,
  );
  assert.equal(storage.files.size, 0);
  assert.equal(locks.held.size, 0);
});

test('a download that starts while a prune is running keeps its new file', async () => {
  const locks = fakeLocks();
  let racing;
  const storage = fakeStorage({
    // another page creates its download as this prune lists the directory
    listing: async () => {
      if (racing) return;
      racing = downloadFile(url, { ...options, storage, locks, fetch: respond() });
      while (![...storage.files.keys()].some((name) => name !== 'orphan')) await tick();
    },
  });
  storage.files.set('orphan', new File(['x'], 'orphan'));
  await pruneDownloads({ storage, locks });
  const { file, release } = await racing;
  assert.deepEqual(new Uint8Array(await file.arrayBuffer()), body);
  assert.equal(storage.files.has('orphan'), false);
  assert.equal(storage.files.size, 1, 'the racing download survived');
  release();
});

test('a prune while another page reads its file leaves it alone', async () => {
  const storage = fakeStorage(),
    locks = fakeLocks();
  const reader = await downloadFile(url, { ...options, storage, locks, fetch: respond() });
  const other = await downloadFile(url, { ...options, storage, locks, fetch: respond() });
  assert.equal(storage.files.size, 2);
  assert.deepEqual(new Uint8Array(await reader.file.arrayBuffer()), body);
  reader.release();
  other.release();
});

test('storage full is an error, not a silent memory fallback', async () => {
  const quota = () => {
    throw new DOMException('full', 'QuotaExceededError');
  };
  for (const hook of ['getDirectory', 'create', 'createWritable']) {
    const locks = fakeLocks(),
      storage = fakeStorage({ [hook]: quota }),
      tracked = trackedResponse(200);
    await assert.rejects(
      downloadFile(url, { ...options, storage, locks, fetch: tracked.fetch }),
      /Not enough browser storage/,
      hook,
    );
    assert.equal(tracked.cancelled, true, hook);
    assert.equal(storage.files.size, 0, hook);
    assert.equal(locks.held.size, 0, hook);
  }
});

test('the in-memory fallback is bounded with or without a content length', async () => {
  await assert.rejects(
    downloadFile(url, { ...options, storage: null, memoryLimit: 1000, fetch: respond() }),
    /too large to download without browser file storage/,
  );
  const unsized = async () => new Response(new Blob([body]).stream());
  await assert.rejects(
    downloadFile(url, { ...options, storage: null, memoryLimit: 1000, fetch: unsized }),
    /too large/,
  );
  const { file } = await downloadFile(url, {
    ...options,
    storage: null,
    memoryLimit: 1e6,
    fetch: unsized,
  });
  assert.equal(file.size, body.length);
});

test('bodyless responses and rejected responses release everything', async () => {
  const storage = fakeStorage(),
    locks = fakeLocks();
  for (const status of [204, 205])
    await assert.rejects(
      downloadFile(url, {
        ...options,
        storage,
        locks,
        fetch: async () => new Response(null, { status }),
      }),
      /returned no data/,
    );
  for (const [status, headers, message] of [
    [404, {}, /HTTP 404/],
    [500, {}, /HTTP 500/],
    [200, { 'content-type': 'text/html; charset=utf-8' }, /web page/],
  ]) {
    const tracked = trackedResponse(status, headers);
    await assert.rejects(
      downloadFile(url, { ...options, storage, locks, fetch: tracked.fetch }),
      message,
    );
    assert.equal(tracked.cancelled, true, `${status} body cancelled`);
  }
  assert.equal(storage.files.size, 0);
  assert.equal(locks.held.size, 0);
});

test('a cancel during finalisation never yields a file', async () => {
  for (const hook of ['close', 'create']) {
    const controller = new AbortController(),
      locks = fakeLocks();
    const storage = fakeStorage({ [hook]: async () => controller.abort() });
    await assert.rejects(
      downloadFile(url, {
        ...options,
        storage,
        locks,
        signal: controller.signal,
        fetch: respond(),
      }),
      { name: 'AbortError' },
      hook,
    );
    assert.equal(storage.files.size, 0, hook);
    assert.equal(locks.held.size, 0, hook);
  }
});

test("a prune leaves an active download's swap file alone", async () => {
  const storage = fakeStorage(),
    locks = fakeLocks();
  for (const name of ['active', 'active.crswap', 'dead', 'dead.crswap'])
    storage.files.set(name, new File([], name));
  let done;
  locks.request('apcg-url-download:active', () => new Promise((resolve) => (done = resolve)));
  await pruneDownloads({ storage, locks });
  assert.deepEqual([...storage.files.keys()].sort(), ['active', 'active.crswap']);
  done();
});

test('a throwing handover releases the download and keeps the previous copy', async () => {
  const released = [];
  const download = (name) => ({ file: new File([], name), release: () => released.push(name) });
  const slot = {};
  await handOver(slot, download('a'), () => {});
  assert.deepEqual(released, []);
  // synchronous throw (loadLog) and rejected promise (openVideo)
  for (const open of [
    () => {
      throw new Error('worker failed');
    },
    async () => {
      throw new Error('worker failed');
    },
  ])
    await assert.rejects(handOver(slot, download('b'), open), /worker failed/);
  assert.deepEqual(released, ['b', 'b']);
  await handOver(slot, download('c'), async () => {});
  assert.deepEqual(released, ['b', 'b', 'a'], 'the previous copy is released on success');
  slot.release();
  assert.deepEqual(released, ['b', 'b', 'a', 'c']);
});

test('review keys separate same-named URL files and keep local keys unchanged', () => {
  const file = new File([new Uint8Array(4)], 'clip.mkv', { lastModified: 0 });
  assert.equal(reviewKey(file), 'fire-review:clip.mkv:4:0');
  assert.equal(reviewKey(null, 'https://a.example/clip.mkv'), '');
  const a = reviewKey(file, 'https://a.example/x/clip.mkv'),
    b = reviewKey(file, 'https://b.example/x/clip.mkv');
  assert.notEqual(a, b);
  assert.notEqual(a, reviewKey(file));
  assert.equal(a, 'fire-review:url:https://a.example/x/clip.mkv:clip.mkv:4:0');
  assert.equal(reviewKey(file, 'https://a.example/x/clip.mkv#t=3'), a);
  assert.notEqual(reviewKey(file, 'https://a.example/x/clip.mkv?id=2'), a);
});
