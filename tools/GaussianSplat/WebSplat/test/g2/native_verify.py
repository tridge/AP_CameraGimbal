#!/usr/bin/env python3
"""Re-run the native verification (pycolmap verify_matches, TwoViewGeometryOptions defaults,
as sfm.py) on a copy of the reference database whose matches are the raw native matches
(stage 4's native_raw.bin, including the pairs verification emptied), and export the result
as WSTV (OUT/geometry.bin).

--seed S sets ransac.random_seed, which reseeds COLMAP's PRNG at every RANSAC call, so the
result no longer depends on thread scheduling. The reference ran unseeded (-1).
"""
import argparse
from pathlib import Path
import shutil
import sqlite3
import sys
import time

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'stage4'))
from export_native import geometry_container, read_geometry  # noqa: E402

MAX_IMAGE_ID = 2 ** 31 - 1


def read_wsmt(path):
    u = np.fromfile(path, '<u4')
    assert u[0] == 0x544d5357 and u[1] == 1
    n, total = int(u[2]), int(u[3])
    table = u[4:4 + 5 * n].reshape(n, 5)
    m = u[4 + 5 * n:].reshape(total, 2)
    return [(int(i), int(j), int(st), m[off:off + c]) for i, j, st, c, off in table]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('workdir', type=Path, help='native work dir (colmap/database.db, colmap/pairs.txt)')
    ap.add_argument('raw', type=Path, help='native_raw.bin (WSMT)')
    ap.add_argument('out', type=Path)
    ap.add_argument('--seed', type=int, default=-1)
    args = ap.parse_args()
    sys.path.insert(0, str(HERE.parents[2]))
    from splat_common import import_pycolmap
    pycolmap = import_pycolmap()
    pycolmap.logging.minloglevel = 2

    args.out.mkdir(parents=True, exist_ok=True)
    db = args.out / 'database.db'
    con = sqlite3.connect('file:%s?mode=ro' % (args.workdir / 'colmap' / 'database.db'), uri=True)
    dst = sqlite3.connect(db)
    con.backup(dst)
    con.close()
    ids = [i for i, in dst.execute('select image_id from images order by image_id')]
    names = dict(dst.execute('select image_id, name from images'))
    order = sorted(ids, key=lambda i: names[i])  # frames.json order is by name
    dst.execute('delete from two_view_geometries')
    dst.execute('delete from matches')
    for i, j, st, m in read_wsmt(args.raw):
        if st:
            continue
        a, b = order[i], order[j]
        if a > b:
            a, b, m = b, a, m[:, ::-1]
        m = np.ascontiguousarray(m, np.uint32)
        dst.execute('insert into matches values (?, ?, ?, ?)', (a * MAX_IMAGE_ID + b, len(m), 2, m.tobytes()))
    dst.commit()
    dst.close()

    opts = pycolmap.TwoViewGeometryOptions()
    opts.ransac.random_seed = args.seed
    t = time.time()
    pycolmap.verify_matches(str(db), str(args.workdir / 'colmap' / 'pairs.txt'), opts)
    secs = time.time() - t
    con = sqlite3.connect(db)
    index = {iid: k for k, iid in enumerate(order)}
    tvg = read_geometry(con, index)
    (args.out / 'geometry.bin').write_bytes(geometry_container(tvg))
    con.close()
    db.unlink()
    print('seed %d: %d pairs, %d verified, %d inliers in %.1f s' % (
        args.seed, len(tvg), sum(1 for e in tvg if len(e[3])), sum(len(e[3]) for e in tvg), secs))


if __name__ == '__main__':
    main()
