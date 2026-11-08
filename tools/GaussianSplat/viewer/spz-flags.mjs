// SPZ flags byte (Node): Spark sets the antialiased flag on every SPZ it writes; a splat trained
// with gsplat classic rasterisation clears it. The SPZ is re-gzipped with a fixed header around
// zlib's raw deflate, as WebSplat's export does in the browser, so both give the same bytes.
import { crc32, deflateRawSync, gunzipSync } from 'node:zlib';

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

export function setAntialiased(spz, on) {
  const u = spzDecoded(spz);
  u[14] = on ? u[14] | 1 : u[14] & ~1;
  const body = deflateRawSync(u);
  const out = Buffer.alloc(10 + body.length + 8);
  out.set([0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255]);
  body.copy(out, 10);
  out.writeUInt32LE(crc32(u), 10 + body.length);
  out.writeUInt32LE(u.length >>> 0, 14 + body.length);
  return out;
}
