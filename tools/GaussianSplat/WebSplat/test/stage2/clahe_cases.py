#!/usr/bin/env python3
"""CLAHE fixtures: inputs and cv2.createCLAHE(clipLimit=3.0, tileGridSize=(8, 8)).apply()
outputs (as extract_frames.py uses it), for test_clahe.mjs. OUT/NAME_WxH.in.u8, NAME.out.u8."""
from pathlib import Path
import sys

import cv2
import numpy as np


def main():
    out = Path(sys.argv[1])
    raw_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(11)
    cases = []
    # tiny, one dimension divisible by 8, neither, both, very elongated
    for w, h in [(1, 1), (2, 3), (7, 5), (8, 3), (3, 8), (16, 9), (9, 16), (13, 77), (640, 7),
                 (5, 512), (131, 77), (64, 64), (640, 512), (512, 640)]:
        cases.append(('random', rng.integers(0, 256, (h, w), dtype=np.uint8)))
        y, x = np.mgrid[0:h, 0:w]
        cases.append(('smooth', ((x * 3 + y * 2) % 256).astype(np.uint8)))
    cases.append(('flat', np.full((64, 80), 77, np.uint8)))
    cases.append(('two-level', np.where(rng.random((48, 40)) < 0.1, 255, 3).astype(np.uint8)))
    if raw_dir:
        # extract_frames.py's feature input for real frames, also rotated
        for i in (0, 120):
            raw = np.fromfile(raw_dir / ('%05d.u16' % i), '<u2').reshape(512, 640)
            c = raw.astype(np.float32) * 0.015625 + 0 - 273.15
            lo, hi = np.percentile(c, (1, 99))
            v = (np.clip((c - lo) / max(hi - lo, 0.5), 0, 1) * 255).astype(np.uint8)
            cases += [('orbit%d' % i, v), ('orbit%d-rot90' % i, np.ascontiguousarray(np.rot90(v)))]
    clahe = cv2.createCLAHE(clipLimit=3.0, tileGridSize=(8, 8))
    for k, (name, a) in enumerate(cases):
        stem = '%02d-%s_%dx%d' % (k, name, a.shape[1], a.shape[0])
        a.tofile(out / (stem + '.in.u8'))
        clahe.apply(a).tofile(out / (stem + '.out.u8'))
    print('%d CLAHE cases (cv2 %s)' % (len(cases), cv2.__version__))


if __name__ == '__main__':
    main()
