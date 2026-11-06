#!/usr/bin/env python3
"""Quality report for a COLMAP model from sfm.py, against the telemetry in frames.json.

Reports what matters for splatting: registered images, track lengths, triangulation
angles, per-image observation counts and reprojection error tails, and how far each
camera moved from telemetry (rotation and position), with the worst images listed.
"""
import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from splat_common import import_pycolmap  # noqa: E402


def report(rec, frames, worst=5):
    pycolmap = import_pycolmap()
    pos = {f['name']: np.array(f['position']) for f in frames}
    rot = {f['name']: np.array(f['rotation']) for f in frames if f.get('rotation') is not None}
    images = list(rec.images.values())
    print('images %d/%d registered, %d points, mean track %.1f, mean reprojection %.2f px'
          % (rec.num_reg_images(), len(frames), rec.num_points3D(), rec.compute_mean_track_length(),
             rec.compute_mean_reprojection_error()))
    centres = {im.image_id: im.projection_center() for im in images}
    angles = []
    pts = list(rec.points3D.values())
    for pt in pts[::max(1, len(pts) // 20000)]:
        ids = [el.image_id for el in pt.track.elements]
        rays = np.array([centres[i] - pt.xyz for i in ids])
        rays /= np.linalg.norm(rays, axis=1, keepdims=True)
        cos = np.clip(rays @ rays.T, -1, 1)
        angles.append(math.degrees(math.acos(cos.min())))
    angles = np.array(angles)
    print('max triangulation angle per point: median %.1f deg, %.0f%% below 2 deg'
          % (np.median(angles), 100 * (angles < 2).mean()))
    rows = []
    for im in images:
        n = im.num_points3D
        errs = []
        for p2 in im.points2D:
            if p2.has_point3D():
                proj = im.project_point(rec.points3D[p2.point3D_id].xyz)
                if proj is not None:
                    errs.append(np.linalg.norm(proj - p2.xy))
        drot = np.nan  # frames without gimbal telemetry have no rotation to compare
        if im.name in rot:
            dR = rot[im.name].T @ im.cam_from_world().rotation.matrix().T
            drot = math.degrees(math.acos(np.clip((np.trace(dR) - 1) / 2, -1, 1)))
        dpos = np.linalg.norm(im.projection_center() - pos[im.name])
        rows.append((im.name, n, np.percentile(errs, 90) if errs else np.nan, drot, dpos))
    obs = np.array([r[1] for r in rows])
    e90 = np.array([r[2] for r in rows])
    drot = np.array([r[3] for r in rows])
    dpos = np.array([r[4] for r in rows])
    print('observations per image: min %d, 5%% %d, median %d' % (obs.min(), np.percentile(obs, 5), np.median(obs)))
    print('per-image 90th percentile reprojection: median %.2f px, worst %.2f px' % (np.nanmedian(e90), np.nanmax(e90)))
    print('rotation change from telemetry: median %.2f, 90%% %.2f, max %.2f deg'
          % (np.nanmedian(drot), np.nanpercentile(drot, 90), np.nanmax(drot)))
    print('position change from telemetry: median %.1f, 90%% %.1f, max %.1f m'
          % (np.median(dpos), np.percentile(dpos, 90), dpos.max()))
    for label, key in (('fewest observations', 1), ('largest rotation change', 3)):
        order = sorted(rows, key=lambda r: r[key], reverse=key != 1)[:worst]
        print('%s: %s' % (label, ', '.join('%s (%d obs, %.2f deg, %.1f m)' % (r[0], r[1], r[3], r[4])
                                           for r in order)))
    return dict(registered=rec.num_reg_images(), points=rec.num_points3D(),
                rotation_change_median_deg=float(np.nanmedian(drot)), position_change_median_m=float(np.median(dpos)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('workdir', type=Path)
    ap.add_argument('sparse', type=Path, nargs='?', help='model directory (default WORKDIR/colmap/sparse/final)')
    args = ap.parse_args()
    pycolmap = import_pycolmap()
    frames = json.loads((args.workdir / 'frames.json').read_text())['frames']
    rec = pycolmap.Reconstruction(str(args.sparse or args.workdir / 'colmap' / 'sparse' / 'final'))
    report(rec, frames)


if __name__ == '__main__':
    main()
