#!/usr/bin/env python3
"""Train an RGB Gaussian splat from an sfm.py reconstruction of extract_rgb_frames.py output.

A check on the RGB camera poses: if the RGB splat looks like the scene, the poses are
consistent. Images are undistorted to a pinhole camera and scaled to --width, then
trained with plain 3DGS (SH colour, L1 + SSIM). Writes OUT/{splat.ply, preview_*.jpg, metrics.json};
splat.ply is a standard 3DGS PLY for any splat viewer (e.g. SuperSplat).

  train_rgb.py RGBWORK --out RGBWORK/splat_rgb
"""
import argparse
import json
from pathlib import Path
import sys
import time

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from splat_common import fatal, import_pycolmap, setup_cuda_env  # noqa: E402

setup_cuda_env()
import torch  # noqa: E402
import torch.nn.functional as F  # noqa: E402
from splat_model import SH_C0  # noqa: E402
from train import canopy_ceiling, knn_scale, save_ply, ssim  # noqa: E402


def undistorted_camera(cam, width):
    """Pinhole intrinsics at the given width and undistortion maps from a COLMAP camera."""
    s = width / cam.width
    h = round(cam.height * s)
    p = cam.params
    if cam.model.name in ('SIMPLE_RADIAL', 'RADIAL'):
        f, cx, cy = p[0], p[1], p[2]
        dist = np.array([p[3], p[4] if cam.model.name == 'RADIAL' else 0.0, 0, 0])
        K0 = np.array([[f, 0, cx], [0, f, cy], [0, 0, 1]])
    elif cam.model.name == 'PINHOLE':
        K0 = np.array([[p[0], 0, p[2]], [0, p[1], p[3]], [0, 0, 1]])
        dist = np.zeros(4)
    else:
        fatal('unsupported camera model %s' % cam.model.name)
    K0[:2] *= s  # images are resized before undistortion
    maps = cv2.initUndistortRectifyMap(K0, dist, None, K0, (width, h), cv2.CV_32FC1)
    K = K0
    return K, h, maps


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('workdir', type=Path)
    ap.add_argument('--sparse', type=Path, help='COLMAP model (default WORKDIR/colmap/sparse/final)')
    ap.add_argument('--out', type=Path, help='output directory (default WORKDIR/splat_rgb)')
    ap.add_argument('--width', type=int, default=1280, help='training image width')
    ap.add_argument('--iterations', type=int, default=30000)
    ap.add_argument('--sh-degree', type=int, default=1,
                    help='view-dependent colour degree; higher degrees go wild when seen from angles not flown')
    ap.add_argument('--holdout', type=int, default=25, help='hold out every Nth image (0 = none)')
    ap.add_argument('--radius', type=float, default=200.0, help='drop Gaussians further than this from the model centre (m)')
    ap.add_argument('--max-vertical', type=float,
                    help='drop Gaussians whose vertical extent (1 sigma) exceeds this (m, 0 = keep); from above '
                         'nothing limits it and they show as haze when viewed obliquely; default: 1 m or half '
                         'the max scale, whichever is larger')
    ap.add_argument('--ceiling-cell', type=float, default=5.0, help='height ceiling grid cell (m)')
    ap.add_argument('--ceiling-margin', type=float, default=5.0,
                    help='drop Gaussians this far above the local top of the SfM points (m, 0 = no ceiling)')
    ap.add_argument('--cap', type=int, default=3000000, help='maximum number of Gaussians')
    ap.add_argument('--max-scale', type=float,
                    help='prune Gaussians larger than this (m); default: radius/100 or 16 ground pixels at '
                         'the median camera distance, whichever is larger')
    ap.add_argument('--ssim-weight', type=float, default=0.2)
    ap.add_argument('--video-range', type=lambda v: [float(x) for x in v.split(',')],
                    help='only use frames between these video times, START,END in s')
    args = ap.parse_args()

    from gsplat import rasterization
    from gsplat.strategy import DefaultStrategy
    from gsplat.strategy.ops import remove
    pycolmap = import_pycolmap()
    device = 'cuda'
    torch.manual_seed(0)
    out = args.out or args.workdir / 'splat_rgb'
    out.mkdir(parents=True, exist_ok=True)
    rec = pycolmap.Reconstruction(str(args.sparse or args.workdir / 'colmap' / 'sparse' / 'final'))
    images = sorted(rec.images.values(), key=lambda im: im.name)
    if args.video_range:  # frame names are video milliseconds
        images = [im for im in images if args.video_range[0] <= int(Path(im.name).stem) / 1000 <= args.video_range[1]]
    if len({im.camera_id for im in images}) != 1:
        fatal('expected one shared camera')
    K, H, maps = undistorted_camera(rec.cameras[images[0].camera_id], args.width)
    W = args.width

    t0 = time.time()
    gt, viewmats = [], []
    for im in images:
        img = cv2.imread(str(args.workdir / 'images' / im.name))
        img = cv2.resize(img, (W, H), interpolation=cv2.INTER_AREA)
        img = cv2.remap(img, maps[0], maps[1], cv2.INTER_LINEAR)
        gt.append(torch.from_numpy(img[..., ::-1].copy()))  # RGB uint8, kept on the CPU
        m = np.eye(4)
        m[:3, :4] = im.cam_from_world().matrix()
        viewmats.append(m)
    viewmats = torch.tensor(np.array(viewmats), dtype=torch.float32, device=device)
    Kt = torch.tensor(K, dtype=torch.float32, device=device)[None]
    print('%d images %dx%d loaded in %.0f s' % (len(images), W, H, time.time() - t0), flush=True)
    test = set(range(args.holdout // 2, len(images), args.holdout)) if args.holdout else set()
    train_ids = [i for i in range(len(images)) if i not in test]

    pts = rec.points3D
    xyz = np.array([p.xyz for p in pts.values()])
    rgb = np.array([p.color for p in pts.values()]) / 255.0
    centres = np.array([im.projection_center() for im in images])
    centre = np.median(xyz, axis=0)
    keep = np.linalg.norm(xyz[:, :2] - centre[:2], axis=1) < args.radius
    xyz, rgb = xyz[keep], rgb[keep]
    ceiling = None
    if args.ceiling_margin > 0:
        # floaters: with few viewing directions (e.g. a nadir hover) Gaussians near the cameras explain
        # a few views and nothing contradicts them; nothing real is far above the SfM surface
        top = float(np.percentile(xyz[:, 2], 99.5)) + args.ceiling_margin
        ceiling = canopy_ceiling(xyz, centre[:2], args.radius, args.ceiling_cell, args.ceiling_margin, top)
        print('height ceiling: median %.1f m, max %.1f m' % (np.median(ceiling['grid']), top), flush=True)
    n = len(xyz)
    print('%d initial points' % n, flush=True)
    sh = torch.zeros(n, (args.sh_degree + 1) ** 2, 3)
    sh[:, 0] = torch.from_numpy((rgb - 0.5) / SH_C0).float()
    params = torch.nn.ParameterDict({k: torch.nn.Parameter(v.to(device)) for k, v in dict(
        means=torch.from_numpy(xyz).float(), scales=torch.log(knn_scale(xyz))[:, None].repeat(1, 3),
        quats=torch.rand(n, 4), opacities=torch.logit(torch.full((n,), 0.1)),
        sh0=sh[:, :1].contiguous(), shN=sh[:, 1:].contiguous()).items()})
    extent = float(np.linalg.norm(centres - centres.mean(0), axis=1).max())
    lrs = dict(means=1.6e-4 * extent, scales=5e-3, quats=1e-3, opacities=5e-2, sh0=2.5e-3, shN=2.5e-3 / 20)
    optimizers = {k: torch.optim.Adam([{'params': params[k], 'lr': lrs[k], 'name': k}], eps=1e-15) for k in params}
    sched = torch.optim.lr_scheduler.ExponentialLR(optimizers['means'], gamma=0.01 ** (1 / args.iterations))
    strategy = DefaultStrategy(refine_stop_iter=args.iterations // 2, reset_every=3000, absgrad=True,
                               grow_grad2d=0.0008, verbose=False)
    strategy.check_sanity(params, optimizers)
    if args.max_scale is None:
        # from high up a ground pixel is tens of cm and the ground needs Gaussians of several metres
        ground_px = float(np.median(np.linalg.norm(centres - centre, axis=1))) / K[0, 0]
        args.max_scale = max(args.radius / 100, 16 * ground_px)
    if args.max_vertical is None:
        args.max_vertical = max(1.0, args.max_scale / 2)
    print('pruning Gaussians larger than %.1f m, taller than %.1f m' % (args.max_scale, args.max_vertical), flush=True)
    state = strategy.initialize_state(scene_scale=args.max_scale * 10)  # gsplat prunes above 0.1 scene_scale
    c = torch.tensor(centre, dtype=torch.float32, device=device)
    if ceiling is not None:
        ceil_grid = torch.from_numpy(ceiling['grid']).to(device)
        ceil_origin = torch.tensor(ceiling['origin'], dtype=torch.float32, device=device)

    def above_ceiling(means):
        if ceiling is None:
            return torch.zeros(len(means), dtype=torch.bool, device=device)
        ij = ((means[:, :2] - ceil_origin) / ceiling['cell']).long().clamp(0, len(ceil_grid) - 1)
        return means[:, 2] > ceil_grid[ij[:, 0], ij[:, 1]]

    def too_tall(params):
        if args.max_vertical <= 0:
            return torch.zeros(len(params['means']), dtype=torch.bool, device=device)
        w, x, y, z = torch.nn.functional.normalize(params['quats'], dim=1).unbind(1)
        r2 = torch.stack([2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)], 1)
        return ((r2 * torch.exp(params['scales'])) ** 2).sum(1).sqrt() > args.max_vertical

    def render(i, sh_degree):
        vm = viewmats[i:i + 1]
        colors = torch.cat([params['sh0'], params['shN']], 1)
        img, _, info = rasterization(params['means'], params['quats'], torch.exp(params['scales']),
                                     torch.sigmoid(params['opacities']), colors, vm, Kt, W, H,
                                     sh_degree=sh_degree, packed=False, absgrad=True, near_plane=1.0)
        return img[0].clamp(0, 1), info

    order = []
    t0 = time.time()
    for step in range(args.iterations):
        if not order:
            order = list(np.random.permutation(train_ids))
        i = int(order.pop())
        pred, info = render(i, min(step // 1000, args.sh_degree))
        target = gt[i].to(device, non_blocking=True).float() / 255
        l1 = (pred - target).abs().mean()
        loss = (1 - args.ssim_weight) * l1 + args.ssim_weight * (
            1 - ssim(pred.permute(2, 0, 1)[None], target.permute(2, 0, 1)[None]))
        strategy.step_pre_backward(params, optimizers, state, step, info)
        loss.backward()
        for opt in optimizers.values():
            opt.step()
            opt.zero_grad(set_to_none=True)
        sched.step()
        strategy.step_post_backward(params, optimizers, state, step, info, packed=False)
        if step % strategy.refine_every == 0:
            with torch.no_grad():
                drop = ((params['means'][:, :2] - c[:2]).norm(dim=1) > args.radius) | above_ceiling(params['means'])
                if step >= args.iterations // 2:
                    drop |= too_tall(params)  # once the geometry has settled
                excess = len(params['means']) - int(drop.sum()) - args.cap
                if excess > 0:
                    o = params['opacities'].detach().clone()
                    o[drop] = float('inf')
                    drop[o.topk(excess, largest=False).indices] = True
            if drop.any():
                remove(params, optimizers, state, drop)
        if step % 1000 == 0 or step == args.iterations - 1:
            print('step %5d  loss %.4f  gaussians %d  mem %.1f GB  %.0f s' % (
                step, loss.item(), len(params['means']), torch.cuda.max_memory_allocated() / 1e9,
                time.time() - t0), flush=True)

    with torch.no_grad():
        tall = too_tall(params)
    if tall.any():
        remove(params, optimizers, state, tall)
    psnr = []
    with torch.no_grad():
        for k, i in enumerate(sorted(test)):
            pred, _ = render(i, args.sh_degree)
            target = gt[i].to(device).float() / 255
            psnr.append(float(-10 * torch.log10(((pred - target) ** 2).mean())))
            if k % max(1, len(test) // 12) == 0:
                both = torch.cat([target, pred], 1).cpu().numpy()
                cv2.imwrite(str(out / ('preview_%s' % images[i].name)), (both[..., ::-1] * 255).astype(np.uint8),
                            [cv2.IMWRITE_JPEG_QUALITY, 90])
    metrics = dict(images=len(images), heldout=len(test), width=W, height=H, gaussians=len(params['means']),
                   heldout_psnr_db=float(np.mean(psnr)) if psnr else None, seconds=time.time() - t0)
    (out / 'metrics.json').write_text(json.dumps(metrics, indent=1))
    frames_info = json.loads((args.workdir / 'frames.json').read_text())
    save_ply(out / 'splat.ply', params, params['sh0'], params['shN'], info=dict(
        kind='rgb', up=[0, 0, 1], target=frames_info.get('target_enu', centre.tolist()), origin=frames_info.get('origin'),
        rasterize_mode='classic'))
    # checkpoint for view_splat.py / splat_site.py
    torch.save(dict(params={k: v.detach().cpu() for k, v in params.items()}, sh_degree=args.sh_degree,
                    viewmats=viewmats.cpu(), K=Kt[0].cpu(), width=W, height=H, names=[im.name for im in images],
                    test=sorted(images[i].name for i in test)), out / 'splat.pt')
    (out / 'scale.json').write_text(json.dumps(dict(kind='rgb', target_enu=frames_info.get('target_enu', centre.tolist()),
                                                    origin=frames_info.get('origin'), gaussians=len(params['means']),
                                                    heldout_psnr_db=metrics['heldout_psnr_db']), indent=1))
    print('held-out PSNR %.2f dB over %d views; wrote %s (%d gaussians) in %.0f s' % (
        metrics['heldout_psnr_db'] or 0, len(test), out, len(params['means']), metrics['seconds']))


if __name__ == '__main__':
    main()
