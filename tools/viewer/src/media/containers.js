/** File-backed demuxers. Index records contain locations, never retained video packets. */
import {
  parseSEI,
  parseMetadataPES,
  parseThermalMetadata,
  normalizeTelemetry,
  packetNals,
} from './telemetry.js';

const decoder = new TextDecoder();
const MAX_SAFE = BigInt(Number.MAX_SAFE_INTEGER);
export function checkAbort(signal) {
  if (signal?.aborted)
    throw signal.reason instanceof Error
      ? signal.reason
      : new DOMException('Aborted', 'AbortError');
}
function safe(value, label = 'offset') {
  if (typeof value === 'bigint') {
    if (value < 0n || value > MAX_SAFE) throw new Error(`Unsafe ${label}`);
    return Number(value);
  }
  if (!Number.isSafeInteger(value) || value < 0) throw new Error(`Invalid ${label}`);
  return value;
}
const dv = (b) => new DataView(b.buffer, b.byteOffset, b.byteLength);
const u32 = (b, p = 0) => dv(b).getUint32(p);
const i32 = (b, p = 0) => dv(b).getInt32(p);
const u16 = (b, p = 0) => dv(b).getUint16(p);
function bigUnsigned(b) {
  let n = 0n;
  for (const value of b) n = n * 256n + BigInt(value);
  return n;
}
function uint(b) {
  return safe(bigUnsigned(b), 'integer');
}
const concat = (parts) => {
  const out = new Uint8Array(parts.reduce((s, p) => s + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
};

class Reader {
  constructor(file, { signal, maxReadBytes = 1048576, readAheadBytes = 65536 } = {}) {
    this.file = file;
    this.signal = signal;
    this.limit = Math.max(188, Math.min(4194304, maxReadBytes));
    this.readAhead = Math.min(this.limit, readAheadBytes);
    this.cache = new Uint8Array();
    this.at = 0;
    this.bytesRead = 0;
  }
  async read(offset, size, signal = this.signal) {
    checkAbort(this.signal);
    checkAbort(this.extraSignal);
    checkAbort(signal);
    safe(offset);
    safe(size, 'read size');
    if (offset + size > this.file.size)
      throw new Error(`Truncated file at ${offset} (${size} bytes)`);
    if (!size) return new Uint8Array();
    if (offset >= this.at && offset + size <= this.at + this.cache.length)
      return this.cache.subarray(offset - this.at, offset - this.at + size);
    if (size > this.limit) {
      const parts = [];
      for (let i = 0; i < size; i += this.limit)
        parts.push(await this.read(offset + i, Math.min(this.limit, size - i), signal));
      return concat(parts);
    }
    const amount = Math.min(Math.max(size, this.readAhead), this.file.size - offset);
    const bytes = new Uint8Array(await this.file.slice(offset, offset + amount).arrayBuffer());
    checkAbort(this.signal);
    checkAbort(this.extraSignal);
    checkAbort(signal);
    if (bytes.length !== amount) throw new Error('Short File.slice read');
    this.cache = bytes;
    this.at = offset;
    this.bytesRead += amount;
    return bytes.subarray(0, size);
  }
}

async function mp4Box(reader, at, end) {
  if (end - at < 8) throw new Error(`Truncated MP4 box at ${at}`);
  const b = await reader.read(at, Math.min(16, end - at));
  let size = u32(b),
    header = 8;
  const type = decoder.decode(b.subarray(4, 8));
  if (size === 1) {
    if (b.length < 16) throw new Error('Truncated extended MP4 box');
    size = safe(bigUnsigned(b.subarray(8, 16)), 'box size');
    header = 16;
  } else if (size === 0) size = end - at;
  if (size < header || at + size > end) throw new Error(`Invalid MP4 ${type} box length at ${at}`);
  return { type, offset: at, data: at + header, size, end: at + size, header };
}
async function* boxes(reader, begin, end) {
  while (begin < end) {
    const box = await mp4Box(reader, begin, end);
    yield box;
    begin = box.end;
  }
}
async function children(reader, box) {
  const result = [];
  for await (const b of boxes(reader, box.data, box.end)) result.push(b);
  return result;
}
const find = (list, type) => list.find((b) => b.type === type);
async function boxBytes(reader, box, limit = 1048576) {
  if (box.end - box.data > limit) throw new Error(`Oversized MP4 ${box.type} metadata`);
  return reader.read(box.data, box.end - box.data);
}
async function table(reader, box, columns, signed = []) {
  if (!box) return [];
  const header = await reader.read(box.data, 8),
    count = u32(header, 4),
    stride = columns * 4;
  if (count > 10000000 || 8 + count * stride > box.end - box.data)
    throw new Error(`Invalid ${box.type} table`);
  const out = [];
  for (let i = 0; i < count; i++) {
    const b = await reader.read(box.data + 8 + i * stride, stride);
    out.push(
      Array.from({ length: columns }, (_, j) =>
        signed.includes(j) ? i32(b, j * 4) : u32(b, j * 4),
      ),
    );
  }
  return out;
}

function entryCache(reader, offset, stride, count) {
  const entries = Math.max(1, Math.floor(4096 / stride));
  let first = -1,
    cache;
  return async (index) => {
    if (index < first || index >= first + (cache?.length ?? 0) / stride) {
      first = Math.floor(index / entries) * entries;
      cache = (
        await reader.read(offset + first * stride, Math.min(entries, count - first) * stride)
      ).slice();
    }
    return cache.subarray((index - first) * stride, (index - first + 1) * stride);
  };
}

function hevcCodecString(config, sampleEntry = 'hvc1') {
  if (config.length < 23) return undefined;
  const profile = config[1],
    spaces = ['', 'A', 'B', 'C'];
  let compatibility = u32(config, 2),
    reversed = 0;
  for (let i = 0; i < 32; i++) {
    reversed = reversed * 2 + (compatibility & 1);
    compatibility >>>= 1;
  }
  const constraints = [...config.subarray(6, 12)];
  while (constraints.length && constraints.at(-1) === 0) constraints.pop();
  return `${sampleEntry}.${spaces[profile >> 6]}${profile & 31}.${reversed.toString(16).toUpperCase()}.${profile & 32 ? 'H' : 'L'}${config[12]}${constraints.map((n) => '.' + n.toString(16).toUpperCase()).join('')}`;
}

async function parseMP4Track(reader, trak, movieTimescale) {
  const tc = await children(reader, trak),
    tkhd = find(tc, 'tkhd'),
    mdia = find(tc, 'mdia');
  if (!mdia || !tkhd) return null;
  const tk = await boxBytes(reader, tkhd),
    id = u32(tk, tk[0] ? 20 : 12);
  const mc = await children(reader, mdia),
    mdhd = await boxBytes(reader, find(mc, 'mdhd')),
    hdlr = await boxBytes(reader, find(mc, 'hdlr'));
  if (decoder.decode(hdlr.subarray(8, 12)) !== 'vide') return null;
  const timescale = u32(mdhd, mdhd[0] ? 20 : 12);
  if (!timescale) throw new Error('Invalid MP4 timescale');
  const minf = find(mc, 'minf');
  if (!minf) throw new Error('Missing MP4 media information');
  const stbl = find(await children(reader, minf), 'stbl');
  if (!stbl) throw new Error('Missing MP4 sample table');
  const sc = await children(reader, stbl),
    stsd = find(sc, 'stsd');
  if (!stsd) throw new Error('Missing MP4 sample description');
  const sd = await reader.read(stsd.data, 8);
  if (u32(sd, 4) !== 1) throw new Error('Multiple MP4 sample descriptions are not supported');
  const entry = await mp4Box(reader, stsd.data + 8, stsd.end);
  if (!['avc1', 'avc3', 'hvc1', 'hev1'].includes(entry.type)) return null;
  const fields = await reader.read(entry.data, 78),
    codec = entry.type.startsWith('av') ? 'avc' : 'hevc';
  let config = null;
  for await (const b of boxes(reader, entry.data + 78, entry.end))
    if (['avcC', 'hvcC'].includes(b.type)) config = await boxBytes(reader, b);
  if (!config) throw new Error('Missing AVC/HEVC configuration');
  const track = {
    id,
    codec,
    codecId: entry.type,
    codecPrivate: config.slice(),
    description: config.slice(),
    width: u16(fields, 24),
    height: u16(fields, 26),
    timescale,
    defaultDuration: null,
    nalLengthSize: (config[codec === 'avc' ? 4 : 21] & 3) + 1,
    annexB: false,
    editOffset: 0,
  };
  if (codec === 'avc')
    track.codecString = `${entry.type}.${[...config.subarray(1, 4)].map((v) => v.toString(16).padStart(2, '0')).join('')}`;
  else track.codecString = hevcCodecString(config, entry.type);
  // Keep unsupported edit lists visible rather than silently treating a movie clock as UTC.
  const edts = find(tc, 'edts');
  if (edts) {
    const elst = find(await children(reader, edts), 'elst');
    if (elst) {
      const b = await boxBytes(reader, elst),
        v = b[0],
        count = u32(b, 4),
        stride = v ? 20 : 12;
      let empty = 0,
        media = null;
      track.edits = [];
      if (count > 16 || 8 + count * stride > b.length) throw new Error('Invalid MP4 edit list');
      for (let i = 0; i < count; i++) {
        const p = 8 + i * stride;
        const duration = v ? safe(dv(b).getBigUint64(p)) : u32(b, p);
        const mediaTime = v ? Number(dv(b).getBigInt64(p + 8)) : i32(b, p + 4);
        const rate = i32(b, p + (v ? 16 : 8));
        if (!Number.isSafeInteger(mediaTime) || rate !== 65536)
          throw new Error('Unsupported MP4 edit rate/time');
        track.edits.push({ duration, mediaTime });
        if (mediaTime === -1 && media === null) empty += duration / movieTimescale;
        else if (media === null && mediaTime >= 0) {
          media = mediaTime;
          track.presentationStart = empty;
          track.presentationEnd = empty + duration / movieTimescale;
        } else throw new Error('Complex MP4 edits are not supported');
      }
      track.editOffset = empty - (media ?? 0) / timescale;
    }
  }
  track._tables = sc;
  return track;
}

async function* classicSamples(reader, track) {
  const sc = track._tables,
    sizesBox = find(sc, 'stsz');
  if (!sizesBox) return;
  const sh = await reader.read(sizesBox.data, 12),
    commonSize = u32(sh, 4),
    count = u32(sh, 8);
  if (!count) return;
  if (count > 10000000 || (!commonSize && 12 + count * 4 > sizesBox.end - sizesBox.data))
    throw new Error('Invalid MP4 sample sizes');
  const offsets = find(sc, 'co64') || find(sc, 'stco');
  if (!offsets) throw new Error('Missing MP4 chunk offsets');
  const oh = await reader.read(offsets.data, 8),
    chunks = u32(oh, 4),
    stride = offsets.type === 'co64' ? 8 : 4;
  if (8 + chunks * stride > offsets.end - offsets.data)
    throw new Error('Truncated MP4 chunk table');
  const offsetAt = entryCache(reader, offsets.data + 8, stride, chunks),
    sizeAt = entryCache(reader, sizesBox.data + 12, 4, count);
  const stsc = await table(reader, find(sc, 'stsc'), 3),
    stts = (await table(reader, find(sc, 'stts'), 2)).filter(([count]) => count > 0);
  const cb = find(sc, 'ctts'),
    cv = cb ? (await reader.read(cb.data, 1))[0] : 0;
  const ctts = (await table(reader, cb, 2, cv === 1 ? [1] : [])).filter(([count]) => count > 0);
  const keysBox = find(sc, 'stss'),
    keys = keysBox ? new Set((await table(reader, keysBox, 1)).map((r) => r[0])) : null;
  if (!stsc.length || stsc[0][0] !== 1 || !stts.length)
    throw new Error('Missing MP4 timing/chunk table');
  if (
    stts.reduce((n, r) => n + r[0], 0) !== count ||
    (ctts.length && ctts.reduce((n, r) => n + r[0], 0) !== count)
  )
    throw new Error('MP4 timing table count mismatch');
  let sample = 0,
    chunkRun = 0,
    timeRun = 0,
    timeLeft = stts[0][0],
    compositionRun = 0,
    compositionLeft = ctts[0]?.[0],
    dts = 0;
  for (let chunk = 1; chunk <= chunks; chunk++) {
    const ob = await offsetAt(chunk - 1);
    let offset = stride === 8 ? safe(bigUnsigned(ob)) : u32(ob);
    while (chunkRun + 1 < stsc.length && stsc[chunkRun + 1][0] <= chunk) chunkRun++;
    if (stsc[chunkRun][2] !== 1) throw new Error('Unsupported MP4 sample description index');
    for (let j = 0; j < stsc[chunkRun][1] && sample < count; j++, sample++) {
      const size = commonSize || u32(await sizeAt(sample));
      if (!size || offset + size > reader.file.size) throw new Error('MP4 sample outside file');
      const duration = stts[timeRun][1],
        composition = ctts[compositionRun]?.[1] ?? 0;
      track.defaultDuration ??= duration / track.timescale;
      yield {
        trackId: track.id,
        pts: (dts + composition) / track.timescale + track.editOffset,
        dts: dts / track.timescale + track.editOffset,
        mediaPTS: (dts + composition) / track.timescale,
        mediaDTS: dts / track.timescale,
        editOffset: track.editOffset,
        duration: duration / track.timescale,
        timestampTicks: dts + composition,
        timescale: track.timescale,
        keyframe: !keys || keys.has(sample + 1),
        offset,
        size,
        extents: [{ offset, size }],
      };
      offset += size;
      dts += duration;
      if (--timeLeft === 0 && timeRun + 1 < stts.length) timeLeft = stts[++timeRun][0];
      if (ctts.length && --compositionLeft === 0 && compositionRun + 1 < ctts.length)
        compositionLeft = ctts[++compositionRun][0];
    }
  }
  if (sample !== count) throw new Error('MP4 chunk/sample count mismatch');
}

async function* fragmentSamples(reader, moof, tracks, defaults) {
  let inheritedBase = moof.offset;
  for (const traf of (await children(reader, moof)).filter((b) => b.type === 'traf')) {
    const list = await children(reader, traf),
      tfhdBox = find(list, 'tfhd');
    if (!tfhdBox) throw new Error('Missing fragmented MP4 tfhd');
    const h = await boxBytes(reader, tfhdBox),
      flags = u32(h) & 0xffffff,
      id = u32(h, 4),
      track = tracks.find((t) => t.id === id);
    let at = 8,
      base = flags & 0x20000 ? moof.offset : inheritedBase;
    if (flags & 1) {
      base = safe(dv(h).getBigUint64(at));
      at += 8;
    }
    const defs = { ...defaults.get(id) };
    if (flags & 2) {
      if (u32(h, at) !== 1) throw new Error('Unsupported fragment sample description');
      at += 4;
    }
    if (flags & 8) {
      defs.duration = u32(h, at);
      at += 4;
    }
    if (flags & 16) {
      defs.size = u32(h, at);
      at += 4;
    }
    if (flags & 32) {
      defs.flags = u32(h, at);
      at += 4;
    }
    const tfdt = find(list, 'tfdt');
    let dts = track?._nextDts ?? 0;
    if (tfdt) {
      const b = await boxBytes(reader, tfdt);
      dts = b[0] ? safe(dv(b).getBigUint64(4), 'decode timestamp') : u32(b, 4);
    }
    let offset = base;
    for (const trun of list.filter((b) => b.type === 'trun')) {
      const head = await reader.read(trun.data, 8),
        f = u32(head) & 0xffffff,
        count = u32(head, 4),
        version = head[0];
      if (count > 10000000) throw new Error('Oversized MP4 fragment');
      let p = trun.data + 8,
        firstFlags;
      if (f & 1) {
        offset = base + i32(await reader.read(p, 4));
        p += 4;
      }
      if (f & 4) {
        firstFlags = u32(await reader.read(p, 4));
        p += 4;
      }
      const stride = [0x100, 0x200, 0x400, 0x800].filter((bit) => f & bit).length * 4;
      if (p + stride * count > trun.end) throw new Error('Truncated MP4 fragment samples');
      for (let i = 0; i < count; i++) {
        const b = await reader.read(p, stride);
        p += stride;
        let q = 0;
        let dur = defs.duration,
          size = defs.size,
          sampleFlags = i === 0 && firstFlags !== undefined ? firstFlags : defs.flags,
          cto = 0;
        if (f & 0x100) {
          dur = u32(b, q);
          q += 4;
        }
        if (f & 0x200) {
          size = u32(b, q);
          q += 4;
        }
        if (f & 0x400) {
          sampleFlags = u32(b, q);
          q += 4;
        }
        if (f & 0x800) cto = version ? i32(b, q) : u32(b, q);
        if (
          !Number.isInteger(size) ||
          !size ||
          !Number.isInteger(dur) ||
          dur < 0 ||
          offset < 0 ||
          offset + size > reader.file.size
        )
          throw new Error('Invalid fragmented MP4 sample');
        if (track) track.defaultDuration ??= dur / track.timescale;
        if (track)
          yield {
            trackId: id,
            pts: (dts + cto) / track.timescale + track.editOffset,
            dts: dts / track.timescale + track.editOffset,
            mediaPTS: (dts + cto) / track.timescale,
            mediaDTS: dts / track.timescale,
            editOffset: track.editOffset,
            duration: dur / track.timescale,
            timestampTicks: dts + cto,
            timescale: track.timescale,
            keyframe: !(sampleFlags & 0x10000),
            offset,
            size,
            extents: [{ offset, size }],
          };
        offset += size;
        dts += dur;
      }
    }
    if (track) track._nextDts = dts;
    inheritedBase = offset;
  }
}

async function* mp4Frames(source) {
  const reader = source.reader,
    priorFragments = [];
  let moov,
    topAt = 0;
  // Stop at moov: scanning every moof first would delay playback and read the entire video.
  while (topAt < source.file.size) {
    const box = await mp4Box(reader, topAt, source.file.size);
    topAt = box.end;
    if (box.type === 'moof') priorFragments.push(box);
    if (box.type === 'moov') {
      moov = box;
      break;
    }
  }
  if (!moov) throw new Error('Missing MP4 moov; recording may not have finalized');
  const mc = await children(reader, moov),
    mvhd = find(mc, 'mvhd');
  let movieTimescale = 1000;
  if (mvhd) {
    const b = await boxBytes(reader, mvhd);
    movieTimescale = u32(b, b[0] ? 20 : 12);
    if (!movieTimescale) throw new Error('Invalid MP4 movie timescale');
  }
  for (const trak of mc.filter((b) => b.type === 'trak')) {
    const track = await parseMP4Track(reader, trak, movieTimescale);
    if (track) source.tracks.push(track);
  }
  if (!source.tracks.length) throw new Error('No supported AVC/HEVC MP4 track');
  const defs = new Map(),
    mvex = find(mc, 'mvex');
  if (mvex)
    for (const track of source.tracks)
      if (track.edits?.some((edit) => edit.mediaTime >= 0 && edit.duration === 0))
        track.presentationEnd = Infinity; // fragmented writer did not know the final duration at moov time
  if (mvex)
    for (const box of await children(reader, mvex))
      if (box.type === 'trex') {
        const b = await boxBytes(reader, box);
        defs.set(u32(b, 4), { duration: u32(b, 12), size: u32(b, 16), flags: u32(b, 20) });
      }
  yield { tracksOnly: true };
  for (const track of source.tracks) yield* classicSamples(reader, track);
  for (const box of priorFragments) yield* fragmentSamples(reader, box, source.tracks, defs);
  while (topAt < source.file.size) {
    let box;
    try {
      box = await mp4Box(reader, topAt, source.file.size);
    } catch (error) {
      source.warnings.push(error.message);
      break;
    }
    if (box.type === 'moof') yield* fragmentSamples(reader, box, source.tracks, defs);
    topAt = box.end;
  }
}

async function ebmlHeader(reader, at, end) {
  if (at >= end) return null;
  const first = (await reader.read(at, 1))[0];
  let idSize = 1,
    marker = 128;
  while (!(first & marker) && idSize <= 4) {
    marker >>= 1;
    idSize++;
  }
  if (idSize > 4 || at + idSize + 1 > end) throw new Error('Invalid EBML ID');
  const id = uint(await reader.read(at, idSize));
  const sizeFirst = (await reader.read(at + idSize, 1))[0];
  let sizeSize = 1;
  marker = 128;
  while (!(sizeFirst & marker) && sizeSize <= 8) {
    marker >>= 1;
    sizeSize++;
  }
  if (sizeSize > 8 || at + idSize + sizeSize > end) throw new Error('Invalid EBML size');
  const bytes = await reader.read(at + idSize, sizeSize);
  let size = BigInt(bytes[0] & (marker - 1));
  for (let i = 1; i < bytes.length; i++) size = size * 256n + BigInt(bytes[i]);
  const unknown = size === (1n << BigInt(7 * sizeSize)) - 1n,
    data = at + idSize + sizeSize;
  const stop = unknown ? end : data + safe(size, 'EBML size');
  if (stop > end) throw new Error(`Truncated EBML element ${id.toString(16)}`);
  return { id, offset: at, data, end: stop, unknown };
}
async function* ebmlChildren(reader, box) {
  let at = box.data;
  while (at < box.end) {
    const child = await ebmlHeader(reader, at, box.end);
    yield child;
    if (child.unknown) throw new Error('Unsupported unknown-size nested EBML element');
    at = child.end;
  }
}
async function ebmlBytes(reader, box, limit = 1048576) {
  if (box.end - box.data > limit) throw new Error('Oversized EBML metadata');
  return reader.read(box.data, box.end - box.data);
}
async function ebmlUint(reader, box) {
  if (box.end - box.data > 8) throw new Error('Oversized EBML integer');
  return uint(await ebmlBytes(reader, box));
}

async function parseMatroskaTracks(source, box, scale) {
  for await (const entry of ebmlChildren(source.reader, box)) {
    if (entry.id !== 0xae) continue;
    const track = {
      timescale: 1e9 / scale,
      nalLengthSize: 0,
      annexB: false,
      defaultDuration: null,
      additionMappings: {},
    };
    let type;
    for await (const field of ebmlChildren(source.reader, entry)) {
      switch (field.id) {
        case 0xd7:
          track.id = await ebmlUint(source.reader, field);
          break;
        case 0x83:
          type = await ebmlUint(source.reader, field);
          break;
        case 0x86:
          track.codecId = decoder.decode(await ebmlBytes(source.reader, field));
          break;
        case 0x63a2:
          track.codecPrivate = (await ebmlBytes(source.reader, field)).slice();
          track.description = track.codecPrivate;
          break;
        case 0x23e383:
          track.defaultDuration = (await ebmlUint(source.reader, field)) / 1e9;
          break;
        case 0x6d80:
          throw new Error('Compressed/encrypted Matroska tracks are unsupported');
        case 0xe0:
          for await (const video of ebmlChildren(source.reader, field)) {
            if (video.id === 0xb0) track.width = await ebmlUint(source.reader, video);
            if (video.id === 0xba) track.height = await ebmlUint(source.reader, video);
          }
          break;
        case 0x41e4: {
          let id = 0,
            mappedType;
          for await (const mapping of ebmlChildren(source.reader, field)) {
            if (mapping.id === 0x41f0) id = await ebmlUint(source.reader, mapping);
            if (mapping.id === 0x41e7) mappedType = await ebmlUint(source.reader, mapping);
          }
          track.additionMappings[id] = mappedType;
          break;
        }
      }
    }
    if (type !== 1 || !track.id) continue;
    track.codec = { V_FFV1: 'ffv1', 'V_MPEG4/ISO/AVC': 'avc', 'V_MPEGH/ISO/HEVC': 'hevc' }[
      track.codecId
    ];
    if (!track.codec) continue;
    if (track.codec === 'avc' || track.codec === 'hevc')
      track.nalLengthSize = ((track.codecPrivate?.[track.codec === 'avc' ? 4 : 21] ?? 3) & 3) + 1;
    if (track.codec === 'avc' && track.codecPrivate?.length >= 4)
      track.codecString = `avc1.${[...track.codecPrivate.subarray(1, 4)].map((v) => v.toString(16).padStart(2, '0')).join('')}`;
    if (track.codec === 'hevc' && track.codecPrivate)
      track.codecString = hevcCodecString(track.codecPrivate);
    source.tracks.push(track);
  }
}

async function blockFrame(
  source,
  block,
  clusterTicks,
  keyframe,
  addition = null,
  durationTicks = null,
  blockHeader = null,
) {
  const size = Math.min(12, block.end - block.data),
    bytes = blockHeader || (await source.reader.read(block.data, size));
  let n = 1,
    marker = 128;
  while (!(bytes[0] & marker) && n <= 8) {
    marker >>= 1;
    n++;
  }
  if (n > 8 || bytes.length < n + 3) throw new Error('Invalid Matroska block header');
  let id = bytes[0] & (marker - 1);
  for (let i = 1; i < n; i++) id = id * 256 + bytes[i];
  const track = source.tracks.find((t) => t.id === id);
  if (!track) return null;
  const relative = dv(bytes).getInt16(n),
    flags = bytes[n + 2];
  if (flags & 6) throw new Error('Laced Matroska video blocks are unsupported');
  const offset = block.data + n + 3,
    payloadSize = block.end - offset;
  if (!payloadSize) throw new Error('Empty Matroska video packet');
  const thermal = addition ? parseThermalMetadata(addition) : null;
  let record = thermal?.telemetry?.schema === 'apcg.telemetry.v1' ? { ...thermal.telemetry } : null;
  if (record) {
    record.thermal = Object.fromEntries(
      ['temperature_scale_k', 'temperature_offset_k', 'minimum_c', 'maximum_c'].map((k) => [
        k,
        thermal[k] ?? null,
      ]),
    );
    // The nested snapshot belongs to RGB; only the outer thermal lens applies.
    record.provenance = {
      ...record.provenance,
      nested_hfov_deg: record.hfov_deg ?? null,
      hfov_source: 'thermal metadata',
    };
    record.hfov_deg =
      Number.isFinite(thermal.hfov_deg) && thermal.hfov_deg > 0 && thermal.hfov_deg < 179
        ? thermal.hfov_deg
        : null;
  }
  return {
    trackId: id,
    pts: (clusterTicks + relative) / track.timescale,
    dts: null,
    duration: durationTicks === null ? track.defaultDuration : durationTicks / track.timescale,
    timestampTicks: clusterTicks + relative,
    timescale: track.timescale,
    keyframe: keyframe === null ? !!(flags & 128) : keyframe,
    offset,
    size: payloadSize,
    extents: [{ offset, size: payloadSize }],
    telemetry: normalizeTelemetry(record, { format: 'matroska-block-addition' }),
    thermalMetadata: thermal,
  };
}

async function blockGroup(source, group, clusterTicks) {
  let block,
    blockHeader,
    reference = false,
    duration = null;
  const additions = [];
  for await (const field of ebmlChildren(source.reader, group)) {
    if (field.id === 0xa1) {
      block = field;
      blockHeader = (
        await source.reader.read(block.data, Math.min(12, block.end - block.data))
      ).slice();
    } else if (field.id === 0xfb) reference = true;
    else if (field.id === 0x9b) duration = await ebmlUint(source.reader, field);
    else if (field.id === 0x75a1) {
      for await (const more of ebmlChildren(source.reader, field)) {
        if (more.id !== 0xa6) continue;
        let id = 1,
          data;
        for await (const item of ebmlChildren(source.reader, more)) {
          if (item.id === 0xee) id = await ebmlUint(source.reader, item);
          if (item.id === 0xa5) data = (await ebmlBytes(source.reader, item)).slice();
        }
        additions.push({ id, data });
      }
    }
  }
  if (!block) return null;
  // Read the track number before resolving its per-track mapping.
  const h = blockHeader;
  let n = 1,
    mark = 128;
  while (!(h[0] & mark) && n <= 8) {
    n++;
    mark >>= 1;
  }
  if (n > h.length || n > 8) throw new Error('Invalid Matroska track number');
  let id = h[0] & (mark - 1);
  for (let i = 1; i < n; i++) id = id * 256 + h[i];
  const track = source.tracks.find((t) => t.id === id);
  const addition = additions.find((a) => track?.additionMappings[a.id] === 0x41504347)?.data;
  return blockFrame(source, block, clusterTicks, !reference, addition, duration, blockHeader);
}

const SEGMENT_CHILDREN = new Set([
  0x1549a966, 0x1654ae6b, 0x1f43b675, 0x114d9b74, 0x1c53bb6b, 0x1941a469, 0x1043a770, 0x1254c367,
]);
async function* matroskaFrames(source) {
  const reader = source.reader;
  let at = 0,
    segment;
  while (at < source.file.size) {
    const h = await ebmlHeader(reader, at, source.file.size);
    if (h.id === 0x18538067) {
      segment = h;
      break;
    }
    if (h.unknown) throw new Error('Unknown-size EBML header');
    at = h.end;
  }
  if (!segment) throw new Error('Missing Matroska Segment');
  let scale = 1000000;
  at = segment.data;
  while (at < segment.end) {
    const h = await ebmlHeader(reader, at, segment.end);
    if (h.id === 0x1549a966) {
      for await (const info of ebmlChildren(reader, h))
        if (info.id === 0x2ad7b1) scale = await ebmlUint(reader, info);
      if (!scale) throw new Error('Invalid Matroska TimestampScale');
      for (const track of source.tracks) track.timescale = 1e9 / scale;
    } else if (h.id === 0x1654ae6b) {
      await parseMatroskaTracks(source, h, scale);
      if (!source.tracks.length) throw new Error('No supported Matroska video track');
      yield { tracksOnly: true };
    } else if (h.id === 0x1f43b675) {
      if (!source.tracks.length) throw new Error('Matroska clusters precede tracks');
      let pos = h.data,
        ticks = null;
      while (pos < h.end) {
        const child = await ebmlHeader(reader, pos, h.end);
        if (h.unknown && SEGMENT_CHILDREN.has(child.id)) break;
        if (child.id === 0xe7) ticks = await ebmlUint(reader, child);
        else if (child.id === 0xa0 || child.id === 0xa3) {
          if (ticks === null) throw new Error('Matroska block precedes Cluster timestamp');
          const frame =
            child.id === 0xa0
              ? await blockGroup(source, child, ticks)
              : await blockFrame(source, child, ticks, null);
          if (frame) yield frame;
        }
        if (child.unknown) throw new Error('Unknown-size Matroska block');
        pos = child.end;
      }
      at = pos;
      continue;
    } else if (h.unknown) throw new Error('Unsupported unknown-size Matroska element');
    at = h.end;
  }
}

function crcMPEG(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte << 24;
    for (let b = 0; b < 8; b++) crc = ((crc << 1) ^ (crc & 0x80000000 ? 0x04c11db7 : 0)) >>> 0;
  }
  return crc >>> 0;
}
function pesTime(b, at) {
  if (at + 5 > b.length || !(b[at] & 1) || !(b[at + 2] & 1) || !(b[at + 4] & 1))
    throw new Error('Invalid PES timestamp');
  return (
    ((b[at] >> 1) & 7) * 2 ** 30 +
    b[at + 1] * 2 ** 22 +
    (b[at + 2] >> 1) * 2 ** 15 +
    b[at + 3] * 128 +
    (b[at + 4] >> 1)
  );
}
const CLOCK_WRAP = 2 ** 33;
export function unwrapTimestamp(value, previous) {
  return previous === null || previous === undefined
    ? value
    : value + Math.round((previous - value) / CLOCK_WRAP) * CLOCK_WRAP;
}
function bitReader(bytes) {
  let pos = 0;
  const bits = (n) => {
    if (n > 32 || pos + n > bytes.length * 8) throw new Error('Truncated SPS');
    let value = 0;
    while (n--) {
      value = value * 2 + ((bytes[pos >> 3] >> (7 - (pos % 8))) & 1);
      pos++;
    }
    return value;
  };
  const ue = () => {
    let zeros = 0;
    while (!bits(1)) if (++zeros > 30) throw new Error('Invalid SPS Exp-Golomb');
    return 2 ** zeros - 1 + bits(zeros);
  };
  return {
    bits,
    ue,
    se: () => {
      const n = ue();
      return n % 2 ? (n + 1) / 2 : -n / 2;
    },
    skip: (n) => {
      while (n > 32) {
        bits(32);
        n -= 32;
      }
      bits(n);
    },
  };
}

/** SPS geometry/configuration is optional; malformed SPS cannot make fabricated geometry. */
function updateAnnexBTrack(track, packet) {
  const sets = (track._sets ||= new Map());
  let keyframe = false;
  for (const nal of packetNals(packet)) {
    const kind = track.codec === 'hevc' ? (nal[0] >> 1) & 63 : nal[0] & 31;
    keyframe ||= track.codec === 'hevc' ? kind >= 16 && kind <= 21 : kind === 5;
    if (!(track.codec === 'hevc' ? [32, 33, 34] : [7, 8]).includes(kind)) continue;
    sets.set(kind, nal.slice());
    if (kind !== (track.codec === 'hevc' ? 33 : 7)) continue;
    try {
      const raw = [];
      for (let i = track.codec === 'hevc' ? 2 : 1; i < nal.length; i++) {
        if (i >= 2 && nal[i] === 3 && nal[i - 1] === 0 && nal[i - 2] === 0) continue;
        raw.push(nal[i]);
      }
      const b = bitReader(raw);
      let chroma = 1,
        separate = 0,
        width,
        height,
        sx = 2,
        sy = 2;
      let left = 0,
        right = 0,
        top = 0,
        bottom = 0;
      if (track.codec === 'hevc') {
        b.bits(4);
        const layers = b.bits(3);
        b.bits(1);
        b.skip(96);
        const profiles = [],
          levels = [];
        for (let i = 0; i < layers; i++) {
          profiles.push(b.bits(1));
          levels.push(b.bits(1));
        }
        if (layers) b.skip(2 * (8 - layers));
        for (let i = 0; i < layers; i++) {
          if (profiles[i]) b.skip(88);
          if (levels[i]) b.skip(8);
        }
        b.ue();
        chroma = b.ue();
        if (chroma === 3) separate = b.bits(1);
        width = b.ue();
        height = b.ue();
        if (b.bits(1)) {
          left = b.ue();
          right = b.ue();
          top = b.ue();
          bottom = b.ue();
        }
        sx = chroma === 1 || chroma === 2 ? 2 : 1;
        sy = chroma === 1 ? 2 : 1;
        track.hevcChroma = chroma;
        track.bitDepthLumaMinus8 = b.ue();
        track.bitDepthChromaMinus8 = b.ue();
      } else {
        const profile = b.bits(8),
          compatibility = b.bits(8),
          level = b.bits(8);
        b.ue();
        track.codecString = `avc1.${[profile, compatibility, level].map((v) => v.toString(16).padStart(2, '0')).join('')}`;
        if ([100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135].includes(profile)) {
          chroma = b.ue();
          if (chroma === 3) separate = b.bits(1);
          b.ue();
          b.ue();
          b.bits(1);
          if (b.bits(1))
            for (let i = 0; i < (chroma === 3 ? 12 : 8); i++)
              if (b.bits(1)) {
                let last = 8,
                  next = 8;
                for (let j = 0; j < (i < 6 ? 16 : 64); j++) {
                  if (next) next = (last + b.se() + 256) % 256;
                  if (next) last = next;
                }
              }
        }
        b.ue();
        const order = b.ue();
        if (order === 0) b.ue();
        else if (order === 1) {
          b.bits(1);
          b.se();
          b.se();
          const count = b.ue();
          if (count > 256) throw new Error('Invalid SPS');
          for (let i = 0; i < count; i++) b.se();
        } else if (order > 2) throw new Error('Invalid SPS');
        b.ue();
        b.bits(1);
        width = (b.ue() + 1) * 16;
        height = b.ue() + 1;
        const frame = b.bits(1);
        height *= 16 * (2 - frame);
        if (!frame) b.bits(1);
        b.bits(1);
        if (b.bits(1)) {
          left = b.ue();
          right = b.ue();
          top = b.ue();
          bottom = b.ue();
        }
        const array = separate ? 0 : chroma;
        sx = array === 1 || array === 2 ? 2 : 1;
        sy = (array === 1 ? 2 : 1) * (2 - frame);
      }
      if (separate) sx = sy = 1;
      width -= sx * (left + right);
      height -= sy * (top + bottom);
      if (width > 0 && height > 0 && width <= 16384 && height <= 16384) {
        track.width = width;
        track.height = height;
      }
    } catch {
      /* Geometry remains unknown; coded bytes remain available to the decoder. */
    }
  }
  track.parameterSets = [...sets.values()];
  return keyframe;
}

function annexBConfig(track) {
  const sets = track._sets;
  if (!sets) return;
  if (track.codec === 'avc' && sets.has(7) && sets.has(8)) {
    const sps = sets.get(7),
      pps = sets.get(8);
    if (sps.length < 4 || sps.length > 65535 || pps.length > 65535) return;
    track.codecPrivate = concat([
      Uint8Array.of(1, sps[1], sps[2], sps[3], 255, 225, sps.length >> 8, sps.length & 255),
      sps,
      Uint8Array.of(1, pps.length >> 8, pps.length & 255),
      pps,
    ]);
    track.description = track.codecPrivate;
  } else if (track.codec === 'hevc' && [32, 33, 34].every((k) => sets.has(k))) {
    const sps = sets.get(33),
      rbsp = [];
    for (let i = 2; i < sps.length; i++) {
      if (i >= 2 && sps[i] === 3 && sps[i - 1] === 0 && sps[i - 2] === 0) continue;
      rbsp.push(sps[i]);
    }
    if (rbsp.length < 13) return;
    const h = new Uint8Array(23);
    h[0] = 1;
    h.set(rbsp.slice(1, 13), 1);
    h[13] = 0xf0;
    h[15] = 0xfc;
    h[16] = 0xfc | (track.hevcChroma ?? 1);
    h[17] = 0xf8 | (track.bitDepthLumaMinus8 ?? 0);
    h[18] = 0xf8 | (track.bitDepthChromaMinus8 ?? 0);
    h[21] = ((((rbsp[0] >> 1) & 7) + 1) << 3) | ((rbsp[0] & 1) << 2) | 3;
    h[22] = 3;
    track.codecPrivate = concat([
      h,
      ...[32, 33, 34].flatMap((k) => {
        const n = sets.get(k);
        return [Uint8Array.of(128 | k, 0, 1, n.length >> 8, n.length & 255), n];
      }),
    ]);
    track.description = track.codecPrivate;
    track.codecString = hevcCodecString(track.codecPrivate);
  }
}

async function* tsFrames(source) {
  const reader = source.reader,
    pending = new Map(),
    sections = new Map(),
    continuity = new Map();
  const metadata = new Map(),
    emittedTicks = new Set(),
    emittedOrder = [],
    queue = [],
    pmtPids = new Set(),
    streamTypes = new Map(),
    asynchronousPids = new Set();
  let clock = null,
    start = 0,
    lastVideo = null,
    asynchronousRecords = 0,
    asynchronousMatches = 0;
  const probe = await reader.read(0, Math.min(188 * 6, source.file.size));
  while (
    start < Math.min(188, probe.length) &&
    !(probe[start] === 0x47 && probe[start + 188] === 0x47)
  )
    start++;
  if (start === Math.min(188, probe.length)) throw new Error('No MPEG-TS packet alignment');
  const warn = (message) => {
    if (!source.warnings.includes(message)) source.warnings.push(message);
  };
  function parseSection(pid, s) {
    if (s.length < 8 || crcMPEG(s)) {
      warn('Invalid MPEG-TS PSI checksum');
      return;
    }
    if (!(s[5] & 1)) return;
    if (pid === 0 && s[0] === 0) {
      for (let at = 8; at + 4 <= s.length - 4; at += 4)
        if (s[at] || s[at + 1]) pmtPids.add((s[at + 2] & 31) * 256 + s[at + 3]);
    } else if (pmtPids.has(pid) && s[0] === 2) {
      let at = 12 + ((s[10] & 15) * 256 + s[11]);
      while (at + 5 <= s.length - 4) {
        const type = s[at],
          id = (s[at + 1] & 31) * 256 + s[at + 2],
          size = (s[at + 3] & 15) * 256 + s[at + 4];
        if (at + 5 + size > s.length - 4) {
          warn('Truncated MPEG-TS PMT descriptor');
          break;
        }
        const descriptors = s.subarray(at + 5, at + 5 + size);
        let klva = false;
        for (let i = 0; i + 2 <= descriptors.length; ) {
          const tag = descriptors[i],
            len = descriptors[i + 1];
          if (i + 2 + len > descriptors.length) break;
          if (
            (tag === 5 || tag === 0x26) &&
            decoder.decode(descriptors.subarray(i + 2, i + 2 + len)).includes('KLVA')
          )
            klva = true;
          i += 2 + len;
        }
        if ([0x1b, 0x24].includes(type)) {
          streamTypes.set(id, type);
          if (!source.tracks.some((t) => t.id === id))
            source.tracks.push({
              id,
              codec: type === 0x24 ? 'hevc' : 'avc',
              codecId: type === 0x24 ? 'V_MPEGH/ISO/HEVC' : 'V_MPEG4/ISO/AVC',
              timescale: 90000,
              defaultDuration: null,
              annexB: true,
              nalLengthSize: 0,
            });
        } else if ((type === 0x15 || type === 6) && klva) {
          streamTypes.set(id, 'klv');
          // ST1402 synchronous metadata (0x15) belongs to an exact video PTS.
          // Private-data KLV (0x06) has its own sampling/presentation times.
          if (type === 6) asynchronousPids.add(id);
          else asynchronousPids.delete(id);
        }
        at += 5 + size;
      }
    }
  }
  function feedPSI(pid, payload, unitStart) {
    let state = sections.get(pid) || new Uint8Array();
    function consume(bytes) {
      state = concat([state, bytes]);
      while (state.length >= 3 && state[0] !== 255) {
        const n = 3 + ((state[1] & 15) * 256 + state[2]);
        if (n > 4096) {
          state = new Uint8Array();
          warn('Oversized MPEG-TS PSI section');
          break;
        }
        if (state.length < n) break;
        parseSection(pid, state.subarray(0, n));
        state = state.slice(n);
      }
      if (state[0] === 255) state = new Uint8Array();
    }
    if (unitStart) {
      const pointer = payload[0];
      if (pointer + 1 > payload.length) {
        sections.delete(pid);
        warn('Invalid MPEG-TS PSI pointer');
        return;
      }
      if (state.length) consume(payload.subarray(1, 1 + pointer));
      state = new Uint8Array();
      consume(payload.subarray(1 + pointer));
    } else if (state.length) consume(payload);
    sections.set(pid, state);
  }
  function finishPES(pid) {
    const state = pending.get(pid);
    pending.delete(pid);
    if (!state?.size) return;
    const b = concat(state.parts);
    if (b.length < 9 || b[0] || b[1] || b[2] !== 1 || (b[6] & 0xc0) !== 0x80) {
      warn('Invalid MPEG-TS PES header');
      return;
    }
    const declared = u16(b, 4),
      end = declared ? 6 + declared : b.length;
    if (end > b.length || 9 + b[8] > end || !(b[7] & 0x80)) {
      warn('Truncated or untimed MPEG-TS PES');
      return;
    }
    let pts, dts;
    try {
      const rawPTS = pesTime(b, 9),
        rawDTS = b[7] & 0x40 ? pesTime(b, 14) : rawPTS;
      dts = unwrapTimestamp(rawDTS, clock);
      pts = unwrapTimestamp(rawPTS, dts);
      if (streamTypes.get(pid) !== 'klv') {
        if (clock === null && metadata.size) {
          // A metadata PTS may wrap before the first video's earlier DTS. The video
          // decode clock establishes the epoch; retain and rebase those early sets.
          const early = [...metadata];
          metadata.clear();
          for (const [tick, record] of early) metadata.set(unwrapTimestamp(tick, dts), record);
        }
        clock = dts;
      }
    } catch (error) {
      warn(error.message);
      return;
    }
    const header = 9 + b[8],
      data = b.subarray(header, end);
    if (streamTypes.get(pid) === 'klv') {
      const errors = [],
        records = parseMetadataPES(data, { errors });
      errors.forEach(warn);
      const legacyCell = records.at(-1)?.provenance?.klv_au_cell_compatibility,
        asynchronous = asynchronousPids.has(pid) || !!legacyCell;
      if (legacyCell)
        warn(
          'Legacy KLV cell fragment flags: validated complete sets use approximate timestamp association',
        );
      if (records.length && !asynchronous && emittedTicks.has(pts))
        warn(
          'Late KLV arrived after its video frame was emitted; telemetry not retroactively applied',
        );
      else if (records.length) {
        metadata.set(pts, { record: records.at(-1), asynchronous, pid });
        if (asynchronous) asynchronousRecords++;
      }
      while (metadata.size > 4096) metadata.delete(metadata.keys().next().value);
      return;
    }
    const track = source.tracks.find((t) => t.id === pid);
    if (!track || !data.length) return;
    const keyframe = updateAnnexBTrack(track, data);
    annexBConfig(track);
    // Current camera emits one AU per PES. Multiple AUDs mean this assumption is unsafe.
    let auds = 0;
    for (const nal of packetNals(data))
      if (
        (track.codec === 'hevc' ? (nal[0] >> 1) & 63 : nal[0] & 31) ===
        (track.codec === 'hevc' ? 35 : 9)
      )
        auds++;
    if (auds > 1) {
      warn('Multiple access units in MPEG-TS PES are unsupported');
      return;
    }
    const record = parseSEI(data, track.codec).at(-1);
    const frame = {
      trackId: pid,
      pts: pts / 90000,
      dts: dts / 90000,
      timestampTicks: pts,
      timescale: 90000,
      duration: null,
      keyframe,
      offset: state.start,
      size: end - header,
      transport: {
        packetStart: state.start,
        packetEnd: state.end,
        pid,
        headerBytes: header,
        pesBytes: end,
      },
      telemetry: normalizeTelemetry(record, { format: 'apcg-sei', container: 'mpegts' }),
      thermalMetadata: null,
    };
    if (lastVideo && lastVideo.trackId === pid && dts > lastVideo.dts * 90000) {
      lastVideo.duration = dts / 90000 - lastVideo.dts;
      track.defaultDuration ??= lastVideo.duration;
    }
    lastVideo = frame;
    queue.push(frame);
  }
  function selectMetadata(frame) {
    const exact = metadata.get(frame.timestampTicks);
    if (exact) return { tick: frame.timestampTicks, entry: exact };
    // Retain the nearest available asynchronous sample for at most the same
    // two-second freshness window as camera poses. A sample may lead by half
    // a video frame, covering quantisation/mux jitter at the start of a clip.
    const duration =
        frame.duration ?? source.tracks.find((t) => t.id === frame.trackId)?.defaultDuration ?? 0,
      lead = Math.max(0, Math.min(0.1, duration / 2)) * 90000;
    let best = null,
      distance = Infinity;
    for (const [tick, entry] of metadata) {
      if (!entry.asynchronous) continue;
      const offset = frame.timestampTicks - tick;
      if (offset < -lead || offset > 180000) continue;
      const delta = Math.abs(offset);
      if (delta < distance || (delta === distance && tick < best.tick)) {
        best = { tick, entry };
        distance = delta;
      }
    }
    return best;
  }
  function discardAsynchronous(pid) {
    for (const [tick, entry] of metadata)
      if (entry.asynchronous && (pid === undefined || entry.pid === pid)) metadata.delete(tick);
  }
  function attach(frame) {
    emittedTicks.add(frame.timestampTicks);
    emittedOrder.push(frame.timestampTicks);
    if (emittedOrder.length > 4096) emittedTicks.delete(emittedOrder.shift());
    const match = selectMetadata(frame);
    if (match) {
      const { tick, entry } = match;
      let record = entry.record;
      const provenance = { format: 'misb-klv', container: 'mpegts' };
      if (entry.asynchronous) {
        asynchronousMatches++;
        const offsetMs = (frame.timestampTicks - tick) / 90;
        // Every reused snapshot gets its own ages; never mutate the original
        // sample or advance its recorded UTC to an invented exposure time.
        record = { ...record };
        for (const key of ['position', 'vehicle_attitude', 'gimbal_attitude', 'velocity'])
          if (record[key] && Number.isFinite(record[key].age_ms))
            record[key] = { ...record[key], age_ms: record[key].age_ms + Math.abs(offsetMs) };
        provenance.association = 'nearest-pts';
        provenance.metadata_pts = tick / 90000;
        provenance.metadata_offset_ms = offsetMs;
        record.time_alignment = {
          ...record.time_alignment,
          basis: 'KLV; approximate transport timestamp association',
        };
      } else metadata.delete(tick);
      frame.klvTelemetry = normalizeTelemetry(record, provenance);
      if (!frame.telemetry) frame.telemetry = frame.klvTelemetry;
    }
    frame.duration ??= source.tracks.find((t) => t.id === frame.trackId)?.defaultDuration ?? null;
    // Bound unmatched metadata in damaged or metadata-only streams.
    while (metadata.size > 4096) metadata.delete(metadata.keys().next().value);
    const earliest =
      queue.reduce((tick, f) => Math.min(tick, f.timestampTicks), frame.timestampTicks) - 180000;
    for (const [tick, entry] of metadata)
      if (entry.asynchronous && tick < earliest) metadata.delete(tick);
    return frame;
  }
  let advertised = false;
  for (let offset = start; offset + 188 <= source.file.size; offset += 188) {
    const packet = await reader.read(offset, 188);
    source.position = offset + 188;
    if (packet[0] !== 0x47) {
      pending.clear();
      discardAsynchronous();
      sections.clear();
      continuity.clear();
      const scan = await reader.read(offset, Math.min(188 * 18, source.file.size - offset));
      let recovered = -1;
      for (let i = 1; i + 188 < scan.length && i <= 188 * 16; i++) {
        if (
          scan[i] === 0x47 &&
          scan[i + 188] === 0x47 &&
          (i + 376 >= scan.length || scan[i + 376] === 0x47)
        ) {
          recovered = i;
          break;
        }
      }
      if (recovered < 0) {
        warn('MPEG-TS sync loss; no alignment in bounded scan');
        offset += scan.length - 188;
      } else {
        warn('MPEG-TS sync loss recovered within bounded scan');
        offset += recovered - 188;
      }
      continue;
    }
    const pid = (packet[1] & 31) * 256 + packet[2],
      startPES = !!(packet[1] & 64),
      control = (packet[3] >> 4) & 3,
      cc = packet[3] & 15;
    let off = 4,
      discontinuity = false;
    if (!control || packet[1] & 128 || packet[3] & 192) {
      pending.delete(pid);
      discardAsynchronous(pid);
      sections.delete(pid);
      warn('Damaged or scrambled MPEG-TS packet');
      continue;
    }
    if (control & 2) {
      if (packet[4] > 183) {
        pending.delete(pid);
        warn('Invalid MPEG-TS adaptation field');
        continue;
      }
      if (packet[4]) discontinuity = !!(packet[5] & 128);
      off += 1 + packet[4];
    }
    if (!(control & 1) || off >= 188) continue;
    const previous = continuity.get(pid);
    if (discontinuity) {
      pending.delete(pid);
      // A video clock discontinuity invalidates all held KLV; a metadata
      // discontinuity invalidates that stream's snapshots only.
      if (streamTypes.get(pid) === 'klv') discardAsynchronous(pid);
      else if (typeof streamTypes.get(pid) === 'number') discardAsynchronous();
      sections.delete(pid);
      continuity.delete(pid);
    } else if (previous !== undefined && cc === previous)
      continue; // Repeated packet.
    else if (previous !== undefined && cc !== (previous + 1) % 16) {
      pending.delete(pid);
      discardAsynchronous(pid);
      sections.delete(pid);
      warn('MPEG-TS continuity gap; incomplete PES discarded');
    }
    continuity.set(pid, cc);
    const payload = packet.subarray(off);
    if (pid === 0 || pmtPids.has(pid)) feedPSI(pid, payload, startPES);
    else if (streamTypes.has(pid)) {
      if (startPES) {
        finishPES(pid);
        pending.set(pid, { parts: [], size: 0, start: offset, end: offset + 188 });
      }
      const state = pending.get(pid);
      if (state) {
        state.size += payload.length;
        if (state.size > source.maxPacketBytes + 512) {
          pending.delete(pid);
          warn('Oversized MPEG-TS PES discarded');
        } else {
          state.parts.push(payload);
          state.end = offset + 188;
          // Sparse asynchronous metadata need not wait for the next metadata
          // PES (which may never arrive) before becoming available to video.
          if (streamTypes.get(pid) === 'klv') {
            if (state.pesBytes === undefined && state.size >= 6) {
              const header = state.parts[0].length >= 6 ? state.parts[0] : concat(state.parts);
              const size = u16(header, 4);
              state.pesBytes = size ? 6 + size : 0;
            }
            if (state.pesBytes && state.size >= state.pesBytes) finishPES(pid);
          }
        }
      }
    }
    if (!advertised && queue.length) {
      advertised = true;
      yield { tracksOnly: true };
    }
    // Allow mux/reorder lag for metadata, without retaining compressed packets.
    // Keep two video PES to learn frame duration, and at most two decode-clock
    // seconds /180 descriptors while waiting for an advertised KLV stream.
    while (queue.length > 2) {
      const first = queue[0],
        expectsKLV = [...streamTypes.values()].includes('klv');
      if (
        expectsKLV &&
        !first.telemetry &&
        !selectMetadata(first) &&
        queue.length <= 180 &&
        clock / 90000 - first.dts <= 2
      )
        break;
      yield attach(queue.shift());
    }
  }
  if ((source.file.size - start) % 188) warn('Truncated final MPEG-TS packet');
  for (const pid of [...pending.keys()]) finishPES(pid);
  while (queue.length) yield attach(queue.shift());
  if ([...metadata.values()].some((entry) => !entry.asynchronous))
    warn('Unmatched KLV remained after the final video frame');
  if (asynchronousRecords && !asynchronousMatches)
    warn('Asynchronous KLV could not be associated with video within the freshness window');
  if (!source.tracks.length) throw new Error('No AVC/HEVC MPEG-TS track advertised');
}

/** Reconstruct one PES on demand; the persistent TS index has a constant-size descriptor. */
async function readTransportPacket(source, frame, signal) {
  const t = frame.transport;
  safe(t.packetStart);
  safe(t.packetEnd);
  safe(t.headerBytes);
  if (
    t.packetEnd > source.file.size ||
    t.packetEnd <= t.packetStart ||
    (t.packetEnd - t.packetStart) % 188
  )
    throw new Error('Invalid TS packet span');
  const out = new Uint8Array(frame.size);
  let skip = t.headerBytes,
    copied = 0,
    continuity = null;
  for (let at = t.packetStart; at < t.packetEnd && copied < out.length; at += 188) {
    if (source.closed) throw new DOMException('Closed', 'AbortError');
    const b = await source.packetReader.read(at, 188, signal);
    if (b[0] !== 0x47) throw new Error('TS packet sync changed during read');
    if ((b[1] & 31) * 256 + b[2] !== t.pid || !(b[3] & 16)) continue;
    const counter = b[3] & 15;
    if (counter === continuity) continue; // Match the indexer's retransmission handling.
    if (continuity !== null && counter !== (continuity + 1) % 16)
      throw new Error('TS continuity changed during read');
    continuity = counter;
    const begin = 4 + (b[3] & 32 ? 1 + b[4] : 0);
    if (begin > 188 || b[1] & 128 || b[3] & 192) throw new Error('Invalid TS payload during read');
    const data = b.subarray(begin);
    if (skip >= data.length) {
      skip -= data.length;
      continue;
    }
    const n = Math.min(data.length - skip, out.length - copied);
    out.set(data.subarray(skip, skip + n), copied);
    copied += n;
    skip = 0;
  }
  if (source.closed) throw new DOMException('Closed', 'AbortError');
  if (copied !== out.length) throw new Error('Incomplete TS packet span');
  return out;
}

/** Walk length-prefixed NAL headers and read SEI only, including AVC/HEVC suffix metadata. */
async function readSampleSEI(source, frame, track, signal) {
  const reader = source.metadataReader,
    lengthSize = track.nalLengthSize;
  if (!lengthSize) return parseSEI(await source.readPacket(frame, { signal }), track.codec);
  const records = [];
  let at = 0,
    count = 0;
  const header = track.codec === 'hevc' ? 2 : 1;
  while (at + lengthSize <= frame.size) {
    checkAbort(signal);
    if (++count > 65536) throw new Error('Too many NAL units in one sample');
    const prefix = await reader.read(frame.offset + at, lengthSize);
    const size = uint(prefix);
    at += lengthSize;
    if (!size || size < header || at + size > frame.size) {
      if (!source.warnings.includes('Invalid NAL length in video sample'))
        source.warnings.push('Invalid NAL length in video sample');
      break;
    }
    const h = await reader.read(frame.offset + at, header);
    const type = track.codec === 'hevc' ? (h[0] >> 1) & 63 : h[0] & 31;
    if ((track.codec === 'hevc' ? [39, 40] : [6]).includes(type)) {
      if (size > 1048576) {
        if (!source.warnings.includes('SEI NAL exceeds metadata size limit'))
          source.warnings.push('SEI NAL exceeds metadata size limit');
      } else {
        const nal = await reader.read(frame.offset + at, size, signal);
        records.push(...parseSEI(concat([Uint8Array.of(0, 0, 1), nal]), track.codec));
      }
    }
    at += size;
  }
  return records;
}

export async function openMedia(file, options = {}) {
  if (
    !file ||
    typeof file.slice !== 'function' ||
    !Number.isSafeInteger(file.size) ||
    file.size < 4
  )
    throw new Error('A nonempty local File or Blob is required');
  const source = {
    file,
    reader: new Reader(file, options),
    packetReader: new Reader(file, options),
    metadataReader: new Reader(file, { ...options, readAheadBytes: 4096 }),
    maxPacketBytes: options.maxPacketBytes ?? 16777216,
    frames: [],
    tracks: [],
    warnings: [],
    position: 0,
    complete: false,
    closed: false,
    indexing: false,
    async readPacket(frame, { signal } = {}) {
      if (this.closed) throw new Error('Media source is closed');
      checkAbort(options.signal);
      checkAbort(signal);
      if (
        !frame ||
        !Number.isSafeInteger(frame.size) ||
        frame.size <= 0 ||
        frame.size > this.maxPacketBytes
      )
        throw new Error('Invalid or oversized coded packet');
      if (frame.transport) return readTransportPacket(this, frame, signal);
      const extents = frame.extents || [{ offset: frame.offset, size: frame.size }];
      if (extents.reduce((n, part) => n + part.size, 0) !== frame.size)
        throw new Error('Packet extent size mismatch');
      const parts = [];
      for (const part of extents) {
        if (this.closed) throw new DOMException('Closed', 'AbortError');
        parts.push(await this.packetReader.read(part.offset, part.size, signal));
      }
      if (this.closed) throw new DOMException('Closed', 'AbortError');
      return concat(parts);
    },
    close() {
      this.closed = true;
      this.reader.cache = this.packetReader.cache = this.metadataReader.cache = new Uint8Array();
    },
    async *index({ signal, batchSize = options.batchSize ?? 64 } = {}) {
      if (this.closed) throw new Error('Media source is closed');
      if (this.indexing || this.complete || this.frames.length)
        throw new Error('Media index already started');
      this.indexing = true;
      this.reader.extraSignal = this.metadataReader.extraSignal = signal;
      let batch = [];
      const keyedTracks = new Set();
      const report = () => ({
        frames: batch.splice(0),
        tracks: this.tracks,
        progress: {
          bytesRead: this.reader.bytesRead + this.metadataReader.bytesRead,
          position: this.position,
          fileSize: file.size,
          complete: this.complete,
        },
        warnings: this.warnings.slice(),
      });
      const iterator =
        this.format === 'mp4'
          ? mp4Frames(this)
          : this.format === 'matroska'
            ? matroskaFrames(this)
            : tsFrames(this);
      try {
        for await (const frame of iterator) {
          checkAbort(options.signal);
          checkAbort(signal);
          if (this.closed) throw new DOMException('Closed', 'AbortError');
          if (frame.tracksOnly) {
            yield report();
            continue;
          }
          if (frame.size > this.maxPacketBytes)
            throw new Error('Coded video packet exceeds configured limit');
          frame.id = this.frames.length;
          if (frame.keyframe) keyedTracks.add(frame.trackId);
          frame.decodable = keyedTracks.has(frame.trackId);
          const track = this.tracks.find((t) => t.id === frame.trackId);
          // Keep preroll for decoding, but never offer it as an image to review.
          frame.presentable =
            track.presentationStart === undefined ||
            (frame.pts >= track.presentationStart - 1e-9 &&
              frame.pts < track.presentationEnd - 1e-9);
          if (frame.presentable && track.presentationEnd !== undefined && frame.duration > 0)
            frame.duration = Math.min(frame.duration, track.presentationEnd - frame.pts);
          frame.telemetry ??= null;
          frame.thermalMetadata ??= null;
          if (this.format !== 'mpegts' && !frame.telemetry) {
            const track = this.tracks.find((t) => t.id === frame.trackId);
            if (track.codec !== 'ffv1') {
              const record = (await readSampleSEI(this, frame, track, signal)).at(-1);
              frame.telemetry = normalizeTelemetry(record, {
                format: 'apcg-sei',
                container: this.format,
              });
            }
          }
          this.frames.push(frame);
          batch.push(frame);
          this.position = Math.max(this.position, frame.offset + frame.size);
          if (batch.length >= Math.max(1, batchSize)) yield report();
        }
        this.complete = true;
        this.position = file.size;
        if (!this.frames.length) throw new Error('No playable video packets found');
        yield report();
      } catch (error) {
        // Preserve the indexed prefix of an interrupted/crashed recording; callers can play it.
        if (
          batch.length &&
          error.name !== 'AbortError' &&
          !signal?.aborted &&
          !options.signal?.aborted &&
          !this.closed
        )
          yield { ...report(), error: error.message };
        throw error;
      } finally {
        this.indexing = false;
        this.reader.extraSignal = this.metadataReader.extraSignal = null;
      }
    },
  };
  const signature = await source.reader.read(0, Math.min(16, file.size));
  if (u32(signature) === 0x1a45dfa3) source.format = 'matroska';
  else if (
    signature.length >= 8 &&
    ['ftyp', 'moov', 'free', 'wide', 'mdat', 'styp'].includes(
      decoder.decode(signature.subarray(4, 8)),
    )
  )
    source.format = 'mp4';
  else source.format = 'mpegts';
  return source;
}
