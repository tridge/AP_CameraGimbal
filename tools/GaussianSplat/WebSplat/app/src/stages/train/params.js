// Training parameters: Brush PLY bytes <-> the WSSP params.bin layout (CONTRACTS.md 6-7):
// header u32[4] 'WSSP' (0x50535357), 1, count, sh_degree; then f32 [count, ...]: means 3,
// scales 3 (log), quats 4 (w, x, y, z), opacities 1 (logit), sh0 3, shN ((d+1)^2 - 1) x 3
// (coefficient-major, channel last). PLY f_rest is channel-major (f_rest_{c*K + k}).
export const WSSP_MAGIC = 0x50535357;

function parseHeader(bytes) {
  const text = new TextDecoder('latin1').decode(bytes.subarray(0, Math.min(bytes.length, 1 << 16)));
  const end = text.indexOf('end_header\n');
  if (!text.startsWith('ply\n') || end < 0) throw new Error('not a PLY file');
  const lines = text.slice(0, end).split('\n');
  if (!lines.includes('format binary_little_endian 1.0'))
    throw new Error('PLY is not binary little-endian');
  let count = null;
  const props = [];
  let inVertex = false;
  for (const l of lines) {
    const w = l.trim().split(/\s+/);
    if (w[0] === 'element') {
      inVertex = w[1] === 'vertex';
      if (inVertex) count = Number(w[2]);
      else if (count !== null) throw new Error('PLY has elements after vertex');
    } else if (w[0] === 'property' && inVertex) {
      if (w[1] !== 'float') throw new Error(`PLY property ${w.at(-1)} is ${w[1]}, not float`);
      props.push(w[2]);
    }
  }
  return { count, props, offset: end + 'end_header\n'.length };
}

export function plyToWssp(bytes) {
  const { count, props, offset } = parseHeader(bytes);
  const stride = props.length;
  if (bytes.length !== offset + count * stride * 4)
    throw new Error('PLY payload does not match its header');
  const data = new Float32Array(bytes.slice(offset).buffer);
  const col = (name) => {
    const k = props.indexOf(name);
    if (k < 0) throw new Error(`PLY has no ${name}`);
    return k;
  };
  const rest = props.filter((p) => p.startsWith('f_rest_')).length;
  const K = rest / 3;
  const degree = Math.round(Math.sqrt(K + 1)) - 1;
  if ((degree + 1) ** 2 - 1 !== K) throw new Error(`PLY has ${rest} f_rest properties`);
  const groups = [
    ['x', 'y', 'z'],
    ['scale_0', 'scale_1', 'scale_2'],
    ['rot_0', 'rot_1', 'rot_2', 'rot_3'],
    ['opacity'],
    ['f_dc_0', 'f_dc_1', 'f_dc_2'],
  ].map((g) => g.map(col));
  // shN [count, K, 3] from f_rest_{c*K + k}
  const shCols = [];
  for (let k = 0; k < K; k++) for (let c = 0; c < 3; c++) shCols.push(col(`f_rest_${c * K + k}`));
  groups.push(shCols);
  const floats = groups.reduce((a, g) => a + g.length, 0);
  const out = new ArrayBuffer(16 + count * floats * 4);
  new Uint32Array(out, 0, 4).set([WSSP_MAGIC, 1, count, degree]);
  let o = 4;
  const f = new Float32Array(out);
  for (const g of groups) {
    for (let i = 0; i < count; i++) {
      const r = i * stride;
      for (let j = 0; j < g.length; j++) f[o + i * g.length + j] = data[r + g[j]];
    }
    o += count * g.length;
  }
  return new Uint8Array(out);
}

export function wsspHeader(bytes) {
  const u = new Uint32Array(bytes.buffer, bytes.byteOffset, 4);
  if (u[0] !== WSSP_MAGIC || u[1] !== 1) throw new Error('not a WSSP v1 file');
  return { count: u[2], degree: u[3] };
}

export function wsspToPly(bytes, comment = 'WebSplat checkpoint parameters') {
  const { count, degree } = wsspHeader(bytes);
  const K = (degree + 1) ** 2 - 1;
  const sizes = [3, 3, 4, 1, 3, 3 * K];
  const floats = sizes.reduce((a, b) => a + b, 0);
  if (bytes.byteLength !== 16 + count * floats * 4)
    throw new Error('WSSP size does not match its header');
  const f = new Float32Array(
    bytes.buffer.slice(bytes.byteOffset + 16, bytes.byteOffset + bytes.byteLength),
  );
  const starts = [];
  let o = 0;
  for (const s of sizes) {
    starts.push(o);
    o += count * s;
  }
  const props = [
    'x',
    'y',
    'z',
    'f_dc_0',
    'f_dc_1',
    'f_dc_2',
    ...Array.from({ length: 3 * K }, (_, i) => `f_rest_${i}`),
    'opacity',
    'scale_0',
    'scale_1',
    'scale_2',
    'rot_0',
    'rot_1',
    'rot_2',
    'rot_3',
  ];
  const head = new TextEncoder().encode(
    `ply\nformat binary_little_endian 1.0\ncomment ${comment}\n` +
      `element vertex ${count}\n` +
      props.map((p) => `property float ${p}\n`).join('') +
      'end_header\n',
  );
  const body = new Float32Array(count * props.length);
  const [m, s, q, op, dc, sh] = starts;
  for (let i = 0; i < count; i++) {
    const r = i * props.length;
    body[r] = f[m + i * 3];
    body[r + 1] = f[m + i * 3 + 1];
    body[r + 2] = f[m + i * 3 + 2];
    for (let c = 0; c < 3; c++) body[r + 3 + c] = f[dc + i * 3 + c];
    for (let c = 0; c < 3; c++)
      for (let k = 0; k < K; k++) body[r + 6 + c * K + k] = f[sh + (i * K + k) * 3 + c];
    const t = r + 6 + 3 * K;
    body[t] = f[op + i];
    for (let c = 0; c < 3; c++) body[t + 1 + c] = f[s + i * 3 + c];
    for (let c = 0; c < 4; c++) body[t + 4 + c] = f[q + i * 4 + c];
  }
  const out = new Uint8Array(head.length + body.byteLength);
  out.set(head);
  out.set(new Uint8Array(body.buffer), head.length);
  return out;
}
