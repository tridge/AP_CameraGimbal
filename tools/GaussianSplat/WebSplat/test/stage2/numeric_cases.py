#!/usr/bin/env python3
"""Synthetic frames and NumPy's answers for extract_frames.py's statistics (blank test,
medians, percentiles, feature image input), for test_numeric.mjs."""
import hashlib
import json
from pathlib import Path
import sys

import numpy as np

TOOLS = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(TOOLS))
from splat_common import raw_to_celsius  # noqa: E402
from extract_frames import is_blank  # noqa: E402


def main():
    out = Path(sys.argv[1])
    raw_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(7)
    cases = []
    flat = np.full((512, 640), 18000, np.uint16)
    flat[100, 200] = 30000
    cases.append(('flat-hot-pixel', flat, {}))
    for spread in (30, 31, 32, 33, 40):  # (p99 - p1) / 64 around the 0.5 C threshold
        cases.append(('spread-%d' % spread, rng.integers(18000, 18000 + spread + 1, (512, 640)).astype(np.uint16), {}))
    cases.append(('normal-odd', np.clip(rng.normal(19000, 300, (77, 101)), 0, 65535).astype(np.uint16),
                  {'temperature_scale_k': 0.04, 'temperature_offset_k': 3.7}))
    cases.append(('wide', rng.integers(0, 65536, (64, 80)).astype(np.uint16),
                  {'temperature_scale_k': 0.01, 'temperature_offset_k': 0}))
    cases.append(('two-values', np.where(rng.random((33, 35)) < 0.3, 1000, 1400).astype(np.uint16), {}))
    if raw_dir:
        for i in (0, 77, 180):
            cases.append(('orbit-%d' % i, np.fromfile(raw_dir / ('%05d.u16' % i), '<u2').reshape(512, 640), {}))
    expected = []
    for name, raw, meta in cases:
        meta = dict(meta)
        c = raw_to_celsius(raw, meta)
        lo, hi = np.percentile(c, (1, 99))
        v = np.clip((c - lo) / max(hi - lo, 0.5), 0, 1)
        feat = (v * 255).astype(np.uint8)
        raw.astype('<u2').tofile(out / (name + '.u16'))
        expected.append(dict(name=name, height=raw.shape[0], width=raw.shape[1], meta=meta,
                             blank=bool(is_blank(raw, meta)), median_c=float(np.median(c)),
                             p1_c=float(np.percentile(c, 1)), lo=float(lo), hi=float(hi),
                             min_c=float(c.min()), max_c=float(c.max()),
                             feature_sha256=hashlib.sha256(feat.tobytes()).hexdigest()))
    (out / 'expected.json').write_text(json.dumps(expected, indent=1))


if __name__ == '__main__':
    main()
