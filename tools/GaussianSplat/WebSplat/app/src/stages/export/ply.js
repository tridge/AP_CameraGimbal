// Splat parameters and 3DGS PLY files.
//
// A splat is {count, shDegree, means, scales, quats, opacities, sh0, shN}: Float32Arrays, each
// [count, ...] row-major as the WSSP checkpoint (CONTRACTS.md 6-7): scales log, quats w,x,y,z
// (not necessarily normalised), opacities logit, sh0 [3], shN [(d+1)^2-1][3] coefficient-major,
// channel last. PLY files are written exactly as train.py save_ply / export_web.py write_ply
// (plyfile, binary little-endian, f_rest channel-major, quaternion normalised).

export const SH_C0 = 0.28209479177387814;
const WSSP_MAGIC = 0x50535357;

export const shCoeffs = (degree) => (degree + 1) ** 2 - 1;

export function emptySplat(count, shDegree) {
  const k = shCoeffs(shDegree);
  return {
    count,
    shDegree,
    means: new Float32Array(count * 3),
    scales: new Float32Array(count * 3),
    quats: new Float32Array(count * 4),
    opacities: new Float32Array(count),
    sh0: new Float32Array(count * 3),
    shN: new Float32Array(count * k * 3),
  };
}

const FIELDS = [
  ['means', 3],
  ['scales', 3],
  ['quats', 4],
  ['opacities', 1],
  ['sh0', 3],
];

// websplat.splat params.bin: u32 'WSSP', 1, count, sh_degree, then the arrays in FIELDS order + shN
export function readParams(buffer) {
  const bytes = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer);
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (dv.getUint32(0, true) !== WSSP_MAGIC) throw new Error('params.bin: not a WSSP file');
  if (dv.getUint32(4, true) !== 1) throw new Error(`params.bin: version ${dv.getUint32(4, true)}`);
  const count = dv.getUint32(8, true);
  const s = emptySplat(count, dv.getUint32(12, true));
  let at = 16;
  for (const name of [...FIELDS.map((f) => f[0]), 'shN']) {
    const n = s[name].length;
    if (at + 4 * n > bytes.length) throw new Error('params.bin: truncated');
    s[name].set(new Float32Array(bytes.slice(at, at + 4 * n).buffer));
    at += 4 * n;
  }
  if (at !== bytes.length) throw new Error('params.bin: trailing bytes');
  return s;
}

export function writeParams(s) {
  const names = [...FIELDS.map((f) => f[0]), 'shN'];
  const total = 16 + names.reduce((a, n) => a + 4 * s[n].length, 0);
  const out = new Uint8Array(total);
  new Uint32Array(out.buffer, 0, 4).set([WSSP_MAGIC, 1, s.count, s.shDegree]);
  let at = 16;
  for (const n of names) {
    out.set(new Uint8Array(s[n].buffer, s[n].byteOffset, s[n].byteLength), at);
    at += 4 * s[n].length;
  }
  return out;
}

// numpy float32: q / np.linalg.norm(q, axis=1, keepdims=True), summed in order
export function normaliseQuats(q) {
  const f = Math.fround;
  const out = new Float32Array(q.length);
  for (let i = 0; i < q.length; i += 4) {
    let ss = 0;
    for (let c = 0; c < 4; c++) ss = f(ss + f(q[i + c] * q[i + c]));
    const n = f(Math.sqrt(ss));
    for (let c = 0; c < 4; c++) out[i + c] = f(q[i + c] / n);
  }
  return out;
}

// thermal export_web.py: sh.mean(2) (torch float32), every channel set to the channel mean
export function greyscale(s) {
  const f = Math.fround;
  const mean3 = (a) => {
    const out = new Float32Array(a.length);
    for (let i = 0; i < a.length; i += 3) {
      const m = f(f(f(a[i] + a[i + 1]) + a[i + 2]) / 3);
      out[i] = out[i + 1] = out[i + 2] = m;
    }
    return out;
  };
  return { ...s, sh0: mean3(s.sh0), shN: mean3(s.shN) };
}

export function plyPropertyNames(shDegree) {
  const rest = 3 * shCoeffs(shDegree);
  return [
    'x',
    'y',
    'z',
    'nx',
    'ny',
    'nz',
    'f_dc_0',
    'f_dc_1',
    'f_dc_2',
    ...Array.from({ length: rest }, (_, i) => `f_rest_${i}`),
    'opacity',
    'scale_0',
    'scale_1',
    'scale_2',
    'rot_0',
    'rot_1',
    'rot_2',
    'rot_3',
  ];
}

// header lines as plyfile writes them; comments without the 'comment ' prefix
export function plyHeader(count, names, comments = []) {
  const lines = ['ply', 'format binary_little_endian 1.0'];
  for (const c of comments) {
    if (/[\r\n]/.test(c)) throw new Error('PLY comment with a line break');
    lines.push(`comment ${c}`);
  }
  lines.push(`element vertex ${count}`, ...names.map((n) => `property float ${n}`), 'end_header');
  return new TextEncoder().encode(lines.join('\n') + '\n');
}

// train.py save_ply layout; quats normalised here unless already done (opts.normalised)
export function writePly(s, { comments = [], normalised = false } = {}) {
  const names = plyPropertyNames(s.shDegree);
  const head = plyHeader(s.count, names, comments);
  for (const c of comments) {
    // plyfile reads the header as ASCII
    if (/[^\x20-\x7e]/.test(c)) throw new Error('PLY comment must be printable ASCII');
  }
  const stride = names.length;
  const k = shCoeffs(s.shDegree);
  const q = normalised ? s.quats : normaliseQuats(s.quats);
  const body = new Float32Array(stride * s.count);
  for (let i = 0; i < s.count; i++) {
    const r = body.subarray(i * stride, (i + 1) * stride);
    r[0] = s.means[3 * i];
    r[1] = s.means[3 * i + 1];
    r[2] = s.means[3 * i + 2];
    r[6] = s.sh0[3 * i];
    r[7] = s.sh0[3 * i + 1];
    r[8] = s.sh0[3 * i + 2];
    // f_rest channel-major: f_rest_{c*k + j} = shN[i][j][c]
    for (let c = 0; c < 3; c++) {
      for (let j = 0; j < k; j++) r[9 + c * k + j] = s.shN[(i * k + j) * 3 + c];
    }
    const o = 9 + 3 * k;
    r[o] = s.opacities[i];
    r[o + 1] = s.scales[3 * i];
    r[o + 2] = s.scales[3 * i + 1];
    r[o + 3] = s.scales[3 * i + 2];
    for (let c = 0; c < 4; c++) r[o + 4 + c] = q[4 * i + c];
  }
  const out = new Uint8Array(head.length + body.byteLength);
  out.set(head);
  out.set(new Uint8Array(body.buffer), head.length);
  return out;
}

const TYPES = {
  char: ['getInt8', 1],
  int8: ['getInt8', 1],
  uchar: ['getUint8', 1],
  uint8: ['getUint8', 1],
  short: ['getInt16', 2],
  int16: ['getInt16', 2],
  ushort: ['getUint16', 2],
  uint16: ['getUint16', 2],
  int: ['getInt32', 4],
  int32: ['getInt32', 4],
  uint: ['getUint32', 4],
  uint32: ['getUint32', 4],
  float: ['getFloat32', 4],
  float32: ['getFloat32', 4],
  double: ['getFloat64', 8],
  float64: ['getFloat64', 8],
};

// any binary little-endian PLY whose first element is the vertex list (train.py, Brush, ...)
export function readPly(input) {
  const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
  const probe = new TextDecoder('latin1').decode(
    bytes.subarray(0, Math.min(bytes.length, 1 << 20)),
  );
  const m = probe.match(/\nend_header\r?\n/); // the terminator line, not text in a comment
  if (!probe.startsWith('ply') || !m) throw new Error('not a PLY file');
  const lines = probe.slice(0, m.index).split(/\r?\n/);
  const comments = [];
  const elements = [];
  for (const line of lines.slice(1)) {
    const w = line.trim().split(/\s+/);
    if (w[0] === 'format' && w[1] !== 'binary_little_endian') {
      throw new Error(`PLY format ${w[1]} is not supported`);
    } else if (w[0] === 'comment') comments.push(line.slice(line.indexOf('comment') + 8));
    else if (w[0] === 'element') elements.push({ name: w[1], count: +w[2], props: [] });
    else if (w[0] === 'property') {
      if (w[1] === 'list') throw new Error('PLY list properties are not supported');
      if (!TYPES[w[1]]) throw new Error(`PLY property type ${w[1]}`);
      elements.at(-1).props.push({ name: w[2], type: w[1] });
    }
  }
  const vertex = elements[0];
  if (!vertex || vertex.name !== 'vertex') throw new Error('PLY has no leading vertex element');
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const stride = vertex.props.reduce((a, p) => a + TYPES[p.type][1], 0);
  const start = m.index + m[0].length;
  if (start + stride * vertex.count > bytes.length) throw new Error('PLY is truncated');
  const props = {};
  let off = 0;
  for (const p of vertex.props) {
    const [get, size] = TYPES[p.type];
    const a = new Float32Array(vertex.count);
    for (let i = 0, at = start + off; i < vertex.count; i++, at += stride) a[i] = dv[get](at, true);
    props[p.name] = a;
    off += size;
  }
  return { count: vertex.count, comments, props, names: vertex.props.map((p) => p.name) };
}

export function apcgMeta(comments) {
  for (const c of comments) {
    if (c.startsWith('apcg_splat ')) return JSON.parse(c.slice('apcg_splat '.length));
  }
  return null;
}

// parameters of a 3DGS PLY (any property order; normals ignored)
export function splatFromPly(ply) {
  const p = ply.props;
  const need = ['x', 'y', 'z', 'f_dc_0', 'f_dc_1', 'f_dc_2', 'opacity'];
  for (let c = 0; c < 4; c++) need.push(`rot_${c}`);
  for (let c = 0; c < 3; c++) need.push(`scale_${c}`);
  for (const n of need) if (!p[n]) throw new Error(`PLY has no ${n}`);
  let rest = 0;
  while (p[`f_rest_${rest}`]) rest++;
  const k = rest / 3;
  const degree = Math.round(Math.sqrt(k + 1)) - 1;
  if (rest % 3 || shCoeffs(degree) !== k) throw new Error(`PLY has ${rest} f_rest values`);
  const s = emptySplat(ply.count, degree);
  for (let i = 0; i < ply.count; i++) {
    for (let c = 0; c < 3; c++) {
      s.means[3 * i + c] = p['xyz'[c]][i];
      s.scales[3 * i + c] = p[`scale_${c}`][i];
      s.sh0[3 * i + c] = p[`f_dc_${c}`][i];
      for (let j = 0; j < k; j++) s.shN[(i * k + j) * 3 + c] = p[`f_rest_${c * k + j}`][i];
    }
    for (let c = 0; c < 4; c++) s.quats[4 * i + c] = p[`rot_${c}`][i];
    s.opacities[i] = p.opacity[i];
  }
  return s;
}
