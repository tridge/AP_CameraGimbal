#!/usr/bin/env python3
"""Re-run sfm.py's own match_pairs() on CUDA from the exported descriptors, without COLMAP's
verification, to recover the raw matches the database no longer holds (pairs below
min_num_inliers are emptied by verification). Also measures how far the CUDA fp16 GEMM's
similarities are from the exact values, in fp16 ulps, and records the backend settings.

Writes OUT/native_raw.bin (matches container) and OUT/native_raw.json.
"""
import argparse
import json
from pathlib import Path
import sys
import time

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2]))  # tools/GaussianSplat
import sfm  # noqa: E402
from export_native import container  # noqa: E402


class StubDB:
    """the database calls match_pairs() makes"""
    def __init__(self, desc, off):
        self.desc, self.off, self.written = desc, off, {}

    def read_descriptors(self, k):
        return np.asarray(self.desc[self.off[k]:self.off[k + 1]])

    def exists_matches(self, a, b):
        return False

    def write_matches(self, a, b, m):
        self.written[(a, b)] = np.array(m, dtype=np.uint32).reshape(-1, 2)


class StubColmap:
    @staticmethod
    def DatabaseTransaction(db):
        return None


def ulp16(x):
    return 2.0 ** (np.floor(np.log2(np.maximum(np.abs(x), 2 ** -14))) - 10)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('native', type=Path, help='export_native.py output dir')
    ap.add_argument('--out', type=Path, default=None, help='default: the native dir')
    ap.add_argument('--error-pairs', type=int, default=40, help='pairs to measure GEMM error on')
    args = ap.parse_args()
    out = args.out or args.native
    import torch
    assert torch.cuda.is_available(), 'needs CUDA (sfm.py ran the reference on CUDA)'

    index = json.loads((args.native / 'index.json').read_text())
    counts = np.array(index['counts'])
    off = np.concatenate([[0], np.cumsum(counts)])
    desc = np.memmap(args.native / 'descriptors.u8', np.uint8, 'r').reshape(-1, 128)
    u = np.fromfile(args.native / 'matches.bin', '<u4')
    tab = u[4:4 + 5 * int(u[2])].reshape(-1, 5)
    pairs = [(int(i), int(j)) for i, j, *_ in tab]

    db = StubDB(desc, off)
    sfm.import_pycolmap = lambda: StubColmap
    ids = list(range(len(counts)))  # frame index as id
    t0 = time.time()
    sfm.match_pairs(db, ids, pairs, 0.8, 'cuda')
    ms = (time.time() - t0) * 1000
    entries = [(i, j, 0, db.written[(i, j)]) if (i, j) in db.written else (i, j, 1, np.zeros((0, 2), np.uint32))
               for i, j in pairs]
    (out / 'native_raw.bin').write_bytes(container(entries))

    # GEMM error: CUDA fp16 sims vs exact sums of the fp16 products, on a spread of pairs
    def q(k):
        x = desc[off[k]:off[k + 1]].astype(np.float32)
        x /= np.maximum(np.linalg.norm(x, axis=1, keepdims=True), 1e-6)
        return x.astype(np.float16)
    worst, hist = 0.0, np.zeros(4, np.int64)
    for p in np.linspace(0, len(pairs) - 1, args.error_pairs).astype(int):
        i, j = pairs[p]
        a, b = q(i), q(j)
        with torch.no_grad():
            sim = (torch.from_numpy(a).cuda() @ torch.from_numpy(b).cuda().T).float().cpu().numpy()
        exact = a.astype(np.float64) @ b.astype(np.float64).T
        err = np.abs(sim - exact) / ulp16(exact)
        worst = max(worst, float(err.max()))
        hist += np.histogram(err, bins=[0, 0.5, 1.0, 2.0, np.inf])[0]
    info = {
        'torch': torch.__version__, 'cuda': torch.version.cuda, 'device': torch.cuda.get_device_name(0),
        'allow_fp16_reduced_precision_reduction': torch.backends.cuda.matmul.allow_fp16_reduced_precision_reduction,
        'allow_tf32': torch.backends.cuda.matmul.allow_tf32,
        'match_ms': round(ms), 'pairs': len(pairs), 'matches': int(sum(len(e[3]) for e in entries)),
        'gemm_error_pairs': args.error_pairs, 'gemm_error_max_fp16_ulps': worst,
        'gemm_error_hist (<0.5, <1, <2, >=2 ulps)': hist.tolist(),
    }
    (out / 'native_raw.json').write_text(json.dumps(info, indent=1))
    print(json.dumps(info, indent=1))


if __name__ == '__main__':
    main()
