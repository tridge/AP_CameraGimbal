#!/usr/bin/env python3
"""Structure from motion on extracted thermal frames, seeded and georeferenced by telemetry.

GPU SIFT features, image pairs chosen from the telemetry view geometry, GPU mutual
nearest-neighbour matching, COLMAP geometric verification, then incremental mapping
with position priors so the model comes out in the local ENU frame (metres) of
frames.json. Telemetry only seeds and anchors the solution: the final poses come
from bundle adjustment of image features.
Writes WORKDIR/colmap/{database.db, pairs.txt, sparse/}.
"""
import argparse
import json
import math
from pathlib import Path
import shutil
import sys
import time

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from splat_common import fatal, import_pycolmap


def view_geometry(frames, target):
    centres = np.array([f['position'] for f in frames])
    rays = target - centres
    ranges = np.linalg.norm(rays, axis=1)
    return rays / ranges[:, None], ranges


def select_pairs(frames, target, max_angle, max_scale, neighbours, sequential):
    """Pairs viewing the target from similar directions and scales, plus time neighbours."""
    dirs, ranges = view_geometry(frames, target)
    n = len(frames)
    cos_max = math.cos(math.radians(max_angle))
    pairs = set()
    for i in range(n):
        cos = dirs @ dirs[i]
        scale = np.maximum(ranges / ranges[i], ranges[i] / ranges)
        ok = np.where((cos >= cos_max) & (scale <= max_scale))[0]
        ok = ok[ok != i]
        best = ok[np.argsort(-cos[ok])][:neighbours]
        for j in best:
            pairs.add((min(i, j), max(i, j)))
        for j in range(i + 1, min(n, i + 1 + sequential)):
            pairs.add((i, j))
    return sorted(pairs)


def match_pairs(db, ids, pairs, ratio, device):
    """Mutual nearest neighbour + ratio test; resumes by skipping pairs already in the database."""
    import torch
    cache = {}

    def descriptors(i):
        if i not in cache:  # host RAM, fp16; 1-2 GB for a few thousand frames
            d = db.read_descriptors(ids[i]).astype(np.float32)
            d /= np.maximum(np.linalg.norm(d, axis=1, keepdims=True), 1e-6)
            cache[i] = torch.from_numpy(d.astype(np.float16)).pin_memory() if device == 'cuda' else \
                torch.from_numpy(d)
        return cache[i].to(device, non_blocking=True)

    pycolmap = import_pycolmap()
    counts = []
    tx = None
    for k, (i, j) in enumerate(pairs):
        if k % 1000 == 0:
            tx = None  # commits the previous batch; per-pair commits are very slow
            tx = pycolmap.DatabaseTransaction(db)
        if k % 5000 == 0:
            print('  matched %d/%d pairs' % (k, len(pairs)), flush=True)
        if db.exists_matches(ids[i], ids[j]):
            continue
        a, b = descriptors(i), descriptors(j)
        if len(a) < 2 or len(b) < 2:
            continue
        with torch.no_grad():
            sim = (a @ b.T).float()
            top = sim.topk(2, dim=1)
            # ratio test on L2 distance: d = sqrt(2 - 2 sim)
            dist = torch.sqrt(torch.clamp(2 - 2 * top.values, min=0))
            good = dist[:, 0] < ratio * dist[:, 1]
            fwd = top.indices[:, 0]
            mutual = sim.argmax(dim=0)[fwd] == torch.arange(len(a), device=device)
            keep = torch.where(good & mutual)[0]
            m = torch.stack([keep, fwd[keep]], dim=1).cpu().numpy().astype(np.uint32)
            del sim, top, dist
        db.write_matches(ids[i], ids[j], m)
        counts.append(len(m))
    del tx
    return counts or [0]


def quat_wxyz(R):
    from scipy.spatial.transform import Rotation
    x, y, z, w = Rotation.from_matrix(R).as_quat()
    return w, x, y, z


def prior_mapping(db_path, images, sparse, frames, camera, ids, thresholds, prior_sigma, use_gpu):
    """Start every camera at its telemetry pose, triangulate, then alternate pose-prior
    bundle adjustment and re-triangulation with tightening reprojection thresholds."""
    pycolmap = import_pycolmap()
    init = sparse / 'telemetry'
    init.mkdir(parents=True, exist_ok=True)
    p = camera.params
    (init / 'cameras.txt').write_text('%d %s %d %d %s\n' % (camera.camera_id, camera.model.name, camera.width,
                                                           camera.height, ' '.join('%.9g' % v for v in p)))
    lines = []
    for f, image_id in zip(frames, ids):
        R = np.array(f['rotation']).T  # cam_from_world
        t = -R @ np.array(f['position'])
        lines.append('%d %s %s %d %s\n\n' % (image_id, ' '.join('%.12g' % v for v in quat_wxyz(R)),
                                              ' '.join('%.9g' % v for v in t), camera.camera_id, f['name']))
    (init / 'images.txt').write_text(''.join(lines))
    (init / 'points3D.txt').write_text('')
    rec = pycolmap.Reconstruction(str(init))
    priors = {i: pycolmap.PosePrior(np.array(f['position']), np.eye(3) * prior_sigma ** 2,
                                    pycolmap.PosePriorCoordinateSystem.CARTESIAN) for f, i in zip(frames, ids)}
    work = sparse / 'work'
    work.mkdir(exist_ok=True)
    t0 = time.time()
    for n, thresh in enumerate(thresholds):
        opts = pycolmap.IncrementalPipelineOptions()
        opts.triangulation.create_max_angle_error = max(2.0, thresh / 10)
        opts.triangulation.continue_max_angle_error = max(2.0, thresh / 10)
        opts.triangulation.merge_max_reproj_error = thresh
        opts.triangulation.complete_max_reproj_error = thresh
        opts.mapper.filter_max_reproj_error = thresh
        opts.ba_refine_focal_length = False
        opts.ba_refine_extra_params = False
        opts.ba_global_max_refinements = 1
        rec = pycolmap.triangulate_points(rec, str(db_path), str(images), str(work), clear_points=True,
                                          options=opts)
        tri = rec.num_points3D()
        ba = pycolmap.BundleAdjustmentOptions()
        ba.loss_function_type = pycolmap.LossFunctionType.CAUCHY
        ba.loss_function_scale = max(1.0, thresh / 4)
        ba.refine_focal_length = n > 0
        ba.refine_extra_params = n > 0
        ba.print_summary = False
        ba.use_gpu = use_gpu
        ba.solver_options.max_num_iterations = 50
        config = pycolmap.BundleAdjustmentConfig()
        for image_id in rec.reg_image_ids():
            config.add_image(image_id)
        prior_opts = pycolmap.PosePriorBundleAdjustmentOptions()
        prior_opts.use_robust_loss_on_prior_position = True
        adjuster = pycolmap.create_pose_prior_bundle_adjuster(ba, prior_opts, config, priors, rec)
        adjuster.solve()
        filtered = pycolmap.ObservationManager(rec).filter_all_points3D(thresh, 1.0)
        print('  round %d: %.0f px, %d points triangulated, %d filtered, mean reprojection %.2f px (%.0f s)'
              % (n, thresh, tri, filtered, rec.compute_mean_reprojection_error(), time.time() - t0), flush=True)
    return rec


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('workdir', type=Path)
    ap.add_argument('--camera-model', default='SIMPLE_RADIAL', choices=['PINHOLE', 'SIMPLE_RADIAL', 'RADIAL'],
                    help='RADIAL (two distortion terms) suits wide-angle RGB lenses')
    ap.add_argument('--max-features', type=int, default=8192)
    ap.add_argument('--max-angle', type=float, default=25.0,
                    help='match frames whose view directions to the target differ by less than this (deg)')
    ap.add_argument('--max-scale', type=float, default=2.2, help='maximum target range ratio for a pair')
    ap.add_argument('--neighbours', type=int, default=30, help='view-direction neighbours per frame')
    ap.add_argument('--sequential', type=int, default=5, help='also match this many following frames')
    ap.add_argument('--ratio', type=float, default=0.8, help='descriptor ratio test')
    ap.add_argument('--prior-sigma', type=float, default=5.0, help='telemetry position std dev (m)')
    ap.add_argument('--verbose', action='store_true', help='show COLMAP progress logging')
    ap.add_argument('--mapper', choices=['incremental', 'prior'], default='incremental',
                    help='incremental: COLMAP incremental SfM with position priors (robust, slow); '
                         'prior: start from telemetry poses and refine by bundle adjustment (fast)')
    ap.add_argument('--thresholds', default='40,16,8,4',
                    help='prior mapper reprojection thresholds per round (px)')
    ap.add_argument('--colmap-dir', type=Path, default=None, help='default WORKDIR/colmap')
    ap.add_argument('--fresh', action='store_true', help='delete any previous colmap directory first')
    args = ap.parse_args()

    pycolmap = import_pycolmap()
    import torch
    if not args.verbose:
        pycolmap.logging.minloglevel = 1

    info = json.loads((args.workdir / 'frames.json').read_text())
    frames = info['frames']
    target = np.array(info['target_enu'])
    out = args.colmap_dir or args.workdir / 'colmap'
    if args.fresh and out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True, exist_ok=True)
    db_path = out / 'database.db'
    images = args.workdir / 'images'
    names = [f['name'] for f in frames]
    K = np.array(info['K'])
    device = 'cuda' if torch.cuda.is_available() else 'cpu'

    t0 = time.time()
    if not db_path.exists():
        params = [K[0, 0], K[0, 2], K[1, 2]] + {'SIMPLE_RADIAL': [0.0], 'RADIAL': [0.0, 0.0]}.get(args.camera_model, [])
        if args.camera_model == 'PINHOLE':
            params = [K[0, 0], K[1, 1], K[0, 2], K[1, 2]]
        reader = pycolmap.ImageReaderOptions(camera_model=args.camera_model,
                                             camera_params=','.join('%.6f' % p for p in params))
        extraction = pycolmap.FeatureExtractionOptions()
        extraction.sift.max_num_features = args.max_features
        extraction.use_gpu = device == 'cuda'
        print('extracting features from %d images' % len(names), flush=True)
        pycolmap.extract_features(str(db_path), str(images), image_names=names,
                                  camera_mode=pycolmap.CameraMode.SINGLE, camera_model=args.camera_model,
                                  reader_options=reader, extraction_options=extraction)
        db = pycolmap.Database.open(str(db_path))
        for camera in db.read_all_cameras():
            camera.has_prior_focal_length = True
            db.update_camera(camera)
        cov = np.eye(3) * args.prior_sigma ** 2
        by_name = {im.name: im.image_id for im in db.read_all_images()}
        for f in frames:
            prior = pycolmap.PosePrior(np.array(f['position']), cov, pycolmap.PosePriorCoordinateSystem.CARTESIAN)
            db.write_pose_prior(by_name[f['name']], prior)
        db.close()
        print('features done in %.0f s' % (time.time() - t0), flush=True)

    db = pycolmap.Database.open(str(db_path))
    by_name = {im.name: im.image_id for im in db.read_all_images()}
    ids = [by_name[n] for n in names]
    pairs_path = out / 'pairs.txt'
    if not pairs_path.exists():
        pairs = select_pairs(frames, target, args.max_angle, args.max_scale, args.neighbours, args.sequential)
        print('matching %d pairs on %s' % (len(pairs), device), flush=True)
        t1 = time.time()
        counts = match_pairs(db, ids, pairs, args.ratio, device)
        tmp = pairs_path.with_suffix('.tmp')
        tmp.write_text(''.join('%s %s\n' % (names[i], names[j]) for i, j in pairs))
        tmp.rename(pairs_path)
        print('matching done in %.0f s, median %d matches/pair' % (time.time() - t1, np.median(counts)), flush=True)
    db.close()

    db = pycolmap.Database.open(str(db_path))
    verified = db.num_verified_image_pairs()
    db.close()
    if verified == 0:
        t1 = time.time()
        print('geometric verification', flush=True)
        pycolmap.verify_matches(str(db_path), str(pairs_path), pycolmap.TwoViewGeometryOptions())
        db = pycolmap.Database.open(str(db_path))
        print('%d verified pairs in %.0f s' % (db.num_verified_image_pairs(), time.time() - t1), flush=True)
        db.close()

    sparse = out / 'sparse'
    if sparse.exists():
        shutil.rmtree(sparse)
    sparse.mkdir()
    if args.mapper == 'prior':
        db = pycolmap.Database.open(str(db_path))
        camera = db.read_all_cameras()[0]
        db.close()
        t1 = time.time()
        print('telemetry-seeded mapping', flush=True)
        best = prior_mapping(db_path, images, sparse, frames, camera, ids,
                             [float(x) for x in args.thresholds.split(',')], args.prior_sigma, device == 'cuda')
        print('mapping done in %.0f s: %d images, %d points' % (time.time() - t1, best.num_reg_images(),
                                                              best.num_points3D()), flush=True)
    else:
        opts = pycolmap.IncrementalPipelineOptions()
        opts.use_prior_position = True
        opts.use_robust_loss_on_prior_position = True
        opts.ba_refine_principal_point = False
        opts.multiple_models = True
        t1 = time.time()
        print('incremental mapping', flush=True)
        registered = [0]

        def progress():
            registered[0] += 1
            if registered[0] % 100 == 0:
                print('  registered %d images (%.0f s)' % (registered[0], time.time() - t1), flush=True)
        recs = pycolmap.incremental_mapping(str(db_path), str(images), str(sparse), opts,
                                            next_image_callback=progress)
        if not recs:
            fatal('mapping failed')
        best = max(recs.values(), key=lambda r: r.num_reg_images())
        print('mapping done in %.0f s: %d models, largest has %d/%d images, %d points'
              % (time.time() - t1, len(recs), best.num_reg_images(), len(names), best.num_points3D()), flush=True)
    final = sparse / 'final'
    final.mkdir(exist_ok=True)
    best.write(str(final))

    # report residual from telemetry so gross misalignment is obvious
    pos = {f['name']: np.array(f['position']) for f in frames}
    errs = [np.linalg.norm(im.projection_center() - pos[im.name]) for im in best.images.values()]
    print('camera centre vs telemetry: median %.1f m, 90%% %.1f m' % (np.median(errs), np.percentile(errs, 90)))
    print(best.summary())


if __name__ == '__main__':
    main()
