import test from 'node:test';
import assert from 'node:assert/strict';
import {
  createMiniMatroska,
  selectDecodeGroup,
  SoftwareDecoder,
  annexBToLengthPrefixed,
} from '../src/media/software-decoder.js';

test('decode groups use decode order and stop at the requested frame', () => {
  const frames = [
    { id: 0, trackId: 1, pts: 0, size: 20, keyframe: true },
    { id: 1, trackId: 2, pts: 0, size: 10, keyframe: true },
    { id: 2, trackId: 1, pts: 0.08, size: 30, keyframe: false },
    { id: 3, trackId: 1, pts: 0.04, size: 15, keyframe: false },
    { id: 4, trackId: 1, pts: 0.12, size: 20, keyframe: true },
  ];
  assert.deepEqual(
    selectDecodeGroup({ frames }, frames[3]).map((f) => f.id),
    [0, 2, 3],
  );
  assert.deepEqual(
    selectDecodeGroup({ frames }, frames[4]).map((f) => f.id),
    [4],
  );
  assert.throws(() => selectDecodeGroup({ frames }, frames[3], { maxFrames: 2 }), /180|2 frames/);
  assert.throws(() => selectDecodeGroup({ frames }, frames[3], { maxBytes: 60 }), /budget/);
});

test('single thermal keyframe yields bounded Matroska with source packet and CodecPrivate', async () => {
  const packet = Uint8Array.of(0xef, 0xab, 0xcd);
  const privateData = Uint8Array.of(0x56, 0x06, 0x1e, 0x2f);
  const track = { codec: 'ffv1', width: 640, height: 512, codecPrivate: privateData };
  const result = createMiniMatroska(track, [{ pts: 7427.117, keyframe: true }], [packet]);
  const bytes = new Uint8Array(await result.blob.arrayBuffer());
  assert.equal(result.origin, 7427.117);
  assert.ok(bytes.length < 1024);
  assert.deepEqual(Array.from(bytes.subarray(0, 4)), [0x1a, 0x45, 0xdf, 0xa3]);
  assert.deepEqual(Array.from(bytes.subarray(-packet.length)), Array.from(packet));
  assert.ok(new TextDecoder().decode(bytes).includes('V_FFV1'));
});

test('mini Matroska rejects missing initialization and unsupported codecs', () => {
  const frame = [{ pts: 0, keyframe: true }];
  const packets = [Uint8Array.of(1)];
  assert.throws(
    () => createMiniMatroska({ codec: 'avc', width: 1, height: 1 }, frame, packets),
    /initialization/,
  );
  assert.throws(
    () => createMiniMatroska({ codec: 'vp9', width: 1, height: 1 }, frame, packets),
    /Unsupported/,
  );
});

test('aborting an in-flight synchronous worker decode terminates it and rejects the caller', async () => {
  let worker;
  let decodeStarted;
  const started = new Promise((resolve) => {
    decodeStarted = resolve;
  });
  const decoder = new SoftwareDecoder({
    workerFactory: () =>
      (worker = {
        terminated: false,
        postMessage(message) {
          if (message.type === 'load')
            queueMicrotask(() =>
              this.onmessage({ data: { id: message.id, result: { loaded: true } } }),
            );
          else decodeStarted();
        },
        terminate() {
          this.terminated = true;
        },
      }),
  });
  const frame = { id: 0, trackId: 1, pts: 0, keyframe: true, size: 3 };
  await decoder.open(new Blob(), {
    frames: [frame],
    tracks: [{ id: 1, codec: 'ffv1', width: 1, height: 1, codecPrivate: Uint8Array.of(1) }],
    readPacket: async () => Uint8Array.of(1, 2, 3),
  });
  const abort = new AbortController();
  const pending = decoder.decode(frame, { signal: abort.signal });
  await started;
  abort.abort();
  await assert.rejects(pending, { name: 'AbortError' });
  assert.equal(worker.terminated, true);
  decoder.close();
});

test('Annex-B AVC/HEVC conversion preserves NAL payloads for Matroska CodecPrivate', () => {
  const input = Uint8Array.of(0, 0, 0, 1, 0x67, 2, 3, 0, 0, 1, 0x68, 4);
  assert.deepEqual(
    Array.from(annexBToLengthPrefixed(input)),
    [0, 0, 0, 3, 0x67, 2, 3, 0, 0, 0, 2, 0x68, 4],
  );
  assert.throws(() => annexBToLengthPrefixed(Uint8Array.of(1, 2, 3)), /start code/);
});

test('packet-read cancellation and input-budget failures preserve an idle warm software worker', async () => {
  let workers = 0,
    worker,
    slow = false,
    release,
    reading;
  const decoder = new SoftwareDecoder({
    workerFactory: () => {
      workers++;
      return (worker = {
        terminated: false,
        terminate() {
          this.terminated = true;
        },
        postMessage(message) {
          queueMicrotask(() =>
            this.onmessage({
              data: {
                id: message.id,
                result:
                  message.type === 'load'
                    ? { loaded: true }
                    : { kind: 'gray16', buffer: new Uint16Array([19000]).buffer, decodeMs: 1 },
              },
            }),
          );
        },
      });
    },
  });
  const frame = { id: 0, trackId: 1, pts: 0, keyframe: true, size: 3 };
  await decoder.open(new Blob(), {
    frames: [frame],
    tracks: [{ id: 1, codec: 'ffv1', width: 1, height: 1, codecPrivate: Uint8Array.of(1) }],
    readPacket: async () => {
      if (slow) {
        reading();
        await new Promise((r) => {
          release = r;
        });
      }
      return Uint8Array.of(1, 2, 3);
    },
  });
  await decoder.decode(frame);
  slow = true;
  const started = new Promise((r) => {
      reading = r;
    }),
    controller = new AbortController();
  const pending = decoder.decode(frame, { signal: controller.signal });
  await started;
  controller.abort();
  release();
  await assert.rejects(pending, { name: 'AbortError' });
  assert.equal(worker.terminated, false);
  slow = false;
  frame.size = 40 * 1024 * 1024;
  await assert.rejects(decoder.decode(frame), /budget/);
  assert.equal(worker.terminated, false);
  frame.size = 3;
  assert.equal((await decoder.decode(frame)).data[0], 19000);
  assert.equal(workers, 1);
  decoder.close();
});

// Exercise worker ownership across a long sequence without relying on WASM's
// platform-dependent failure point. Returned samples must survive recycling.
test('long software playback recycles runtime state and preserves decoded pixels', async () => {
  const workers = [];
  const decoder = new SoftwareDecoder({
    workerFactory: () => {
      const worker = {
        decodes: 0,
        terminated: false,
        terminate() {
          this.terminated = true;
        },
        postMessage(message) {
          assert.equal(this.terminated, false);
          if (message.type === 'decode') this.decodes++;
          queueMicrotask(() =>
            this.onmessage({
              data: {
                id: message.id,
                result:
                  message.type === 'load'
                    ? { loaded: true }
                    : { kind: 'gray16', buffer: new Uint16Array([19000]).buffer, decodeMs: 1 },
              },
            }),
          );
        },
      };
      workers.push(worker);
      return worker;
    },
  });
  const frame = { id: 0, trackId: 1, pts: 0, keyframe: true, size: 3 };
  await decoder.open(new Blob(), {
    frames: [frame],
    tracks: [{ id: 1, codec: 'ffv1', width: 1, height: 1, codecPrivate: Uint8Array.of(1) }],
    readPacket: async () => Uint8Array.of(1, 2, 3),
  });
  for (let i = 0; i < 1000; i++) assert.equal((await decoder.decode(frame)).data[0], 19000);
  assert.equal(workers.length, 32);
  assert.ok(workers.every((worker) => worker.decodes <= 32));
  assert.ok(workers.slice(0, -1).every((worker) => worker.terminated));
  // Model large output dimensions without allocating full pixel buffers in the
  // fake worker: byte-based recycling must trigger before the 32-call limit.
  decoder.source.tracks[0].width = decoder.source.tracks[0].height = 4096;
  for (let i = 0; i < 3; i++) assert.equal((await decoder.decode(frame)).data[0], 19000);
  assert.equal(workers.length, 34);
  assert.equal(workers.at(-1).decodes, 1);
  decoder.close();
  assert.ok(workers.every((worker) => worker.terminated));
});
