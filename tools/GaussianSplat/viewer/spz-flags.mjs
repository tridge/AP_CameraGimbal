// SPZ flags byte (Node): Spark sets the antialiased flag on every SPZ it writes; a splat trained
// with gsplat classic rasterisation clears it. The SPZ is re-gzipped with a fixed header (and the
// embedded metadata, if any: spz-meta.mjs) around zlib's raw deflate, as WebSplat's export does
// in the browser, so both give the same bytes.
import { crc32, deflateRawSync, gunzipSync } from 'node:zlib';
import { gzipHeader, metaForFlags, spzMetaText } from './spz-meta.mjs';

// the decompressed SPZ, checked: gzip, magic, version 2 or 3, SH degree <= 3, header length and
// the decoded length its count implies; throws otherwise
export function spzDecoded(spz) {
  let u;
  try {
    u = gunzipSync(spz);
  } catch (e) {
    throw new Error(`not a gzip file (${e.message})`);
  }
  if (u.length < 16) throw new Error(`SPZ header is ${u.length} bytes, not 16`);
  if (u.readUInt32LE(0) !== 0x5053474e) throw new Error('not an SPZ file (magic)');
  const version = u.readUInt32LE(4);
  if (version !== 2 && version !== 3) throw new Error(`SPZ version ${version} is not supported`);
  const count = u.readUInt32LE(8);
  const degree = u[12];
  if (degree > 3) throw new Error(`SPZ SH degree ${degree}`);
  const k = (degree + 1) ** 2 - 1;
  const size = 16 + count * (9 + 1 + 3 + 3 + (version === 2 ? 3 : 4) + 3 * k);
  if (u.length !== size) throw new Error(`SPZ is ${u.length} bytes decoded, ${size} expected`);
  return u;
}

export function spzFlags(spz) {
  return spzDecoded(spz)[14];
}

// the gzip file of a decompressed SPZ; meta as gzipHeader
export function gzipSpz(u, meta = null) {
  const head = gzipHeader(meta);
  const body = deflateRawSync(u);
  const out = Buffer.alloc(head.length + body.length + 8);
  out.set(head);
  body.copy(out, head.length);
  out.writeUInt32LE(crc32(u), head.length + body.length);
  out.writeUInt32LE(u.length >>> 0, head.length + 4 + body.length);
  return out;
}

// embedded metadata is kept, its rasterize_mode set to match the flag
export function setAntialiased(spz, on) {
  const u = spzDecoded(spz);
  u[14] = on ? u[14] | 1 : u[14] & ~1;
  return gzipSpz(u, metaForFlags(spzMetaText(spz), u[14]));
}
