#!/usr/bin/env python3
"""Add a splat to a static site whose viewer (viewer/) renders on the visitor's GPU.

  (cd viewer && npm ci && npm run build)   # once: the viewer and the SPZ encoder
  export_web.py SPLATDIR SITE DATE/FLIGHT/RUN [--title TITLE]
  export_web.py SPLATDIR --ply OUT.ply [--title TITLE]

SPLATDIR holds splat.pt and scale.json (train.py or train_rgb.py) and optionally
description.txt. Writes SITE/DATE/FLIGHT/RUN/{splat.spz, splat.ply, meta.json, index.html}
(splat.ply is a download carrying the metadata in its header), copies the source video given
with --video to SITE/videos/ (shared by splats made from the same recording), copies the
viewer to SITE/viewer/ and rewrites SITE/index.html. SITE is plain static files for any web
server. --ply instead writes one PLY carrying the same metadata in its header, for opening in
the viewer's open-a-file page (viewer/dist/index.html, e.g. plot.ardupilot.org/splat/).

Thermal splats are written in greyscale: every colour channel holds the scaled temperature, and
the viewer decodes the blended value and applies the colormap per pixel, as view_splat.py does.
The emission lobe is projected onto the spherical harmonics, and the per-frame source strength
is left at 1.
"""
import argparse
import html
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from splat_common import fatal, setup_cuda_env  # noqa: E402

WEB = Path(__file__).resolve().parent / 'viewer'


def start_camera(ckpt, target):
    """The view view_splat.py opens with: a middle hot training view, or an oblique view of the
    target from the same side when that view looks nearly straight down."""
    viewmats = ckpt.get('viewmats_refined', ckpt['viewmats']).numpy()
    hot = set(ckpt.get('hot', []))
    hot_ids = [i for i, n in enumerate(ckpt['names']) if n in hot]
    i = hot_ids[len(hot_ids) // 2] if hot_ids else len(viewmats) // 2
    c2w = np.linalg.inv(viewmats[i])
    fov = math.degrees(2 * math.atan(ckpt['height'] / 2 / float(ckpt['K'][1, 1])))
    axis = c2w[:3, 2]
    if axis[2] > -0.94:
        depth = max(1.0, float((target - c2w[:3, 3]) @ axis))
        return dict(position=c2w[:3, 3].tolist(), target=(c2w[:3, 3] + axis * depth).tolist(), fov_deg=fov)
    dist = float(np.linalg.norm(c2w[:3, 3] - target))
    side = -c2w[:3, 1][:2]
    side = side / max(np.linalg.norm(side), 1e-6)
    el = math.radians(50)
    pos = target + dist * np.array([-side[0] * math.cos(el), -side[1] * math.cos(el), math.sin(el)])
    return dict(position=pos.tolist(), target=target.tolist(), fov_deg=fov)


def write_ply(path, params, sh, info=None):
    from plyfile import PlyData, PlyElement
    n = len(params['means'])
    sh = sh.numpy()
    rest = sh[:, 1:, :].transpose(0, 2, 1).reshape(n, -1)
    q = params['quats'].numpy()
    q = q / np.linalg.norm(q, axis=1, keepdims=True)
    cols = [params['means'].numpy(), np.zeros((n, 3)), sh[:, 0, :], rest, params['opacities'].numpy()[:, None],
            params['scales'].numpy(), q]
    names = ['x', 'y', 'z', 'nx', 'ny', 'nz'] + ['f_dc_%d' % i for i in range(3)] + \
        ['f_rest_%d' % i for i in range(rest.shape[1])] + ['opacity'] + \
        ['scale_%d' % i for i in range(3)] + ['rot_%d' % i for i in range(4)]
    data = np.concatenate(cols, axis=1).astype(np.float32)
    el = np.empty(n, dtype=[(a, 'f4') for a in names])
    for i, a in enumerate(names):
        el[a] = data[:, i]
    comments = ['apcg_splat ' + json.dumps(info, separators=(',', ':'))] if info else []
    PlyData([PlyElement.describe(el, 'vertex')], comments=comments).write(str(path))


def write_index(site):
    rows = []
    def order(m):  # newest date first, run12 after run9
        return [int(t) if t.isdigit() else t for t in re.split(r'(\d+)', m.parent.relative_to(site).as_posix())]
    for m in sorted(site.glob('*/*/*/meta.json'), key=order, reverse=True):
        info = json.loads(m.read_text())
        rel = m.parent.relative_to(site).as_posix()
        ply = info.get('downloads', {}).get('ply')
        dl = ' &middot; <a href="%s/splat.ply" download>PLY</a> <span class="kind">%.0f MB</span>' % (
            html.escape(rel), ply / 1e6) if ply else ''
        video = info.get('video')
        if video:
            dl += ' &middot; <a href="videos/%s" download>video</a> <span class="kind">%.0f MB</span>' % (
                html.escape(video['name']), video['bytes'] / 1e6)
        rows.append('<li><a href="%s/">%s</a> <span class="kind">%s, %s Gaussians</span>%s<p>%s</p></li>' % (
            html.escape(rel), html.escape(info['title']), info['kind'], format(info['gaussians'], ','), dl,
            html.escape(info.get('description', ''))))
    (site / 'index.html').write_text("""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Gaussian splats</title>
<style>body { font: 15px system-ui, sans-serif; max-width: 900px; margin: 2em auto; padding: 0 1em; }
li { margin: 1em 0; } .kind { color: #666; font-size: 13px; } p { margin: 0.3em 0; color: #333; }</style>
</head><body><h1>Gaussian splats</h1>
<p>3D Gaussian splats from UAV thermal and RGB video. They render in your browser using your own
GPU; the larger ones need a desktop or a recent phone.
<a href="viewer/">Open your own splat file</a> (read on your device, never uploaded).
The PLY downloads are standard 3DGS PLY files; thermal ones hold the scaled temperature in every
colour channel, with the temperature scale and start view in a <code>comment apcg_splat</code>
header line, and open with temperatures in the viewer here or at
<a href="https://plot.ardupilot.org/splat/">plot.ardupilot.org/splat</a>.
The videos are the recordings each splat was made from, with the telemetry embedded (thermal
MKV with per-frame camera telemetry, or MPEG-TS with MISB ST 0601 KLV).
Made with the <a href="https://github.com/ArduPilot/AP_CameraGimbal/tree/master/tools/GaussianSplat">AP_CameraGimbal
GaussianSplat tools</a>.</p>
<ul>
%s
</ul></body></html>
""" % '\n'.join(rows))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('splat', type=Path, help='directory with splat.pt and scale.json')
    ap.add_argument('site', type=Path, nargs='?', help='site directory')
    ap.add_argument('path', nargs='?', help='DATE/FLIGHT/RUN within the site')
    ap.add_argument('--ply', type=Path, help='write one PLY with the metadata in its header instead')
    ap.add_argument('--title', help='page title (default: from PATH)')
    ap.add_argument('--video', type=Path, help='source video with embedded telemetry, offered as a download')
    ap.add_argument('--video-name', help='file name for the video download (default: its own name)')
    args = ap.parse_args()
    if args.ply:
        parts = args.splat.resolve().parts[-3:]
    else:
        if not args.site or not args.path:
            fatal('give SITE and DATE/FLIGHT/RUN, or --ply OUT.ply')
        parts = Path(args.path).parts
        if len(parts) != 3 or any(p in ('.', '..') for p in parts):
            fatal('PATH must be DATE/FLIGHT/RUN')
        if not (WEB / 'dist' / 'viewer.js').exists():
            fatal('build the viewer first: cd %s && npm ci && npm run build' % WEB)
        args.out = args.site / args.path

    setup_cuda_env()  # splat_model uses gsplat
    import torch
    from splat_model import bake_lobe_into_sh
    ckpt = torch.load(args.splat / 'splat.pt', map_location='cpu', weights_only=False)
    meta = json.loads((args.splat / 'scale.json').read_text())
    p = {k: v.float() for k, v in ckpt['params'].items()}
    rgb = meta.get('kind') == 'rgb'
    if 'sh0' not in p:
        fatal('%s has no SH colour' % (args.splat / 'splat.pt'))
    if rgb:
        sh = torch.cat([p['sh0'], p['shN']], 1)
    else:
        with torch.no_grad():
            dev = 'cuda' if torch.cuda.is_available() else 'cpu'
            sh0, shN = bake_lobe_into_sh({k: v.to(dev) for k, v in p.items()}, ckpt['sh_degree'])
        sh = torch.cat([sh0, shN], 1).cpu()
        sh = sh.mean(2, keepdim=True).expand(-1, -1, 3).contiguous()  # view_splat.py averages the channels

    target = np.array(meta['hotspot']['enu'] if meta.get('hotspot') else meta.get('target_enu', [0, 0, 0]))
    desc = args.splat / 'description.txt'
    info = dict(kind='rgb' if rgb else 'thermal', gaussians=len(p['means']), sh_degree=ckpt['sh_degree'],
                title=args.title or ' '.join(parts),
                description=desc.read_text().strip() if desc.exists() else '',
                camera=start_camera(ckpt, target), up=[0, 0, 1], target=target.tolist(),
                origin=meta.get('origin'), rasterize_mode='classic')  # train.py, train_rgb.py: gsplat classic
    if not rgb:
        ts = meta['temperature_scale']
        if ts.get('kind', 'log1p') != 'log1p':
            fatal('unsupported temperature scale %s' % ts.get('kind'))
        info.update(temperature_scale=ts, hotspot=meta.get('hotspot'), colormap='ironbow',
                    display_range_c=[math.floor(ts['t0_c']), math.ceil(ts['t1_c'])])
    if args.ply:
        write_ply(args.ply, p, sh, info)
        print('wrote %s: %d %s Gaussians' % (args.ply, info['gaussians'], info['kind']))
        return
    args.out.mkdir(parents=True, exist_ok=True)
    write_ply(args.out / 'splat.ply', p, sh, info)  # also the download
    subprocess.run(['node', str(WEB / 'ply2spz.mjs'), str(args.out / 'splat.ply'), str(args.out / 'splat.spz')],
                   check=True, cwd=WEB)
    info['downloads'] = {k: (args.out / ('splat.' + k)).stat().st_size for k in ('ply', 'spz')}
    if args.video:
        name = args.video_name or args.video.name
        if '/' in name or name.startswith('.'):
            fatal('bad video name %s' % name)
        dst = args.site / 'videos' / name
        dst.parent.mkdir(exist_ok=True)
        if not dst.exists() or dst.stat().st_size != args.video.stat().st_size:
            shutil.copyfile(args.video, dst)
        info['video'] = dict(name=name, bytes=dst.stat().st_size)
    (args.out / 'meta.json').write_text(json.dumps(info, indent=1))
    (args.out / 'index.html').write_text((WEB / 'splat.html').read_text().replace('@ROOT@', '../../../'))
    shutil.copytree(WEB / 'dist', args.site / 'viewer', dirs_exist_ok=True)
    write_index(args.site)
    print('wrote %s: %d %s Gaussians, SH degree %d' % (args.out, info['gaussians'], info['kind'], ckpt['sh_degree']))


if __name__ == '__main__':
    main()
