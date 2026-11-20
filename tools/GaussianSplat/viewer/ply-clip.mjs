// The viewer's ceiling download: a PLY without the Gaussians above a height. No DOM.

// a binary little-endian PLY without the vertices whose height (dot(up, xyz)) is above ceiling,
// or null if the file is not one this can filter (a single vertex element with x, y, z)
const PLY_SIZES = {
  char: 1,
  uchar: 1,
  int8: 1,
  uint8: 1,
  short: 2,
  ushort: 2,
  int16: 2,
  uint16: 2,
  int: 4,
  uint: 4,
  int32: 4,
  uint32: 4,
  float: 4,
  float32: 4,
  double: 8,
  float64: 8,
};
// raw header lines (byte slices up to and including end_header's), parsed as bytes: TextDecoder's
// 'latin1' is really windows-1252 and would not round-trip
function headerLines(bytes) {
  const lines = [];
  const limit = Math.min(bytes.length, 1 << 20);
  for (let a = 0; a < limit; ) {
    let b = bytes.indexOf(0x0a, a);
    if (b < 0 || b >= limit) return null;
    const raw = bytes.subarray(a, b + 1);
    let text = '';
    for (let i = 0; i < raw.length - 1; i++) text += String.fromCharCode(raw[i]);
    text = text.replace(/\r$/, '');
    lines.push({ raw, text });
    a = b + 1;
    if (text.trim() === 'end_header') return { lines, start: a };
  }
  return null;
}

export function clipPly(bytes, up, ceiling) {
  const h = headerLines(bytes);
  if (!h || !h.lines[0].text.startsWith('ply')) return null;
  const { lines, start } = h;
  if (!lines.some((l) => l.text.trim() === 'format binary_little_endian 1.0')) return null;
  let count = null;
  let vertexLine = -1;
  let stride = 0;
  const offs = {};
  let elements = 0;
  lines.forEach((l, k) => {
    const w = l.text.trim().split(/\s+/);
    if (w[0] === 'element') {
      elements++;
      if (w[1] === 'vertex') vertexLine = k;
      count = w[1] === 'vertex' && elements === 1 ? Number(w[2]) : null;
    } else if (w[0] === 'property') {
      const size = PLY_SIZES[w[1]];
      if (!size)
        stride = NaN; // a list
      else if (['x', 'y', 'z'].includes(w[2]) && size === 4 && w[1].startsWith('float'))
        offs[w[2]] = stride;
      stride += size ?? 0;
    }
  });
  if (elements !== 1 || !Number.isInteger(count) || !Number.isFinite(stride)) return null;
  if (offs.x == null || offs.y == null || offs.z == null) return null;
  if (start + count * stride > bytes.length) return null;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const keep = new Uint8Array(count);
  let kept = 0;
  for (let i = 0, o = start; i < count; i++, o += stride) {
    const z =
      up[0] * view.getFloat32(o + offs.x, true) +
      up[1] * view.getFloat32(o + offs.y, true) +
      up[2] * view.getFloat32(o + offs.z, true);
    if (z <= ceiling) ((keep[i] = 1), kept++);
  }
  // header bytes unchanged but for the vertex count and the apcg_splat metadata's
  const ascii = (t) => Uint8Array.from(t, (c) => c.charCodeAt(0));
  const parts = lines.map((l, k) => {
    if (k === vertexLine) return ascii(`element vertex ${kept}\n`);
    if (/^comment apcg_splat /.test(l.text)) {
      try {
        // the comment's own bytes as UTF-8; written back ASCII-only, as json.dumps does
        const utf8 = new TextDecoder().decode(l.raw).replace(/\r?\n$/, '');
        const info = JSON.parse(utf8.slice('comment apcg_splat '.length));
        if ('gaussians' in info) info.gaussians = kept;
        info.clipped_above = ceiling;
        const json = JSON.stringify(info).replace(
          /[\u007f-￿]/g,
          (c) => `\\u${c.charCodeAt(0).toString(16).padStart(4, '0')}`,
        );
        return ascii(`comment apcg_splat ${json}\n`);
      } catch {
        return l.raw;
      }
    }
    return l.raw;
  });
  const headLen = parts.reduce((n, p) => n + p.length, 0);
  const out = new Uint8Array(headLen + kept * stride);
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  for (let i = 0; i < count; i++) {
    if (!keep[i]) continue;
    out.set(bytes.subarray(start + i * stride, start + (i + 1) * stride), at);
    at += stride;
  }
  return { bytes: out, kept, removed: count - kept };
}
