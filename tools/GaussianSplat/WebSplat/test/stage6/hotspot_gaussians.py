#!/usr/bin/env python3
"""The Gaussians that make one pixel of a held-out view: front-to-back alpha blending of a Brush
(or 3DGS) PLY at that pixel, unclamped, with full SH and DC-only colour, opacity, scale and
whether each Gaussian lies inside the 5b bounds.

  hotspot_gaussians.py PREPARE_DIR SPLAT.ply VIEW ROW COL [--top 15] [--json OUT]

PREPARE_DIR: a 5b output (dataset.json, ceiling.f32). Colour channel 0 (thermal splats are grey).
Uses the EWA 2D covariance with a 0.3 px dilation, as 3DGS; Brush's exported PLY already has its
3D-filter floor baked in."""
import argparse
import json
from pathlib import Path

import numpy as np

C0 = 0.28209479177387814
C1 = 0.4886025119029199
C2 = [1.0925484305920792, -1.0925484305920792, 0.31539156525252005, -1.0925484305920792, 0.5462742152960396]
C3 = [-0.5900435899266435, 2.890611442640554, -0.4570457994644658, 0.3731763325901154, -0.4570457994644658,
      1.445305721320277, -0.5900435899266435]


def sh_basis(d):
    x, y, z = d[:, 0], d[:, 1], d[:, 2]
    xx, yy, zz, xy, yz, xz = x * x, y * y, z * z, x * y, y * z, x * z
    return np.stack([
        -C1 * y, C1 * z, -C1 * x,
        C2[0] * xy, C2[1] * yz, C2[2] * (2 * zz - xx - yy), C2[3] * xz, C2[4] * (xx - yy),
        C3[0] * y * (3 * xx - yy), C3[1] * xy * z, C3[2] * y * (4 * zz - xx - yy),
        C3[3] * z * (2 * zz - 3 * xx - 3 * yy), C3[4] * x * (4 * zz - xx - yy), C3[5] * z * (xx - yy),
        C3[6] * x * (xx - 3 * yy)], 1)


def read_ply(path):
    b = path.read_bytes()
    e = b.index(b'end_header\n') + 11
    h = b[:e].decode('latin1')
    props = [l.split()[2] for l in h.split('\n') if l.startswith('property')]
    n = int(h.split('element vertex ')[1].split('\n')[0])
    a = np.frombuffer(b[e:], np.float32).reshape(n, len(props))
    return {p: a[:, k] for k, p in enumerate(props)}


def quat_rot(q):
    q = q / np.linalg.norm(q, axis=1, keepdims=True)
    w, x, y, z = q.T
    return np.stack([
        1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
        2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
        2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)], 1).reshape(-1, 3, 3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('prepare', type=Path)
    ap.add_argument('ply', type=Path)
    ap.add_argument('view')
    ap.add_argument('row', type=int)
    ap.add_argument('col', type=int)
    ap.add_argument('--top', type=int, default=15)
    ap.add_argument('--json', type=Path)
    a = ap.parse_args()
    d = json.loads((a.prepare / 'dataset.json').read_text())
    im = next(x for x in d['images'] if x['name'].startswith(a.view))
    V = np.array(im['viewmat'], np.float64).reshape(4, 4)
    K = np.array(d['K'], np.float64)
    g = read_ply(a.ply)
    mu = np.stack([g['x'], g['y'], g['z']], 1).astype(np.float64)
    pc = mu @ V[:3, :3].T + V[:3, 3]
    z = pc[:, 2]
    u = K[0, 0] * pc[:, 0] / z + K[0, 2]
    v = K[1, 1] * pc[:, 1] / z + K[1, 2]
    px, py = a.col + 0.5, a.row + 0.5
    s = np.exp(np.stack([g['scale_0'], g['scale_1'], g['scale_2']], 1).astype(np.float64))
    near = (z > 0.01) & (np.abs(u - px) < 3 * K[0, 0] * s.max(1) / np.maximum(z, 1e-3) + 2) \
        & (np.abs(v - py) < 3 * K[1, 1] * s.max(1) / np.maximum(z, 1e-3) + 2)
    idx = np.nonzero(near)[0]
    R = quat_rot(np.stack([g[f'rot_{i}'] for i in range(4)], 1)[idx].astype(np.float64))
    M = R * s[idx][:, None, :]
    cov3 = M @ M.transpose(0, 2, 1)
    W = V[:3, :3]
    zi, xi, yi = z[idx], pc[idx, 0], pc[idx, 1]
    J = np.zeros((len(idx), 2, 3))
    J[:, 0, 0] = K[0, 0] / zi
    J[:, 0, 2] = -K[0, 0] * xi / zi ** 2
    J[:, 1, 1] = K[1, 1] / zi
    J[:, 1, 2] = -K[1, 1] * yi / zi ** 2
    T = J @ W
    cov2 = T @ cov3 @ T.transpose(0, 2, 1) + 0.3 * np.eye(2)
    det = cov2[:, 0, 0] * cov2[:, 1, 1] - cov2[:, 0, 1] ** 2
    inv = np.stack([cov2[:, 1, 1], -cov2[:, 0, 1], cov2[:, 0, 0]], 1) / det[:, None]
    dx, dy = px - u[idx], py - v[idx]
    power = -0.5 * (inv[:, 0] * dx * dx + 2 * inv[:, 1] * dx * dy + inv[:, 2] * dy * dy)
    opac = 1 / (1 + np.exp(-g['opacity'][idx].astype(np.float64)))
    alpha = np.minimum(0.99, opac * np.exp(power))
    keep = (alpha >= 1 / 255) & (power <= 0)
    idx, alpha, zi = idx[keep], alpha[keep], zi[keep]
    order = np.argsort(zi)
    idx, alpha, zi = idx[order], alpha[order], zi[order]
    cam = -V[:3, :3].T @ V[:3, 3]
    dirs = mu[idx] - cam
    dirs /= np.linalg.norm(dirs, axis=1, keepdims=True)
    rest = sorted((p for p in g if p.startswith('f_rest_')), key=lambda p: int(p.split('_')[-1]))
    kk = len(rest) // 3
    sh = np.stack([g[p][idx] for p in rest[:kk]], 1).astype(np.float64)  # channel 0, coefficients 1..
    dc = g['f_dc_0'][idx].astype(np.float64) * C0 + 0.5
    col = dc + (sh * sh_basis(dirs)[:, :kk]).sum(1)
    Tr = np.cumprod(np.concatenate([[1.0], 1 - alpha[:-1]]))
    w = Tr * alpha
    b = d['bounds']
    cl = np.fromfile(a.prepare / d['bounds']['ceiling']['file'], np.float32)
    n = b['ceiling']['n']
    x, y, zz = mu[idx, 0], mu[idx, 1], mu[idx, 2]
    ci = np.clip(((x - b['ceiling']['origin'][0]) / b['ceiling']['cell']).astype(int), 0, n - 1)
    cj = np.clip(((y - b['ceiling']['origin'][1]) / b['ceiling']['cell']).astype(int), 0, n - 1)
    inside = ((x - b['centre'][0]) ** 2 + (y - b['centre'][1]) ** 2 <= b['radius'] ** 2) & (zz >= b['z_min']) \
        & (zz <= cl[ci * n + cj])
    total, total_dc = float((w * np.maximum(col, 0)).sum()), float((w * np.maximum(dc, 0)).sum())
    print('%s pixel (%d, %d): %d Gaussians blend (alpha >= 1/255), transmittance left %.4f' % (
        a.view, a.row, a.col, len(idx), float(Tr[-1] * (1 - alpha[-1])) if len(idx) else 1.0))
    print('value unclamped %.4f (full SH), %.4f (DC only); %d of them outside the 5b bounds' % (
        total, total_dc, int((~inside).sum())))
    top = np.argsort(-w * np.abs(col))[:a.top]
    print('  rank  depth m   weight  opacity  max scale m  colour(SH)  colour(DC)  contrib  inside')
    rows = []
    for r, k in enumerate(top):
        rows.append(dict(gid=int(idx[k]), depth=float(zi[k]), weight=float(w[k]), opacity=float(opac[keep][order][k]),
                         max_scale=float(s[idx[k]].max()), colour=float(col[k]), colour_dc=float(dc[k]),
                         contrib=float(w[k] * max(col[k], 0)), inside=bool(inside[k]), xyz=mu[idx[k]].tolist()))
        print('  %4d %8.2f %8.4f %8.4f %12.3f %11.4f %11.4f %8.4f  %s' % (
            r, zi[k], w[k], rows[-1]['opacity'], rows[-1]['max_scale'], col[k], dc[k], rows[-1]['contrib'], inside[k]))
    if a.json:
        a.json.write_text(json.dumps(dict(view=a.view, pixel=[a.row, a.col], value=total, value_dc=total_dc,
                                          blended=len(idx), outside=int((~inside).sum()), top=rows), indent=1))


if __name__ == '__main__':
    main()
