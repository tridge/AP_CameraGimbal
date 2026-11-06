#!/usr/bin/env python3
"""Train a thermal Gaussian splat with gsplat from an sfm.py reconstruction.

Training uses the 16-bit raw frames mapped to a compressive temperature scale
(TempScale, stored in scale.json) so both background texture and hot features drive
the optimisation; rendered values decode to apparent temperature, not a calibrated
surface measurement. View-dependent emission uses SH, and optionally (--lobe) a
per-Gaussian emission lobe for sharply directional sources. Every frame showing a hot
feature is kept, the rest are spread evenly up to --max-images, and hot pixels get
extra loss weight. Pose and per-image gain/offset refinement are opt-in.
Writes OUT/{splat.pt, scale.json, splat.ply, splat_color.ply, metrics.json, preview_*.png}.
"""
import argparse
import json
import math
from pathlib import Path
import sys
import time

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from splat_common import fatal, setup_cuda_env, triangulate_rays

setup_cuda_env()
import torch  # noqa: E402
import torch.nn.functional as F  # noqa: E402
from splat_model import SH_C0, bake_lobe_into_sh, has_lobe, init_lobe, view_values  # noqa: E402


class TempScale:
    """v = log1p((T - t0)/k) / log1p((t1 - t0)/k), clamped to [0, 1]."""

    def __init__(self, t0, t1, k):
        self.t0, self.t1, self.k = t0, t1, k
        self.norm = math.log1p((t1 - t0) / k)

    def forward(self, celsius):
        x = np.clip(celsius - self.t0, 0, None) / self.k
        return np.clip(np.log1p(x) / self.norm, 0, 1)

    def inverse(self, v):
        return self.t0 + self.k * np.expm1(np.asarray(v) * self.norm)

    def todict(self):
        return dict(kind='log1p', t0_c=self.t0, t1_c=self.t1, k_c=self.k)


def ssim(a, b, window=11):
    """a, b: [B, C, H, W] in [0, 1]."""
    sigma = 1.5
    g = torch.exp(-(torch.arange(window, device=a.device) - window // 2) ** 2 / (2 * sigma ** 2))
    g = (g / g.sum()).float()
    w = (g[:, None] @ g[None, :])[None, None].repeat(a.shape[1], 1, 1, 1)
    conv = lambda x: F.conv2d(x, w, padding=window // 2, groups=a.shape[1])  # noqa: E731
    mu_a, mu_b = conv(a), conv(b)
    s_a = conv(a * a) - mu_a ** 2
    s_b = conv(b * b) - mu_b ** 2
    s_ab = conv(a * b) - mu_a * mu_b
    c1, c2 = 0.01 ** 2, 0.03 ** 2
    m = ((2 * mu_a * mu_b + c1) * (2 * s_ab + c2)) / ((mu_a ** 2 + mu_b ** 2 + c1) * (s_a + s_b + c2))
    return m.mean()


def so3_exp(w):
    """Rodrigues; w: [B, 3] -> [B, 3, 3]."""
    theta = w.norm(dim=1, keepdim=True).clamp(min=1e-12)
    k = w / theta
    K = torch.zeros(w.shape[0], 3, 3, device=w.device)
    K[:, 0, 1], K[:, 0, 2], K[:, 1, 2] = -k[:, 2], k[:, 1], -k[:, 0]
    K = K - K.transpose(1, 2)
    t = theta[:, :, None]
    return torch.eye(3, device=w.device)[None] + torch.sin(t) * K + (1 - torch.cos(t)) * (K @ K)


def choose_images(images, frames, args, target):
    """All hot frames plus evenly spread others; returns (chosen, test set, hot set, hot_c)."""
    def elevation(im):
        d = im.projection_center() - target
        return math.degrees(math.atan2(d[2], math.hypot(d[0], d[1])))
    images = [im for im in images if elevation(im) >= args.min_elevation]
    # images seeded from telemetry but barely tied to the point cloud keep telemetry-quality poses
    weak = [im.name for im in images if im.num_points3D < args.min_obs]
    if weak:
        print('skipping %d frames with fewer than %d SfM observations' % (len(weak), args.min_obs))
    images = [im for im in images if im.num_points3D >= args.min_obs]
    # drop SfM mis-registrations: camera far from its telemetry position compared with the rest
    offset = np.array([np.linalg.norm(im.projection_center() - np.array(frames[im.name]['position']))
                       for im in images])
    limit = max(args.max_pose_offset, 4 * float(np.median(offset)))
    bad = [im.name for im, o in zip(images, offset) if o > limit]
    if bad:
        print('skipping %d frames more than %.0f m from telemetry: %s' % (len(bad), limit, ', '.join(bad)))
    images = [im for im, o in zip(images, offset) if o <= limit]
    maxima = np.array([f['maximum_c'] for f in frames.values()])
    hot_c = float(np.median(maxima)) + args.hot_delta
    hot = [im for im in images if frames[im.name]['maximum_c'] > hot_c]
    cold = [im for im in images if frames[im.name]['maximum_c'] <= hot_c]
    n_cold = max(0, args.max_images - len(hot)) if args.max_images else len(cold)
    if n_cold < len(cold):
        cold = [cold[int(i)] for i in np.linspace(0, len(cold) - 1, n_cold)]
    chosen = sorted(hot + cold, key=lambda im: im.name)
    test = set()
    if args.holdout:
        test |= {im.name for im in cold[args.holdout // 2::args.holdout]}
    if args.hot_holdout:
        test |= {im.name for im in hot[args.hot_holdout // 2::args.hot_holdout]}
    return chosen, test, {im.name for im in hot}, hot_c


def load_scene(workdir, sparse, args, device):
    import pycolmap
    info = json.loads((workdir / 'frames.json').read_text())
    rec = pycolmap.Reconstruction(str(sparse))
    frames = {f['name']: f for f in info['frames']}
    target = np.array(info['target_enu'])

    cams = list(rec.cameras.values())
    if len(cams) != 1:
        fatal('expected one shared camera, got %d' % len(cams))
    cam = cams[0]
    W, H = cam.width, cam.height
    p = cam.params
    if cam.model.name == 'SIMPLE_RADIAL':
        f, cx, cy, k1 = p
        K = np.array([[f, 0, cx], [0, f, cy], [0, 0, 1]])
        dist = np.array([k1, 0, 0, 0])
    elif cam.model.name == 'PINHOLE':
        K = np.array([[p[0], 0, p[2]], [0, p[1], p[3]], [0, 0, 1]])
        dist = None
    else:
        fatal('unsupported camera model %s' % cam.model.name)
    maps = cv2.initUndistortRectifyMap(K, dist, None, K, (W, H), cv2.CV_32FC1) if dist is not None else None

    all_images = sorted(rec.images.values(), key=lambda im: im.name)
    images, test, hot, hot_c = choose_images(all_images, frames, args, target)
    print('%d of %d registered images (%d hot above %.1f C), %d held out'
          % (len(images), len(all_images), len(hot), hot_c, len(test)))
    raw_images = []
    for im in images:
        fr = frames[im.name]
        raw = cv2.imread(str(workdir / 'thermal' / im.name), cv2.IMREAD_UNCHANGED)
        c = raw.astype(np.float32) * fr['temperature_scale_k'] + fr['temperature_offset_k'] - 273.15
        if maps is not None:
            c = cv2.remap(c, maps[0], maps[1], cv2.INTER_LINEAR, borderMode=cv2.BORDER_REFLECT)
        raw_images.append(c)
    sample = np.concatenate([c[::8, ::8].ravel() for c in raw_images])
    t0 = args.t0 if args.t0 is not None else float(np.percentile(sample, 0.5)) - 2
    t1 = args.t1 if args.t1 is not None else float(max(np.max(c) for c in raw_images))
    scale = TempScale(t0, t1, args.k)
    print('temperature scale: %.1f .. %.1f C, knee %.1f C' % (t0, t1, scale.k))
    mapped = np.stack([scale.forward(c).astype(np.float16) for c in raw_images])
    del raw_images
    gt = torch.from_numpy(mapped).to(device)
    viewmats = torch.stack([torch.from_numpy(np.vstack([im.cam_from_world().matrix(), [0, 0, 0, 1]])).float()
                            for im in images]).to(device)

    # scene bounds: a cylinder about the target, floor from the robust SfM point spread, and a
    # canopy-height ceiling per grid cell so nothing survives in the empty air above the trees
    near = np.array([pt.xyz for pt in rec.points3D.values()
                     if np.linalg.norm(pt.xyz[:2] - target[:2]) <= args.radius * 1.2])
    z_lo, z_hi = np.percentile(near[:, 2], [1, 99])
    bounds = dict(centre=target[:2].tolist(), radius=args.radius * 1.1,
                  z_min=float(z_lo - args.height_margin), z_max=float(z_hi + args.height_margin))
    bounds['ceiling'] = canopy_ceiling(near, target[:2], args.radius * 1.2, args.ceiling_cell,
                                       args.ceiling_margin, bounds['z_max'])
    print('scene bounds: radius %.0f m, z %.1f .. %.1f m, canopy ceiling median %.1f m'
          % (bounds['radius'], bounds['z_min'], bounds['z_max'], np.median(bounds['ceiling']['grid'])))

    # initial points inside the bounds, valued from the radiometric frames that observe them
    index = {im.image_id: n for n, im in enumerate(images)}
    pts, obs, unseen = [], [], []
    for pt in rec.points3D.values():
        if np.linalg.norm(pt.xyz[:2] - target[:2]) > args.radius or \
                not bounds['z_min'] <= pt.xyz[2] <= ceiling_at(bounds['ceiling'], pt.xyz[None, :2])[0]:
            continue
        seen = [el for el in pt.track.elements if el.image_id in index][:4]
        k = len(pts)
        pts.append(pt.xyz)
        if not seen:
            unseen.append(k)  # e.g. points from an RGB reconstruction with poses transferred to thermal
        for el in seen:
            xy = rec.images[el.image_id].points2D[el.point2D_idx].xy
            obs.append((k, index[el.image_id], xy[0], xy[1]))
    if len(pts) < 100:
        fatal('only %d sparse points within %.0f m of the target' % (len(pts), args.radius))
    obs = np.array(obs, dtype=np.float64).reshape(-1, 4)
    xy = obs[:, 2:4]
    if dist is not None and len(xy):  # keypoints are distorted; frames were undistorted
        xy = cv2.undistortPoints(xy[:, None, :], K, dist, P=K)[:, 0, :]
    if unseen:
        # value points no thermal frame observed by projecting them into (undistorted) frames
        P = np.array(pts)[unseen]
        hits = np.zeros(len(unseen), dtype=int)
        extra = []
        for n, vm in enumerate(viewmats.cpu().numpy()):
            cam_pts = P @ vm[:3, :3].T + vm[:3, 3]
            z = cam_pts[:, 2]
            u = K[0, 0] * cam_pts[:, 0] / np.maximum(z, 1e-6) + K[0, 2]
            v = K[1, 1] * cam_pts[:, 1] / np.maximum(z, 1e-6) + K[1, 2]
            ok = np.where((z > 1) & (u >= 0) & (u < W) & (v >= 0) & (v < H) & (hits < 4))[0]
            hits[ok] += 1
            extra += [(unseen[i], n, u[i], v[i]) for i in ok]
        if extra:
            extra = np.array(extra, dtype=np.float64)
            obs = np.vstack([obs, extra])
            xy = np.vstack([xy, extra[:, 2:4]])
    xi, yi = np.round(xy[:, 0]).astype(int), np.round(xy[:, 1]).astype(int)
    ok = (xi >= 0) & (xi < W) & (yi >= 0) & (yi < H)
    k, n = obs[ok, 0].astype(int), obs[ok, 1].astype(int)
    sums = np.bincount(k, mapped[n, yi[ok], xi[ok]].astype(np.float64), minlength=len(pts))
    counts = np.bincount(k, minlength=len(pts))
    keep = counts > 0
    xyz = np.array(pts)[keep]
    val = sums[keep] / counts[keep]
    names = [im.name for im in images]
    print('%d images %dx%d, %d initial points' % (len(images), W, H, len(xyz)))
    hotspot = locate_hotspot(mapped, viewmats.cpu().numpy(), K, [n for n, im in enumerate(images) if im.name in hot])
    if hotspot:
        print('hot spot from SfM poses at ENU %s m (%d frames, median ray error %.1f px)'
              % (np.round(hotspot['enu'], 1), hotspot['frames'], hotspot['median_ray_error_px']))
    return dict(gt=gt, viewmats=viewmats, K=torch.tensor(K, dtype=torch.float32, device=device),
                W=W, H=H, xyz=xyz, val=val, names=names, bounds=bounds, hotspot=hotspot, test=test, hot=hot, hot_c=hot_c,
                centres=np.array([im.projection_center() for im in images]),
                scale=scale, target=target, info=info)


def canopy_ceiling(points, centre, radius, cell, margin, default):
    """Grid of the local top-of-canopy height (90th percentile of SfM points, 3x3 max) plus margin,
    capped at default (the global ceiling) since sparse SfM outliers sit high in the air."""
    from scipy.ndimage import maximum_filter
    n = int(np.ceil(2 * radius / cell))
    origin = np.asarray(centre) - radius
    ij = np.clip(((points[:, :2] - origin) / cell).astype(int), 0, n - 1)
    grid = np.full((n, n), np.nan)
    key = ij[:, 0] * n + ij[:, 1]
    order = np.argsort(key)
    key, z = key[order], points[order, 2]
    starts = np.flatnonzero(np.r_[True, key[1:] != key[:-1]])
    for a, b in zip(starts, np.r_[starts[1:], len(key)]):
        if b - a >= 10:
            grid.flat[key[a]] = np.percentile(z[a:b], 90)
    filled = maximum_filter(np.nan_to_num(grid, nan=-1e9), size=3)
    filled = np.minimum(np.where(filled < -1e8, default, filled + margin), default)
    return dict(origin=origin.tolist(), cell=cell, grid=filled.astype(np.float32))


def ceiling_at(ceiling, xy):
    g = ceiling['grid']
    ij = np.clip(((np.asarray(xy) - ceiling['origin']) / ceiling['cell']).astype(int), 0, len(g) - 1)
    return g[ij[:, 0], ij[:, 1]]


def locate_hotspot(values, viewmats, K, ids):
    """Triangulate the hottest pixel of each listed frame; values [n, H, W], viewmats [n, 4, 4] world->camera."""
    if len(ids) < 5:
        return None
    Ki = np.linalg.inv(K)
    W = values.shape[2]
    centres, dirs = [], []
    for i in ids:
        flat = int(np.argmax(values[i]))
        c2w = np.linalg.inv(viewmats[i])
        d = c2w[:3, :3] @ Ki @ np.array([flat % W, flat // W, 1.0])
        centres.append(c2w[:3, 3])
        dirs.append(d / np.linalg.norm(d))
    X, err = triangulate_rays(centres, dirs, float(K[0, 0]))
    return dict(enu=X.tolist(), median_ray_error_px=err, frames=len(ids))


def knn_scale(xyz, k=4):
    from scipy.spatial import cKDTree
    d, _ = cKDTree(xyz).query(xyz, k=k, workers=-1)
    return torch.from_numpy(d[:, 1:].mean(1)).float().clamp(min=1e-3)


def focus_seed(scene, centre, radius, n):
    """n extra points spread through a sphere about centre, valued like their nearest SfM point."""
    from scipy.spatial import cKDTree
    rng = np.random.default_rng(0)
    d = rng.normal(size=(n, 3))
    d *= (radius * rng.random(n) ** (1 / 3) / np.linalg.norm(d, axis=1))[:, None]
    pts = np.asarray(centre) + d
    _, j = cKDTree(scene['xyz']).query(pts, workers=-1)
    return pts, scene['val'][j]


def init_params(scene, args, device):
    xyz_np, val_np = scene['xyz'], scene['val']
    if args.focus_radius > 0 and args.focus_points > 0:
        pts, vals = focus_seed(scene, scene['focus'], args.focus_radius, args.focus_points)
        xyz_np, val_np = np.vstack([xyz_np, pts]), np.concatenate([val_np, vals])
    xyz = torch.from_numpy(xyz_np).float().to(device)
    n = len(xyz)
    dist = knn_scale(xyz_np).to(device)
    v = torch.from_numpy(val_np).float().to(device)
    sh = torch.zeros(n, (args.sh_degree + 1) ** 2, 3, device=device)
    sh[:, 0, :] = ((v - 0.5) / SH_C0)[:, None]
    p = dict(means=xyz, scales=torch.log(dist)[:, None].repeat(1, 3), quats=torch.rand(n, 4, device=device),
             opacities=torch.logit(torch.full((n,), 0.1, device=device)),
             sh0=sh[:, :1, :].contiguous(), shN=sh[:, 1:, :].contiguous())
    if args.lobe:
        p.update(init_lobe(n, device))
    return torch.nn.ParameterDict({k: torch.nn.Parameter(v) for k, v in p.items()})


def save_ply(path, params, sh0, shN):
    """Standard 3DGS PLY."""
    from plyfile import PlyData, PlyElement
    means = params['means'].detach().cpu().numpy()
    sh0 = sh0.detach().cpu().numpy()[:, 0, :]
    rest = shN.detach().cpu().numpy().transpose(0, 2, 1).reshape(len(means), -1)
    attrs = ['x', 'y', 'z', 'nx', 'ny', 'nz'] + ['f_dc_%d' % i for i in range(3)] + \
            ['f_rest_%d' % i for i in range(rest.shape[1])] + ['opacity'] + \
            ['scale_%d' % i for i in range(3)] + ['rot_%d' % i for i in range(4)]
    q = F.normalize(params['quats'].detach(), dim=1).cpu().numpy()
    data = np.concatenate([means, np.zeros_like(means), sh0, rest,
                           params['opacities'].detach().cpu().numpy()[:, None],
                           params['scales'].detach().cpu().numpy(), q], axis=1).astype(np.float32)
    el = np.empty(len(means), dtype=[(a, 'f4') for a in attrs])
    for i, a in enumerate(attrs):
        el[a] = data[:, i]
    PlyData([PlyElement.describe(el, 'vertex')]).write(str(path))


def colormap_sh(sh0, shN, cmap_name):
    """Approximate colormapped SH: DC through the colormap, higher bands by its local slope."""
    from colormaps import colormap_lut
    lut = colormap_lut(cmap_name)
    sh0 = sh0.detach().cpu().numpy()[:, 0, :]
    shN = shN.detach().cpu().numpy()
    v = np.clip(sh0.mean(1) * SH_C0 + 0.5, 0, 1)
    idx = v * 255
    lo = np.floor(idx).astype(int).clip(0, 254)
    frac = (idx - lo)[:, None]
    rgb = lut[lo] * (1 - frac) + lut[lo + 1] * frac
    slope = (lut[lo + 1] - lut[lo]) * 255
    rest = shN.mean(2, keepdims=True) * slope[:, None, :]
    return torch.from_numpy((rgb - 0.5) / SH_C0)[:, None, :].float(), torch.from_numpy(rest).float()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('workdir', type=Path)
    ap.add_argument('--sparse', type=Path, default=None, help='COLMAP model (default WORKDIR/colmap/sparse/final)')
    ap.add_argument('--out', type=Path, default=None, help='output directory (default WORKDIR/splat)')
    ap.add_argument('--iterations', type=int, default=30000)
    ap.add_argument('--max-images', type=int, default=500,
                    help='use all hot frames plus evenly spread others up to this many (0 = all)')
    ap.add_argument('--hot-delta', type=float, default=10.0,
                    help='hot threshold: degrees above the median frame maximum')
    ap.add_argument('--hot-weight', type=float, default=20.0, help='extra loss weight on hot pixels')
    ap.add_argument('--hot-tolerance', type=int, default=0,
                    help='hot pixels match the hottest render within this many pixels (pose error allowance)')
    ap.add_argument('--sh-degree', type=int, default=3)
    ap.add_argument('--lobe', action='store_true', help='add a per-Gaussian directional emission lobe')
    ap.add_argument('--source-gain', action='store_true',
                    help='learn a per-frame, time-smoothed strength for the emission near the hot spot '
                         '(for sources whose output varies during the flight)')
    ap.add_argument('--source-radius', type=float, default=3.0,
                    help='the source gain applies to Gaussians within this distance of the hot spot (m)')
    ap.add_argument('--gain-smooth', type=float, default=1.0, help='weight of the source gain smoothness penalty')
    ap.add_argument('--radius', type=float, default=150, help='model this horizontal radius about the target (m)')
    ap.add_argument('--height-margin', type=float, default=15.0,
                    help='allowed height beyond the 1-99%% SfM point heights (m)')
    ap.add_argument('--ceiling-cell', type=float, default=5.0, help='canopy ceiling grid cell (m)')
    ap.add_argument('--ceiling-margin', type=float, default=5.0,
                    help='allowed height above the local canopy top (m)')
    ap.add_argument('--gain-scope', choices=['source', 'scene'], default='source',
                    help='source: --source-gain scales emission near the hot spot; scene: it scales every '
                         'lobe, absorbing scene-wide brightness drift over the flight')
    ap.add_argument('--max-scale', type=float, default=5.0, help='prune Gaussians larger than this (m)')
    ap.add_argument('--min-obs', type=int, default=30,
                    help='skip frames with fewer SfM point observations (their pose is not image-refined)')
    ap.add_argument('--max-pose-offset', type=float, default=20.0,
                    help='skip frames whose SfM position is further than this (or 4x the median) from telemetry (m)')
    ap.add_argument('--min-elevation', type=float, default=20.0,
                    help='skip frames viewing the target from below this elevation (deg)')
    ap.add_argument('--t0', type=float, default=None, help='temperature scale floor (C)')
    ap.add_argument('--t1', type=float, default=None, help='temperature scale ceiling (C)')
    ap.add_argument('--k', type=float, default=8.0, help='temperature scale knee (C); smaller compresses hot more')
    ap.add_argument('--pose-opt', action='store_true', help='refine camera poses during training')
    ap.add_argument('--appearance', action='store_true', help='learn a per-image gain/offset')
    ap.add_argument('--cap', type=int, default=800000, help='maximum number of Gaussians')
    ap.add_argument('--grow-grad2d', type=float, default=0.0002, help='densification gradient threshold (thermal contrast is low; RGB default is 0.0008)')
    ap.add_argument('--refine-every', type=int, default=100)
    ap.add_argument('--ssim-weight', type=float, default=0.2)
    ap.add_argument('--holdout', type=int, default=20, help='hold out every Nth non-hot image (0=none)')
    ap.add_argument('--hot-holdout', type=int, default=8, help='hold out every Nth hot image (0=none)')
    ap.add_argument('--colormap', default='ironbow', help='colormap for splat_color.ply')
    ap.add_argument('--focus-radius', type=float, default=0.0,
                    help='concentrate detail within this distance of the hot spot (m, 0 = off)')
    ap.add_argument('--focus-enu', type=lambda s: [float(v) for v in s.split(',')],
                    help='focus centre E,N,U in m (default: the triangulated hot spot)')
    ap.add_argument('--focus-points', type=int, default=20000, help='extra initial points in the focus region')
    ap.add_argument('--focus-grow', type=float, default=4.0,
                    help='densification gradient multiplier for Gaussians in the focus region')
    args = ap.parse_args()
    if args.source_gain and args.gain_scope == 'scene' and not args.lobe:
        fatal('--gain-scope scene scales the lobes: it needs --lobe')

    from gsplat import rasterization
    from gsplat.strategy import DefaultStrategy
    from gsplat.strategy.ops import remove

    device = 'cuda'
    torch.manual_seed(0)
    np.random.seed(0)
    sparse = args.sparse or args.workdir / 'colmap' / 'sparse' / 'final'
    out = args.out or args.workdir / 'splat'
    out.mkdir(parents=True, exist_ok=True)
    scene = load_scene(args.workdir, sparse, args, device)
    names = scene['names']
    n_img = len(names)
    test_ids = [i for i, n in enumerate(names) if n in scene['test']]
    train_ids = [i for i, n in enumerate(names) if n not in scene['test']]
    v_hot = float(scene['scale'].forward(np.array(scene['hot_c'])))

    focus = None
    if args.focus_radius > 0:
        if args.focus_enu is None and not scene['hotspot']:
            fatal('--focus-radius needs --focus-enu or at least 5 hot frames to locate the hot spot')
        scene['focus'] = args.focus_enu or scene['hotspot']['enu']
        focus = torch.tensor(scene['focus'], dtype=torch.float32, device=device)
        print('focus region: %.0f m about ENU %s, %d seed points, gradient x%g' % (
            args.focus_radius, np.round(scene['focus'], 1), args.focus_points, args.focus_grow))
    params = init_params(scene, args, device)
    extent = float(np.linalg.norm(scene['centres'] - scene['centres'].mean(0), axis=1).max())
    # 3DGS learning rates are relative to scene extent; drone ranges are hundreds of metres
    lrs = dict(means=1.6e-4 * extent, scales=5e-3, quats=1e-3, opacities=5e-2, sh0=2.5e-3, shN=2.5e-3 / 20,
               lobe_amp=1e-2, lobe_dir=2e-3, lobe_kappa=1e-2)
    optimizers = {k: torch.optim.Adam([{'params': params[k], 'lr': lrs[k], 'name': k}], eps=1e-15)
                  for k in params}
    means_sched = torch.optim.lr_scheduler.ExponentialLR(optimizers['means'], gamma=0.01 ** (1 / args.iterations))

    strategy = DefaultStrategy(refine_stop_iter=args.iterations // 2, reset_every=3000, absgrad=True,
                               grow_grad2d=args.grow_grad2d, refine_every=args.refine_every, verbose=False)
    strategy.check_sanity(params, optimizers)
    # size thresholds (prune above 0.1x, split above 0.01x) follow the modelled region, not camera spread
    state = strategy.initialize_state(scene_scale=args.max_scale * 10)
    bounds = scene['bounds']
    b_centre = torch.tensor(bounds['centre'], device=device)

    ceil_grid = torch.from_numpy(bounds['ceiling']['grid']).to(device)
    ceil_origin = torch.tensor(bounds['ceiling']['origin'], dtype=torch.float32, device=device)

    def outside(means):
        ij = ((means[:, :2] - ceil_origin) / bounds['ceiling']['cell']).long().clamp(0, len(ceil_grid) - 1)
        return ((means[:, :2] - b_centre).norm(dim=1) > bounds['radius']) | \
            (means[:, 2] < bounds['z_min']) | (means[:, 2] > ceil_grid[ij[:, 0], ij[:, 1]])

    # per-image corrections; sparse so an image's state only moves when it is rendered
    pose_rot = torch.nn.Embedding(n_img, 3, sparse=True, device=device)  # radians
    pose_trans = torch.nn.Embedding(n_img, 3, sparse=True, device=device)  # metres
    appearance = torch.nn.Embedding(n_img, 2, sparse=True, device=device)  # log gain, offset
    source_gain = torch.nn.Embedding(n_img, 1, sparse=True, device=device)  # log lobe strength
    for e in (pose_rot, pose_trans, appearance, source_gain):
        torch.nn.init.zeros_(e.weight)
    pose_rot.weight.requires_grad_(args.pose_opt)
    pose_trans.weight.requires_grad_(args.pose_opt)
    appearance.weight.requires_grad_(args.appearance)
    source_gain.weight.requires_grad_(args.source_gain)
    extra_opts = []
    if args.source_gain:
        extra_opts.append(torch.optim.SparseAdam(source_gain.parameters(), lr=3e-2))
    # names sort in time order; neighbours among training frames for smoothing and held-out interpolation
    train_pos = np.array(train_ids)
    source = None
    if args.source_gain and args.gain_scope == 'source':
        if not scene['hotspot']:
            fatal('--source-gain needs at least 5 hot frames to locate the source')
        source = (torch.tensor(scene['hotspot']['enu'], dtype=torch.float32, device=device), args.source_radius)

    def train_neighbours(i):
        k = np.searchsorted(train_pos, i)
        return [int(train_pos[j]) for j in (k - 1, k + (0 if k < len(train_pos) and train_pos[k] != i else 1))
                if 0 <= j < len(train_pos) and train_pos[j] != i]

    def gain_for(i_list, refined=True):
        if not args.source_gain:
            return None
        if refined:
            return torch.exp(source_gain(torch.tensor(i_list, device=device))[:, 0])
        with torch.no_grad():  # held-out: interpolate from training frames either side in time
            out = []
            for i in i_list:
                nb = train_neighbours(i)
                out.append(torch.exp(source_gain.weight[nb, 0].mean()) if nb else torch.ones((), device=device))
            return torch.stack(out)
    if args.pose_opt:
        extra_opts += [torch.optim.SparseAdam(pose_rot.parameters(), lr=2e-4),
                       torch.optim.SparseAdam(pose_trans.parameters(), lr=2e-3)]
    if args.appearance:
        extra_opts.append(torch.optim.SparseAdam(appearance.parameters(), lr=2e-3))

    K = scene['K'][None]
    W, H = scene['W'], scene['H']

    def viewmat(i_list, refined=True):
        vm = scene['viewmats'][i_list]
        if refined and args.pose_opt:
            idx = torch.tensor(i_list, device=device)
            corr = torch.eye(4, device=device)[None].repeat(len(i_list), 1, 1)
            corr[:, :3, :3] = so3_exp(pose_rot(idx))
            corr[:, :3, 3] = pose_trans(idx)
            vm = corr @ vm
        return vm

    def render(i_list, sh_degree, refined=True):
        vm = viewmat(i_list, refined)
        campos = torch.linalg.inv(vm)[:, :3, 3]
        values = view_values(params, campos, sh_degree, gain_for(i_list, refined), source)[..., None]  # [C, N, 1]
        img, alpha, info = rasterization(params['means'], params['quats'], torch.exp(params['scales']),
                                         torch.sigmoid(params['opacities']), values, vm,
                                         K.repeat(len(i_list), 1, 1), W, H, packed=False, absgrad=True,
                                         near_plane=1.0)
        return img[..., 0], info

    t_start = time.time()
    order = []
    for step in range(args.iterations):
        if not order:
            order = list(np.random.permutation(train_ids))
        i = int(order.pop())
        img, info = render([i], min(step // 1000, args.sh_degree))
        pred = img[0]
        if args.appearance:
            a = appearance(torch.tensor([i], device=device))[0]
            pred = pred * torch.exp(a[0]) + a[1]
        target = scene['gt'][i].float()
        hot_mask = (target > v_hot).float()
        l1 = (pred - target).abs().mean()
        if hot_mask.any():
            # small sources: compare with the hottest rendered pixel nearby so a 1 px pose error is not
            # learned as a dimmer source
            r = args.hot_tolerance
            near_max = F.max_pool2d(pred[None, None], 2 * r + 1, stride=1, padding=r)[0, 0] if r else pred
            l1 = l1 + args.hot_weight * ((near_max - target).abs() * hot_mask).sum() / pred.numel()
        loss = (1 - args.ssim_weight) * l1 + args.ssim_weight * (1 - ssim(pred[None, None], target[None, None]))
        if args.appearance:
            loss = loss + 1e-2 * (a ** 2).sum()
        if args.source_gain:
            nb = train_neighbours(i)
            g = source_gain(torch.tensor([i] + nb, device=device))[:, 0]
            # the source covers few pixels, so these must stay weak against its data term
            loss = loss + args.gain_smooth * 1e-4 * ((g[0] - g[1:]) ** 2).sum() + 1e-5 * g[0] ** 2
        strategy.step_pre_backward(params, optimizers, state, step, info)
        loss.backward()
        for opt in optimizers.values():
            opt.step()
            opt.zero_grad(set_to_none=True)
        for opt in extra_opts:
            if step > 500:
                opt.step()
            opt.zero_grad(set_to_none=True)
        means_sched.step()
        if focus is not None and args.focus_grow != 1 and info['means2d'].absgrad is not None:
            with torch.no_grad():  # densify the focus region more readily
                near = (params['means'] - focus).norm(dim=1) < args.focus_radius
                info['means2d'].absgrad[..., near, :] *= args.focus_grow
        strategy.step_post_backward(params, optimizers, state, step, info, packed=False)
        if step % strategy.refine_every == 0:
            with torch.no_grad():
                drop = outside(params['means'])  # floaters drift towards the cameras
                excess = len(params['means']) - int(drop.sum()) - args.cap
                if excess > 0:  # hard budget: drop the least opaque
                    o = params['opacities'].detach().clone()
                    o[drop] = float('inf')
                    drop[o.topk(excess, largest=False).indices] = True
            if drop.any():
                remove(params, optimizers, state, drop)
        if step % 1000 == 0 or step == args.iterations - 1:
            g = state['grad2d'] / state['count'].clamp_min(1) if state.get('grad2d') is not None else None
            g99 = float(g[state['count'] > 0].quantile(0.99)) if g is not None and (state['count'] > 0).any() else 0
            print('step %5d  loss %.4f  l1 %.4f  gaussians %d  grad99 %.5f  mem %.1f GB  %.0f s' %
                  (step, loss.item(), l1.item(), len(params['means']), g99,
                   torch.cuda.max_memory_allocated() / 1e9, time.time() - t_start), flush=True)

    # held-out evaluation, in scaled units and in Celsius at the hot feature
    to_c = scene['scale'].inverse
    metrics = dict(cold=[], hot=[])
    with torch.no_grad():
        for i in test_ids:
            img, _ = render([i], args.sh_degree, refined=False)
            pred = img[0].clamp(0, 1)
            gt = scene['gt'][i].float()
            psnr = float(-10 * torch.log10(F.mse_loss(pred, gt)))
            m_gt, m_pr = gt > v_hot, pred > v_hot
            row = dict(name=names[i], psnr=psnr, true_peak_c=float(to_c(gt.max().item())),
                       rendered_peak_c=float(to_c(pred.max().item())),
                       hot_iou=float((m_gt & m_pr).sum() / max(1, (m_gt | m_pr).sum())))
            metrics['hot' if names[i] in scene['hot'] else 'cold'].append(row)
            both = torch.cat([gt, pred], 1).cpu().numpy()
            cv2.imwrite(str(out / ('preview_%s' % names[i])), (both * 255).astype(np.uint8))
    summary = {}
    for kind, rows in metrics.items():
        if rows:
            summary[kind] = dict(views=len(rows), psnr_db=float(np.mean([r['psnr'] for r in rows])),
                                 peak_error_c=float(np.median([r['rendered_peak_c'] - r['true_peak_c'] for r in rows])),
                                 hot_iou=float(np.mean([r['hot_iou'] for r in rows])),
                                 false_hot_views=int(sum(r['rendered_peak_c'] > scene['hot_c'] for r in rows))
                                 if kind == 'cold' else None)
            print('held-out %s: %s' % (kind, ', '.join('%s %s' % (k, ('%.2f' % v) if isinstance(v, float) else v)
                                                      for k, v in summary[kind].items())))
    (out / 'metrics.json').write_text(json.dumps(dict(summary=summary, views=metrics), indent=1))

    refined = viewmat(list(range(n_img))).detach().cpu()
    frame_gains = None
    if args.source_gain:
        with torch.no_grad():
            frame_gains = torch.tensor([float(gain_for([i], refined=names[i] not in scene['test'])[0])
                                        for i in range(n_img)])
    # re-locate the hot spot with the refined poses (telemetry pointing can be biased by degrees)
    hot_ids = [i for i, n in enumerate(names) if n in scene['hot']]
    hotspot = locate_hotspot(scene['gt'].float().cpu().numpy(), refined.numpy(), scene['K'].cpu().numpy(), hot_ids)
    if hotspot:
        print('hot spot at ENU %s m from %d frames, median ray error %.1f px'
              % (np.round(hotspot['enu'], 1), hotspot['frames'], hotspot['median_ray_error_px']))
    pose_change = dict(rotation_deg=torch.rad2deg(pose_rot.weight.detach().norm(dim=1)).cpu().numpy(),
                       translation_m=pose_trans.weight.detach().norm(dim=1).cpu().numpy())
    pose_change = {k: dict(median=float(np.median(v)), p90=float(np.percentile(v, 90)), max=float(v.max()))
                   for k, v in pose_change.items()}
    if args.pose_opt:
        print('training pose refinement: rotation %s deg, translation %s m'
              % (pose_change['rotation_deg'], pose_change['translation_m']))

    torch.save(dict(params={k: v.detach().cpu() for k, v in params.items()}, sh_degree=args.sh_degree,
                    K=scene['K'].cpu(), width=W, height=H, viewmats=scene['viewmats'].cpu(),
                    viewmats_refined=refined, pose_rot=pose_rot.weight.detach().cpu(),
                    pose_trans=pose_trans.weight.detach().cpu(), appearance=appearance.weight.detach().cpu(),
                    source_gain=frame_gains,
                    names=names, test=sorted(scene['test']), hot=sorted(scene['hot'])), out / 'splat.pt')
    meta = dict(temperature_scale=scene['scale'].todict(), hot_threshold_c=scene['hot_c'],
                bounds={k: v for k, v in bounds.items() if k != 'ceiling'}, origin=scene['info']['origin'], target_enu=scene['info']['target_enu'], hotspot=hotspot,
                source_region=dict(enu=scene['hotspot']['enu'], radius_m=args.source_radius) if source else None,
                coordinates='ENU metres, z up from takeoff altitude',
                values='apparent temperature from alpha-blended scaled values, not calibrated surface temperature',
                focus=dict(enu=scene['focus'], radius_m=args.focus_radius) if focus is not None else None,
                gaussians=len(params['means']), iterations=args.iterations, lobe=args.lobe,
                options=vars(args) | dict(workdir=str(args.workdir), sparse=str(sparse), out=str(out)),
                heldout=summary, pose_refinement=pose_change if args.pose_opt else None,
                source=scene['info']['source'])
    (out / 'scale.json').write_text(json.dumps(meta, indent=1, default=str))
    if args.source_gain:
        gains = frame_gains.numpy()
        utc = {f['name']: f.get('utc_us') for f in scene['info']['frames']}
        rows = ['frame,utc_us,heldout,hot,source_gain']
        for i, n in enumerate(names):
            rows.append('%s,%s,%d,%d,%.4f' % (n, utc.get(n), n in scene['test'], n in scene['hot'], gains[i]))
        (out / 'source_gain.csv').write_text('\n'.join(rows) + '\n')
        hot_g = [float(gains[i]) for i, n in enumerate(names) if n in scene['hot'] and n not in scene['test']]
        print('source gain over hot training frames: min %.2f, median %.2f, max %.2f (source_gain.csv)'
              % (min(hot_g), np.median(hot_g), max(hot_g)))
    with torch.no_grad():
        sh0, shN = bake_lobe_into_sh(params, args.sh_degree)
    save_ply(out / 'splat.ply', params, sh0, shN)
    save_ply(out / 'splat_color.ply', params, *colormap_sh(sh0, shN, args.colormap))
    print('wrote %s (%d gaussians) in %.0f s' % (out, len(params['means']), time.time() - t_start))


if __name__ == '__main__':
    main()
