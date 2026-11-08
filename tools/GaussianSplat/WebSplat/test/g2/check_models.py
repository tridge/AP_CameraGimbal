#!/usr/bin/env python3
"""Validity and equivalence of the two-view models in a WSTV container B against A (both from
the same raw matches and keypoints).

Every verified pair of B must have a rank-2 F, a valid essential matrix E (two equal singular
values, the third zero, within --e-tol relative to the largest: COLMAP's LO step refits E on
all inliers with the non-minimal five-point solver, so native E deviate up to 0.02) and a
non-singular H, all finite and non-zero.
Pairs whose E differs from A's (normalised Frobenius, scale and sign removed, > --model-tol)
are reported with their consensus on the raw matches (COLMAP 3.13's squared Sampson error on
unit camera rays <= (4 px / f)^2): inlier counts, inlier residual sums (COLMAP's tie-break)
and how many matches change membership. The test only requires the counts to agree within
--support-tol: valid matrices with near-equal consensus, not the same RANSAC optimum (residual
sums and memberships can differ; native E is not reproduced in these pairs).
"""
import argparse
import json
import sys

import numpy as np

from compare_geometry import model_diff, read


def read_wsmt(path):
    u = np.fromfile(path, '<u4')
    assert u[0] == 0x544d5357 and u[1] == 1
    n, total = int(u[2]), int(u[3])
    t = u[4:4 + 5 * n].reshape(n, 5)
    m = u[4 + 5 * n:].reshape(total, 2)
    return {(int(i), int(j)): m[off:off + c] for i, j, _, c, off in t}


def read_keypoints(path):
    raw = open(path, 'rb').read()
    head = np.frombuffer(raw, '<u4', 4)
    n = int(head[2])
    idx = np.frombuffer(raw, '<u4', 2 * n, 16).reshape(n, 2)
    kp = np.frombuffer(raw, '<f4', 6 * int(idx[:, 0].sum()), 16 + 8 * n).reshape(-1, 6)
    return [kp[o:o + c, :2].astype(float) for c, o in idx]


def sampson(r1, r2, E):
    Ex1 = r1 @ E.T
    Etx2 = r2 @ E
    num = np.sum(r2 * Ex1, 1) ** 2
    return num / (Ex1[:, 0] ** 2 + Ex1[:, 1] ** 2 + Etx2[:, 0] ** 2 + Etx2[:, 1] ** 2)


def valid(model, tol, e_tol):
    bad = []
    F, E, H = (model[s].reshape(3, 3) for s in (slice(0, 9), slice(9, 18), slice(18, 27)))
    for name, M in (('F', F), ('E', E), ('H', H)):
        if not np.isfinite(M).all() or not M.any():
            bad.append(name + ' missing')
    if not bad:
        s = np.linalg.svd(F, compute_uv=False)
        if s[2] > tol * s[0]:
            bad.append('F rank')
        s = np.linalg.svd(E, compute_uv=False)
        if abs(s[0] - s[1]) > e_tol * s[0] or s[2] > e_tol * s[0]:
            bad.append('E not essential')
        s = np.linalg.svd(H, compute_uv=False)
        if s[2] < 1e-12 * s[0]:
            bad.append('H singular')
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('a', help='reference WSTV (e.g. native, seed 0)')
    ap.add_argument('b', help='WSTV under test')
    ap.add_argument('features', help='WSFT keypoints')
    ap.add_argument('raw', help='WSMT raw matches the geometries were verified from')
    ap.add_argument('camera', help='camera.json (SIMPLE_RADIAL with k1 = 0 or SIMPLE_PINHOLE)')
    ap.add_argument('--tol', type=float, default=1e-6, help='F rank tolerance')
    ap.add_argument('--e-tol', type=float, default=0.05)
    ap.add_argument('--model-tol', type=float, default=1e-6)
    ap.add_argument('--support-tol', type=int, default=2)
    ap.add_argument('--json', default=None)
    args = ap.parse_args()
    A, B = read(args.a), read(args.b)
    raw = read_wsmt(args.raw)
    kps = read_keypoints(args.features)
    cam = json.load(open(args.camera))
    f, cx, cy = cam['params'][:3]
    assert len(cam['params']) == 3 or not any(cam['params'][3:]), 'distortion not supported here'
    rays = lambda k, ids: (lambda x: x / np.linalg.norm(x, axis=1, keepdims=True))(
        np.c_[(kps[k][ids, 0] - cx) / f, (kps[k][ids, 1] - cy) / f, np.ones(len(ids))])
    thr = (4.0 / f) ** 2
    invalid, differ, unexplained = {}, [], []
    for p, (cfg, inl, model) in B.items():
        if not len(inl):
            continue
        b = valid(model, args.tol, args.e_tol)
        if b:
            invalid[str(p)] = b
        dE = model_diff(A[p][2], model)[1]
        if dE > args.model_tol:
            m = raw[p]
            r1, r2 = rays(p[0], m[:, 0]), rays(p[1], m[:, 1])
            ra = sampson(r1, r2, A[p][2][9:18].reshape(3, 3))
            rb = sampson(r1, r2, model[9:18].reshape(3, 3))
            ia, ib = ra <= thr, rb <= thr
            sa, sb = int(ia.sum()), int(ib.sum())
            # COLMAP's InlierSupportMeasurer: more inliers, then the smaller inlier residual sum
            e = {'pair': list(p), 'config': cfg, 'matches': len(m), 'dE': round(dE, 4), 'support_a': sa,
                 'support_b': sb, 'residual_sum_ratio_b_over_a': round(float(rb[ib].sum() / max(ra[ia].sum(), 1e-300)), 3),
                 'membership_differs': int((ia != ib).sum())}
            differ.append(e)
            if abs(sa - sb) > args.support_tol:
                unexplained.append(e)
    ratios = [e['residual_sum_ratio_b_over_a'] for e in differ]
    res = {'E_differs_count': len(differ),
           'support_equal': sum(e['support_a'] == e['support_b'] for e in differ),
           'support_plus1': sum(e['support_b'] - e['support_a'] == 1 for e in differ),
           'support_minus1': sum(e['support_b'] - e['support_a'] == -1 for e in differ),
           'residual_sum_ratio_max': max(ratios, default=None), 'residual_sum_ratio_min': min(ratios, default=None),
           'membership_differs_max': max((e['membership_differs'] for e in differ), default=0),
           'verified': sum(1 for e in B.values() if len(e[1])), 'invalid': invalid,
           'E_differs': differ, 'E_unexplained': unexplained}
    print(json.dumps(res, indent=1))
    if args.json:
        json.dump(res, open(args.json, 'w'), indent=1)
    ok = not invalid and not unexplained
    print('PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
