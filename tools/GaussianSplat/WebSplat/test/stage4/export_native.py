#!/usr/bin/env python3
"""Export the native COLMAP database's descriptors, raw matches and verified geometry for the
stage 4 tests.

Writes OUT/{index.json, descriptors.u8, matches.bin, geometry.bin, quant.bin}:
  descriptors.u8  all images' uint8 descriptors concatenated in frames.json order
  matches.bin     the matches table, WSMT v1 (CONTRACTS.md 4). Raw matches except that COLMAP's
                  verification has emptied pairs below min_num_inliers (see native_rematch.py)
  geometry.bin    two_view_geometries, WSTV v1: inliers, config, F/E/H and cam_j_from_cam_i
  quant.bin       sfm.py's normalised fp16 descriptors (raw f16 bits) for the first images,
                  to check the JS quantisation bit for bit
"""
import argparse
import json
from pathlib import Path
import sqlite3
import struct

import numpy as np

MAGIC, VERSION = 0x544d5357, 1
MAX_IMAGE_ID = 2 ** 31 - 1


def pair_ids(pair_id):
    return pair_id // MAX_IMAGE_ID, pair_id % MAX_IMAGE_ID


def container(entries):
    """entries: [(i, j, status, matches uint32 (m, 2))]"""
    total = sum(len(m) for *_, m in entries)
    head = [MAGIC, VERSION, len(entries), total]
    table, off = [], 0
    for i, j, status, m in entries:
        table += [i, j, status, len(m), off]
        off += len(m)
    body = np.concatenate([m.reshape(-1) for *_, m in entries]) if entries else np.zeros(0)
    return np.array(head + table, dtype='<u4').tobytes() + body.astype('<u4').tobytes()


def geometry_container(entries):
    """entries: [(i, j, config, inliers (m, 2), model f64[34])]"""
    total = sum(len(e[3]) for e in entries)
    head = [0x56545357, VERSION, len(entries), total]
    table, off = [], 0
    for i, j, config, m, _ in entries:
        table += [i, j, config, len(m), off]
        off += len(m)
    body = np.concatenate([e[3].reshape(-1) for e in entries]) if entries else np.zeros(0)
    models = np.concatenate([e[4] for e in entries]) if entries else np.zeros(0)
    return (np.array(head + table, dtype='<u4').tobytes() + body.astype('<u4').tobytes()
            + models.astype('<f8').tobytes())


def quat_mul(a, b):
    w1, x1, y1, z1 = a
    w2, x2, y2, z2 = b
    return np.array([w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2, w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
                     w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2, w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2])


def read_geometry(con, index):
    rows = con.execute('select pair_id, rows, cols, data, config, F, E, H, qvec, tvec from two_view_geometries').fetchall()
    out = []
    for pid, nr, nc, data, config, F, E, H, qv, tv in rows:
        id1, id2 = pair_ids(pid)
        m = np.frombuffer(data, dtype=np.uint32).reshape(nr, nc) if nr else np.zeros((0, 2), np.uint32)
        f = lambda b, n: np.frombuffer(b, '<f8').copy() if b else np.zeros(n)
        F, E, H, q, t = f(F, 9), f(E, 9), f(H, 9), f(qv, 4), f(tv, 3)
        i, j = index[id1], index[id2]
        if i > j:  # express for frame order: inverse relation (x2^T F x1 = 0 -> F^T, H^-1, pose inverse)
            i, j, m = j, i, m[:, ::-1]
            F, E = F.reshape(3, 3).T.ravel(), E.reshape(3, 3).T.ravel()
            H = np.linalg.inv(H.reshape(3, 3)).ravel() if H.any() else H
            if q.any():
                qi = np.array([q[0], -q[1], -q[2], -q[3]])
                tq = quat_mul(quat_mul(qi, np.concatenate([[0], t])), q)
                q, t = qi, -tq[1:]
        out.append((i, j, config, np.ascontiguousarray(m), np.concatenate([F, E, H, q, t])))
    out.sort(key=lambda e: (e[0], e[1]))
    return out


def read_pairs(con, table, index):
    rows = con.execute('select pair_id, rows, cols, data from %s' % table).fetchall()
    out = []
    for r in rows:
        id1, id2 = pair_ids(r[0])
        m = np.frombuffer(r[3], dtype=np.uint32).reshape(r[1], r[2]) if r[1] else np.zeros((0, 2), np.uint32)
        i, j = index[id1], index[id2]
        if i > j:  # COLMAP keys pairs by image id; express as frame-order (i < j)
            i, j, m = j, i, m[:, ::-1]
        out.append((i, j, 0, np.ascontiguousarray(m)))
    out.sort(key=lambda e: (e[0], e[1]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('workdir', type=Path, help='native sfm.py work dir (frames.json, colmap/)')
    ap.add_argument('out', type=Path)
    ap.add_argument('--quant-images', type=int, default=4)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    frames = json.loads((args.workdir / 'frames.json').read_text())['frames']
    names = [f['name'] for f in frames]
    con = sqlite3.connect('file:%s?mode=ro' % (args.workdir / 'colmap' / 'database.db'), uri=True)
    by_name = dict((n, i) for i, n in con.execute('select image_id, name from images'))
    ids = [by_name[n] for n in names]
    index = {iid: k for k, iid in enumerate(ids)}

    counts, quant = [], []
    with open(args.out / 'descriptors.u8', 'wb') as f:
        for k, iid in enumerate(ids):
            rows, cols, data = con.execute('select rows, cols, data from descriptors where image_id=?', (iid,)).fetchone()
            assert cols == 128
            d = np.frombuffer(data, dtype=np.uint8).reshape(rows, cols)
            f.write(d.tobytes())
            counts.append(rows)
            if k < args.quant_images:  # exactly sfm.py's normalisation and fp16 cast
                x = d.astype(np.float32)
                x /= np.maximum(np.linalg.norm(x, axis=1, keepdims=True), 1e-6)
                quant.append(x.astype(np.float16).view(np.uint16))
    (args.out / 'quant.bin').write_bytes(np.concatenate(quant).astype('<u2').tobytes())

    raw = read_pairs(con, 'matches', index)
    (args.out / 'matches.bin').write_bytes(container(raw))
    tvg = read_geometry(con, index)
    (args.out / 'geometry.bin').write_bytes(geometry_container(tvg))
    (args.out / 'index.json').write_text(json.dumps({
        'names': names, 'image_ids': ids, 'counts': counts, 'quant_images': args.quant_images,
        'raw_pairs': len(raw), 'raw_matches': int(sum(len(m) for *_, m in raw)),
        'verified_pairs': sum(1 for e in tvg if len(e[3])), 'verified_matches': int(sum(len(e[3]) for e in tvg)),
        'configs': {str(c): sum(1 for e in tvg if e[2] == c) for c in sorted(set(e[2] for e in tvg))},
    }, indent=1))
    print('%d images, %d descriptors, %d raw pairs / %d matches, %d verified pairs / %d inliers' % (
        len(ids), sum(counts), len(raw), sum(len(m) for *_, m in raw),
        sum(1 for e in tvg if len(e[3])), sum(len(e[3]) for e in tvg)))


if __name__ == '__main__':
    main()
