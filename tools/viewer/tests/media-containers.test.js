import test from 'node:test';
import assert from 'node:assert/strict';
import { openMedia, unwrapTimestamp } from '../src/media/containers.js';

const encode = (s) => new TextEncoder().encode(s);
const join = (...parts) => Uint8Array.from(parts.flatMap((p) => [...p]));
const n32 = (n) => {
  const b = new Uint8Array(4);
  new DataView(b.buffer).setUint32(0, n);
  return b;
};
const n64 = (n) => {
  const b = new Uint8Array(8);
  new DataView(b.buffer).setBigUint64(0, BigInt(n));
  return b;
};
const hex = (h) => Uint8Array.from(h.match(/../g), (v) => parseInt(v, 16));
const box = (type, ...parts) => {
  const data = join(...parts);
  return join(n32(data.length + 8), encode(type), data);
};
const full = (type, flags, ...parts) => box(type, n32(flags), ...parts);
const zeros = (n) => new Uint8Array(n);
const packet = Uint8Array.of(0, 0, 0, 2, 0x65, 0x80);

function mp4Track({
  fragmented = false,
  offset = 0,
  count = 3,
  edits = [],
  composition = [80, -40, 0],
  zeroRuns = false,
} = {}) {
  const tkhd = full('tkhd', 0, n32(0), n32(0), n32(1), zeros(64));
  const mdhd = full('mdhd', 0, n32(0), n32(0), n32(1000), n32(10000));
  const hdlr = full('hdlr', 0, n32(0), encode('vide'), zeros(12));
  const fields = zeros(78);
  new DataView(fields.buffer).setUint16(24, 640);
  new DataView(fields.buffer).setUint16(26, 512);
  const avcC = hex('0142001effe100056742001e800100026880');
  const stsd = full('stsd', 0, n32(1), box('avc1', fields, box('avcC', avcC)));
  const stsz = full('stsz', 0, n32(packet.length), n32(fragmented ? 0 : count));
  const stts = full(
    'stts',
    0,
    n32(fragmented ? 0 : zeroRuns ? 4 : 1),
    ...(fragmented
      ? []
      : zeroRuns
        ? [n32(0), n32(999), n32(1), n32(40), n32(0), n32(888), n32(count - 1), n32(40)]
        : [n32(count), n32(40)]),
  );
  const tables = fragmented
    ? []
    : [
        full('stsc', 0, n32(1), n32(1), n32(count), n32(1)),
        full('co64', 0, n32(1), n64(offset)),
        full('stss', 0, n32(2), n32(1), n32(3)),
        full(
          'ctts',
          0x01000000,
          n32(composition.length + (zeroRuns ? 2 : 0)),
          ...(zeroRuns ? [n32(0), n32(999)] : []),
          ...composition.flatMap((n, i) => [
            ...(zeroRuns && i === 1 ? [n32(0), n32(888)] : []),
            n32(1),
            n32(n),
          ]),
        ),
      ];
  const edts = edits.length
    ? [
        box(
          'edts',
          full(
            'elst',
            0,
            n32(edits.length),
            ...edits.flatMap(([duration, mediaTime]) => [
              n32(duration),
              n32(mediaTime),
              n32(65536),
            ]),
          ),
        ),
      ]
    : [];
  return box(
    'trak',
    tkhd,
    ...edts,
    box('mdia', mdhd, hdlr, box('minf', box('stbl', stsd, stsz, stts, ...tables))),
  );
}
function movie(track, ...extra) {
  return box('moov', full('mvhd', 0, n32(0), n32(0), n32(1000), n32(10000)), track, ...extra);
}

function virtualFile(size, segments) {
  let maximum = 0,
    total = 0,
    calls = 0;
  return {
    size,
    get maximum() {
      return maximum;
    },
    get total() {
      return total;
    },
    get calls() {
      return calls;
    },
    slice(start, end) {
      assert.ok(end >= start && start >= 0 && end <= size);
      maximum = Math.max(maximum, end - start);
      total += end - start;
      calls++;
      return {
        async arrayBuffer() {
          const out = new Uint8Array(end - start);
          for (const [offset, data] of segments) {
            const first = Math.max(start, offset),
              stop = Math.min(end, offset + data.length);
            if (stop > first) out.set(data.subarray(first - offset, stop - offset), first - start);
          }
          return out.buffer;
        },
      };
    },
  };
}
async function indexAll(file, options = {}) {
  const source = await openMedia(file, options);
  const batches = [];
  for await (const batch of source.index()) batches.push(batch);
  return { source, batches };
}

test('classic MP4 co64 seeks above 2GiB; signed B-frame PTS stays distinct from DTS', async () => {
  const offset = 2 ** 31 + 4096,
    payload = join(packet, packet, packet);
  const prefix = join(box('ftyp', encode('isom'), n32(0)), movie(mp4Track({ offset })));
  const size = offset + payload.length;
  const file = virtualFile(size, [
    [0, prefix],
    [prefix.length, join(n32(1), encode('mdat'), n64(size - prefix.length))],
    [offset, payload],
  ]);
  const { source, batches } = await indexAll(file, { maxReadBytes: 512, batchSize: 1 });
  assert.equal(source.format, 'mp4');
  assert.equal(source.tracks[0].codec, 'avc');
  assert.deepEqual(
    source.frames.map((f) => f.pts),
    [0.08, 0, 0.08],
  );
  assert.deepEqual(
    source.frames.map((f) => f.dts),
    [0, 0.04, 0.08],
  );
  assert.deepEqual(
    source.frames.map((f) => f.keyframe),
    [true, false, true],
  );
  assert.equal(source.frames[0].offset, offset);
  assert.deepEqual(await source.readPacket(source.frames[2]), packet);
  assert.ok(batches.some((b) => b.frames.length && !b.progress.complete));
  assert.ok(file.maximum <= 512);
  assert.ok(file.total < 20000, `read ${file.total}`);
});

function fragmentedMP4(repeat = 1, samplePayload = packet, edits = []) {
  const prefix = join(
    box('ftyp', encode('isom'), n32(0)),
    movie(
      mp4Track({ fragmented: true, edits }),
      box('mvex', full('trex', 0, n32(1), n32(1), n32(40), n32(samplePayload.length), n32(0))),
    ),
  );
  const payload = join(samplePayload, samplePayload);
  function fragment(pts) {
    const tfhd = full('tfhd', 0x020000, n32(1));
    const tfdt = full('tfdt', 0x01000000, n64(pts));
    const trun = (offset) =>
      full(
        'trun',
        0x01000f01,
        n32(2),
        n32(offset),
        n32(40),
        n32(samplePayload.length),
        n32(0),
        n32(40),
        n32(40),
        n32(samplePayload.length),
        n32(0x10000),
        n32(-40),
      );
    let moof = box('moof', box('traf', tfhd, tfdt, trun(0)));
    moof = box('moof', box('traf', tfhd, tfdt, trun(moof.length + 8)));
    return join(moof, box('mdat', payload));
  }
  return join(prefix, ...Array.from({ length: repeat }, (_, i) => fragment(123456 + i * 80)));
}

test('fragmented MP4 defaults, 64bit tfdt, signed composition, first batch before full scan', async () => {
  const data = fragmentedMP4(100),
    file = virtualFile(data.length, [[0, data]]);
  const source = await openMedia(file, { maxReadBytes: 512, batchSize: 1 });
  const iterator = source.index();
  assert.equal((await iterator.next()).value.tracks[0].width, 640);
  const first = (await iterator.next()).value.frames[0];
  assert.equal(first.pts, 123.496);
  assert.equal(first.dts, 123.456);
  assert.equal(first.keyframe, true);
  assert.ok(file.total < data.length / 4, `${file.total}/${data.length}`);
  for await (const ignored of iterator) void ignored;
  assert.equal(source.frames.length, 200);
  assert.equal(source.frames[1].pts, 123.456);
  assert.equal(source.frames[1].keyframe, false);
  assert.deepEqual(await source.readPacket(first), packet);
});

function vint(n, count) {
  const value = BigInt(n);
  count ??= Array.from({ length: 8 }, (_, i) => i + 1).find(
    (i) => value < (1n << BigInt(7 * i)) - 1n,
  );
  const out = new Uint8Array(count);
  let bits = value | (1n << BigInt(7 * count));
  for (let i = count - 1; i >= 0; i--) {
    out[i] = Number(bits & 255n);
    bits >>= 8n;
  }
  return out;
}
const eb = (id, data) => join(hex(id), vint(data.length), data);
const eu = (id, n) => eb(id, n <= 255 ? Uint8Array.of(n) : n32(n));
function thermalMKV({
  unknownClusters = false,
  laced = false,
  second = true,
  hfov = 24.2,
  codecId = 'V_FFV1',
  codecPrivate = Uint8Array.of(7, 8, 9),
} = {}) {
  const mapping = eb('41e4', join(eu('41f0', 2), eu('41e7', 0x41504347)));
  const track = eb(
    'ae',
    join(
      eu('d7', 1),
      eu('83', 1),
      eb('86', encode(codecId)),
      eb('63a2', codecPrivate),
      eu('23e383', 200000000),
      eb('e0', join(eu('b0', 640), eu('ba', 512))),
      mapping,
    ),
  );
  const header = join(
    eb('1a45dfa3', eb('4282', encode('matroska'))),
    hex('1853806701ffffffffffffff'),
    eb('1549a966', eu('2ad7b1', 1000000)),
    eb('1654ae6b', track),
  );
  const meta = {
    schema: 'apcg.thermal.v1',
    rotation_deg: 180,
    hfov_deg: hfov,
    temperature_scale_k: 1 / 64,
    temperature_offset_k: 0,
    telemetry: {
      schema: 'apcg.telemetry.v1',
      utc_us: 1787882912367000,
      hfov_deg: 60,
      named_values: { TMAX: { value: 17, valid: true } },
    },
  };
  const block = eb(
    'a1',
    join(Uint8Array.of(0x81, 0xff, 0xfb, laced ? 2 : 0), Uint8Array.of(11, 22, 33)),
  );
  const group = eb(
    'a0',
    join(block, eb('75a1', eb('a6', join(eu('ee', 2), eb('a5', encode(JSON.stringify(meta))))))),
  );
  const cluster = (time) => {
    const data = join(eu('e7', time), group);
    return unknownClusters ? join(hex('1f43b675ff'), data) : eb('1f43b675', data);
  };
  return join(header, cluster(2000), ...(second ? [cluster(2200)] : []));
}

test('thermal Matroska resolves APCG mapping, negative relative ticks, raw packets and independent keyframes', async () => {
  const { source } = await indexAll(new Blob([thermalMKV()]), { maxReadBytes: 256 });
  assert.equal(source.tracks[0].codec, 'ffv1');
  assert.equal(source.tracks[0].codecId, 'V_FFV1');
  assert.deepEqual(source.tracks[0].codecPrivate, Uint8Array.of(7, 8, 9));
  assert.deepEqual(
    source.frames.map((f) => f.pts),
    [1.995, 2.195],
  );
  assert.equal(source.frames[0].dts, null);
  assert.equal(source.frames[0].duration, 0.2);
  assert.equal(source.frames[0].keyframe, true);
  assert.equal(source.frames[0].telemetry.utc_us, 1787882912367000);
  assert.equal(source.frames[0].thermalMetadata.rotation_deg, 180);
  assert.equal(source.frames[0].telemetry.hfov_deg, 24.2);
  assert.equal(source.frames[0].telemetry.provenance.nested_hfov_deg, 60);
  assert.equal(source.frames[0].telemetry.thermal.temperature_scale_k, 1 / 64);
  assert.equal(source.frames[0].telemetry.named_values.TMAX.valid, true);
  assert.deepEqual(await source.readPacket(source.frames[0]), Uint8Array.of(11, 22, 33));
});

test('unknown thermal FOV never falls back to the nested RGB lens; Matroska AVC uses its actual profile', async () => {
  const { source } = await indexAll(new Blob([thermalMKV({ hfov: null })]));
  assert.equal(source.frames[0].telemetry.hfov_deg, null);
  const { source: avc } = await indexAll(
    new Blob([
      thermalMKV({
        codecId: 'V_MPEG4/ISO/AVC',
        codecPrivate: hex('01640028ffe1000567640028800100026880'),
      }),
    ]),
  );
  assert.equal(avc.tracks[0].codecString, 'avc1.640028');
});

test('unknown-size Matroska clusters stop at sibling clusters; lacing is rejected', async () => {
  const { source } = await indexAll(new Blob([thermalMKV({ unknownClusters: true })]));
  assert.equal(source.frames.length, 2);
  await assert.rejects(indexAll(new Blob([thermalMKV({ laced: true })])), /Laced/);
});

function crc(bytes) {
  let n = 0xffffffff;
  for (const b of bytes) {
    n ^= b << 24;
    for (let i = 0; i < 8; i++) n = ((n << 1) ^ (n & 0x80000000 ? 0x04c11db7 : 0)) >>> 0;
  }
  return n32(n);
}
const section = (data) => {
  data = data.slice();
  data[1] = 0xb0 | ((data.length + 1) >> 8);
  data[2] = (data.length + 1) & 255;
  return join(data, crc(data));
};
function tsPacket(pid, payload, cc = 0, unitStart = true) {
  assert.ok(payload.length <= 184);
  const packet = new Uint8Array(188);
  packet.fill(255);
  packet.set([
    0x47,
    (pid >> 8) | (unitStart ? 64 : 0),
    pid & 255,
    (payload.length === 184 ? 0x10 : 0x30) | cc,
  ]);
  if (payload.length < 184) {
    packet[4] = 183 - payload.length;
    if (packet[4]) packet[5] = 0;
  }
  packet.set(payload, 188 - payload.length);
  return packet;
}
function pts(n) {
  n %= 2 ** 33;
  return Uint8Array.of(
    0x21 | (Math.floor(n / 2 ** 29) & 14),
    Math.floor(n / 2 ** 22) & 255,
    1 | (Math.floor(n / 2 ** 14) & 254),
    Math.floor(n / 128) & 255,
    1 | ((n % 128) * 2),
  );
}
const pes = (id, time, data) =>
  join(
    Uint8Array.of(0, 0, 1, id),
    Uint8Array.of((data.length + 8) >> 8, (data.length + 8) & 255, 0x84, 0x80, 5),
    pts(time),
    data,
  );
function tsFixture({
  metadataAfter = false,
  gap = false,
  bFrames = false,
  repeats = 5,
  largePayload = false,
  delayMetadata = false,
  asynchronous = false,
  metadataOffset = 0,
  metadataEvery = 1,
  metadataCount = Infinity,
  metadataDiscontinuity = -1,
  metadataFlags = 0xdf,
} = {}) {
  const pat = section(Uint8Array.of(0, 0, 0, 0, 1, 0xc1, 0, 0, 0, 1, 0xf0, 0));
  const pmt = section(
    Uint8Array.of(
      2,
      0,
      0,
      0,
      1,
      0xc1,
      0,
      0,
      0xe1,
      1,
      0xf0,
      0,
      0x1b,
      0xe1,
      1,
      0xf0,
      0,
      asynchronous ? 6 : 0x15,
      0xe1,
      2,
      0xf0,
      6,
      5,
      4,
      75,
      76,
      86,
      65,
    ),
  );
  const klv = hex(
    '060e2b34020b01010e0103010100000069020800065f1ab5f649f10b044d5431310c0e47656f64657469632057475338340d04cdd211ec0e0469f59be40f0213110502071c06020000070200005a04000000005b04000000001002226b1102138f1204000000001304eaaaaaa114040000000041010801020fc2',
  );
  const cell = join(Uint8Array.of(0, 0, metadataFlags, 0, klv.length), klv);
  const video = Uint8Array.of(0, 0, 1, 9, 0xf0, 0, 0, 1, 0x65, 0x80);
  const packets = [
    tsPacket(0, join(Uint8Array.of(0), pat)),
    tsPacket(0x1000, join(Uint8Array.of(0), pmt)),
  ];
  let videoCC = 0,
    metadataCC = 0;
  const delayed = [];
  function packetize(pid, payload, initialCC) {
    const out = [];
    for (let at = 0; at < payload.length; at += 184)
      out.push(tsPacket(pid, payload.subarray(at, at + 184), initialCC++ % 16, at === 0));
    return out;
  }
  for (let i = 0; i < repeats; i++) {
    const time = 2 ** 33 - 6000 + i * 3000;
    let data = pes(
      0xe0,
      time,
      largePayload ? join(video, new Uint8Array(12000).fill(0x55)) : video,
    );
    if (bFrames) {
      const presentation = 2 ** 33 - 6000 + [6000, 12000, 9000, 18000, 15000][i];
      const pt = pts(presentation),
        dt = pts(time);
      pt[0] = (pt[0] & 15) | 0x30;
      dt[0] = (dt[0] & 15) | 0x10;
      data = join(data.subarray(0, 6), Uint8Array.of(0x84, 0xc0, 10), pt, dt, data.subarray(14));
      data[4] = (data.length - 6) >> 8;
      data[5] = (data.length - 6) & 255;
    }
    const v = packetize(257, data, gap && i === 1 ? 3 : videoCC),
      metadataTime =
        (bFrames ? 2 ** 33 - 6000 + [6000, 12000, 9000, 18000, 15000][i] : time) + metadataOffset,
      m =
        i % metadataEvery === 0 && i < metadataCount
          ? packetize(
              258,
              pes(asynchronous ? 0xbd : 0xfc, metadataTime, asynchronous ? klv : cell),
              metadataCC,
            )
          : [];
    if (i === metadataDiscontinuity && m.length && m[0][4]) m[0][5] |= 128;
    videoCC += v.length;
    metadataCC += m.length;
    if (delayMetadata) {
      packets.push(...v);
      delayed.push(...m);
    } else packets.push(...(metadataAfter ? [...v, ...m] : [...m, ...v]));
  }
  packets.push(...delayed);
  return join(...packets);
}

test('MPEG-TS PAT/PMT, nonzero PTS wrap, KLV before/after video, scattered payload reads', async () => {
  for (const metadataAfter of [false, true]) {
    const { source } = await indexAll(new Blob([tsFixture({ metadataAfter })]), {
      maxReadBytes: 256,
      batchSize: 1,
    });
    assert.equal(source.frames.length, 5);
    assert.equal(source.tracks[0].id, 257);
    assert.equal(source.frames[0].timestampTicks, 2 ** 33 - 6000);
    assert.equal(source.frames[2].timestampTicks, 2 ** 33);
    for (const f of source.frames) {
      assert.equal(f.telemetry.utc_us, 1793418186869233);
      assert.equal(f.keyframe, true);
    }
    assert.ok(source.frames.every((f, i, a) => !i || f.pts > a[i - 1].pts));
    assert.deepEqual(
      await source.readPacket(source.frames[0]),
      Uint8Array.of(0, 0, 1, 9, 0xf0, 0, 0, 1, 0x65, 0x80),
    );
    assert.deepEqual(source.warnings, []);
  }
  assert.equal(unwrapTimestamp(0, 2 ** 33 - 1), 2 ** 33);
});

test('damaged MPEG-TS continuity retains playable prefix with an explicit warning', async () => {
  const { source } = await indexAll(new Blob([tsFixture({ gap: true })]));
  assert.ok(source.frames.length < 5);
  assert.ok(source.warnings.some((w) => /continuity/.test(w)));
});

test('cancellation is checked after asynchronous slice and source close blocks stale packet reads', async () => {
  const abort = new AbortController();
  const file = {
    size: 1000,
    slice(a, b) {
      return {
        async arrayBuffer() {
          abort.abort();
          return new ArrayBuffer(b - a);
        },
      };
    },
  };
  await assert.rejects(openMedia(file, { signal: abort.signal }), { name: 'AbortError' });
  const source = await openMedia(new Blob([thermalMKV()]));
  const iterator = source.index({ signal: abort.signal });
  await assert.rejects(iterator.next(), { name: 'AbortError' });
  source.close();
  await assert.rejects(source.readPacket({ size: 1, offset: 0 }), /closed/);
});

test('truncated final fragment preserves indexed frames and reports the damaged tail', async () => {
  const complete = fragmentedMP4(3),
    damaged = complete.subarray(0, complete.length - 5);
  const source = await openMedia(new Blob([damaged]), { batchSize: 1 });
  let error;
  try {
    for await (const ignored of source.index()) void ignored;
  } catch (e) {
    error = e;
  }
  assert.ok(source.frames.length >= 4);
  assert.ok(error || source.warnings.length);
});

test('abort with a partially accumulated batch rejects without publishing stale frames', async () => {
  const bytes = thermalMKV(),
    abort = new AbortController();
  let source;
  const file = {
    size: bytes.length,
    slice(start, end) {
      return {
        async arrayBuffer() {
          if (source?.frames.length) abort.abort();
          return bytes.slice(start, end).buffer;
        },
      };
    },
  };
  source = await openMedia(file, { maxReadBytes: 188 });
  const iterator = source.index({ batchSize: 64, signal: abort.signal });
  assert.equal((await iterator.next()).value.frames.length, 0); // Tracks first.
  await assert.rejects(iterator.next(), { name: 'AbortError' });
  assert.equal(source.frames.length, 1);
});

test('MP4 edit list maps movie PTS/DTS and retains exact original media clocks', async () => {
  const offset = 4096,
    prefix = join(
      box('ftyp', encode('isom'), n32(0)),
      movie(
        mp4Track({
          offset,
          edits: [
            [200, -1],
            [1000, 80],
          ],
        }),
      ),
    );
  const data = join(packet, packet, packet),
    size = offset + data.length;
  const file = virtualFile(size, [
    [0, prefix],
    [prefix.length, join(n32(size - prefix.length), encode('mdat'))],
    [offset, data],
  ]);
  const { source } = await indexAll(file);
  assert.equal(source.tracks[0].editOffset, 0.12000000000000001);
  assert.deepEqual(
    source.frames.map((f) => f.mediaPTS),
    [0.08, 0, 0.08],
  );
  assert.deepEqual(
    source.frames.map((f) => f.mediaDTS),
    [0, 0.04, 0.08],
  );
  for (const f of source.frames) {
    assert.ok(Math.abs(f.pts - f.mediaPTS - 0.12) < 1e-14);
    assert.ok(Math.abs(f.dts - f.mediaDTS - 0.12) < 1e-14);
  }
});

test('SEI indexing jumps VCL payloads and still discovers suffix telemetry', async () => {
  const record = { schema: 'apcg.telemetry.v1', utc_us: 1787882912367000 };
  const user = join(hex('8d646b4e556f4a908b7c35e629510321'), encode(JSON.stringify(record)));
  const sei = join(Uint8Array.of(6, 5, user.length), user, Uint8Array.of(128));
  const vcl = new Uint8Array(1048576).fill(0x55);
  vcl[0] = 0x65;
  const sample = join(n32(vcl.length), vcl, n32(sei.length), sei);
  const data = fragmentedMP4(1, sample),
    file = virtualFile(data.length, [[0, data]]);
  const { source } = await indexAll(file);
  assert.equal(source.frames[0].telemetry.utc_us, record.utc_us);
  assert.equal(source.frames[1].telemetry.utc_us, record.utc_us);
  assert.ok(file.total < 300000, `SEI scan read ${file.total}/${file.size}`);
});

test('TS index descriptors remain constant-sized for large PES and reconstruct exact coded bytes', async () => {
  const data = tsFixture({ largePayload: true, repeats: 40 });
  const { source } = await indexAll(new Blob([data]));
  assert.equal(source.frames.length, 40);
  const frame = source.frames[0];
  assert.equal(frame.extents, undefined);
  assert.equal(Object.keys(frame.transport).length, 5);
  assert.ok(JSON.stringify(source.frames.map((f) => f.transport)).length < 8000);
  assert.ok((await source.readPacket(frame)).length > 12000);
  assert.deepEqual(
    (await source.readPacket(frame)).subarray(0, 10),
    Uint8Array.of(0, 0, 1, 9, 0xf0, 0, 0, 1, 0x65, 0x80),
  );
});

test('TS B-frame PTS reorder retains monotonically increasing DTS across wrap', async () => {
  const { source } = await indexAll(new Blob([tsFixture({ bFrames: true })]));
  assert.equal(source.frames.length, 5);
  assert.deepEqual(
    source.frames.map((f) => f.timestampTicks),
    [2 ** 33, 2 ** 33 + 6000, 2 ** 33 + 3000, 2 ** 33 + 12000, 2 ** 33 + 9000],
  );
  assert.ok(source.frames.every((f, i, a) => !i || f.dts > a[i - 1].dts));
  assert.ok(source.frames.every((f) => f.telemetry?.utc_us === 1793418186869233));
});

test('TS inserted byte resynchronizes in bounded window and late KLV is diagnosed', async () => {
  const data = tsFixture();
  const damaged = join(data.subarray(0, 188 * 6), Uint8Array.of(123), data.subarray(188 * 6));
  const { source } = await indexAll(new Blob([damaged]));
  assert.ok(source.frames.length >= 3);
  assert.ok(source.warnings.some((w) => /recovered/.test(w)));
  const delayed = (await indexAll(new Blob([tsFixture({ delayMetadata: true, repeats: 8 })])))
    .source;
  assert.ok(delayed.frames.every((f) => f.klvTelemetry?.utc_us === 1793418186869233));
  assert.ok(!delayed.warnings.some((w) => /Late KLV/.test(w)));
  const late = (await indexAll(new Blob([tsFixture({ delayMetadata: true, repeats: 100 })])))
    .source;
  assert.ok(late.warnings.some((w) => /Late KLV/.test(w)));
});

test('asynchronous KLV tolerates sub-frame mux jitter before/after video across PTS wrap', async () => {
  for (const metadataAfter of [false, true])
    for (const metadataOffset of [-450, 450]) {
      const { source } = await indexAll(
        new Blob([tsFixture({ asynchronous: true, metadataAfter, metadataOffset })]),
      );
      assert.equal(source.frames.length, 5);
      for (const frame of source.frames) {
        assert.ok(frame.telemetry?.position);
        assert.equal(frame.telemetry.utc_us, 1793418186869233);
        assert.equal(frame.telemetry.provenance.association, 'nearest-pts');
        assert.ok(Math.abs(frame.telemetry.provenance.metadata_offset_ms) <= 5);
        assert.ok(frame.telemetry.position.age_ms <= 5);
        assert.equal(
          frame.telemetry.position.age_ms,
          Math.abs(frame.telemetry.provenance.metadata_offset_ms),
        );
      }
      assert.deepEqual(source.warnings, []);
    }
});

test('synchronous KLV still requires exact matching PTS', async () => {
  const { source } = await indexAll(new Blob([tsFixture({ metadataOffset: 450 })]));
  assert.ok(source.frames.every((f) => !f.telemetry));
  assert.ok(source.warnings.some((w) => /Unmatched KLV/.test(w)));
});

test('legacy unmarked complete KLV cells use validated, approximate timestamp association', async () => {
  const { source } = await indexAll(
    new Blob([tsFixture({ metadataFlags: 0x0f, metadataOffset: 450 })]),
  );
  assert.ok(source.frames.every((f) => f.telemetry?.position.age_ms === 5));
  assert.equal(
    source.frames[0].telemetry.provenance.klv_au_cell_compatibility,
    'unmarked-complete-cell',
  );
  assert.equal(source.frames[0].telemetry.provenance.association, 'nearest-pts');
  assert.ok(source.warnings.some((w) => /Legacy KLV/.test(w)));
});

test('sparse asynchronous snapshots age independently and expire without waiting for another PES', async () => {
  const { source } = await indexAll(
    new Blob([tsFixture({ asynchronous: true, repeats: 100, metadataCount: 1 })]),
  );
  const initial = source.frames[0],
    near = source.frames[30],
    stale = source.frames.at(-1);
  assert.equal(initial.telemetry.position.age_ms, 0);
  assert.ok(Math.abs(near.telemetry.position.age_ms - 1000) < 1e-6);
  assert.equal(
    initial.telemetry.position.age_ms,
    0,
    'reusing a snapshot must not mutate its earlier frame',
  );
  assert.equal(stale.telemetry, null);
  assert.ok(source.frames.filter((f) => f.telemetry).length > 50);
});

test('asynchronous matching respects reordered B-frame presentation times and bounded leading samples', async () => {
  const { source } = await indexAll(
    new Blob([tsFixture({ asynchronous: true, bFrames: true, metadataOffset: 450 })]),
  );
  assert.ok(source.frames.every((f) => f.telemetry?.position.age_ms === 5));
  const leading = (
    await indexAll(
      new Blob([tsFixture({ asynchronous: true, metadataOffset: 9000, metadataCount: 1 })]),
    )
  ).source;
  assert.equal(leading.frames[0].telemetry, null);
  assert.equal(leading.frames[1].telemetry, null);
  assert.ok(leading.frames[3].telemetry);
});

test('metadata discontinuities invalidate held asynchronous snapshots', async () => {
  const { source } = await indexAll(
    new Blob([
      tsFixture({
        asynchronous: true,
        repeats: 20,
        metadataEvery: 10,
        metadataDiscontinuity: 10,
        metadataOffset: 9000,
      }),
    ]),
  );
  assert.equal(source.frames[11].telemetry, null);
  assert.ok(source.frames[14].telemetry);
});

test('MP4 edit intervals hide preroll and postroll but retain decode packets, including fragments', async () => {
  const offset = 4096,
    payload = join(packet, packet, packet);
  const prefix = join(
    box('ftyp', encode('isom'), n32(0)),
    movie(mp4Track({ offset, composition: [0, 0, 0], edits: [[40, 40]] })),
  );
  const size = offset + payload.length;
  const { source } = await indexAll(
    virtualFile(size, [
      [0, prefix],
      [prefix.length, join(n32(size - prefix.length), encode('mdat'))],
      [offset, payload],
    ]),
  );
  assert.deepEqual(
    source.frames.map((f) => f.presentable),
    [false, true, false],
  );
  assert.equal(source.frames[0].keyframe, true);
  assert.equal(source.frames[1].decodable, true);
  assert.equal(source.frames[1].pts, 0);
  assert.equal(source.frames[1].duration, 0.04);
  const { source: fragmented } = await indexAll(
    new Blob([fragmentedMP4(2, packet, [[80, 123496]])]),
  );
  assert.deepEqual(
    fragmented.frames.map((f) => f.presentable),
    [true, false, false, true],
  );
  const { source: openEnded } = await indexAll(new Blob([fragmentedMP4(2, packet, [[0, 123496]])]));
  assert.equal(openEnded.tracks[0].presentationEnd, Infinity);
  assert.deepEqual(
    openEnded.frames.map((f) => f.presentable),
    [true, false, true, true],
  );
});

test('truncated final Matroska cluster retains complete preceding thermal frame', async () => {
  const bytes = thermalMKV(),
    source = await openMedia(new Blob([bytes.subarray(0, bytes.length - 30)])),
    batches = [];
  await assert.rejects(async () => {
    for await (const batch of source.index()) batches.push(batch);
  }, /Truncated/);
  assert.equal(source.frames.length, 1);
  assert.ok(batches.some((b) => b.frames.length === 1 && b.error));
  assert.deepEqual(await source.readPacket(source.frames[0]), Uint8Array.of(11, 22, 33));
});

test('compact TS re-read matches index handling of duplicate transport packets', async () => {
  const data = tsFixture({ largePayload: true, repeats: 2 });
  const insert = 188 * 5,
    duplicate = data.subarray(insert, insert + 188);
  const retransmitted = join(
    data.subarray(0, insert + 188),
    duplicate,
    data.subarray(insert + 188),
  );
  const original = (await indexAll(new Blob([data]))).source;
  const repeated = (await indexAll(new Blob([retransmitted]))).source;
  assert.equal(repeated.frames.length, original.frames.length);
  assert.deepEqual(
    await repeated.readPacket(repeated.frames[0]),
    await original.readPacket(original.frames[0]),
  );
});

test('zero-count timing runs are skipped at the beginning and between samples', async () => {
  const prefix = movie(mp4Track({ zeroRuns: true }));
  const offset = prefix.length + 8;
  const file = new Blob([
    movie(mp4Track({ zeroRuns: true, offset })),
    box('mdat', packet, packet, packet),
  ]);
  const { source } = await indexAll(file);
  assert.deepEqual(
    source.frames.map((f) => f.dts),
    [0, 0.04, 0.08],
  );
  assert.deepEqual(
    source.frames.map((f) => f.pts),
    [0.08, 0, 0.08],
  );
});
