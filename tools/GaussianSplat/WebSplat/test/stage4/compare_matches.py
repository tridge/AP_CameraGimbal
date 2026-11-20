#!/usr/bin/env python3
"""Compare a browser matches container with native sfm.py matching, within float tolerance.

Reference: NATIVE/native_raw.bin, sfm.py's own match_pairs() re-run on CUDA on the reference
GPU model without COLMAP verification (native_rematch.py). The database's matches table is
the post-verification state (pairs below min_num_inliers are emptied), so the raw re-run is
first validated against it: identical on every pair the database kept, fewer than
--min-inliers matches on every pair it emptied.

Every container is validated first (header, contiguous offsets, totals, statuses, unique
pairs, feature indices in range, one-to-one matches within a pair). Each browser difference
must then be a result sfm.py's rule can produce when every similarity takes an fp16 value
within --ulps fp16 ulps of its exact value (float64 sums of the fp16 products are exact): the
rule is evaluated as torch does it, d = float32(sqrt(max(float32(2 - 2 s), 0))), keep
d0 < float32(float32(ratio) * d1), forward argmax, reverse argmax with the first row on ties.
A reported match needs all predicates to pass for some permitted values; a reported
non-match needs one to fail for some permitted values.
"""
import argparse
import json
from pathlib import Path

import numpy as np

MAGIC, STATUS_SKIPPED = 0x544d5357, 1


def read(path):
    u = np.fromfile(path, '<u4')
    if len(u) < 4 or u[0] != MAGIC or u[1] != 1:
        raise SystemExit('%s: not a WSMT v1 container' % path)
    n, total = int(u[2]), int(u[3])
    if len(u) != 4 + 5 * n + 2 * total:
        raise SystemExit('%s: truncated or oversized container' % path)
    return u[4:4 + 5 * n].reshape(n, 5).copy(), u[4 + 5 * n:].reshape(-1, 2).copy()


def validate(tab, mm, counts):
    """errors in a decoded container (no collapsing: duplicates are errors)"""
    err = []
    expect = 0
    seen = set()
    for p, (i, j, st, c, o) in enumerate(tab.tolist()):
        if o != expect:
            err.append('pair %d: offset %d, expected %d' % (p, o, expect))
        expect = o + c
        if (i, j) in seen:
            err.append('pair %d: duplicate pair (%d, %d)' % (p, i, j))
        seen.add((i, j))
        if i >= j or j >= len(counts):
            err.append('pair %d: frames (%d, %d) not i < j < %d' % (p, i, j, len(counts)))
            continue
        if st not in (0, STATUS_SKIPPED) or (st == STATUS_SKIPPED and c):
            err.append('pair %d: status %d with %d matches' % (p, st, c))
        m = mm[o:o + c]
        if len(m) != c:
            err.append('pair %d: matches beyond the data' % p)
            continue
        if c and (m[:, 0].max() >= counts[i] or m[:, 1].max() >= counts[j]):
            err.append('pair %d: feature index out of range' % p)
        if len(np.unique(m[:, 0])) != c or len(np.unique(m[:, 1])) != c:
            err.append('pair %d: matches not one-to-one' % p)
    if expect != len(mm):
        err.append('pairs hold %d matches, data has %d' % (expect, len(mm)))
    return err


def as_dict(tab, mm):
    return {(int(i), int(j)): (int(st), mm[o:o + c]) for i, j, st, c, o in tab}


def ulp16(x):
    return 2.0 ** (np.floor(np.log2(np.maximum(np.abs(x), 2 ** -14))) - 10)


def permitted(v, tol):
    """lowest and highest fp16 values within tol fp16 ulps of exact v (arrays)"""
    v = np.asarray(v, np.float64)
    h = v.astype(np.float16)                 # nearest
    dn = np.nextafter(h, np.float16(-np.inf))
    up = np.nextafter(h, np.float16(np.inf))
    lim = tol * ulp16(v)
    lo = np.where(np.abs(dn.astype(np.float64) - v) <= lim, dn, h)
    hi = np.where(np.abs(up.astype(np.float64) - v) <= lim, up, h)
    lo = np.where(np.abs(h.astype(np.float64) - v) <= lim, np.minimum(lo, h), lo)
    return lo.astype(np.float64), hi.astype(np.float64)


def dist(s):
    """torch: sqrt(clamp(2 - 2 s, min=0)) in float32"""
    s = np.float32(s)
    return np.float32(np.sqrt(np.maximum(np.float32(2) - np.float32(2) * s, np.float32(0))))


def feasible(exact, r, c, present, ratio, tol):
    """can sfm.py's rule report (r, c) as present/absent with every sim an fp16 value within tol ulps?"""
    rq = np.float32(ratio)
    xlo, xhi = permitted(exact[r, c], tol)
    rlo, rhi = permitted(np.delete(exact[r], c), tol)            # other columns of row r
    others = np.delete(np.arange(exact.shape[0]), r)
    clo, chi = permitted(exact[others, c], tol)                  # other rows of column c
    if present:  # best case: x high, competitors low
        s0, s1 = float(xhi), float(rlo.max())
        fwd = s0 > s1           # equal values fail the strict ratio test anyway
        rat = dist(s0) < np.float32(rq * dist(s1))
        top = clo.max()
        rev = s0 > top or (s0 == top and others[clo == top].min() > r)
        return bool(fwd and rat and rev)
    s0, s1 = float(xlo), float(rhi.max())                        # worst case: x low, competitors high
    fwd = s1 >= s0
    rat = not dist(s0) < np.float32(rq * dist(max(s1, s0) if fwd else s1))
    top = chi.max()
    rev = top > s0 or (top == s0 and others[chi == top].min() < r)
    return bool(fwd or rat or rev)


def needed(exact, r, c, present, ratio):
    """smallest tolerance (fp16 ulps, to 0.01) making the result feasible"""
    for t in np.arange(0.5, 8.0, 0.01):
        if feasible(exact, r, c, present, ratio, t):
            return round(float(t), 2)
    return float('inf')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('native', type=Path, help='export_native.py output dir with native_raw.bin')
    ap.add_argument('browser', type=Path, help='browser matches container')
    ap.add_argument('--raw', type=Path, default=None, help='default NATIVE/native_raw.bin')
    ap.add_argument('--ratio', type=float, default=0.8)
    ap.add_argument('--ulps', type=float, default=0.51,
                    help='similarity tolerance (fp16 ulps); CUDA GEMM error measured <= 0.505 on 40 sampled pairs')
    ap.add_argument('--min-inliers', type=int, default=15, help='COLMAP TwoViewGeometryOptions.min_num_inliers')
    ap.add_argument('--selftest', action='store_true', help='check that the checker rejects known-bad inputs')
    args = ap.parse_args()

    index = json.loads((args.native / 'index.json').read_text())
    counts = np.array(index['counts'])
    off = np.concatenate([[0], np.cumsum(counts)])
    desc = np.memmap(args.native / 'descriptors.u8', np.uint8, 'r').reshape(-1, 128)
    raw_path = args.raw or args.native / 'native_raw.bin'

    def q(k):
        x = desc[off[k]:off[k + 1]].astype(np.float32)
        x /= np.maximum(np.linalg.norm(x, axis=1, keepdims=True), 1e-6)
        return x.astype(np.float16)

    def exact_sims(i, j):
        return q(i).astype(np.float64) @ q(j).astype(np.float64).T

    containers = {name: read(p) for name, p in
                  [('database', args.native / 'matches.bin'), ('raw', raw_path), ('browser', args.browser)]}

    if args.selftest:
        raise SystemExit(selftest(containers['browser'], counts, exact_sims, args))

    fails = []
    for name, (tab, mm) in containers.items():
        fails += ['%s: %s' % (name, e) for e in validate(tab, mm, counts)[:5]]
    db, raw, web = (as_dict(*containers[k]) for k in ('database', 'raw', 'browser'))
    S = lambda e: set(map(tuple, e[1].tolist()))

    # 1. the raw re-run is native: equal to the database wherever verification kept the matches
    if set(raw) != set(db):
        fails.append('raw re-run pair set differs from the database')
    kept_diff = sum(len(S(raw[k]) ^ S(db[k])) for k in db if k in raw and len(db[k][1]))
    emptied = [k for k in db if not len(db[k][1])]
    emptied_bad = [k for k in emptied if k in raw and len(raw[k][1]) >= args.min_inliers]
    if kept_diff:
        fails.append('raw re-run differs from the database in %d kept matches' % kept_diff)
    if emptied_bad:
        fails.append('%d emptied pairs have >= %d raw matches' % (len(emptied_bad), args.min_inliers))

    # 2. browser vs raw: same pairs in the same order and statuses, every difference producible
    if [tuple(t[:2]) for t in containers['browser'][0].tolist()] != [tuple(t[:2]) for t in containers['raw'][0].tolist()]:
        fails.append('browser pair list differs from native')
    if any(k in web and web[k][0] != raw[k][0] for k in raw):
        fails.append('browser statuses differ from native')
    diff, unexplained, worst = 0, 0, []
    for key in sorted(raw):
        if key not in web:
            continue
        d = S(raw[key]) ^ S(web[key])
        if not d:
            continue
        exact = exact_sims(*key)
        bset = S(web[key])
        for r, c in sorted(d):
            diff += 1
            present = (r, c) in bset
            unexplained += not feasible(exact, r, c, present, args.ratio, args.ulps)
            worst.append((needed(exact, r, c, present, args.ratio), key[0], key[1], r, c, 'extra' if present else 'missing'))
    worst.sort(reverse=True)
    if unexplained:
        fails.append('%d differences not producible within %g ulps' % (unexplained, args.ulps))

    total = sum(len(v[1]) for v in raw.values())
    rep = {'pairs': len(raw), 'raw_matches': total, 'browser_matches': sum(len(v[1]) for v in web.values()),
           'raw_vs_database_kept_diff': kept_diff, 'emptied_by_verification': len(emptied),
           'emptied_raw_matches': int(sum(len(raw[k][1]) for k in emptied if k in raw)),
           'differing_matches': diff, 'differing_fraction': diff / max(total, 1), 'unexplained': unexplained,
           'tolerance_ulps': args.ulps,
           'largest needed tolerance (ulps, i, j, row, col, browser)': worst[:5]}
    info = raw_path.with_suffix('.json')
    if info.exists():
        rep['native_backend'] = json.loads(info.read_text())
    print(json.dumps(rep))
    for f in fails:
        print('FAIL', f)
    print('PASS matches equal native within %g fp16 ulps' % args.ulps if not fails else 'FAIL')
    raise SystemExit(1 if fails else 0)


def selftest(browser, counts, exact_sims, args):
    tab, mm = browser
    ok = True

    def expect(cond, what):
        nonlocal ok
        print('%s %s' % ('ok  ' if cond else 'FAIL', what))
        ok &= bool(cond)

    expect(not validate(tab, mm, counts), 'the browser container is valid')
    # numeric rule: codex's counterexample, a robust match dropped, a bogus match added
    ex01 = exact_sims(0, 1)
    expect(not feasible(ex01, 295, 5733, True, args.ratio, args.ulps),
           'extra (295, 5733) in pair (0, 1) rejected (strict f32 ratio test fails at the best fp16 values)')
    p = next(p for p in range(len(tab)) if tab[p, 3] >= 100)
    i, j, _, c, o = tab[p]
    ex = exact_sims(i, j)
    m = mm[o:o + c]
    robust = next(t for t in range(len(m)) if needed(ex, m[t, 0], m[t, 1], False, args.ratio) > 4)
    expect(not feasible(ex, m[robust, 0], m[robust, 1], False, args.ratio, args.ulps),
           'dropping a robust match in pair (%d, %d) rejected' % (i, j))
    used = set(m[:, 0].tolist())
    bogus = next(r for r in range(counts[i]) if r not in used)
    expect(not feasible(ex, bogus, m[0, 1], True, args.ratio, args.ulps), 'a bogus extra match rejected')

    # container corruptions, each must be reported
    def corrupt(fn):
        t, x = tab.copy(), mm.copy()
        t, x = fn(t, x)
        return validate(t, x, counts)
    q = p  # a pair with >= 100 matches

    def dup_row(t, x):
        x = x.copy()
        x[t[q, 4] + 1, 0] = x[t[q, 4], 0]
        return t, x

    def dup_col(t, x):
        x = x.copy()
        x[t[q, 4] + 1, 1] = x[t[q, 4], 1]
        return t, x

    def dup_pair(t, x):
        t = t.copy()
        t[q + 1, :2] = t[q, :2]
        return t, x

    def dup_match(t, x):  # the same match twice, replacing its neighbour
        x = x.copy()
        x[t[q, 4] + 1] = x[t[q, 4]]
        return t, x

    def bad_offset(t, x):
        t = t.copy()
        t[q, 4] += 1
        return t, x

    def bad_index(t, x):
        x = x.copy()
        x[t[q, 4], 1] = counts[t[q, 1]]
        return t, x

    for fn in (dup_pair, dup_match, dup_row, dup_col, bad_offset, bad_index):
        expect(corrupt(fn), 'corruption %s reported' % fn.__name__)
    print('PASS selftest: bad inputs rejected' if ok else 'FAIL selftest')
    return 0 if ok else 1


if __name__ == '__main__':
    main()
