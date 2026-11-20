#!/usr/bin/env python3
"""Per-frame SHA-256 of a native extract_frames.py work dir's pixels, in the WebSplat layouts:
thermal/NNNNN.png as u16 little-endian [H, W] and images/NNNNN.png as u8 [H, W], frames.json order.
usage: ref_hashes.py WORKDIR OUT.json"""
import hashlib
import json
import sys
from pathlib import Path

import cv2
import numpy as np


def main():
    work, out = Path(sys.argv[1]), Path(sys.argv[2])
    info = json.loads((work / 'frames.json').read_text())
    rows = []
    for f in info['frames']:
        t = cv2.imread(str(work / 'thermal' / f['name']), cv2.IMREAD_UNCHANGED)
        c = cv2.imread(str(work / 'images' / f['name']), cv2.IMREAD_UNCHANGED)
        assert t.dtype == np.uint16 and c.dtype == np.uint8, f['name']
        rows.append(dict(name=f['name'], thermal=hashlib.sha256(t.astype('<u2').tobytes()).hexdigest(),
                         features=hashlib.sha256(c.tobytes()).hexdigest()))
    out.write_text(json.dumps(dict(width=info['width'], height=info['height'], frames=rows), indent=1))
    print('%d frames' % len(rows))


if __name__ == '__main__':
    main()
