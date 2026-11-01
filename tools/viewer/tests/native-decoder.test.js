import test from 'node:test';
import assert from 'node:assert/strict';
import { FrameDecoder } from '../src/media/native-decoder.js';

function fakeBrowser(t, { supported = true, pipelineDepth = 0 } = {}) {
  const saved = {
    VideoDecoder: globalThis.VideoDecoder,
    EncodedVideoChunk: globalThis.EncodedVideoChunk,
    OffscreenCanvas: globalThis.OffscreenCanvas,
  };
  const counts = { probes: 0, configured: 0, framesCreated: 0, framesClosed: 0, decodersClosed: 0 };
  globalThis.EncodedVideoChunk = class {
    constructor(values) {
      Object.assign(this, values);
    }
  };
  globalThis.OffscreenCanvas = class {
    constructor(width, height) {
      this.width = width;
      this.height = height;
    }
    getContext() {
      return {
        drawImage: (frame) => {
          this.timestamp = frame.timestamp;
        },
        getImageData: () => ({ data: new Uint8ClampedArray([this.timestamp / 1000, 0, 0, 255]) }),
      };
    }
  };
  globalThis.VideoDecoder = class extends EventTarget {
    static async isConfigSupported() {
      counts.probes++;
      return { supported };
    }
    constructor(callbacks) {
      super();
      this.callbacks = callbacks;
      this.state = 'unconfigured';
      this.decodeQueueSize = 0;
      this.pending = [];
    }
    configure() {
      counts.configured++;
      this.state = 'configured';
    }
    decode(chunk) {
      this.decodeQueueSize++;
      queueMicrotask(() => {
        if (this.state === 'closed') return;
        this.pending.push(chunk);
        if (this.pending.length > pipelineDepth) this.outputFrame(this.pending.shift());
        this.decodeQueueSize--;
        this.dispatchEvent(new Event('dequeue'));
      });
    }
    outputFrame(chunk) {
      counts.framesCreated++;
      let closed = false;
      this.callbacks.output({
        timestamp: chunk.timestamp,
        codedWidth: 1,
        codedHeight: 1,
        displayWidth: 1,
        displayHeight: 1,
        close() {
          assert.equal(closed, false, 'VideoFrame must close exactly once');
          closed = true;
          counts.framesClosed++;
        },
      });
    }
    async flush() {
      while (this.pending.length) this.outputFrame(this.pending.shift());
    }
    close() {
      assert.notEqual(this.state, 'closed');
      this.state = 'closed';
      counts.decodersClosed++;
    }
  };
  t.after(() => {
    for (const [key, value] of Object.entries(saved)) {
      if (value === undefined) delete globalThis[key];
      else globalThis[key] = value;
    }
  });
  return counts;
}

const track = {
  id: 1,
  codec: 'avc',
  codecString: 'avc1.640028',
  width: 1,
  height: 1,
  nalLengthSize: 4,
  description: Uint8Array.of(1, 2),
};
const makeFrames = () =>
  [0, 0.1, 0.03, 0.06, 0.2].map((pts, id) => ({
    id,
    trackId: 1,
    pts,
    keyframe: id === 0,
    size: 1,
  }));

test('persistent native decode selects B-frame PTS and reuses the decoder across playback', async (t) => {
  const counts = fakeBrowser(t);
  const frames = makeFrames();
  const reads = [];
  const decoder = new FrameDecoder(new Blob(), {
    tracks: [track],
    frames,
    readPacket: async (frame) => {
      reads.push(frame.id);
      return Uint8Array.of(1);
    },
  });
  for (const id of [0, 2, 3, 1, 4]) {
    const output = await decoder.decode(frames[id]);
    assert.equal(output.frameId, id);
    assert.equal(output.pts, frames[id].pts);
    assert.equal(output.data[0], Math.round(frames[id].pts * 1000));
  }
  assert.equal(counts.probes, 1);
  assert.equal(counts.configured, 1);
  assert.equal(decoder.stats.submittedPackets, 5);
  assert.deepEqual(reads, [0, 1, 2, 3, 4]);
  assert.ok(decoder.stats.cacheHits >= 1);
  assert.ok(decoder.stats.maxCacheBytes <= 32 * 1024 * 1024);
  decoder.close();
  assert.equal(counts.framesCreated, counts.framesClosed);
  assert.equal(counts.decodersClosed, 1);
});

test('unsupported native codec is probed once and software fallback remains available', async (t) => {
  const counts = fakeBrowser(t, { supported: false });
  const frames = makeFrames();
  const decoder = new FrameDecoder(new Blob(), {
    tracks: [track],
    frames,
    readPacket: async () => Uint8Array.of(1),
  });
  let softwareCalls = 0;
  decoder.software = {
    async decode(frame) {
      softwareCalls++;
      return { kind: 'rgba', frameId: frame.id };
    },
    close() {},
  };
  await decoder.decode(frames[0]);
  await decoder.decode(frames[1]);
  assert.equal(counts.probes, 1);
  assert.equal(counts.configured, 0);
  assert.equal(softwareCalls, 2);
  decoder.close();
});

test('superseded native packet read cannot submit into a closed decoder', async (t) => {
  const counts = fakeBrowser(t);
  const frames = makeFrames();
  let release;
  let began;
  const started = new Promise((resolve) => {
    began = resolve;
  });
  const decoder = new FrameDecoder(new Blob(), {
    tracks: [track],
    frames,
    readPacket: async () => {
      began();
      await new Promise((resolve) => {
        release = resolve;
      });
      return Uint8Array.of(1);
    },
  });
  const abort = new AbortController();
  const decoding = decoder.decode(frames[0], { signal: abort.signal });
  await started;
  abort.abort();
  release();
  await assert.rejects(decoding, { name: 'AbortError' });
  assert.equal(counts.decodersClosed, 1);
  assert.equal(decoder.stats.submittedPackets, 0);
  decoder.close();
  assert.equal(counts.framesCreated, counts.framesClosed);
});

test('hardware pipeline pending outputs do not cause per-frame decoder restarts', async (t) => {
  const counts = fakeBrowser(t, { pipelineDepth: 4 });
  const frames = Array.from({ length: 30 }, (_, id) => ({
    id,
    trackId: 1,
    pts: id / 30,
    keyframe: id === 0,
    size: 1,
  }));
  const decoder = new FrameDecoder(new Blob(), {
    tracks: [track],
    frames,
    readPacket: async () => Uint8Array.of(1),
  });
  for (const frame of frames) assert.equal((await decoder.decode(frame)).frameId, frame.id);
  assert.equal(counts.configured, 1);
  assert.equal(decoder.stats.submittedPackets, frames.length);
  decoder.close();
  assert.equal(counts.framesCreated, counts.framesClosed);
});
