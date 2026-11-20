#!/usr/bin/env python3
"""Native floor for G2: sfm.py's own prior_mapping() on a copy of the reference database whose
two_view_geometries are replaced by a WSTV container (e.g. a native re-verification), so the
spread caused by verification RANSAC alone can be measured. Writes OUT/final (COLMAP binary).
"""
import argparse
import json
from pathlib import Path
import sqlite3
import sys
import time

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2]))
sys.path.insert(0, str(HERE))
from compare_geometry import read  # noqa: E402

MAX_IMAGE_ID = 2 ** 31 - 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('workdir', type=Path, help='native work dir (frames.json, colmap/database.db)')
    ap.add_argument('geometry', help='WSTV container')
    ap.add_argument('out', type=Path)
    args = ap.parse_args()
    import sfm
    from splat_common import import_pycolmap
    pycolmap = import_pycolmap()
    pycolmap.logging.minloglevel = 2
    args.out.mkdir(parents=True, exist_ok=True)
    db = args.out / 'database.db'
    src = sqlite3.connect('file:%s?mode=ro' % (args.workdir / 'colmap' / 'database.db'), uri=True)
    dst = sqlite3.connect(db)
    src.backup(dst)
    src.close()
    info = json.loads((args.workdir / 'frames.json').read_text())
    frames = info['frames']
    by_name = dict((n, i) for i, n in dst.execute('select image_id, name from images'))
    ids = [by_name[f['name']] for f in frames]
    dst.execute('delete from two_view_geometries')
    for (i, j), (config, m, model) in read(args.geometry).items():
        a, b = ids[i], ids[j]
        assert a < b
        m = np.ascontiguousarray(m, np.uint32)
        f = lambda x: np.ascontiguousarray(x, '<f8').tobytes()
        dst.execute('insert into two_view_geometries (pair_id, rows, cols, data, config, F, E, H, qvec, tvec) '
                    'values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)',
                    (a * MAX_IMAGE_ID + b, len(m), 2, m.tobytes(), config, f(model[:9]), f(model[9:18]),
                     f(model[18:27]), f(model[27:31]), f(model[31:34])))
    dst.commit()
    camera = pycolmap.Database.open(str(db)).read_all_cameras()[0]
    dst.close()
    t = time.time()
    images = args.workdir / 'images'
    rec = sfm.prior_mapping(db, images, args.out, frames, camera, ids, [40, 16, 8, 4], 5.0, False)
    print('mapping %.1f s' % (time.time() - t))
    (args.out / 'final').mkdir(exist_ok=True)
    rec.write(str(args.out / 'final'))
    db.unlink()


if __name__ == '__main__':
    main()
