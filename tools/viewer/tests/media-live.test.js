import test from 'node:test';
import assert from 'node:assert/strict';
import { openMedia } from '../src/media/containers.js';
import {
  LiveError,
  LiveRecorder,
  MatroskaLiveDemuxer,
  clusterTicks,
  retimeCluster,
} from '../src/media/matroska-live.js';
import { LiveStore, StoreFullError } from '../src/media/live-store.js';
import { parseLiveUrl, liveName } from '../src/media/live-source.js';
import { headBytes, join, liveCluster, liveHead, eb } from './helpers/live-stream.js';

const stream = (ticks) => join(headBytes(), ...ticks.map((t, i) => liveCluster(t, i)));

async function indexFile(bytes) {
  const source = await openMedia(new Blob([bytes]));
  for await (const batch of source.index()) void batch;
  return source;
}
function recorder(options = {}) {
  const clock = { now: 1e6 };
  const store = new LiveStore({ segmentBytes: 256, ...options });
  return { store, clock, rec: new LiveRecorder({ store, now: () => clock.now }) };
}
async function saved(store) {
  const { parts } = await store.snapshot();
  return new Uint8Array(await new Blob(parts).arrayBuffer());
}

test('demuxer frames the prefix and clusters at every chunk split', () => {
  const bytes = stream([1000, 1200, 1400]);
  for (let split = 1; split < bytes.length; split++) {
    const demuxer = new MatroskaLiveDemuxer(),
      items = [...demuxer.push(bytes.slice(0, split)), ...demuxer.push(bytes.slice(split))];
    assert.deepEqual(
      items.map((i) => i.type),
      ['head', 'cluster', 'cluster', 'cluster'],
    );
    assert.deepEqual(items[2].bytes, liveCluster(1200, 1));
  }
  const demuxer = new MatroskaLiveDemuxer(),
    types = [];
  for (const byte of bytes) types.push(...demuxer.push(Uint8Array.of(byte)).map((i) => i.type));
  assert.equal(types.length, 4);
});

test('demuxer rejects MPEG-TS, a known-size Segment and oversized elements', () => {
  assert.throws(
    () => new MatroskaLiveDemuxer().push(Uint8Array.of(0x47, 0x40, 0, 0x10)),
    (e) => e instanceof LiveError && e.code === 'mpegts' && e.terminal,
  );
  const head = liveHead();
  assert.throws(
    () => new MatroskaLiveDemuxer().push(join(head.ebml, eb('18538067', head.info))),
    /unknown-size/,
  );
  const big = join(headBytes(), Uint8Array.of(0x1f, 0x43, 0xb6, 0x75, 0x10, 0x40, 0x00, 0x01));
  assert.throws(() => new MatroskaLiveDemuxer().push(big), /Oversized/);
  assert.throws(
    () => new MatroskaLiveDemuxer().push(join(headBytes(), eb('1549a966', new Uint8Array(2)))),
    /Unexpected/,
  );
});

test('a recorded live stream indexes as the same file would, without the source Duration', async () => {
  const { rec, store } = recorder(),
    bytes = stream([1000, 1200, 1400, 1600]),
    frames = [];
  rec.beginConnection();
  for (let at = 0; at < bytes.length; at += 37)
    frames.push(...(await rec.feed(bytes.slice(at, at + 37))).frames);
  const file = await saved(store),
    source = await indexFile(file);
  assert.equal(frames.length, 4);
  assert.deepEqual(
    frames.map((f) => f.id),
    [0, 1, 2, 3],
  );
  for (const [i, frame] of frames.entries()) {
    const other = source.frames[i];
    for (const key of ['pts', 'offset', 'size', 'keyframe', 'duration', 'timestampTicks'])
      assert.equal(frame[key], other[key], key);
    assert.deepEqual(frame.extents, other.extents);
    assert.deepEqual(frame.telemetry, other.telemetry);
    assert.deepEqual(frame.thermalMetadata, other.thermalMetadata);
    assert.deepEqual(await store.read(frame.offset, frame.size), await source.readPacket(other));
  }
  // 0x4489 Duration is gone; everything else of the prefix is the publisher's
  assert.equal(file.length, bytes.length - 11);
  assert.ok(store.segments.length > 1);
});

test('a reconnect to the same publisher drops the repeated join cluster and keeps the gap', async () => {
  const { rec, clock } = recorder();
  rec.beginConnection();
  const first = (await rec.feed(stream([1000, 1200]))).frames;
  // half a cluster is lost with the connection
  await rec.feed(liveCluster(1400, 2).slice(0, 20));
  clock.now += 3000;
  rec.beginConnection();
  const again = await rec.feed(join(headBytes(), liveCluster(1200, 1), liveCluster(4200, 3)));
  assert.equal(again.tracks, undefined);
  assert.equal(first.length, 2);
  assert.deepEqual(
    again.frames.map((f) => [f.id, f.epoch, f.pts]),
    [[2, 0, 4.2]],
  );
});

test('a restarted publisher starts a new epoch after the frames held, retimed in the stored bytes', async () => {
  const { rec, store, clock } = recorder();
  rec.beginConnection();
  await rec.feed(stream([250, 450]));
  clock.now += 2000;
  rec.beginConnection();
  const next = (await rec.feed(stream([0, 200]))).frames;
  assert.deepEqual(
    next.map((f) => [f.id, f.epoch, f.timestampTicks, f.sourceTimestampTicks]),
    [
      [2, 1, 2450, 0],
      [3, 1, 2650, 200],
    ],
  );
  // a later timestamp that our clock cannot account for is also a new publisher
  clock.now += 1000;
  rec.beginConnection();
  const later = (await rec.feed(join(headBytes(), liveCluster(900000, 9)))).frames;
  assert.deepEqual([later[0].epoch, later[0].timestampTicks], [2, 3650]);
  const source = await indexFile(await saved(store));
  assert.deepEqual(
    source.frames.map((f) => f.timestampTicks),
    [250, 450, 2450, 2650, 3650],
  );
  for (const frame of [...next, ...later]) {
    const other = source.frames[frame.id];
    assert.deepEqual(frame.extents, other.extents);
    assert.deepEqual(frame.telemetry, other.telemetry);
    assert.deepEqual(await store.read(frame.offset, frame.size), await source.readPacket(other));
  }
});

test('retiming grows the timestamp and size fields and leaves the block bytes alone', () => {
  // 126 bytes of content: two more for the timestamp take the size field to two bytes
  const block = eb('a3', new Uint8Array(121).fill(9)),
    cluster = eb('1f43b675', join(eb('e7', Uint8Array.of(255)), block)),
    out = retimeCluster(cluster, 70000);
  assert.equal(cluster.length, 5 + 126);
  assert.equal(clusterTicks(out), 70000);
  assert.equal(out.length, cluster.length + 2 + 1);
  assert.deepEqual(out.slice(-block.length), block);
});

test('changed tracks end the session', async () => {
  const { rec } = recorder();
  rec.beginConnection();
  await rec.feed(stream([0]));
  rec.beginConnection();
  await assert.rejects(
    rec.feed(headBytes(liveHead({ width: 320 }))),
    (e) => e.code === 'tracks' && e.terminal,
  );
});

test('store reads across segments and the tail, and stops when memory is exhausted', async () => {
  const written = [],
    store = new LiveStore({
      segmentBytes: 10,
      writeSegment: async (index, bytes) => {
        written.push(index);
        return new Blob([bytes]);
      },
    }),
    all = Uint8Array.from({ length: 37 }, (_, i) => i);
  for (let at = 0; at < all.length; at += 7)
    assert.equal(await store.append(all.slice(at, at + 7)), at);
  await store.sealing;
  assert.deepEqual(written, [0, 1]);
  assert.deepEqual(await store.read(0, 37), all);
  assert.deepEqual(await store.read(12, 20), all.slice(12, 32));
  assert.deepEqual(await store.read(30, 7), all.slice(30));
  await assert.rejects(store.read(30, 8), /past the end/);
  assert.equal(store.memory, 9);

  const small = new LiveStore({ segmentBytes: 8, memoryLimit: 20 });
  await small.append(new Uint8Array(12));
  await assert.rejects(small.append(new Uint8Array(12)), StoreFullError);
  assert.equal((await small.snapshot()).size, 12);
});

test('a failed segment write keeps the received bytes and stops the recording', async () => {
  const store = new LiveStore({
    segmentBytes: 4,
    writeSegment: async () => {
      throw new DOMException('full', 'QuotaExceededError');
    },
  });
  await store.append(Uint8Array.of(1, 2, 3, 4, 5));
  await assert.rejects(store.append(Uint8Array.of(6)), /storage is full/);
  assert.deepEqual(await store.read(1, 3), Uint8Array.of(2, 3, 4));
  assert.equal((await store.snapshot()).size, 5);
});

test('a cluster that does not parse is not stored, and earlier frames of the message are kept', async () => {
  const { rec, store } = recorder(),
    // a BlockGroup whose block has a laced flag
    bad = eb(
      '1f43b675',
      join(eb('e7', Uint8Array.of(9)), eb('a3', Uint8Array.of(0x81, 0, 0, 2, 1))),
    ),
    out = { frames: [] };
  rec.beginConnection();
  await rec.feed(stream([1000]));
  const size = store.size;
  await assert.rejects(rec.feed(join(liveCluster(1200, 1), bad), out), /Laced/);
  assert.equal(out.frames.length, 1);
  assert.equal(store.size, size + liveCluster(1200, 1).length);
  rec.beginConnection();
  await rec.feed(join(headBytes(), liveCluster(1400, 2)));
  const source = await indexFile(await saved(store));
  assert.deepEqual(
    source.frames.map((f) => f.timestampTicks),
    [1000, 1200, 1400],
  );
});

test('a repeated join cluster does not hide a publisher restart behind it', async () => {
  const { rec, clock } = recorder();
  rec.beginConnection();
  await rec.feed(stream([100]));
  clock.now += 60000;
  rec.beginConnection();
  const next = (await rec.feed(join(headBytes(), liveCluster(100, 0), liveCluster(200, 1)))).frames;
  assert.deepEqual(
    next.map((f) => [f.epoch, f.timestampTicks]),
    [[1, 60100]],
  );
});

test('a large cluster arriving in relay-sized messages is joined once', () => {
  const big = liveCluster(5, 0, { payload: new Uint8Array(3000000).fill(7) }),
    bytes = join(headBytes(), big, liveCluster(6, 1)),
    demuxer = new MatroskaLiveDemuxer(),
    items = [];
  let joins = 0;
  for (let at = 0; at < bytes.length; at += 16384) {
    const before = demuxer.chunks.length;
    items.push(...demuxer.push(bytes.slice(at, at + 16384)));
    if (demuxer.chunks.length <= 1 && before > 1) joins++;
  }
  assert.deepEqual(
    items.map((i) => i.type),
    ['head', 'cluster', 'cluster'],
  );
  assert.deepEqual(items[1].bytes, big);
  assert.ok(joins <= 3, `joined ${joins} times`);
});

test('a short segment write is never used for the saved recording', async () => {
  const store = new LiveStore({
    segmentBytes: 4,
    writeSegment: async (index, bytes) => new Blob([bytes.subarray(0, 2)]),
  });
  await store.append(Uint8Array.of(1, 2, 3, 4));
  await assert.rejects(store.append(Uint8Array.of(5)), /storage failed/);
  assert.deepEqual(await saved(store), Uint8Array.of(1, 2, 3, 4));
});

test('without file storage sealed segments are Blobs and count against the memory limit', async () => {
  const store = new LiveStore({ segmentBytes: 4, memoryLimit: 10 });
  await store.append(Uint8Array.of(1, 2, 3, 4));
  await store.append(Uint8Array.of(5, 6, 7, 8));
  await store.sealing;
  assert.ok(store.segments.every((s) => s.blob instanceof Blob && !s.bytes));
  assert.deepEqual(await store.read(2, 4), Uint8Array.of(3, 4, 5, 6));
  await assert.rejects(store.append(Uint8Array.of(9, 9, 9)), StoreFullError);
});

test('live addresses: wss only from https, and never a password in the address', () => {
  const https = 'https://plot.example/video/';
  assert.equal(
    parseLiveUrl(' wss://proxy.example:40003 ', https).url.href,
    'wss://proxy.example:40003/',
  );
  assert.equal(parseLiveUrl('https://proxy.example:40003/x', https).url.protocol, 'wss:');
  assert.equal(parseLiveUrl('ws://127.0.0.1:9/', 'http://127.0.0.1/').url.protocol, 'ws:');
  assert.throws(() => parseLiveUrl('ws://proxy.example:40003/', https), /wss:\/\//);
  for (const bad of [
    'wss://h:1/?pw=secret',
    'wss://user:secret@h:1/',
    'wss://h:1/?t=secret',
    'secret',
  ])
    assert.throws(
      () => parseLiveUrl(bad, https),
      (e) => !e.message.includes('secret'),
    );
  assert.equal(
    liveName(new URL('wss://proxy.example:40003/'), Date.UTC(2026, 10, 26, 1, 2, 3)),
    'live-proxy.example-40003-20261126T010203Z.mkv',
  );
});
