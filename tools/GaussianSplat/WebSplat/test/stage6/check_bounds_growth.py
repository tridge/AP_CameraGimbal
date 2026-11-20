#!/usr/bin/env python3
"""train.py's refinement rules on a finished run (codex stage 6 round 1, findings 4 and 5):
Gaussians outside the bounds in the final PLY (train.py's outside(): beyond the cylinder, below
the floor, above the canopy ceiling of their cell; distance = how far it must move to be inside,
vertically or into a neighbouring cell whose ceiling clears it; more than TOLERANCE_M fails), and, from an e2e.mjs report, whether the
Gaussian count grows after refine_stop_iter (it may only fall, by bounds pruning).

  check_bounds_growth.py PREPARE_DIR SPLAT.ply [--report report.json]

Exits nonzero if any Gaussian is further out than TOLERANCE_M or the count grows after
refine_stop_iter."""
import argparse
import json
import sys
from pathlib import Path

import numpy as np

# train.py prunes at steps 0, 100, .. 29900 and trains 99 more steps: its own finals have 1-2
# Gaussians a few mm outside (page baselines: 2.7 mm at most). Anything further out was not pruned.
TOLERANCE_M = 0.01


def means(path):
    b = path.read_bytes()
    e = b.index(b'end_header\n') + 11
    h = b[:e].decode('latin1')
    props = [l.split()[2] for l in h.split('\n') if l.startswith('property')]
    n = int(h.split('element vertex ')[1].split('\n')[0])
    a = np.frombuffer(b[e:], np.float32).reshape(n, len(props))
    return a[:, [props.index('x'), props.index('y'), props.index('z')]].astype(np.float64)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('prepare', type=Path)
    ap.add_argument('ply', type=Path)
    ap.add_argument('--report', type=Path)
    a = ap.parse_args()
    d = json.loads((a.prepare / 'dataset.json').read_text())
    b = d['bounds']
    grid = np.fromfile(a.prepare / b['ceiling']['file'], np.float32).astype(np.float64)
    n = b['ceiling']['n']
    m = means(a.ply)
    # the cell index in float32, as train.py (torch) and the Brush fork compute it: at an exact cell
    # edge float64 can pick the neighbouring cell, whose ceiling may differ by metres
    m32 = m.astype(np.float32)
    ij = np.clip(((m32[:, :2] - np.array(b['ceiling']['origin'], np.float32)) / np.float32(b['ceiling']['cell']))
                 .astype(np.int64), 0, n - 1)
    r = np.hypot(m[:, 0] - b['centre'][0], m[:, 1] - b['centre'][1])
    excess = np.maximum.reduce([r - b['radius'], b['z_min'] - m[:, 2], m[:, 2] - grid[ij[:, 0] * n + ij[:, 1]]])
    # above its cell's ceiling: the ceiling steps by metres between cells, so also take the horizontal
    # distance to a neighbouring cell (2 rings) whose ceiling clears the Gaussian, if that is shorter
    o, cell = np.array(b['ceiling']['origin']), b['ceiling']['cell']
    for k in np.nonzero(m[:, 2] > grid[ij[:, 0] * n + ij[:, 1]])[0]:
        for di in range(-2, 3):
            for dj in range(-2, 3):
                i, j = ij[k, 0] + di, ij[k, 1] + dj
                if (di or dj) and 0 <= i < n and 0 <= j < n and grid[i * n + j] >= m[k, 2]:
                    lo = o + cell * np.array([i, j])
                    gap = np.maximum(np.maximum(lo - m[k, :2], m[k, :2] - lo - cell), 0)
                    excess[k] = min(excess[k], float(np.hypot(*gap)))
                    excess[k] = max(excess[k], r[k] - b['radius'], b['z_min'] - m[k, 2])
    out = excess > 0
    far = excess > TOLERANCE_M
    print('%s: %d Gaussians, %d outside the bounds (largest %.4f m), %d by more than %.2f m' % (
        a.ply, len(m), int(out.sum()), float(excess.max()), int(far.sum()), TOLERANCE_M))
    ok = not far.any()
    if a.report:
        stop = d['training']['strategy']['refine_stop_iter']
        rep = json.loads(a.report.read_text())
        s = [(x['done'], x['gaussians']) for x in rep.get('eta', []) if x.get('gaussians') and x['done'] >= stop]
        rises = [(s[k][0], s[k - 1][1], s[k][1]) for k in range(1, len(s)) if s[k][1] > s[k - 1][1]]
        print('after step %d: %d samples, count %s -> %s; %d rises%s' % (
            stop, len(s), s[0][1] if s else '-', s[-1][1] if s else '-', len(rises),
            (': ' + ', '.join('%d: %d -> %d' % x for x in rises[:5])) if rises else ''))
        ok &= not rises and len(s) > 0
    print('PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
