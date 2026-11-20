// RgbDecoder with a mocked WebCodecs: each keyframe group submitted once, selections drained,
// other outputs closed; a runtime failure moves the rest to FFmpeg; parameter sets in band.
// usage (WebSplat directory): node --test test/v2/*.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  RgbDecoder,
  parameterSetsAnnexB,
  withParameterSets,
} from '../../app/src/stages/select/rgb-decoder.js';

// 120 frames at 30 fps, a keyframe every 30
const frames = Array.from({ length: 120 }, (_, i) => ({
  id: i,
  trackId: 1,
  pts: i / 30,
  keyframe: i % 30 === 0,
  size: 10,
}));
const track = { id: 1, codec: 'avc', codecString: 'avc1.64001f', width: 64, height: 36 };

function mockWebCodecs({
  lag = 0,
  holdUntilFlush = false,
  failAt = null,
  configureThrows = false,
  bitmapThrows = false,
  drawThrows = false,
} = {}) {
  const stats = {
    submitted: 0,
    bitmaps: 0,
    bitmapsClosed: 0,
    open: 0,
    closed: 0,
    configured: 0,
    decoders: 0,
    decodersClosed: 0,
  };
  class VideoFrame {
    constructor(timestamp) {
      this.timestamp = timestamp;
      stats.open++;
    }
    close() {
      if (this.done) throw new Error('closed twice');
      this.done = true;
      stats.closed++;
    }
  }
  class VideoDecoder {
    static async isConfigSupported() {
      return { supported: true };
    }
    constructor({ output, error }) {
      this.output = output;
      this.error = error;
      this.queue = [];
      this.state = 'unconfigured';
      this.decodeQueueSize = 0;
      stats.decoders++;
    }
    configure() {
      if (configureThrows) throw new TypeError('unsupported configuration');
      this.state = 'configured';
      stats.configured++;
    }
    decode(chunk) {
      stats.submitted++;
      if (failAt !== null && Math.round(chunk.timestamp * 30e-6) === failAt) {
        this.state = 'closed';
        queueMicrotask(() => this.error(new DOMException('decode error', 'EncodingError')));
        return;
      }
      this.queue.push(chunk.timestamp);
      if (!holdUntilFlush)
        while (this.queue.length > lag) this.output(new VideoFrame(this.queue.shift()));
    }
    async flush() {
      while (this.queue.length) this.output(new VideoFrame(this.queue.shift()));
    }
    close() {
      if (this.state !== 'closed') stats.decodersClosed++;
      this.state = 'closed';
    }
  }
  const g = globalThis;
  const saved = ['VideoDecoder', 'EncodedVideoChunk', 'createImageBitmap', 'OffscreenCanvas'].map(
    (k) => [k, g[k]],
  );
  g.VideoDecoder = VideoDecoder;
  g.EncodedVideoChunk = class {
    constructor(o) {
      Object.assign(this, o);
    }
  };
  g.createImageBitmap = (frame) => {
    if (bitmapThrows) throw new DOMException('bad frame', 'InvalidStateError');
    stats.bitmaps++;
    return Promise.resolve({ t: frame.timestamp, close: () => stats.bitmapsClosed++ });
  };
  g.OffscreenCanvas = class {
    getContext() {
      let t = 0;
      return {
        drawImage: (bmp) => {
          if (drawThrows) throw new Error('draw failed');
          t = bmp.t;
        },
        // the frame number in every byte, to check frames come back in place
        getImageData: (x, y, w, h) => ({
          data: new Uint8ClampedArray(w * h * 4).fill(Math.round(t * 30e-6)),
        }),
      };
    }
  };
  return { stats, restore: () => saved.forEach(([k, v]) => (g[k] = v)) };
}

function media() {
  return {
    source: { tracks: [track], readPacket: async (f) => Uint8Array.of(0, 0, 0, 1, 0x65, f.id) },
    demuxed: frames,
    track: { id: 1 },
  };
}

const wanted = [5, 15, 25, 65, 95, 100];
// groups 0..25, 60..65, 90..100
const packets = 26 + 6 + 11;

async function run(dec) {
  const out = [];
  for await (const { index, rgba } of dec.decode(wanted)) out.push([index, rgba[0]]);
  return out;
}

for (const [name, opts] of [
  ['immediate output', {}],
  ['two frames of decoder lag', { lag: 2 }],
  ['outputs only at flush', { holdUntilFlush: true }],
]) {
  test(`WebCodecs, ${name}: each group submitted once, one image per selection`, async () => {
    const m = mockWebCodecs(opts);
    try {
      const dec = new RgbDecoder(media(), { width: 32, height: 18 });
      const out = await run(dec);
      assert.deepEqual(
        out,
        wanted.map((i) => [i, i]),
      );
      assert.equal(dec.used, 'webcodecs');
      assert.equal(m.stats.submitted, packets);
      assert.equal(m.stats.bitmaps, wanted.length);
      assert.equal(m.stats.closed, m.stats.open); // every VideoFrame closed
      assert.equal(m.stats.configured, 1);
    } finally {
      m.restore();
    }
  });
}

test('a WebCodecs failure moves that group and the rest to FFmpeg', async () => {
  const m = mockWebCodecs({ failAt: 62 });
  try {
    const dec = new RgbDecoder(media(), { width: 32, height: 18 });
    const software = [];
    dec._batch = async (slot, batch) => {
      software.push(batch.key);
      return new Map(batch.wanted.map((i) => [i, new Uint8ClampedArray(32 * 18 * 4).fill(i)]));
    };
    const out = await run(dec);
    assert.deepEqual(
      out,
      wanted.map((i) => [i, i]),
    );
    assert.equal(dec.used, 'webcodecs+software');
    assert.equal(dec.fallback.group, 1);
    assert.match(dec.fallback.reason, /decode error/);
    assert.deepEqual(software, [60, 90]);
    assert.equal(m.stats.closed, m.stats.open);
  } finally {
    m.restore();
  }
});

test('no WebCodecs: FFmpeg for every group', async () => {
  const dec = new RgbDecoder(media(), { width: 32, height: 18 });
  dec._batch = async (slot, batch) =>
    new Map(batch.wanted.map((i) => [i, new Uint8ClampedArray(4).fill(i)]));
  const saved = globalThis.VideoDecoder;
  delete globalThis.VideoDecoder;
  try {
    assert.deepEqual(
      await run(dec),
      wanted.map((i) => [i, i]),
    );
    assert.equal(dec.used, 'software');
  } finally {
    if (saved) globalThis.VideoDecoder = saved;
  }
});

test('parameter sets: from avcC / hvcC, put in front of keyframes that lack them', () => {
  const sps = Uint8Array.of(0x67, 0x64, 0x00, 0x1f, 0xac),
    pps = Uint8Array.of(0x68, 0xee, 0x3c);
  const avcC = Uint8Array.of(
    1,
    0x64,
    0,
    0x1f,
    0xff,
    0xe1,
    0,
    sps.length,
    ...sps,
    1,
    0,
    pps.length,
    ...pps,
  );
  const sets = parameterSetsAnnexB({ codec: 'avc', codecPrivate: avcC });
  assert.deepEqual(
    sets.map((n) => [...n]),
    [[...sps], [...pps]],
  );
  const idr = Uint8Array.of(0, 0, 0, 1, 0x65, 1, 2, 3);
  const out = withParameterSets(idr, sets, 'avc');
  assert.deepEqual([...out], [0, 0, 0, 1, ...sps, 0, 0, 0, 1, ...pps, ...idr]);
  const complete = Uint8Array.of(0, 0, 0, 1, ...sps, 0, 0, 0, 1, ...pps, 0, 0, 1, 0x65, 9);
  assert.equal(withParameterSets(complete, sets, 'avc'), complete);
  // an SPS without a PPS: the PPS is added
  const spsOnly = Uint8Array.of(0, 0, 0, 1, ...sps, 0, 0, 1, 0x65, 9);
  assert.deepEqual([...withParameterSets(spsOnly, sets, 'avc')], [0, 0, 0, 1, ...pps, ...spsOnly]);
  // hvcC: 22-byte header, numOfArrays, then [type, count, (length, NAL)*] per array
  const vps = Uint8Array.of(0x40, 1, 2),
    hsps = Uint8Array.of(0x42, 1, 3, 4),
    hpps = Uint8Array.of(0x44, 1, 5);
  const arr = (t, n) => [0x80 | t, 0, 1, 0, n.length, ...n];
  const hvcC = Uint8Array.of(
    1,
    ...new Array(21).fill(0),
    3,
    ...arr(32, vps),
    ...arr(33, hsps),
    ...arr(34, hpps),
  );
  const hsets = parameterSetsAnnexB({ codec: 'hevc', codecPrivate: hvcC });
  assert.deepEqual(
    hsets.map((n) => [...n]),
    [[...vps], [...hsps], [...hpps]],
  );
  const hidr = Uint8Array.of(0, 0, 1, 0x26, 1, 7);
  assert.equal(withParameterSets(hidr, hsets, 'hevc').length, 3 * 4 + 3 + 4 + 3 + hidr.length);
  // SPS and PPS in band but no VPS: only the VPS is added
  const noVps = Uint8Array.of(0, 0, 0, 1, ...hsps, 0, 0, 0, 1, ...hpps, ...hidr);
  assert.deepEqual([...withParameterSets(noVps, hsets, 'hevc')], [0, 0, 0, 1, ...vps, ...noVps]);
  const all = Uint8Array.of(0, 0, 0, 1, ...vps, ...noVps);
  assert.equal(withParameterSets(all, hsets, 'hevc'), all);
});

test('WebCodecs keyframes of an Annex B track carry the parameter sets', async () => {
  const m = mockWebCodecs();
  const seen = [];
  const Real = globalThis.EncodedVideoChunk;
  globalThis.EncodedVideoChunk = class {
    constructor(o) {
      Object.assign(this, o);
      if (o.type === 'key') seen.push([...o.data.slice(0, 5)]);
    }
  };
  try {
    const sps = Uint8Array.of(0x67, 1),
      pps = Uint8Array.of(0x68, 2);
    const t = {
      ...track,
      annexB: true,
      codecPrivate: Uint8Array.of(1, 0x64, 0, 0x1f, 0xff, 0xe1, 0, 2, ...sps, 1, 0, 2, ...pps),
    };
    const md = { ...media(), source: { ...media().source, tracks: [t] } };
    const dec = new RgbDecoder(md, { width: 32, height: 18 });
    for await (const x of dec.decode([65])) assert.equal(x.index, 65);
    assert.deepEqual(seen, [[0, 0, 0, 1, 0x67]]); // the keyframe at 60, SPS first
  } finally {
    globalThis.EncodedVideoChunk = Real;
    m.restore();
  }
});

for (const [name, opts, group] of [
  ['configure() throws', { configureThrows: true }, 0],
  ['createImageBitmap throws synchronously', { bitmapThrows: true }, 0],
  ['drawImage throws', { drawThrows: true }, 0],
]) {
  test(`WebCodecs ${name}: FFmpeg takes over, nothing left open`, async () => {
    const m = mockWebCodecs(opts);
    try {
      const dec = new RgbDecoder(media(), { width: 32, height: 18 });
      dec._batch = async (slot, batch) =>
        new Map(batch.wanted.map((i) => [i, new Uint8ClampedArray(32 * 18 * 4).fill(i)]));
      assert.deepEqual(
        await run(dec),
        wanted.map((i) => [i, i]),
      );
      assert.equal(dec.used, 'webcodecs+software');
      assert.equal(dec.fallback.group, group);
      assert.equal(m.stats.closed, m.stats.open); // VideoFrames
      assert.equal(m.stats.bitmapsClosed, m.stats.bitmaps);
      assert.equal(m.stats.decodersClosed, m.stats.decoders);
    } finally {
      m.restore();
    }
  });
}

test('thinMatches: a consistent keypoint subset above the match budget', async () => {
  const { thinMatches } = await import('../../app/src/stages/map/priormap.js');
  // 10 images, pairs (i, i+1) and (i, i+2), 2000 matches each between equal keypoint ids
  const geometry = [];
  for (let i = 0; i < 10; i++)
    for (const j of [i + 1, i + 2])
      if (j < 10)
        geometry.push({
          i,
          j,
          config: 2,
          matches: Uint32Array.from({ length: 4000 }, (_, q) => q >> 1),
        });
  const total = geometry.length * 2000;
  const same = thinMatches(geometry, total);
  assert.equal(same.geometry, geometry);
  assert.equal(same.kept, 1);
  const t = thinMatches(geometry, total / 4);
  assert.ok(Math.abs(t.kept - 0.5) < 1e-12);
  const n = t.geometry.reduce((k, e) => k + e.matches.length / 2, 0);
  assert.ok(n > total / 4 - 3000 && n < total / 4 + 3000, n);
  // image 2 keeps one subset of its keypoints in all four of its pairs: their union is about
  // half its 2000 keypoints (independent choices per pair would give about 88%)
  const union = new Set();
  for (const e of t.geometry) {
    if (e.i === 2) e.matches.forEach((k, q) => q % 2 === 0 && union.add(k));
    if (e.j === 2) e.matches.forEach((k, q) => q % 2 === 1 && union.add(k));
  }
  assert.ok(union.size > 900 && union.size < 1100, union.size);
  // pairs left with fewer than 15 matches are dropped
  const tiny = [
    { i: 0, j: 1, config: 2, matches: Uint32Array.from({ length: 40 }, (_, q) => q >> 1) },
  ];
  assert.equal(
    thinMatches([...tiny, ...geometry], 100).geometry.some((e) => e.matches.length < 30),
    false,
  );
});
