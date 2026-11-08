// Reader for COLMAP binary sparse models (cameras.bin, images.bin, points3D.bin), enough for
// train.py's scene loading. Poses are images.bin's cam_from_world (COLMAP writes the same values
// to frames.bin for single-camera rigs).

const MODEL_PARAMS = { 0: 3, 1: 4, 2: 4, 3: 5, 4: 8, 5: 8, 6: 12, 7: 5, 8: 4, 9: 5, 10: 12 };
const MODEL_NAMES = {
  0: 'SIMPLE_PINHOLE',
  1: 'PINHOLE',
  2: 'SIMPLE_RADIAL',
  3: 'RADIAL',
  4: 'OPENCV',
  5: 'OPENCV_FISHEYE',
  6: 'FULL_OPENCV',
  7: 'FOV',
  8: 'SIMPLE_RADIAL_FISHEYE',
  9: 'RADIAL_FISHEYE',
  10: 'THIN_PRISM_FISHEYE',
};

const view = (buf) =>
  buf instanceof DataView
    ? buf
    : ArrayBuffer.isView(buf)
      ? new DataView(buf.buffer, buf.byteOffset, buf.byteLength)
      : new DataView(buf);

const u64 = (dv, o) => {
  const v = dv.getBigUint64(o, true);
  if (v > BigInt(Number.MAX_SAFE_INTEGER)) throw new Error('COLMAP model: 64-bit value too large');
  return Number(v);
};

export function readCameras(buf) {
  const dv = view(buf);
  const n = u64(dv, 0);
  let o = 8;
  const cams = [];
  for (let k = 0; k < n; k++) {
    const id = dv.getUint32(o, true);
    const modelId = dv.getInt32(o + 4, true);
    const width = u64(dv, o + 8);
    const height = u64(dv, o + 16);
    o += 24;
    const np = MODEL_PARAMS[modelId];
    if (np == null) throw new Error(`COLMAP model: unknown camera model ${modelId}`);
    const params = [];
    for (let i = 0; i < np; i++, o += 8) params.push(dv.getFloat64(o, true));
    cams.push({ id, model: MODEL_NAMES[modelId], width, height, params });
  }
  return cams;
}

/** Eigen's Quaternion::normalized() */
function normalize(q) {
  const n = Math.sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  return q.map((v) => v / n);
}

/** Eigen's Quaternion::toRotationMatrix(), q = [w, x, y, z] */
export function quatToMatrix([w, x, y, z]) {
  const tx = 2 * x,
    ty = 2 * y,
    tz = 2 * z;
  const twx = tx * w,
    twy = ty * w,
    twz = tz * w;
  const txx = tx * x,
    txy = ty * x,
    txz = tz * x;
  const tyy = ty * y,
    tyz = tz * y,
    tzz = tz * z;
  return [
    [1 - (tyy + tzz), txy - twz, txz + twy],
    [txy + twz, 1 - (txx + tzz), tyz - twx],
    [txz - twy, tyz + twx, 1 - (txx + tyy)],
  ];
}

const cross = (a, b) => [
  a[1] * b[2] - a[2] * b[1],
  a[2] * b[0] - a[0] * b[2],
  a[0] * b[1] - a[1] * b[0],
];

/** COLMAP ProjectionCenter: rotation.inverse() * -translation (Eigen quaternion arithmetic) */
function projectionCentre([w, x, y, z], t) {
  const n2 = w * w + x * x + y * y + z * z;
  const qi = [w / n2, -x / n2, -y / n2, -z / n2];
  const v = [-t[0], -t[1], -t[2]];
  const qv = qi.slice(1);
  const uv = cross(qv, v).map((a) => a + a);
  const c = cross(qv, uv);
  return [v[0] + qi[0] * uv[0] + c[0], v[1] + qi[0] * uv[1] + c[1], v[2] + qi[0] * uv[2] + c[2]];
}

/** images.bin: [{id, name, cameraId, q, t, R (cam_from_world), centre, numPoints2D,
 * numPoints3D, points2D (byte offset of the x, y, point3D_id records)}]; xy(image, idx) reads one. */
export function readImages(buf) {
  const dv = view(buf);
  const n = u64(dv, 0);
  let o = 8;
  const images = [];
  const dec = new TextDecoder();
  const bytes = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);
  for (let k = 0; k < n; k++) {
    const id = dv.getUint32(o, true);
    const q = normalize([0, 1, 2, 3].map((i) => dv.getFloat64(o + 4 + 8 * i, true)));
    const t = [0, 1, 2].map((i) => dv.getFloat64(o + 36 + 8 * i, true));
    const cameraId = dv.getUint32(o + 60, true);
    o += 64;
    const end = bytes.indexOf(0, o);
    const name = dec.decode(bytes.subarray(o, end));
    o = end + 1;
    const np2 = u64(dv, o);
    o += 8;
    const points2D = o;
    let n3 = 0;
    for (let i = 0; i < np2; i++, o += 24) {
      // point3D_id is int64, -1 (all ones) = none
      if (dv.getInt32(o + 20, true) !== -1 || dv.getUint32(o + 16, true) !== 0xffffffff) n3++;
    }
    images.push({
      id,
      name,
      cameraId,
      q,
      t,
      R: quatToMatrix(q),
      centre: projectionCentre(q, t),
      numPoints2D: np2,
      numPoints3D: n3,
      points2D,
    });
  }
  return {
    images,
    xy: (im, idx) => [
      dv.getFloat64(im.points2D + 24 * idx, true),
      dv.getFloat64(im.points2D + 24 * idx + 8, true),
    ],
  };
}

/** points3D.bin in file order: {count, ids, xyz Float64Array [n*3], trackStart Uint32Array [n+1],
 * trackImage, trackPoint2D (Uint32Array)} */
export function readPoints3D(buf) {
  const dv = view(buf);
  const n = u64(dv, 0);
  let o = 8;
  const ids = new Float64Array(n);
  const xyz = new Float64Array(3 * n);
  const trackStart = new Uint32Array(n + 1);
  let total = 0;
  // first pass: track lengths
  const offs = new Float64Array(n);
  for (let k = 0; k < n; k++) {
    offs[k] = o;
    const len = u64(dv, o + 43);
    trackStart[k] = total;
    total += len;
    o += 51 + 8 * len;
  }
  trackStart[n] = total;
  const trackImage = new Uint32Array(total);
  const trackPoint2D = new Uint32Array(total);
  for (let k = 0; k < n; k++) {
    let p = offs[k];
    ids[k] = u64(dv, p);
    for (let i = 0; i < 3; i++) xyz[3 * k + i] = dv.getFloat64(p + 8 + 8 * i, true);
    p += 51;
    for (let e = trackStart[k]; e < trackStart[k + 1]; e++, p += 8) {
      trackImage[e] = dv.getUint32(p, true);
      trackPoint2D[e] = dv.getUint32(p + 4, true);
    }
  }
  return { count: n, ids, xyz, trackStart, trackImage, trackPoint2D };
}
