#!/usr/bin/env python3
"""Export what G2 needs from the native reference database, in the stage formats.

Writes OUT/:
  features.bin  WSFT v1 (CONTRACTS.md 3): keypoints (x, y, a11, a12, a21, a22) and descriptors
                of every frame, frames.json order
  camera.json   the database camera (model, size, params as sfm.py stored them, prior focal)
The raw matches (native_raw.bin) and verified geometry (geometry.bin) are stage 4's exports.
"""
import argparse
import json
from pathlib import Path
import sqlite3
import struct

import numpy as np

MODELS = {0: 'SIMPLE_PINHOLE', 1: 'PINHOLE', 2: 'SIMPLE_RADIAL', 3: 'RADIAL'}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('workdir', type=Path, help='native work dir (frames.json, colmap/database.db)')
    ap.add_argument('out', type=Path)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    frames = json.loads((args.workdir / 'frames.json').read_text())['frames']
    con = sqlite3.connect('file:%s?mode=ro' % (args.workdir / 'colmap' / 'database.db'), uri=True)
    by_name = dict((n, i) for i, n in con.execute('select image_id, name from images'))
    ids = [by_name[f['name']] for f in frames]

    kps, descs = [], []
    for iid in ids:
        rows, cols, data = con.execute('select rows, cols, data from keypoints where image_id=?', (iid,)).fetchone()
        k = np.frombuffer(data, '<f4').reshape(rows, cols)
        assert cols == 6, cols
        rows2, cols2, d = con.execute('select rows, cols, data from descriptors where image_id=?', (iid,)).fetchone()
        assert rows2 == rows and cols2 == 128
        kps.append(k)
        descs.append(np.frombuffer(d, np.uint8).reshape(rows, 128))
    counts = [len(k) for k in kps]
    offsets = np.concatenate([[0], np.cumsum(counts)[:-1]]).astype(np.uint32)
    with open(args.out / 'features.bin', 'wb') as f:
        f.write(struct.pack('<4I', 0x54465357, 1, len(ids), 128))
        f.write(np.stack([np.array(counts, np.uint32), offsets], 1).astype('<u4').tobytes())
        f.write(np.concatenate(kps).astype('<f4').tobytes())
        f.write(np.concatenate(descs).tobytes())

    (cid, model, w, h, params, prior), = con.execute(
        'select camera_id, model, width, height, params, prior_focal_length from cameras')
    cam = {'model': MODELS[model], 'width': w, 'height': h,
           'params': np.frombuffer(params, '<f8').tolist(), 'prior_focal_length': bool(prior)}
    (args.out / 'camera.json').write_text(json.dumps(cam, indent=1))
    print('%d images, %d keypoints; camera %s' % (len(ids), sum(counts), cam))


if __name__ == '__main__':
    main()
