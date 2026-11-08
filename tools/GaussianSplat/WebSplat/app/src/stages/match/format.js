// Stage 4 containers (CONTRACTS.md section 4), little-endian.
// Matches 'WSMT' v1, u32:
//   header   [MAGIC, 1, numPairs, numMatches]
//   pairs    numPairs x [i, j, status, count, offset]   offset in matches, contiguous
//   matches  numMatches x [featureIndexInI, featureIndexInJ]
//   status: 0 matched (count may be 0), 1 skipped (an image has < 2 descriptors; count 0)
// Geometry 'WSTV' v1 (two-view verification output):
//   header   u32 [MAGIC, 1, numPairs, numInliers]
//   pairs    u32 numPairs x [i, j, config, count, offset]
//   inliers  u32 numInliers x [featureIndexInI, featureIndexInJ]
//   models   f64 numPairs x [F 9, E 9, H 9, qvec 4 (w,x,y,z), tvec 3]   cam_j_from_cam_i
export const MAGIC = 0x544d5357; // 'WSMT'
export const GEOM_MAGIC = 0x56545357; // 'WSTV'
export const VERSION = 1;
export const STATUS_MATCHED = 0,
  STATUS_SKIPPED = 1;
export const MODEL_F64 = 34;
const MAX_CONFIG = 8; // COLMAP TwoViewGeometry::ConfigurationType

function u32view(buf) {
  if (buf instanceof Uint32Array) return buf;
  const bytes =
    buf instanceof ArrayBuffer
      ? new Uint8Array(buf)
      : new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
  if (bytes.byteLength % 4)
    throw new Error(`container length ${bytes.byteLength} is not a multiple of 4`);
  // copy when unaligned (e.g. a Node Buffer slice)
  return bytes.byteOffset % 4
    ? new Uint32Array(bytes.slice().buffer)
    : new Uint32Array(bytes.buffer, bytes.byteOffset, bytes.byteLength / 4);
}

function encode(magic, results, key, models) {
  const total = results.reduce((s, r) => s + r.matches.length / 2, 0);
  const words = 4 + 5 * results.length + 2 * total;
  const out = new Uint32Array(words + (models ? 2 * MODEL_F64 * results.length : 0));
  out.set([magic, VERSION, results.length, total]);
  let off = 0;
  const m = 4 + 5 * results.length;
  results.forEach((r, p) => {
    if (r.matches.length % 2) throw new Error('odd match array length');
    const count = r.matches.length / 2;
    out.set([r.i, r.j, r[key], count, off], 4 + 5 * p);
    out.set(r.matches, m + 2 * off);
    off += count;
  });
  if (models) {
    const f = new Float64Array(MODEL_F64 * results.length);
    results.forEach((r, p) => r.model && f.set(r.model, MODEL_F64 * p));
    out.set(new Uint32Array(f.buffer), words);
  }
  return out;
}

// opts.counts (descriptors per frame) also checks frame and feature indices
function decode(buf, magic, key, models, opts = {}) {
  const u = u32view(buf);
  const name = magic === MAGIC ? 'WSMT' : 'WSTV';
  if (u.length < 4) throw new Error(`${name}: truncated header`);
  if (u[0] !== magic) throw new Error(`not a ${name} container`);
  if (u[1] !== VERSION) throw new Error(`${name}: unsupported version ${u[1]}`);
  const n = u[2],
    total = u[3];
  const words = 4 + 5 * n + 2 * total;
  const want = words + (models ? 2 * MODEL_F64 * n : 0);
  if (u.length !== want) throw new Error(`${name}: ${u.length} words, header implies ${want}`);
  const counts = opts.counts;
  const m = 4 + 5 * n,
    out = [];
  const f = models ? new Float64Array(u.slice(words).buffer) : null;
  let expect = 0;
  for (let p = 0; p < n; p++) {
    const [i, j, st, count, off] = u.subarray(4 + 5 * p, 9 + 5 * p);
    if (off !== expect) throw new Error(`${name}: pair ${p} offset ${off}, expected ${expect}`);
    expect += count;
    if (expect > total) throw new Error(`${name}: pair ${p} overruns ${total} matches`);
    if (i === j) throw new Error(`${name}: pair ${p} joins frame ${i} to itself`);
    if (magic === MAGIC) {
      if (st !== STATUS_MATCHED && st !== STATUS_SKIPPED)
        throw new Error(`${name}: pair ${p} status ${st}`);
      if (st === STATUS_SKIPPED && count)
        throw new Error(`${name}: skipped pair ${p} has ${count} matches`);
    } else if (st > MAX_CONFIG) throw new Error(`${name}: pair ${p} config ${st}`);
    const mm = u.subarray(m + 2 * off, m + 2 * (off + count));
    if (counts) {
      if (i >= counts.length || j >= counts.length)
        throw new Error(`${name}: pair ${p} frame out of range`);
      for (let k = 0; k < mm.length; k += 2) {
        if (mm[k] >= counts[i] || mm[k + 1] >= counts[j])
          throw new Error(`${name}: pair ${p} feature index out of range`);
      }
    }
    const e = { i, j, [key]: st, matches: mm };
    if (models) e.model = f.subarray(MODEL_F64 * p, MODEL_F64 * (p + 1));
    out.push(e);
  }
  if (expect !== total)
    throw new Error(`${name}: pairs hold ${expect} matches, header says ${total}`);
  return out;
}

// results: [{i, j, status, matches: Uint32Array(2 * count)}]
export const encodeMatches = (results) => encode(MAGIC, results, 'status', false);
export const decodeMatches = (buf, opts) => decode(buf, MAGIC, 'status', false, opts);
// results: [{i, j, config, matches (inliers), model: Float64Array(34)}]
export const encodeGeometry = (results) => encode(GEOM_MAGIC, results, 'config', true);
export const decodeGeometry = (buf, opts) => decode(buf, GEOM_MAGIC, 'config', true, opts);
