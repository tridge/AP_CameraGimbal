#!/usr/bin/env python3
"""Build the G1 Brush dataset from a native reference workdir, using train.py's own scene loading
(view selection, frozen holdouts, undistortion with the refined camera, temperature scale,
spatially filtered initial points valued from the radiometric frames).

Writes OUT/:
  sparse/0/{cameras,images,points3D}.txt  undistorted PINHOLE camera, chosen views (world->camera)
  images/NAME.png    scaled temperature as 16-bit grayscale, v = round(65535 * scale(T)), no gamma
  init.ply           initial Gaussians (train.py init_params: thermal SH DC, knn scales, opacity 0.1)
  eval_views.txt     frozen holdout names (train.py --holdout/--hot-holdout)
  g1.json            scale, hot threshold, names, splits, intrinsics, poses, apcg_splat metadata
  gt.npy             float32 [n, H, W] scaled targets before 16-bit quantisation (for metrics)
  brush_args.txt     Brush options equivalent to train.py's for this dataset
"""
import argparse
import json
import math
from pathlib import Path
import sys

import numpy as np

GS = Path(__file__).resolve().parents[3]  # tools/GaussianSplat
sys.path.insert(0, str(GS))


def train_args(extra):
    """train.py's argument defaults (SH-only baseline) plus overrides."""
    import train
    saved = sys.argv
    sys.argv = ['train.py', 'WORKDIR'] + extra
    try:
        ap_args = {}
        orig = train.argparse.ArgumentParser.parse_args

        def capture(self, *a, **k):
            ns = orig(self, *a, **k)
            ap_args['ns'] = ns
            raise SystemExit(0)
        train.argparse.ArgumentParser.parse_args = capture
        try:
            train.main()
        except SystemExit:
            pass
        finally:
            train.argparse.ArgumentParser.parse_args = orig
    finally:
        sys.argv = saved
    return ap_args['ns']


def write_ply(path, xyz, val, sh_degree, seed=0):
    from plyfile import PlyData, PlyElement
    import torch
    from train import knn_scale
    from splat_model import SH_C0
    n = len(xyz)
    torch.manual_seed(seed)
    dist = knn_scale(xyz).numpy()
    dc = ((val - 0.5) / SH_C0).astype(np.float32)
    n_rest = 3 * ((sh_degree + 1) ** 2 - 1)
    q = torch.rand(n, 4).numpy()  # as train.py init_params
    q /= np.linalg.norm(q, axis=1, keepdims=True)
    attrs = ['x', 'y', 'z', 'nx', 'ny', 'nz', 'f_dc_0', 'f_dc_1', 'f_dc_2'] + \
        ['f_rest_%d' % i for i in range(n_rest)] + ['opacity', 'scale_0', 'scale_1', 'scale_2',
                                                     'rot_0', 'rot_1', 'rot_2', 'rot_3']
    el = np.zeros(n, dtype=[(a, 'f4') for a in attrs])
    el['x'], el['y'], el['z'] = xyz[:, 0], xyz[:, 1], xyz[:, 2]
    for c in range(3):
        el['f_dc_%d' % c] = dc
    el['opacity'] = math.log(0.1 / 0.9)
    for c in range(3):
        el['scale_%d' % c] = np.log(dist)
    for c in range(4):
        el['rot_%d' % c] = q[:, c]
    PlyData([PlyElement.describe(el, 'vertex')]).write(str(path))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('workdir', type=Path, help='native reference workdir (frames.json, thermal/, colmap/)')
    ap.add_argument('out', type=Path)
    ap.add_argument('--sparse', type=Path, default=None)
    ap.add_argument('--train-arg', action='append', default=[], help='extra train.py option (repeatable)')
    a = ap.parse_args()

    import cv2
    import train
    args = train_args(a.train_arg)
    sparse = a.sparse or a.workdir / 'colmap' / 'sparse' / 'final'
    scene = train.load_scene(a.workdir, sparse, args, 'cpu')
    out = a.out
    (out / 'sparse' / '0').mkdir(parents=True, exist_ok=True)
    (out / 'images').mkdir(exist_ok=True)

    names = scene['names']
    gt = scene['gt'].float().numpy()  # train.py's float16 targets, as it trains on them
    # train.py quantises targets to float16; recompute float32 targets for the 16-bit transport
    info = scene['info']
    frames = {f['name']: f for f in info['frames']}
    K = scene['K'].numpy().astype(np.float64)
    import pycolmap
    rec = pycolmap.Reconstruction(str(sparse))
    cam = list(rec.cameras.values())[0]
    # as train.load_scene: SIMPLE_RADIAL frames are undistorted to the same K, PINHOLE used as is
    p = cam.params
    if cam.model.name == 'SIMPLE_RADIAL':
        f, cx, cy, k1 = p
        Kd = np.array([[f, 0, cx], [0, f, cy], [0, 0, 1]])
        maps = cv2.initUndistortRectifyMap(Kd, np.array([k1, 0, 0, 0]), None, Kd, (cam.width, cam.height),
                                           cv2.CV_32FC1)
    elif cam.model.name == 'PINHOLE':
        Kd, maps = np.array([[p[0], 0, p[2]], [0, p[1], p[3]], [0, 0, 1]]), None
    else:
        sys.exit('unsupported camera model %s' % cam.model.name)
    if not np.allclose(Kd, K):
        sys.exit('camera intrinsics differ from train.load_scene: %s vs %s' % (Kd.tolist(), K.tolist()))
    W, H = scene['W'], scene['H']
    gt32 = np.empty((len(names), H, W), np.float32)
    for n, name in enumerate(names):
        fr = frames[name]
        raw = cv2.imread(str(a.workdir / 'thermal' / name), cv2.IMREAD_UNCHANGED)
        c = raw.astype(np.float32) * fr['temperature_scale_k'] + fr['temperature_offset_k'] - 273.15
        if maps is not None:
            c = cv2.remap(c, maps[0], maps[1], cv2.INTER_LINEAR, borderMode=cv2.BORDER_REFLECT)
        gt32[n] = scene['scale'].forward(c)
        cv2.imwrite(str(out / 'images' / name), np.round(gt32[n] * 65535).astype(np.uint16))
    err16 = float(np.abs(gt32 - gt).max())
    if err16 > 1e-3:  # float16 rounding is below 5e-4
        sys.exit('targets differ from train.load_scene by %.2e' % err16)
    print('float32 targets vs train.py float16 targets: max |diff| %.2e' % err16)
    np.save(out / 'gt.npy', gt32)

    (out / 'sparse/0/cameras.txt').write_text(
        '# CAMERA_ID MODEL WIDTH HEIGHT PARAMS[]\n1 PINHOLE %d %d %.10g %.10g %.10g %.10g\n'
        % (W, H, K[0, 0], K[1, 1], K[0, 2], K[1, 2]))
    from scipy.spatial.transform import Rotation
    lines = ['# IMAGE_ID QW QX QY QZ TX TY TZ CAMERA_ID NAME', '# (empty points line)']
    vms = scene['viewmats'].numpy().astype(np.float64)
    for n, (name, vm) in enumerate(zip(names, vms)):
        x, y, z, w = Rotation.from_matrix(vm[:3, :3]).as_quat()
        t = vm[:3, 3]
        lines.append('%d %.12g %.12g %.12g %.12g %.12g %.12g %.12g 1 %s' % (n + 1, w, x, y, z, *t, name))
        lines.append('')
    (out / 'sparse/0/images.txt').write_text('\n'.join(lines) + '\n')
    (out / 'sparse/0/points3D.txt').write_text('# POINT3D_ID X Y Z R G B ERROR TRACK[]\n')

    write_ply(out / 'init.ply', scene['xyz'], scene['val'], args.sh_degree)
    test = sorted(scene['test'])
    (out / 'eval_views.txt').write_text('\n'.join(test) + '\n')

    v_hot = float(scene['scale'].forward(np.array(scene['hot_c'])))
    hotspot = scene['hotspot']
    apcg = dict(kind='thermal', up=[0, 0, 1], target=(hotspot or {}).get('enu', info['target_enu']),
                temperature_scale=scene['scale'].todict(), origin=info['origin'])
    meta = dict(names=names, test=test, hot=sorted(scene['hot']), hot_c=scene['hot_c'], v_hot=v_hot,
                temperature_scale=scene['scale'].todict(), width=W, height=H, K=K.tolist(),
                viewmats=vms.tolist(), initial_points=len(scene['xyz']), hotspot=hotspot,
                apcg_splat=apcg, train_options=vars(args) | dict(workdir=None),
                gt_quantisation=dict(transport='png16', max_vs_train_py_float16=err16))
    (out / 'g1.json').write_text(json.dumps(meta, indent=1, default=str))
    # Brush equivalents of train.py's choices (Brush's default black background; no noise, SSIM 0.2, hot weight)
    brush = ['--total-train-iters', str(args.iterations), '--sh-degree', str(args.sh_degree),
             '--ssim-weight', str(args.ssim_weight),
             '--background-noise-strength', '0', '--hot-weight', str(args.hot_weight),
             '--hot-threshold', '%.8f' % v_hot, '--max-resolution', str(max(W, H)),
             '--max-splats', str(args.cap),
             # thermal contrast is low: train.py densifies at 1/4 of the RGB gradient threshold;
             # 0.0004 (Brush RGB default 0.0025) gives about train.py's Gaussian count here
             '--growth-grad-threshold', '0.0004']
    (out / 'brush_args.txt').write_text(' '.join(brush) + '\n')
    print('%d views (%d held out: %s), %d initial points, v_hot %.4f, scale %s'
          % (len(names), len(test), ' '.join(test), len(scene['xyz']), v_hot, scene['scale'].todict()))


if __name__ == '__main__':
    main()
