#!/usr/bin/env python3
"""Stage 3: compare two WSFT feature containers (A reference, B under test).

--mode exact (like for like, e.g. the fork vs native COLMAP CPU SIFT on the same images):
  every image has the same keypoint count, row k of B corresponds to row k of A (no reordering),
  |x, y| differences <= --px, every affine component (a11 a12 a21 a22: scale and orientation)
  within --affine, descriptor bytes within --desc-tol, and at most --max-desc-rows of all rows
  differ at all. --selftest additionally requires corrupted copies of B (reordered rows, an
  extra keypoint, orientations rotated 90 degrees, descriptors +2) to fail.
--mode stats (e.g. against the reference database's GPU SIFT, which is not deterministic):
  reports counts, scale distributions and how many of A's keypoints B has nearby (--px); no
  pass/fail beyond structural validity.
"""
import argparse
import json
import sys

import numpy as np
from scipy.spatial import cKDTree


def read(path):
    raw = open(path, 'rb').read()
    h = np.frombuffer(raw, '<u4', 4)
    assert h[0] == 0x54465357 and h[1] == 1, path
    n, dim = int(h[2]), int(h[3])
    idx = np.frombuffer(raw, '<u4', 2 * n, 16).reshape(n, 2)
    total = int(idx[:, 0].sum())
    assert len(raw) == 16 + 8 * n + 24 * total + dim * total, path
    kp = np.frombuffer(raw, '<f4', 6 * total, 16 + 8 * n).reshape(total, 6)
    d = np.frombuffer(raw, np.uint8, dim * total, 16 + 8 * n + 24 * total).reshape(total, dim)
    return [(kp[o:o + c], d[o:o + c]) for c, o in idx]


def pct(x):
    x = np.asarray(x, float)
    return {'mean': float(x.mean()), 'p5': float(np.percentile(x, 5)), 'median': float(np.median(x)),
            'p95': float(np.percentile(x, 95)), 'min': float(x.min()), 'max': float(x.max())}


def exact(A, B, args):
    bad, rows, drows, dmax, pmax, amax = [], 0, 0, 0, 0.0, 0.0
    if len(A) != len(B):
        return {'pass': False, 'failures': ['image count %d vs %d' % (len(A), len(B))]}
    for k, ((ka, da), (kb, db)) in enumerate(zip(A, B)):
        if len(ka) != len(kb):
            bad.append('image %d: %d vs %d keypoints' % (k, len(ka), len(kb)))
            continue
        rows += len(ka)
        if not len(ka):
            continue
        # NaN/inf would make every difference comparison false: reject them first
        if not (np.isfinite(ka).all() and np.isfinite(kb).all()):
            bad.append('image %d: non-finite keypoint values' % k)
            continue
        p = float(np.abs(ka[:, :2].astype(float) - kb[:, :2]).max())
        a = float(np.abs(ka[:, 2:].astype(float) - kb[:, 2:]).max())
        dd = np.abs(da.astype(int) - db.astype(int)).max(1)
        pmax, amax, dmax = max(pmax, p), max(amax, a), max(dmax, int(dd.max()))
        drows += int((dd > 0).sum())
        if p > args.px:
            bad.append('image %d: position differs by %.3g px' % (k, p))
        if a > args.affine:
            bad.append('image %d: affine shape differs by %.3g' % (k, a))
        if dd.max() > args.desc_tol:
            bad.append('image %d: descriptor byte differs by %d' % (k, dd.max()))
    frac = drows / max(rows, 1)
    if frac > args.max_desc_rows:
        bad.append('%.2e of descriptor rows differ (limit %.1e)' % (frac, args.max_desc_rows))
    return {'pass': not bad, 'rows': rows, 'max_position_diff_px': pmax, 'max_affine_diff': amax,
            'max_descriptor_diff': dmax, 'descriptor_rows_differing': drows,
            'descriptor_rows_differing_fraction': frac, 'failures': bad[:20], 'num_failures': len(bad)}


def stats(A, B, args):
    same, ca, cb, sa_all, sb_all = [], [], [], [], []
    scale = lambda k: np.sqrt(np.abs(np.linalg.det(k[:, 2:6].reshape(-1, 2, 2).astype(float))))
    for (ka, _), (kb, _) in zip(A, B):
        ca.append(len(ka))
        cb.append(len(kb))
        sa, sb = scale(ka), scale(kb)
        sa_all.append(sa)
        sb_all.append(sb)
        if len(ka) and len(kb):
            dist, j = cKDTree(kb[:, :2]).query(ka[:, :2])
            same.append(float(((dist <= args.px) & (np.abs(sb[j] / sa - 1) <= 0.05)).mean()))
    ca, cb = np.array(ca), np.array(cb)
    return {'images': len(A), 'keypoints': {'a': int(ca.sum()), 'b': int(cb.sum())},
            'count_per_image': {'a': pct(ca), 'b': pct(cb), 'ratio_b_over_a': pct(cb / np.maximum(ca, 1))},
            'scale_px': {'a': pct(np.concatenate(sa_all)), 'b': pct(np.concatenate(sb_all))},
            'near_fraction_of_a (within %g px, scale within 5%%)' % args.px: pct(same)}


def corruptions(B):
    rng = np.random.default_rng(0)
    k0 = next(k for k, (kp, _) in enumerate(B) if len(kp) > 2)
    rot = np.array([[0, -1], [1, 0]], np.float32)  # orientation + 90 degrees

    def change(f):
        C = [(kp.copy(), d.copy()) for kp, d in B]
        f(C)
        return C

    def reorder(C):
        kp, d = C[k0]
        p = rng.permutation(len(kp))
        C[k0] = (kp[p], d[p])

    def extra(C):
        kp, d = C[k0]
        C[k0] = (np.vstack([kp, kp[:1]]), np.vstack([d, d[:1]]))

    def rotate(C):
        for kp, _ in C:
            kp[:, 2:] = (kp[:, 2:].reshape(-1, 2, 2) @ rot.T).reshape(-1, 4)

    def nan(C):
        kp, d = C[k0]
        kp[0, 2] = np.nan  # one affine component

    def plus2(C):
        for i, (kp, d) in enumerate(C):
            C[i] = (kp, np.minimum(d.astype(int) + 2, 255).astype(np.uint8))

    return {name: change(f) for name, f in [('reordered rows', reorder), ('extra keypoint', extra),
                                             ('orientations rotated 90 deg', rotate),
                                             ('descriptors +2', plus2), ('NaN affine component', nan)]}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--mode', choices=['exact', 'stats'], required=True)
    ap.add_argument('--px', type=float, default=None, help='exact: default 1e-3 px; stats: 0.5 px')
    ap.add_argument('--affine', type=float, default=1e-4)
    ap.add_argument('--desc-tol', type=int, default=1)
    ap.add_argument('--max-desc-rows', type=float, default=1e-3)
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--json', default=None)
    args = ap.parse_args()
    args.px = args.px if args.px is not None else (1e-3 if args.mode == 'exact' else 0.5)
    A, B = read(args.a), read(args.b)
    if args.mode == 'stats':
        res = stats(A, B, args)
        ok = True
    else:
        res = exact(A, B, args)
        ok = res['pass']
        if args.selftest:
            res['selftest'] = {}
            for name, C in corruptions(B).items():
                r = exact(A, C, args)
                res['selftest'][name] = 'caught' if not r['pass'] else 'MISSED'
                ok = ok and not r['pass']
    print(json.dumps(res, indent=1))
    if args.json:
        json.dump(res, open(args.json, 'w'), indent=1)
    print('PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
