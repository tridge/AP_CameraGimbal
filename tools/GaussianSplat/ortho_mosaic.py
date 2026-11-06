#!/usr/bin/env python3
"""Best-view orthophoto from an OpenDroneMap project.

ODM textures its 2.5D mesh from whichever views its seam optimiser picks, which for an orbit
of oblique frames smears texture outward from the centre. This builds an orthophoto on ODM's
DSM instead: each cell takes its colour from the single steepest, closest view that sees it
(depth-tested against the DSM, avoiding masked pixels and frame edges), using ODM's undistorted
images and refined cameras:

  ortho_mosaic.py ODMPROJECT [--resolution 0.15]
  odm_import.py ODMPROJECT ... --ortho ODMPROJECT/odm_orthophoto/best_view.tif

Writes a north-up RGBA GeoTIFF in the DSM's CRS.
"""
import argparse
import json
from pathlib import Path
import sys
import time

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from splat_common import fatal  # noqa: E402


def rotvec_matrix(rv):
    return cv2.Rodrigues(np.asarray(rv, dtype=np.float64))[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('project', type=Path, help='ODM project directory (after odm_dem)')
    ap.add_argument('--resolution', type=float, default=0.15, help='output cell size (m)')
    ap.add_argument('--smooth', type=float, default=2.0, help='median filter the DSM over this size first (m)')
    ap.add_argument('--edge', type=float, default=0.08, help='feather this fraction of the frame width at its edges')
    ap.add_argument('--out', type=Path, help='default PROJECT/odm_orthophoto/best_view.tif')
    args = ap.parse_args()

    import rasterio
    from rasterio.transform import Affine
    from scipy.ndimage import distance_transform_edt, map_coordinates, median_filter
    proj = args.project
    rec = json.loads((proj / 'opensfm' / 'undistorted' / 'reconstruction.json').read_text())[0]
    off = np.array([float(v) for v in (proj / 'odm_georeferencing' / 'coords.txt').read_text().split('\n')[1].split()[:2]] + [0.0])

    with rasterio.open(proj / 'odm_dem' / 'dsm.tif') as src:
        scale = src.res[0] / args.resolution
        h, w = int(src.height * scale), int(src.width * scale)
        dsm = src.read(1, out_shape=(h, w), resampling=rasterio.enums.Resampling.bilinear).astype(np.float32)
        valid = dsm != src.nodata if src.nodata is not None else np.isfinite(dsm)
        tf = src.transform * Affine.scale(src.width / w, src.height / h)
        crs = src.crs
    if not valid.any():
        fatal('empty DSM')
    _, (ii, jj) = distance_transform_edt(~valid, return_indices=True)
    k = max(3, int(round(args.smooth / args.resolution)) | 1)
    dsm = median_filter(dsm[ii, jj], size=k)
    rows, cols = np.nonzero(valid)
    xs, ys = rasterio.transform.xy(tf, rows, cols)
    pts = np.stack([np.asarray(xs) - off[0], np.asarray(ys) - off[1], dsm[rows, cols]], axis=1)  # reconstruction frame
    m = len(pts)
    print('%d cells of %.2f m (%dx%d)' % (m, args.resolution, w, h), flush=True)

    best = np.zeros(m, np.float32)
    colour = np.zeros((m, 3), np.uint8)
    t0 = time.time()
    shots = sorted(rec['shots'].items())
    for n, (name, shot) in enumerate(shots):
        cam = rec['cameras'][shot['camera']]
        W, H = cam['width'], cam['height']
        f = cam['focal'] * max(W, H)
        R = rotvec_matrix(shot['rotation'])
        t = np.array(shot['translation'])
        centre = -R.T @ t
        pc = pts @ R.T + t
        z = pc[:, 2]
        near = np.flatnonzero(z > 1)
        u = f * pc[near, 0] / z[near] + W / 2
        v = f * pc[near, 1] / z[near] + H / 2
        inside = (u >= 0) & (u < W - 1) & (v >= 0) & (v < H - 1)
        near, u, v, zn = near[inside], u[inside], v[inside], z[near][inside]
        if not len(near):
            continue
        ray = pts[near] - centre
        dist = np.linalg.norm(ray, axis=1)
        sin_el = (-ray[:, 2] / dist).clip(0.05, 1)
        block = max(1, int(np.ceil(2 * f * args.resolution / np.median(zn))))
        bw = W // block + 1
        bi = (v // block).astype(np.int64) * bw + (u // block).astype(np.int64)
        zbuf = np.full(bw * (H // block + 1), np.inf, np.float32)
        np.minimum.at(zbuf, bi, zn)
        seen = zn <= zbuf[bi] + 0.5 + 0.005 * zn + 2 * args.resolution / sin_el
        edge = np.minimum(np.minimum(u, W - u), np.minimum(v, H - v)) / (args.edge * W)
        score = sin_el / dist * edge.clip(0, 1)
        mask_path = proj / 'opensfm' / 'undistorted' / 'masks' / (name + '.png')
        if mask_path.exists():
            mk = cv2.imread(str(mask_path), cv2.IMREAD_GRAYSCALE)
            seen &= mk[v.astype(int), u.astype(int)] > 0
        better = seen & (score > best[near])
        if not better.any():
            continue
        img = cv2.imread(str(proj / 'opensfm' / 'undistorted' / 'images' / name), cv2.IMREAD_COLOR)
        sel = near[better]
        for c in range(3):
            colour[sel, c] = map_coordinates(img[..., 2 - c], [v[better], u[better]], order=1, mode='nearest')
        best[sel] = score[better]
        if (n + 1) % 50 == 0:
            print('  %d/%d views, %.0f s' % (n + 1, len(shots), time.time() - t0), flush=True)

    out = np.zeros((4, h, w), np.uint8)
    for c in range(3):
        out[c, rows, cols] = colour[:, c]
    out[3, rows, cols] = np.where(best > 0, 255, 0)
    dst = args.out or proj / 'odm_orthophoto' / 'best_view.tif'
    with rasterio.open(dst, 'w', driver='GTiff', width=w, height=h, count=4, dtype='uint8', crs=crs, transform=tf,
                       compress='deflate', photometric='RGB', alpha='YES') as f:
        f.write(out)
    print('%.0f%% of cells coloured; wrote %s in %.0f s' % (100 * (best > 0).mean(), dst, time.time() - t0))


if __name__ == '__main__':
    main()
