// Splat metadata inside an SPZ (browser and Node): the PLY's `comment apcg_splat {json}` object,
// as UTF-8 JSON in one subfield of the gzip header's FEXTRA field (RFC 1952), subfield ID 'AP'.
// gzip readers skip FEXTRA, so the file stays a plain SPZ for other tools. Layout:
//   1f 8b 08 FLG=04 MTIME=0 XFL=0 OS=ff | XLEN | 'A' 'P' LEN | JSON | deflate | CRC32 ISIZE
// Without metadata the header is the fixed 10 bytes (FLG 0) the exporters always wrote.

export const SPZ_META_ID = [0x41, 0x50]; // 'AP'
const MAX_META = 0xffff - 4; // XLEN holds the subfield's 4 header bytes too

const enc = new TextEncoder();

// the gzip member header; meta: JSON text (or its UTF-8 bytes), or null for none
export function gzipHeader(meta = null) {
  if (meta == null) return new Uint8Array([0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255]);
  const m = typeof meta === 'string' ? enc.encode(meta) : meta;
  if (m.length > MAX_META)
    throw new Error(`SPZ metadata is ${m.length} bytes (at most ${MAX_META})`);
  const h = new Uint8Array(16 + m.length);
  h.set([0x1f, 0x8b, 8, 4, 0, 0, 0, 0, 0, 255]);
  const dv = new DataView(h.buffer);
  dv.setUint16(10, 4 + m.length, true);
  h.set(SPZ_META_ID, 12);
  dv.setUint16(14, m.length, true);
  h.set(m, 16);
  return h;
}

// {body (offset of the deflate data), extra (FEXTRA bytes or null)} of a gzip member, or null if
// the header is not a complete gzip header
export function gzipHeaderInfo(b) {
  if (b.length < 10 || b[0] !== 0x1f || b[1] !== 0x8b || b[2] !== 8 || b[3] & 0xe0) return null;
  const flg = b[3];
  let at = 10;
  let extra = null;
  if (flg & 4) {
    if (b.length < 12) return null;
    const xlen = b[10] | (b[11] << 8);
    if (b.length < 12 + xlen) return null;
    extra = b.subarray(12, 12 + xlen);
    at = 12 + xlen;
  }
  for (const bit of [8, 16]) {
    if (!(flg & bit)) continue;
    while (at < b.length && b[at] !== 0) at++;
    if (at >= b.length) return null;
    at++;
  }
  if (flg & 2) at += 2;
  return at <= b.length ? { body: at, extra } : null;
}

// the metadata subfield's bytes, or null (no FEXTRA, no 'AP' subfield, or an extra field that is
// not a whole sequence of subfields, RFC 1952 2.3.1.1)
export function spzMetaBytes(b) {
  const extra = gzipHeaderInfo(b)?.extra;
  if (!extra) return null;
  let found = null;
  let at = 0;
  while (at < extra.length) {
    if (at + 4 > extra.length) return null;
    const len = extra[at + 2] | (extra[at + 3] << 8);
    if (at + 4 + len > extra.length) return null;
    if (!found && extra[at] === SPZ_META_ID[0] && extra[at + 1] === SPZ_META_ID[1]) {
      found = extra.subarray(at + 4, at + 4 + len);
    }
    at += 4 + len;
  }
  return found;
}

export function spzMetaText(b) {
  const m = spzMetaBytes(b);
  if (!m) return null;
  try {
    return new TextDecoder('utf-8', { fatal: true }).decode(m);
  } catch {
    return null;
  }
}

// the embedded metadata object, or null if absent or not a JSON object
export function spzMeta(b) {
  const t = spzMetaText(b);
  if (t == null) return null;
  try {
    const o = JSON.parse(t);
    return o && typeof o === 'object' && !Array.isArray(o) ? o : null;
  } catch {
    return null;
  }
}

// the same gzip member with a new header (meta as gzipHeader); the deflate data and trailer are
// kept byte for byte
export function withSpzMeta(b, meta) {
  const info = gzipHeaderInfo(b);
  if (!info) throw new Error('not a gzip file');
  const h = gzipHeader(meta);
  const out = new Uint8Array(h.length + b.length - info.body);
  out.set(h);
  out.set(b.subarray(info.body), h.length);
  return out;
}

// metadata JSON text with its top-level rasterize_mode set to mode, the rest of the text as it was
export function setMetaMode(text, mode) {
  const rest = (o) => JSON.stringify({ ...o, rasterize_mode: undefined });
  const orig = JSON.parse(text);
  const ok = (t) => {
    try {
      const o = JSON.parse(t);
      return o.rasterize_mode === mode && rest(o) === rest(orig);
    } catch {
      return false;
    }
  };
  // an existing value (the top-level key, not text inside a string)
  for (const m of text.matchAll(/("rasterize_mode"\s*:\s*)"(?:[^"\\]|\\.)*"/g)) {
    const t = `${text.slice(0, m.index)}${m[1]}${JSON.stringify(mode)}${text.slice(m.index + m[0].length)}`;
    if (ok(t)) return t;
  }
  // otherwise inserted after the opening brace, in the text's own layout
  const m = text.match(/^(\s*)\{(\s*)/);
  if (m && !('rasterize_mode' in orig)) {
    const sep = /^\s*\{\s*\}\s*$/.test(text) ? '' : ',';
    const ws = m[2];
    const field = ws.includes('\n')
      ? `"rasterize_mode": ${JSON.stringify(mode)}${sep}${ws}`
      : `"rasterize_mode":${JSON.stringify(mode)}${sep}`;
    const t = text.slice(0, m[0].length) + field + text.slice(m[0].length);
    if (ok(t)) return t;
  }
  return JSON.stringify({ ...orig, rasterize_mode: mode });
}

// the embedded metadata for an SPZ whose flags byte is now flags: its rasterize_mode set to match
// the antialiased bit (text that is not JSON is kept as it was); null without metadata
export function metaForFlags(text, flags) {
  if (text == null) return null;
  try {
    return setMetaMode(text, flags & 1 ? 'antialiased' : 'classic');
  } catch {
    return text;
  }
}
