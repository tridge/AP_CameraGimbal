#!/usr/bin/env python3
"""Give thermal frames the camera poses found by SfM on the co-mounted RGB camera.

The RGB and thermal lenses share the gimbal, so a thermal frame's pose is the RGB pose at the
same instant, rotated by a small fixed boresight offset. The RGB poses (sfm.py on the output of
extract_rgb_frames.py) are interpolated to each thermal timestamp. The clock offset between the
two cameras and the boresight rotation are fitted so that the hottest pixel of the hot thermal
frames triangulates to one point (a compact heat source), then the result is written as a
COLMAP model the trainer can use, carrying the RGB reconstruction's 3D points:

  transfer_poses.py RGBWORK THERMALWORK
  train.py THERMALWORK --sparse THERMALWORK/colmap_rgb/sparse/final --min-obs 0 ...

The transferred poses are usually too loose for a splat or a map; --seed writes a new thermal
workdir whose pose priors are the transferred poses, for a thermal SfM run that refines them:

  transfer_poses.py RGBWORK THERMALWORK --seed SEEDED
  sfm.py SEEDED --mapper prior --prior-sigma 3 ...
"""
import argparse
import json
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from splat_common import fatal, import_pycolmap, triangulate_rays  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('rgb', type=Path, help='RGB workdir (extract_rgb_frames.py + sfm.py)')
    ap.add_argument('thermal', type=Path, help='thermal workdir (extract_frames.py)')
    ap.add_argument('--rgb-sparse', type=Path, help='RGB model (default: RGB/colmap/sparse/final)')
    ap.add_argument('--out', type=Path, help='output model (default: THERMAL/colmap_rgb/sparse/final)')
    ap.add_argument('--max-gap', type=float, default=1.0, help='longest RGB pose gap to interpolate across (s)')
    ap.add_argument('--hot-min-c', type=float, default=150.0, help='frames used for the fit: maximum above this (C)')
    ap.add_argument('--edge', type=float, default=0.03, help='ignore hot spots this fraction of the width from the frame edge')
    ap.add_argument('--cluster', type=float, default=40.0,
                    help='fit only the largest group of hot frames whose hot spots land within this (m); 0 to use all')
    ap.add_argument('--dt-range', type=float, default=3.0, help='clock offsets searched: +- this many seconds')
    ap.add_argument('--dt', type=float, help='use this clock offset (s) instead of fitting it')
    ap.add_argument('--no-boresight', action='store_true', help='assume the lenses are exactly aligned')
    ap.add_argument('--seed', type=Path, metavar='DIR',
                    help='also write a thermal workdir with the transferred poses as its priors (frames linked)')
    args = ap.parse_args()

    import cv2
    from scipy.optimize import minimize
    from scipy.spatial.transform import Rotation, Slerp
    pycolmap = import_pycolmap()

    rgb_info = json.loads((args.rgb / 'frames.json').read_text())
    th_info = json.loads((args.thermal / 'frames.json').read_text())
    rec = pycolmap.Reconstruction(str(args.rgb_sparse or args.rgb / 'colmap' / 'sparse' / 'final'))
    utc = {f['name']: f['utc_us'] / 1e6 for f in rgb_info['frames']}
    poses = sorted((utc[im.name], im.cam_from_world().inverse()) for im in rec.images.values() if im.name in utc)
    if len(poses) < 10:
        fatal('only %d registered RGB frames' % len(poses))
    times = np.array([t for t, _ in poses])
    rots = Rotation.from_matrix([p.rotation.matrix() for _, p in poses])  # world_from_cam
    centres = np.array([p.translation for _, p in poses])
    slerp = Slerp(times, rots)
    print('%d registered RGB frames over %.0f s, median spacing %.2f s' % (
        len(times), times[-1] - times[0], np.median(np.diff(times))))

    def rgb_pose(t):
        """world_from_cam rotation and centre at time t, or None outside or across a gap."""
        i = np.searchsorted(times, t)
        if i == 0 or i == len(times) or times[i] - times[i - 1] > args.max_gap:
            return None
        a = (t - times[i - 1]) / (times[i] - times[i - 1])
        return slerp([t])[0], centres[i - 1] * (1 - a) + centres[i] * a

    K = np.array(th_info['K'])
    Ki = np.linalg.inv(K)
    frames = th_info['frames']
    hot = []
    for f in frames:
        if f['maximum_c'] >= args.hot_min_c:
            raw = cv2.imread(str(args.thermal / 'thermal' / f['name']), cv2.IMREAD_UNCHANGED)
            y, x = np.unravel_index(np.argmax(raw), raw.shape)
            m = args.edge * raw.shape[1]
            if min(x, y, raw.shape[1] - 1 - x, raw.shape[0] - 1 - y) < m:
                continue  # often the aircraft itself, or a source half out of view
            hot.append((f['utc_us'] / 1e6, Ki @ np.array([x, y, 1.0])))
    fixed = args.dt is not None and args.no_boresight  # nothing to fit; hot frames only report the result
    if len(hot) < 5 and not fixed:
        fatal('only %d thermal frames above %.0f C to fit the clock offset and boresight' % (len(hot), args.hot_min_c))
    if args.cluster > 0 and len(hot) >= 5:
        # several hot sources: keep the frames whose hot spot lands on the ground near the most others
        ground = float(np.median([p.xyz[2] for p in rec.points3D.values()]))
        hits = []
        for t, ray in hot:
            p = rgb_pose(t)
            d = p[0].apply(ray) if p is not None else None
            hits.append(p[1][:2] + d[:2] * (ground - p[1][2]) / d[2] if d is not None and d[2] < -0.1 else None)
        ok = [i for i, h in enumerate(hits) if h is not None]
        near = {i: [j for j in ok if np.linalg.norm(hits[i] - hits[j]) < args.cluster] for i in ok}
        if near:
            keep = max(near.values(), key=len)
            if len(keep) < len(hot):
                print('hot spots land in several places; using %d/%d frames within %.0f m of ENU %s' % (
                    len(keep), len(hot), args.cluster, np.round(np.mean([hits[i] for i in keep], axis=0), 0)))
            hot = [hot[i] for i in keep]
        if len(hot) < 5 and not fixed:
            fatal('only %d thermal frames see the main hot source' % len(hot))

    def residual(params):
        dt, rv = params[0], params[1:]
        Rb = Rotation.from_rotvec(np.radians(rv))
        C, D = [], []
        for t, ray in hot:
            p = rgb_pose(t + dt)
            if p is not None:
                d = (p[0] * Rb).apply(ray)
                C.append(p[1])
                D.append(d / np.linalg.norm(d))
        if len(C) < 0.5 * len(hot):
            return 1e6, None, len(C)
        X, err = triangulate_rays(C, D, K[0, 0])
        return err, X, len(C)

    if args.dt is None:
        grid = [(residual([dt, 0, 0, 0])[0], dt) for dt in np.arange(-args.dt_range, args.dt_range + 1e-9, 0.1)]
        start = min(grid)[1]
        print('clock offset search: best %.1f s (%.1f px) at zero boresight' % (start, min(grid)[0]))
    else:
        start = args.dt
    x0 = np.array([start, 0.0, 0.0, 0.0])
    if args.no_boresight:
        res = minimize(lambda p: residual([p[0], 0, 0, 0])[0], x0[:1], method='Nelder-Mead',
                       options=dict(xatol=0.005, fatol=0.01)) if args.dt is None else None
        best = np.array([res.x[0] if res is not None else start, 0, 0, 0])
    else:
        fixed_dt = args.dt is not None

        def objective(p):
            return residual([start, *p] if fixed_dt else p)[0]
        p0 = x0[1:] if fixed_dt else x0
        # default simplex steps are tiny around zero; start with 0.2 s and 1 deg
        steps = np.array([1.0, 1.0, 1.0] if fixed_dt else [0.2, 1.0, 1.0, 1.0])
        simplex = np.vstack([p0] + [p0 + np.eye(len(p0))[i] * steps[i] for i in range(len(p0))])
        res = minimize(objective, p0, method='Nelder-Mead',
                       options=dict(xatol=0.005, fatol=0.01, maxiter=2000, initial_simplex=simplex))
        best = np.array([start, *res.x]) if fixed_dt else res.x
    err, X, n = residual(best) if hot else (float('nan'), None, 0)
    if X is None:
        print('clock offset %+.3f s, boresight %s deg (given; too few posed hot frames to check)' % (
            best[0], np.round(best[1:], 2)))
        X, err = np.full(3, np.nan), float('nan')
    else:
        print('clock offset %+.3f s, boresight %s deg: hot spot rays from %d/%d frames meet at ENU %s m, '
              'median error %.1f px' % (best[0], np.round(best[1:], 2), n, len(hot), np.round(X, 1), err))

    # thermal model: nominal pinhole camera, interpolated RGB pose * boresight, RGB 3D points
    Rb = Rotation.from_rotvec(np.radians(best[1:]))
    out = args.out or args.thermal / 'colmap_rgb' / 'sparse' / 'final'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'cameras.txt').write_text('1 PINHOLE %d %d %.9g %.9g %.9g %.9g\n' % (
        th_info['width'], th_info['height'], K[0, 0], K[1, 1], K[0, 2], K[1, 2]))
    lines, n_out, seeded = [], 0, []
    for f in frames:
        p = rgb_pose(f['utc_us'] / 1e6 + best[0])
        if p is None:
            continue
        seeded.append(dict(f, position=p[1].tolist(), rotation=(p[0] * Rb).as_matrix().tolist()))
        R_cw = (p[0] * Rb).inv()
        q = R_cw.as_quat()  # x, y, z, w
        t = -R_cw.apply(p[1])
        n_out += 1
        lines.append('%d %.12g %.12g %.12g %.12g %.9g %.9g %.9g 1 %s\n\n' % (n_out, q[3], q[0], q[1], q[2], *t, f['name']))
    (out / 'images.txt').write_text(''.join(lines))
    (out / 'points3D.txt').write_text(''.join(
        '%d %.6f %.6f %.6f %d %d %d 0\n' % (i + 1, *pt.xyz, *pt.color) for i, pt in enumerate(rec.points3D.values())))
    (out / 'transfer.json').write_text(json.dumps(dict(
        clock_offset_s=float(best[0]), boresight_rotvec_deg=best[1:].tolist(), hot_frames=len(hot),
        hot_frames_used=n, hotspot_enu=X.tolist(), median_ray_error_px=float(err),
        rgb_model=str(args.rgb_sparse or args.rgb / 'colmap' / 'sparse' / 'final')), indent=1))
    print('wrote %d of %d thermal frames and %d points to %s' % (n_out, len(frames), rec.num_points3D(), out))
    if args.seed:
        args.seed.mkdir(parents=True, exist_ok=True)
        for d in ('thermal', 'images'):
            if (args.thermal / d).exists() and not (args.seed / d).exists():
                (args.seed / d).symlink_to((args.thermal / d).resolve())
        (args.seed / 'frames.json').write_text(json.dumps(dict(th_info, frames=seeded), indent=1))
        print('wrote %d seeded frames to %s' % (len(seeded), args.seed))


if __name__ == '__main__':
    main()
