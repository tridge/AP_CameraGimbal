#!/usr/bin/env python3
"""Compare two WSTV verified-geometry containers (CONTRACTS.md 4) pair by pair.

Prints verified pairs and inliers of each, the pairs verified by only one, configuration
agreement, and inlier-set agreement (identical pairs, Jaccard). --identical fails unless the
pair tables and inliers are identical; with --model-tol, F and H must also agree to that
normalised (scale- and sign-free) Frobenius difference. E is reported only: check_models.py
judges E differences.
--min-ratio R fails if B has fewer than R times A's verified pairs or inliers.
"""
import argparse
import json
import sys

import numpy as np


def read(path):
    u = np.fromfile(path, '<u4')
    assert u[0] == 0x56545357 and u[1] == 1, path
    n, total = int(u[2]), int(u[3])
    words = 4 + 5 * n + 2 * total
    assert len(u) == words + 68 * n, path
    table = u[4:4 + 5 * n].reshape(n, 5)
    m = u[4 + 5 * n:words].reshape(total, 2)
    models = np.frombuffer(u[words:].tobytes(), '<f8').reshape(n, 34)
    return {(int(i), int(j)): (int(c), m[off:off + cnt], models[k]) for k, (i, j, c, cnt, off) in enumerate(table)}


def model_diff(a, b):
    """per matrix (F, E, H): Frobenius difference after normalising scale and sign (2 if only one
    is zero, 0 if both are)"""
    out = []
    for s in (slice(0, 9), slice(9, 18), slice(18, 27)):
        x, y = a[s], b[s]
        nx, ny = np.linalg.norm(x), np.linalg.norm(y)
        if nx == 0 or ny == 0:
            out.append(0.0 if nx == ny else 2.0)
            continue
        x, y = x / nx, y / ny
        out.append(float(min(np.linalg.norm(x - y), np.linalg.norm(x + y))))
    return out


def summary(g):
    v = [p for p, e in g.items() if len(e[1])]
    cfg = {}
    for p in v:
        cfg[g[p][0]] = cfg.get(g[p][0], 0) + 1
    return len(v), int(sum(len(g[p][1]) for p in v)), cfg


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--identical', action='store_true')
    ap.add_argument('--model-tol', type=float, default=None)
    ap.add_argument('--allow', type=int, default=0,
                    help='with --identical: up to this many verified pairs may differ in a few inliers '
                         '(floating point at RANSAC thresholds), keeping config and inlier Jaccard >= 0.9')
    ap.add_argument('--min-ratio', type=float, default=None)
    ap.add_argument('--json', default=None, help='write the numbers here')
    ap.add_argument('--subset', action='store_true', help="compare only B's pairs (a sampled run)")
    args = ap.parse_args()
    A, B = read(args.a), read(args.b)
    if args.subset:
        A = {p: A[p] for p in B}
    sa, sb = summary(A), summary(B)
    res = {'a': {'pairs': len(A), 'verified': sa[0], 'inliers': sa[1], 'configs': sa[2]},
           'b': {'pairs': len(B), 'verified': sb[0], 'inliers': sb[1], 'configs': sb[2]}}
    same_pairs = list(A) == list(B)
    va = {p for p, e in A.items() if len(e[1])}
    vb = {p for p, e in B.items() if len(e[1])}
    both = sorted(va & vb)
    same_cfg = sum(A[p][0] == B[p][0] for p in both)
    ident = 0
    jac = []
    model_err = [0.0, 0.0, 0.0]
    model_bad = {'F': [], 'E': [], 'H': []}
    for p in both:
        ma, mb = A[p][1], B[p][1]
        if len(ma) == len(mb) and (ma == mb).all():
            ident += 1
        sa_ = {tuple(x) for x in ma.tolist()}
        sb_ = {tuple(x) for x in mb.tolist()}
        jac.append(len(sa_ & sb_) / len(sa_ | sb_))
        for k, d in enumerate(model_diff(A[p][2], B[p][2])):
            model_err[k] = max(model_err[k], d)
            if args.model_tol is not None and d > args.model_tol:
                model_bad['FEH'[k]].append(list(p))
    jac = np.array(jac) if jac else np.zeros(1)
    res.update({'jaccard_min': float(jac.min()), 'same_pair_table': same_pairs, 'verified_both': len(both), 'only_a': len(va - vb),
                'only_b': len(vb - va), 'same_config': same_cfg, 'identical_inliers': ident,
                'jaccard_mean': float(jac.mean()), 'jaccard_median': float(np.median(jac)),
                'jaccard_p5': float(np.percentile(jac, 5)), 'max_model_diff_FEH': model_err,
                'model_mismatch_pairs': model_bad,
                'inlier_ratio_b_over_a': sb[1] / max(sa[1], 1), 'pair_ratio_b_over_a': sb[0] / max(sa[0], 1)})
    only_a = sorted(va - vb)
    only_b = sorted(vb - va)
    res['only_a_inliers'] = [len(A[p][1]) for p in only_a]
    res['only_b_inliers'] = [len(B[p][1]) for p in only_b]
    print(json.dumps(res, indent=1))
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(res, f, indent=1)
    ok = True
    if args.identical:
        ok = same_pairs and not only_a and not only_b and len(both) - ident <= args.allow \
            and same_cfg == len(both) and all(A[p][0] == B[p][0] for p in A) and jac.min() >= 0.9
        if args.model_tol is not None:
            # F and H must agree; E differences are judged by check_models.py (validity, support)
            ok = ok and not model_bad['F'] and not model_bad['H']
    if args.min_ratio is not None:
        ok = ok and res['inlier_ratio_b_over_a'] >= args.min_ratio and res['pair_ratio_b_over_a'] >= args.min_ratio
    print('PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
