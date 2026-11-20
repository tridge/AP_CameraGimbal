#!/usr/bin/env python3
"""Stage 7's start view against export_web.py's: render the exported PLY (gsplat classic, as the
viewer renders it, at the viewer's 1280x900 canvas) from the start camera in its header and from
the native policy's camera (NATIVE.json, the middle hot view), and compare the hottest rendered
temperature. Requires the stage 7 view to show the hot source (peak above the dataset's hot
threshold) and not less than the native view.
  start_view.py SPLAT.ply NATIVE_CAMERA.json DATASET_DIR (g1.json: hot_c, temperature scale)
"""
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
import metrics  # noqa: E402
from render_ply import load_ply, render  # noqa: E402

W, H = 1280, 900


def viewmat(cam):
    import torch
    c, t = np.array(cam['position']), np.array(cam['target'])
    f = (t - c) / np.linalg.norm(t - c)
    r = np.cross(f, [0, 0, 1.0])
    r /= np.linalg.norm(r)
    u = np.cross(r, f)
    R = np.stack([r, -u, f])
    m = np.eye(4)
    m[:3, :3], m[:3, 3] = R, -R @ c
    return torch.tensor(m, dtype=torch.float32, device='cuda')


def main():
    import torch
    from plyfile import PlyData
    ply, native, ds = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    meta = json.loads((ds / 'g1.json').read_text())
    to_c = metrics.Scale(meta['temperature_scale']).inverse
    info = json.loads([c for c in PlyData.read(str(ply)).comments if c.startswith('apcg_splat ')][0][11:])
    cams = {'stage 7 (hottest view)': info['camera'], "export_web.py's (middle hot view)": json.loads(native.read_text())}
    s = load_ply(ply, 'cuda')
    peaks = {}
    with torch.no_grad():
        for k, cam in cams.items():
            f = H / 2 / math.tan(math.radians(cam['fov_deg']) / 2)
            K = torch.tensor([[f, 0, W / 2], [0, f, H / 2], [0, 0, 1]], dtype=torch.float32, device='cuda')
            img = render(s, [viewmat(cam)], K, W, H, near=0.5)[0]
            peaks[k] = float(to_c(img.max()))
            print('%s: peak %.1f C' % (k, peaks[k]))
    new, old = peaks['stage 7 (hottest view)'], peaks["export_web.py's (middle hot view)"]
    ok = new >= meta['hot_c'] and new >= old
    print('%s start view shows the hot source: %.1f C (hot threshold %.1f C, native view %.1f C)'
          % ('PASS' if ok else 'FAIL', new, meta['hot_c'], old))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
