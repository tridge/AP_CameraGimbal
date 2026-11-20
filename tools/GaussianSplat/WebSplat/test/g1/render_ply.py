#!/usr/bin/env python3
"""Render a 3DGS PLY (train.py's splat.ply or a Brush export) at the G1 held-out views with gsplat,
decoding values as the viewer does: SH (+0.5, clamped at 0), mean of the colour channels, alpha
blended on black. Writes OUT.npz: names (g1.json test order) and renders, float32 [views, H, W]
clamped to [0, 1]."""
import argparse
import json
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))  # tools/GaussianSplat
from splat_common import setup_cuda_env  # noqa: E402

setup_cuda_env()


def load_ply(path, device):
    import torch
    from plyfile import PlyData
    v = PlyData.read(str(path))['vertex']
    names = v.data.dtype.names
    t = lambda *k: torch.tensor(np.stack([np.asarray(v[x], np.float32) for x in k], 1), device=device)  # noqa: E731
    rest = sorted([n for n in names if n.startswith('f_rest_')], key=lambda n: int(n[7:]))
    n_coef = len(rest) // 3 + 1
    sh = torch.zeros(len(v.data), n_coef, 3, device=device)
    sh[:, 0] = t('f_dc_0', 'f_dc_1', 'f_dc_2')
    if rest:  # standard layout: f_rest is channel-major
        sh[:, 1:] = t(*rest).reshape(-1, 3, n_coef - 1).transpose(1, 2)
    return dict(means=t('x', 'y', 'z'), scales=torch.exp(t('scale_0', 'scale_1', 'scale_2')),
                quats=t('rot_0', 'rot_1', 'rot_2', 'rot_3'), opacities=torch.sigmoid(t('opacity')[:, 0]),
                sh=sh, degree=int(round(n_coef ** 0.5)) - 1)


def render(splat, viewmats, K, W, H, near=1.0):
    import torch
    from gsplat import rasterization
    out = []
    q = torch.nn.functional.normalize(splat['quats'], dim=1)
    for vm in viewmats:
        img, _, _ = rasterization(splat['means'], q, splat['scales'], splat['opacities'], splat['sh'],
                                  vm[None], K[None], W, H, sh_degree=splat['degree'], near_plane=near,
                                  packed=False)
        out.append(img[0].mean(-1).clamp(0, 1).cpu().numpy())
    return np.stack(out).astype(np.float32)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('ply', type=Path)
    ap.add_argument('dataset', type=Path, help='G1 dataset (g1.json)')
    ap.add_argument('out', type=Path, help='output .npz')
    a = ap.parse_args()
    import torch
    meta = json.loads((a.dataset / 'g1.json').read_text())
    idx = {n: i for i, n in enumerate(meta['names'])}
    vms = torch.tensor(np.array(meta['viewmats'])[[idx[n] for n in meta['test']]], dtype=torch.float32, device='cuda')
    K = torch.tensor(meta['K'], dtype=torch.float32, device='cuda')
    splat = load_ply(a.ply, 'cuda')
    with torch.no_grad():
        imgs = render(splat, vms, K, meta['width'], meta['height'])
    if a.out.suffix != '.npz':
        sys.exit('output must be .npz')
    np.savez(a.out, names=np.array(meta['test']), renders=imgs)
    print('%s: %d gaussians, SH degree %d, %d views -> %s' % (a.ply.name, len(splat['means']), splat['degree'],
                                                              len(imgs), a.out))


if __name__ == '__main__':
    main()
