#!/usr/bin/env python3
"""Run sfm.py's select_pairs() on {"frames": [...], "target_enu": [...]} from stdin and print
the pairs as JSON [[i, j], ...] (for test_pairs.mjs)."""
import json
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))  # tools/GaussianSplat
import sfm  # noqa: E402

info = json.load(sys.stdin)
pairs = sfm.select_pairs(info['frames'], np.array(info['target_enu']), 25.0, 2.2, 30, 5)
print(json.dumps([[int(i), int(j)] for i, j in pairs]))
