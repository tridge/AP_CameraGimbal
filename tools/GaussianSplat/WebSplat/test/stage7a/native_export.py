#!/usr/bin/env python3
"""Native exports of the tiny model: builds a train.py-style splat directory (splat.pt,
scale.json) from TINY/{params.bin,dataset.json,frames.json}, then runs export_web.py (--ply and
site mode, which also makes the SPZ with ply2spz.mjs) and train.py's save_ply.
  native_export.py TINY OUT  ->  OUT/{splatdir/, export_web.ply, site/…/{splat.ply,splat.spz,meta.json}, train.ply}
"""
import json
from pathlib import Path
import subprocess
import sys

import numpy as np

GS = Path(__file__).resolve().parents[3]  # tools/GaussianSplat
sys.path.insert(0, str(GS))
# 'end_header' in the title: readers must find the terminator line, not this text
TITLE = 'tiny model end_header\nend_header'
SITE_PATH = '2026-11-08/tiny/run1'


def read_params(path):
    b = Path(path).read_bytes()
    magic, version, n, deg = np.frombuffer(b[:16], '<u4')
    assert magic == 0x50535357 and version == 1, 'not WSSP v1'
    k = (deg + 1) ** 2 - 1
    a = np.frombuffer(b[16:], '<f4').copy()
    out, at = {}, 0
    for name, w in (('means', 3), ('scales', 3), ('quats', 4), ('opacities', 1), ('sh0', 3), ('shN', 3 * k)):
        out[name] = a[at:at + n * w].reshape(n, w) if w > 1 else a[at:at + n].copy()
        at += n * w
    assert at == len(a)
    out['sh0'] = out['sh0'].reshape(n, 1, 3)
    out['shN'] = out['shN'].reshape(n, k, 3)
    return out, int(deg)


def main():
    tiny, out = Path(sys.argv[1]), Path(sys.argv[2])
    import torch
    params, deg = read_params(tiny / 'params.bin')
    ds = json.loads((tiny / 'dataset.json').read_text())
    fr = json.loads((tiny / 'frames.json').read_text())
    sd = out / 'splatdir'
    sd.mkdir(parents=True, exist_ok=True)
    p = {k: torch.from_numpy(np.array(v, np.float32)) for k, v in params.items()}
    names = [im['name'] for im in ds['images']]
    torch.save(dict(params=p, sh_degree=deg, K=torch.tensor(ds['K'], dtype=torch.float32),
                    width=ds['width'], height=ds['height'],
                    viewmats=torch.tensor(np.array([np.reshape(im['viewmat'], (4, 4)) for im in ds['images']]),
                                          dtype=torch.float32),
                    names=names, test=[], hot=[im['name'] for im in ds['images'] if im['hot']]),
               sd / 'splat.pt')
    # as train.py writes scale.json: Python floats
    scale = {k: (float(v) if k != 'kind' else v) for k, v in ds['scale'].items()}
    origin = {k: (float(v) if k != 'frame' else v) for k, v in fr['origin'].items()}
    hotspot = dict(enu=[float(x) for x in ds['hotspot']['enu']], frames=ds['hotspot']['frames'],
                   median_ray_error_px=float(ds['hotspot']['median_ray_error_px']))
    bounds = dict(centre=[float(x) for x in ds['bounds']['centre']], radius=float(ds['bounds']['radius']))
    (sd / 'scale.json').write_text(json.dumps(dict(
        temperature_scale=scale, origin=origin, target_enu=[float(x) for x in fr['target_enu']],
        hotspot=hotspot, bounds=bounds), indent=1))

    py = sys.executable
    subprocess.run([py, str(GS / 'export_web.py'), str(sd), '--ply', str(out / 'export_web.ply'),
                    '--title', TITLE], check=True)
    subprocess.run([py, str(GS / 'export_web.py'), str(sd), str(out / 'site'), SITE_PATH, '--title', TITLE],
                   check=True)

    import train
    from splat_common import trim_info
    train.save_ply(out / 'train.ply', p, p['sh0'], p['shN'], info=dict(
        kind='thermal', up=[0, 0, 1], target=hotspot['enu'], temperature_scale=scale, origin=origin,
        rasterize_mode='classic', **trim_info(bounds)))
    print('wrote', out)


if __name__ == '__main__':
    main()
