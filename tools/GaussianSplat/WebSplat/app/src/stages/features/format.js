// Stage 3 features container 'WSFT' v1 (CONTRACTS.md 3), little-endian:
//   header  u32[4]  MAGIC, VERSION, numImages, descriptorDim
//   index   numImages x u32[2]  [count, offset]
//   keypts  total x f32[6]      x, y, a11, a12, a21, a22
//   descs   total x u8[dim]
export const FEAT_MAGIC = 0x54465357;

export function decodeFeatures(buf) {
  const bytes =
    buf instanceof ArrayBuffer
      ? new Uint8Array(buf)
      : new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
  const aligned = bytes.byteOffset % 4 ? bytes.slice() : bytes;
  const head = new Uint32Array(aligned.buffer, aligned.byteOffset, 4);
  if (head[0] !== FEAT_MAGIC) throw new Error('not a WSFT container');
  if (head[1] !== 1) throw new Error(`WSFT: unsupported version ${head[1]}`);
  const n = head[2],
    dim = head[3];
  const index = new Uint32Array(aligned.buffer, aligned.byteOffset + 16, 2 * n);
  const counts = new Uint32Array(n),
    offsets = new Uint32Array(n);
  let total = 0;
  for (let k = 0; k < n; k++) {
    counts[k] = index[2 * k];
    offsets[k] = index[2 * k + 1];
    if (offsets[k] !== total)
      throw new Error(`WSFT: image ${k} offset ${offsets[k]}, expected ${total}`);
    total += counts[k];
  }
  const kpAt = 16 + 8 * n;
  const want = kpAt + 24 * total + dim * total;
  if (aligned.byteLength !== want)
    throw new Error(`WSFT: ${aligned.byteLength} bytes, header implies ${want}`);
  const keypoints = new Float32Array(aligned.buffer, aligned.byteOffset + kpAt, 6 * total);
  const descriptors = new Uint8Array(
    aligned.buffer,
    aligned.byteOffset + kpAt + 24 * total,
    dim * total,
  );
  return { numImages: n, dim, counts, offsets, total, keypoints, descriptors };
}

export const imageKeypoints = (f, k) =>
  f.keypoints.subarray(6 * f.offsets[k], 6 * (f.offsets[k] + f.counts[k]));

// images: [{keypoints: Float32Array(6 n), descriptors: Uint8Array(128 n)}] in frame order
export function encodeFeatures(images, dim = 128) {
  const n = images.length;
  const total = images.reduce((s, im) => s + im.keypoints.length / 6, 0);
  const kpAt = 16 + 8 * n;
  const out = new Uint8Array(kpAt + 24 * total + dim * total);
  const u = new Uint32Array(out.buffer, 0, 4 + 2 * n);
  u.set([FEAT_MAGIC, 1, n, dim]);
  const kp = new Float32Array(out.buffer, kpAt, 6 * total);
  let off = 0;
  images.forEach((im, k) => {
    const c = im.keypoints.length / 6;
    if (im.descriptors.length !== c * dim) throw new Error(`image ${k}: descriptor size`);
    u[4 + 2 * k] = c;
    u[5 + 2 * k] = off;
    kp.set(im.keypoints, 6 * off);
    out.set(im.descriptors, kpAt + 24 * total + dim * off);
    off += c;
  });
  return out;
}
