#!/usr/bin/env python3
"""Dense point cloud of the ground around a site from an RGB sfm.py reconstruction.

Frames whose view axis meets the ground within --radius of the centre are undistorted,
run through COLMAP PatchMatch stereo (CUDA) and fused. Fusion is limited to a box about the
centre, which drops the aircraft structure in frame and the sky:

  dense_rgb.py RGBWORK [--centre E,N,U] [--radius 250]

Writes RGBWORK/dense/{images,sparse,stereo,...} and RGBWORK/dense/fused.ply (ENU metres);
surface_map.py turns that into a surface model and orthophoto.
"""
import argparse
import json
from pathlib import Path
import shutil
import sys
import time

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from splat_common import fatal, import_pycolmap  # noqa: E402


def ground_level(rec, centre, radius):
    xyz = np.array([p.xyz for p in rec.points3D.values()])
    near = xyz[np.linalg.norm(xyz[:, :2] - centre[:2], axis=1) < radius]
    if len(near) < 100:
        fatal('only %d SfM points within %.0f m of the centre' % (len(near), radius))
    return float(np.percentile(near[:, 2], 20))


def select_images(rec, centre, radius, ground_z, max_range, max_images):
    """Images whose optical axis meets the ground plane within radius of centre, evenly thinned."""
    chosen = []
    for im in rec.images.values():
        pose = im.cam_from_world().inverse()
        c = pose.translation
        axis = pose.rotation.matrix()[:, 2]
        if axis[2] > -0.05:  # at or above the horizon
            continue
        s = (ground_z - c[2]) / axis[2]
        hit = c + s * axis
        if s < max_range and np.linalg.norm(hit[:2] - centre[:2]) < radius:
            chosen.append(im.name)
    chosen.sort()
    if max_images and len(chosen) > max_images:
        chosen = [chosen[i] for i in np.linspace(0, len(chosen) - 1, max_images).round().astype(int)]
    return chosen


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('workdir', type=Path, help='RGB workdir (extract_rgb_frames.py + sfm.py)')
    ap.add_argument('--sparse', type=Path, help='COLMAP model (default WORKDIR/colmap/sparse/final)')
    ap.add_argument('--out', type=Path, help='dense workspace (default WORKDIR/dense)')
    ap.add_argument('--centre', type=lambda s: np.array([float(v) for v in s.split(',')]),
                    help='site centre E,N,U in m (default: frames.json target)')
    ap.add_argument('--radius', type=float, default=250.0, help='horizontal radius of the site (m)')
    ap.add_argument('--below', type=float, default=40.0, help='keep points down to this far below ground level (m)')
    ap.add_argument('--above', type=float, default=60.0, help='keep points up to this far above ground level (m)')
    ap.add_argument('--max-range', type=float, default=700.0, help='skip frames looking further than this (m)')
    ap.add_argument('--max-images', type=int, default=600, help='evenly thin the chosen frames to this many')
    ap.add_argument('--max-image-size', type=int, default=1600, help='stereo image size (longest side, px)')
    ap.add_argument('--src-images', type=int, default=10, help='stereo source views per frame')
    ap.add_argument('--fresh', action='store_true', help='delete a previous dense workspace first')
    args = ap.parse_args()

    pycolmap = import_pycolmap()
    sparse = args.sparse or args.workdir / 'colmap' / 'sparse' / 'final'
    out = args.out or args.workdir / 'dense'
    if args.fresh and out.exists():
        shutil.rmtree(out)
    rec = pycolmap.Reconstruction(str(sparse))
    centre = args.centre if args.centre is not None else np.array(
        json.loads((args.workdir / 'frames.json').read_text())['target_enu'], dtype=float)
    ground_z = ground_level(rec, centre, args.radius)
    names = select_images(rec, centre, args.radius, ground_z, args.max_range, args.max_images)
    if len(names) < 10:
        fatal('only %d frames look at the site' % len(names))
    print('site %s m, ground level %.1f m, %d frames' % (np.round(centre[:2], 1), ground_z, len(names)), flush=True)

    t0 = time.time()
    if not (out / 'sparse').exists():
        opts = pycolmap.UndistortCameraOptions()
        opts.max_image_size = args.max_image_size
        # stereo source images are picked from shared points, so drop the frames not chosen
        keep, reg = set(names), set(rec.reg_frame_ids())
        for im in list(rec.images.values()):
            if im.name not in keep and im.frame_id in reg:
                rec.deregister_frame(im.frame_id)
        subset = out / 'sparse_subset'
        subset.mkdir(parents=True, exist_ok=True)
        rec.write(str(subset))
        pycolmap.undistort_images(str(out), str(subset), str(args.workdir / 'images'),
                                  num_patch_match_src_images=args.src_images, undistort_options=opts)
        print('undistorted in %.0f s' % (time.time() - t0), flush=True)
    t1 = time.time()
    for geometric in (False, True):  # the geometric pass needs every image's photometric depth map
        pm = pycolmap.PatchMatchOptions()
        pm.max_image_size = args.max_image_size
        pm.geom_consistency = geometric
        pm.filter = geometric
        pycolmap.patch_match_stereo(str(out), options=pm)
        print('%s stereo in %.0f s' % ('geometric' if geometric else 'photometric', time.time() - t1), flush=True)
        t1 = time.time()
    t1 = time.time()
    fo = pycolmap.StereoFusionOptions()
    fo.max_image_size = args.max_image_size
    lo = np.array([centre[0] - args.radius, centre[1] - args.radius, ground_z - args.below])
    hi = np.array([centre[0] + args.radius, centre[1] + args.radius, ground_z + args.above])
    fo.bounding_box = (lo.astype(np.float32), hi.astype(np.float32))
    fused = pycolmap.stereo_fusion(str(out / 'fused.ply'), str(out), options=fo)
    print('fused %d points in %.0f s' % (fused.num_points3D(), time.time() - t1))
    (out / 'site.json').write_text(json.dumps(dict(centre_enu=centre.tolist(), radius_m=args.radius,
                                                   ground_z=ground_z, frames=names), indent=1))
    print('wrote %s in %.0f s' % (out / 'fused.ply', time.time() - t0))


if __name__ == '__main__':
    main()
