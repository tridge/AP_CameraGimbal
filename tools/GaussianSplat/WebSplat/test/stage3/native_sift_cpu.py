#!/usr/bin/env python3
"""Native COLMAP CPU SIFT (pycolmap, VLFeat, sfm.py's options on device=cpu: use_gpu alone is overridden by device=auto) on the reference
CLAHE images, exported as WSFT in frames.json order. The reference itself used GPU SIFT
(SiftGPU), which is not deterministic; this is the like-for-like check of the wasm build.
"""
import argparse
import json
from pathlib import Path
import sqlite3
import struct
import sys
import time

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from splat_common import import_pycolmap  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('workdir', type=Path)
    ap.add_argument('out', type=Path, help='WSFT output')
    ap.add_argument('--threads', type=int, default=8)
    args = ap.parse_args()
    pycolmap = import_pycolmap()
    pycolmap.logging.minloglevel = 2
    frames = json.loads((args.workdir / 'frames.json').read_text())['frames']
    names = [f['name'] for f in frames]
    db = args.out.with_suffix('.db')
    db.unlink(missing_ok=True)
    ex = pycolmap.FeatureExtractionOptions()
    ex.sift.max_num_features = 8192
    ex.use_gpu = False
    ex.num_threads = args.threads
    t = time.time()
    pycolmap.extract_features(str(db), str(args.workdir / 'images'), image_names=names,
                              camera_mode=pycolmap.CameraMode.SINGLE, camera_model='SIMPLE_RADIAL',
                              extraction_options=ex, device=pycolmap.Device.cpu)
    secs = time.time() - t
    con = sqlite3.connect(db)
    by = dict((n, i) for i, n in con.execute('select image_id, name from images'))
    kps, ds = [], []
    for n in names:
        r, c, d = con.execute('select rows, cols, data from keypoints where image_id=?', (by[n],)).fetchone()
        kps.append(np.frombuffer(d, '<f4').reshape(r, c))
        r, c, d = con.execute('select rows, cols, data from descriptors where image_id=?', (by[n],)).fetchone()
        ds.append(np.frombuffer(d, np.uint8).reshape(r, 128))
    con.close()
    db.unlink()
    counts = [len(k) for k in kps]
    off = np.concatenate([[0], np.cumsum(counts)[:-1]])
    with open(args.out, 'wb') as f:
        f.write(struct.pack('<4I', 0x54465357, 1, len(names), 128))
        f.write(np.stack([counts, off], 1).astype('<u4').tobytes())
        f.write(np.concatenate(kps).astype('<f4').tobytes())
        f.write(np.concatenate(ds).tobytes())
    print('%d images, %d keypoints, %.1f s (%d threads)' % (len(names), sum(counts), secs, args.threads))


if __name__ == '__main__':
    main()
