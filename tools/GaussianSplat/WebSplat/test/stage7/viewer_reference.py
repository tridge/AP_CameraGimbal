#!/usr/bin/env python3
"""gsplat references for the viewer checks: the exported PLY rendered as the viewer renders it
(classic, channel mean, alpha-blended on black, decoded with the header's temperature scale) at
the viewer's camera (look-at with z up, vertical FOV, centred principal point), size W x H.
  viewer_reference.py PLY OUT.json [--size W H] [--view DATASET NAME]
Default camera: the PLY header's start camera; --view: training view NAME of DATASET/g1.json
(its centre and optical axis, looked at with z up). OUT.json: camera, width, height, peak_c (the
hottest pixel with alpha > 0.5), argmax [x, y] (top-left pixel coordinates), argmax_c.
"""
import argparse
import json
import math
from pathlib import Path
import sys

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2]))
sys.path.insert(0, str(HERE.parent / 'g1'))
from splat_common import setup_cuda_env  # noqa: E402

setup_cuda_env()
from start_view import viewmat  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ply', type=Path)
    ap.add_argument('out', type=Path)
    ap.add_argument('--size', nargs=2, type=int, default=[1280, 900])
    ap.add_argument('--view', nargs=2)
    a = ap.parse_args()
    import torch
    from gsplat import rasterization
    from plyfile import PlyData
    from render_ply import load_ply
    W, H = a.size
    info = json.loads([c for c in PlyData.read(str(a.ply)).comments if c.startswith('apcg_splat ')][0][11:])
    cam = info['camera']
    if a.view:
        meta = json.loads((Path(a.view[0]) / 'g1.json').read_text())
        vm = np.array(meta['viewmats'][meta['names'].index(a.view[1])])
        c2w = np.linalg.inv(vm)
        pos = c2w[:3, 3]
        cam = dict(position=pos.tolist(), target=(pos + 30 * c2w[:3, 2]).tolist(),
                   fov_deg=math.degrees(2 * math.atan(meta['height'] / 2 / meta['K'][1][1])))
    s = load_ply(a.ply, 'cuda')
    f = H / 2 / math.tan(math.radians(cam['fov_deg']) / 2)
    K = torch.tensor([[f, 0, W / 2], [0, f, H / 2], [0, 0, 1]], dtype=torch.float32, device='cuda')
    with torch.no_grad():
        img, alpha, _ = rasterization(s['means'], torch.nn.functional.normalize(s['quats'], dim=1), s['scales'],
                                      s['opacities'], s['sh'], viewmat(cam)[None], K[None], W, H,
                                      sh_degree=s['degree'], near_plane=0.5, packed=False)
    v = img[0].mean(-1).clamp(0, 1).cpu().numpy()
    al = alpha[0, ..., 0].cpu().numpy()
    ts = info['temperature_scale']
    norm = math.log1p((ts['t1_c'] - ts['t0_c']) / ts['k_c'])
    to_c = lambda x: ts['t0_c'] + ts['k_c'] * math.expm1(float(x) * norm)  # noqa: E731
    vv = np.where(al > 0.5, v, -1)
    y, x = np.unravel_index(int(np.argmax(vv)), vv.shape)
    out = dict(camera=cam, width=W, height=H, temperature_scale=ts, peak_c=to_c(vv[y, x]), argmax=[int(x), int(y)],
               argmax_c=to_c(v[y, x]))
    a.out.write_text(json.dumps(out, indent=1))
    print('%s: gsplat peak %.1f C at %s (%dx%d)' % (a.ply.name, out['peak_c'], out['argmax'], W, H))


if __name__ == '__main__':
    main()
