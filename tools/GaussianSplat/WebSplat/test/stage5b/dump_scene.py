#!/usr/bin/env python3
"""Run train.py's scene loading only (its own main() up to the optimiser and densification set-up,
no training) and dump it in the CONTRACTS.md 5b dataset form for compare_dataset.py.

  dump_scene.py WORKDIR SPARSE OUT [--options scale.json] [--train-arg=--max-images=60 ...]

Captured from train.py itself (monkeypatched, nothing re-implemented): load_scene's views, holdouts,
hot set, temperature scale, float16 targets, undistorted Celsius images (cv2.remap outputs), bounds,
canopy ceiling, initial points and values, hot spot; main()'s Adam learning rates, means LR
schedule, DefaultStrategy arguments and scene_scale. The hot spot re-located after training with
the refined poses (main()'s second locate_hotspot call) is recomputed the way main() does it.
Writes OUT/{dataset.json, targets.f16, celsius.f32, points.f32, points64.npy, ceiling.f32}.
"""
import argparse
import dataclasses
import json
from pathlib import Path
import sys

import numpy as np

GS = Path(__file__).resolve().parents[3]  # tools/GaussianSplat
sys.path.insert(0, str(GS))


class Captured(Exception):
    pass


def option_argv(options):
    """train.py command-line flags from a scale.json 'options' dict."""
    argv = []
    for k, v in options.items():
        if k in ('workdir', 'sparse', 'out'):
            continue
        flag = '--' + k.replace('_', '-')
        if v is True:
            argv.append(flag)
        elif v is False or v is None:
            continue
        elif isinstance(v, list):
            argv += [flag, ','.join(str(x) for x in v)]
        else:
            argv += [flag, str(v)]
    return argv


def rejections(train, rec, scene, args):
    """Per-view rejection reasons, from train.choose_images itself run with later filters relaxed:
    elevation (min_obs 0, no pose-offset limit, no budget), then min_obs, then pose offset, then the
    cold budget. The pose-offset limit is choose_images' formula on the views left by min_obs."""
    import copy
    frames = {f['name']: f for f in scene['info']['frames']}
    target = np.array(scene['info']['target_enu'])
    ims = sorted(rec.images.values(), key=lambda im: im.name)

    def kept(**over):
        a = copy.copy(args)
        for k, v in over.items():
            setattr(a, k, v)
        return {im.name for im in train.choose_images(ims, frames, a, target)[0]}
    allv = {im.name for im in ims}
    elev = kept(min_obs=0, max_pose_offset=float('inf'), max_images=0)
    obs = kept(max_pose_offset=float('inf'), max_images=0)
    pose = kept(max_images=0)
    chosen = set(scene['names'])
    off = [np.linalg.norm(im.projection_center() - np.array(frames[im.name]['position'])) for im in ims
           if im.name in obs]
    limit = max(args.max_pose_offset, 4 * float(np.median(off))) if off else args.max_pose_offset
    return dict(elevation=sorted(allv - elev), min_obs=sorted(elev - obs), pose_offset=sorted(obs - pose),
                budget=sorted(pose - chosen), pose_offset_limit_m=limit)


def init_constants(train):
    """Initialisation and rendering constants as written in train.py's source."""
    import inspect
    import re
    src = inspect.getsource(train)
    one = lambda pat: re.findall(pat, src)  # noqa: E731
    op = one(r'opacities=torch\.logit\(torch\.full\(\(n,\), ([0-9.e-]+)')
    ms = one(r"d\[:, 1:\]\.mean\(1\)\)\.float\(\)\.clamp\(min=([0-9.e-]+)\)")
    npl = one(r'near_plane=([0-9.e-]+)\)')
    quats = one(r'quats=(torch\.rand)\(')
    prune = one(r'if step % (strategy\.refine_every) == 0:\n\s+with torch\.no_grad\(\):\n\s+drop = outside')
    seed = one(r'torch\.manual_seed\((\d+)\)')
    shs = one(r'render\(\[i\], min\(step // (\d+), args\.sh_degree\)\)')
    und = one(r'cv2\.initUndistortRectifyMap\(K, dist, None, K, \(W, H\), cv2\.CV_32FC1\)')
    rmp = one(r'cv2\.remap\(c, maps\[0\], maps\[1\], cv2\.INTER_LINEAR, borderMode=cv2\.BORDER_REFLECT\)')
    found = [op, ms, npl, quats, prune, seed, shs, und, rmp]
    if any(len(x) != 1 for x in found):
        sys.exit('train.py source changed: constants not found (%s)' % found)
    k = inspect.signature(train.knn_scale).parameters['k'].default
    return dict(opacity=float(op[0]), knn=k, min_scale=float(ms[0]), quats=quats[0], seed=int(seed[0]),
                near_plane=float(npl[0]), prune_outside_bounds='every refine_every steps',
                sh_degree_schedule='min(step // %s, sh_degree)' % shs[0],
                # the contract's description of exactly these two calls (load_scene, on float32 Celsius)
                undistortion_method='cv2.initUndistortRectifyMap(K, [k1,0,0,0], newK=K) + remap INTER_LINEAR, '
                                    'BORDER_REFLECT, on celsius float32')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('workdir', type=Path)
    ap.add_argument('sparse', type=Path)
    ap.add_argument('out', type=Path)
    ap.add_argument('--options', type=Path, help='scale.json whose "options" are train.py arguments')
    ap.add_argument('--train-arg', action='append', default=[], help='extra train.py argument (repeatable)')
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)

    import cv2
    import torch
    import gsplat.strategy
    import train

    extra = option_argv(json.loads(a.options.read_text())['options']) if a.options else []
    extra += [x for arg in a.train_arg for x in arg.split('=', 1)]
    cap = dict(remap=[], adam={})

    orig_remap = cv2.remap

    def remap(*args, **kw):
        out = orig_remap(*args, **kw)
        cap['remap'].append(out.copy())
        return out

    orig_load = train.load_scene

    def load_scene(workdir, sparse, args, device):
        cap['args'] = args
        cap['scene'] = orig_load(workdir, sparse, args, 'cpu')
        return cap['scene']

    orig_init = train.init_params
    orig_adam = torch.optim.Adam

    class Adam(orig_adam):
        def __init__(self, groups, **kw):
            for g in groups:
                cap['adam'][g['name']] = dict(lr=g['lr'], eps=kw.get('eps'))
            super().__init__(groups, **kw)

    orig_exp = torch.optim.lr_scheduler.ExponentialLR

    class ExponentialLR(orig_exp):
        def __init__(self, opt, gamma, **kw):
            cap['means_gamma'] = gamma
            super().__init__(opt, gamma, **kw)

    real_strategy = gsplat.strategy.DefaultStrategy

    class Strategy:
        def __init__(self, **kw):
            cap['strategy_args'] = kw
            cap['strategy'] = dataclasses.asdict(real_strategy(**kw))
            self.refine_every = kw.get('refine_every')

        def check_sanity(self, *a, **k):
            pass

        def initialize_state(self, scene_scale):
            cap['scene_scale'] = scene_scale
            raise Captured()

    train.load_scene = load_scene
    train.init_params = lambda scene, args, device: orig_init(scene, args, 'cpu')
    train.cv2.remap = remap
    torch.optim.Adam = Adam
    torch.optim.lr_scheduler.ExponentialLR = ExponentialLR
    gsplat.strategy.DefaultStrategy = Strategy
    sys.argv = ['train.py', str(a.workdir), '--sparse', str(a.sparse), '--out', str(a.out / 'train_out')] + extra
    try:
        train.main()
        sys.exit('train.main() did not reach the strategy set-up')
    except Captured:
        pass
    finally:
        train.cv2.remap = orig_remap

    import pycolmap
    s, args = cap['scene'], cap['args']
    rec = pycolmap.Reconstruction(str(a.sparse))
    names = s['names']
    gt = s['gt'].numpy()
    vms = s['viewmats'].numpy()
    hot_ids = [i for i, n in enumerate(names) if n in s['hot']]
    # main(): re-located after training with the refined poses (= SfM poses without --pose-opt)
    post = train.locate_hotspot(s['gt'].float().numpy(), vms, s['K'].numpy(), hot_ids)
    v_hot = float(s['scale'].forward(np.array(s['hot_c'])))
    extent = float(np.linalg.norm(s['centres'] - s['centres'].mean(0), axis=1).max())
    if len(cap['remap']) not in (0, len(names)):
        sys.exit('captured %d remaps for %d images' % (len(cap['remap']), len(names)))
    if cap['remap']:
        np.stack(cap['remap']).astype(np.float32).tofile(a.out / 'celsius.f32')
    gt.astype(np.float16).view(np.uint16).tofile(a.out / 'targets.f16')
    pts = np.concatenate([s['xyz'], s['val'][:, None]], 1)
    np.save(a.out / 'points64.npy', pts)
    pts.astype(np.float32).tofile(a.out / 'points.f32')
    ceil = s['bounds']['ceiling']
    ceil['grid'].astype(np.float32).tofile(a.out / 'ceiling.f32')
    K = s['K'].numpy().astype(np.float64)
    cam = list(rec.cameras.values())[0]
    cp = [float(x) for x in cam.params]
    if cam.model.name == 'SIMPLE_RADIAL':
        K64 = [[cp[0], 0, cp[1]], [0, cp[0], cp[2]], [0, 0, 1]]
    else:  # PINHOLE (load_scene refuses other models)
        K64 = [[cp[0], 0, cp[2]], [0, cp[1], cp[3]], [0, 0, 1]]
    skipped = rejections(train, rec, s, args)
    frames_pos = {f['name']: k for k, f in enumerate(s['info']['frames'])}
    by_name = {im.name: im.image_id for im in rec.images.values()}
    ds = dict(
        format='websplat.dataset', version=1, producer='train.py load_scene (test/stage5b/dump_scene.py)',
        width=s['W'], height=s['H'], K=K64,
        K_float32=K.tolist(), camera=dict(model=cam.model.name, params=[float(p) for p in cam.params]),
        scale=s['scale'].todict(), hot_c=s['hot_c'], v_hot=v_hot,
        images=[dict(name=n, frame=frames_pos[n], image_id=by_name[n],
                     split='test' if n in s['test'] else 'train', hot=n in s['hot'],
                     viewmat=[float(x) for x in vms[i].ravel()]) for i, n in enumerate(names)],
        registered=sorted(im.name for im in rec.images.values()), skipped=skipped,
        init=init_constants(train),
        bounds={k: v for k, v in s['bounds'].items() if k != 'ceiling'} | dict(
            ceiling=dict(origin=ceil['origin'], cell=ceil['cell'], n=len(ceil['grid']), file='ceiling.f32')),
        hotspot=s['hotspot'], hotspot_post=post, extent_m=extent,
        lr={k: v['lr'] for k, v in cap['adam'].items()}, adam_eps={k: v['eps'] for k, v in cap['adam'].items()},
        means_lr_gamma=cap['means_gamma'], strategy=cap['strategy'], strategy_args=cap['strategy_args'],
        scene_scale=cap['scene_scale'],
        targets=dict(file='targets.f16', dtype='f16', shape=list(gt.shape)),
        celsius=dict(file='celsius.f32', dtype='f32', shape=list(gt.shape)) if cap['remap'] else None,
        points=dict(count=len(pts), file='points.f32', dtype='f32', shape=[len(pts), 4], order='pycolmap points3D iteration'),
        options={k: (v if not isinstance(v, Path) else str(v)) for k, v in vars(args).items()
                 if k not in ('workdir', 'sparse', 'out')},
    )
    (a.out / 'dataset.json').write_text(json.dumps(ds, indent=1, default=str))
    print('%d views (%d test), %d hot, scale %s, %d points, hot spot %s'
          % (len(names), len(s['test']), len(s['hot']), s['scale'].todict(), len(pts),
             np.round(s['hotspot']['enu'], 3) if s['hotspot'] else None))


if __name__ == '__main__':
    main()
