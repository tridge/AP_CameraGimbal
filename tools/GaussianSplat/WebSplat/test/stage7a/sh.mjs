// Real SH as gsplat / 3DGS evaluate them: value = sum_j basis_j(dir) * coeff_j + 0.5, with dir the
// unit vector from the camera centre to the Gaussian.
const C0 = 0.28209479177387814;
const C1 = 0.4886025119029199;
const C2 = [
  1.0925484305920792, -1.0925484305920792, 0.31539156525252005, -1.0925484305920792,
  0.5462742152960396,
];
const C3 = [
  -0.5900435899266435, 2.890611442640554, -0.4570457994644658, 0.3731763325901154,
  -0.4570457994644658, 1.445305721320277, -0.5900435899266435,
];

export function shBasis(degree, d) {
  const [x, y, z] = d;
  const b = [C0];
  if (degree < 1) return b;
  b.push(-C1 * y, C1 * z, -C1 * x);
  if (degree < 2) return b;
  const xx = x * x,
    yy = y * y,
    zz = z * z,
    xy = x * y,
    yz = y * z,
    xz = x * z;
  b.push(C2[0] * xy, C2[1] * yz, C2[2] * (2 * zz - xx - yy), C2[3] * xz, C2[4] * (xx - yy));
  if (degree < 3) return b;
  b.push(
    C3[0] * y * (3 * xx - yy),
    C3[1] * xy * z,
    C3[2] * y * (4 * zz - xx - yy),
    C3[3] * z * (2 * zz - 3 * xx - 3 * yy),
    C3[4] * x * (4 * zz - xx - yy),
    C3[5] * z * (xx - yy),
    C3[6] * x * (xx - 3 * yy),
  );
  return b;
}

// channel-mean scaled value of Gaussian i of a splat seen from campos (clamped at 0 as gsplat)
export function viewValue(s, i, campos) {
  const p = [0, 1, 2].map((c) => s.means[3 * i + c] - campos[c]);
  const n = Math.hypot(...p);
  const b = shBasis(
    s.shDegree,
    p.map((x) => x / n),
  );
  const k = (s.shDegree + 1) ** 2 - 1;
  let sum = 0;
  for (let c = 0; c < 3; c++) {
    let v = b[0] * s.sh0[3 * i + c];
    for (let j = 0; j < k; j++) v += b[j + 1] * s.shN[(i * k + j) * 3 + c];
    sum += Math.max(v + 0.5, 0);
  }
  return sum / 3;
}
