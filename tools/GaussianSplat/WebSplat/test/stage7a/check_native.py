#!/usr/bin/env python3
"""Stage 7a native checks of the JS exports of the tiny model:
  - plyfile reads JS splat.ply / train.ply to exactly the source parameters (params.bin), the
    quaternions normalised as numpy does, and the apcg_splat header to the expected info;
  - G1's native PLY loader (render_ply.load_ply, gsplat conventions) gives identical tensors for
    the JS and the native PLY;
  - the JS SH evaluation used for the expected viewer values (expected.json) equals gsplat's
    spherical_harmonics, and gsplat's render at each Gaussian centre is alpha * value;
  - gsplat renders of the JS PLY at the viewer cameras, plus negative controls (quaternion read
    x,y,z,w; SH direction reversed; up axis -y; opacity without the logit; linear scales; gsplat
    'antialiased' mode), for
    compare_views.py.
  check_native.py TINY NATIVE JS OUT.npz      (exit 1 on failure)
"""
import json
from pathlib import Path
import sys

import numpy as np

GS = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(GS))
sys.path.insert(0, str(GS / 'WebSplat' / 'test' / 'g1'))
from splat_common import setup_cuda_env  # noqa: E402

setup_cuda_env()
import torch  # noqa: E402
from plyfile import PlyData  # noqa: E402
from render_ply import load_ply  # noqa: E402
from native_export import read_params  # noqa: E402

failures = 0


def check(name, ok, detail=''):
    global failures
    print('%s %s%s' % ('PASS' if ok else 'FAIL', name, (': ' + detail) if detail else ''))
    failures += not ok


def quat_mul(a, b):
    aw, ax, ay, az = a.unbind(-1)
    bw, bx, by, bz = b.unbind(-1)
    return torch.stack([aw * bw - ax * bx - ay * by - az * bz, aw * bx + ax * bw + ay * bz - az * by,
                        aw * by - ax * bz + ay * bw + az * bx, aw * bz + ax * by - ay * bx + az * bw], -1)


def main():
    tiny, native, js, out = map(Path, sys.argv[1:5])
    params, deg = read_params(tiny / 'params.bin')
    n = len(params['means'])
    k = (deg + 1) ** 2 - 1
    q = params['quats'] / np.linalg.norm(params['quats'], axis=1, keepdims=True)
    for name, grey in (('train.ply', False), ('splat.ply', True)):
        ply = PlyData.read(str(js / name))
        v = ply['vertex']
        sh0, shN = params['sh0'][:, 0, :], params['shN']
        if grey:  # export_web.py: channel mean
            sh0 = np.repeat(torch.from_numpy(params['sh0']).mean(2).numpy(), 3, 1)
            shN = torch.from_numpy(shN).mean(2, keepdim=True).expand(-1, -1, 3).numpy()
        exp = dict(x=params['means'][:, 0], y=params['means'][:, 1], z=params['means'][:, 2],
                   nx=np.zeros(n), ny=np.zeros(n), nz=np.zeros(n), opacity=params['opacities'])
        for c in range(3):
            exp['f_dc_%d' % c] = sh0[:, c]
            exp['scale_%d' % c] = params['scales'][:, c]
            for j in range(k):
                exp['f_rest_%d' % (c * k + j)] = shN[:, j, c]
        for c in range(4):
            exp['rot_%d' % c] = q[:, c]
        names = list(v.data.dtype.names)
        bad = [a for a in exp if not np.array_equal(np.asarray(v[a], np.float32), np.asarray(exp[a], np.float32))]
        check('plyfile reads JS %s to the source parameters' % name,
              not bad and len(names) == len(exp) and all(v.data.dtype[a] == np.float32 for a in names),
              ('differ: %s' % bad) if bad else '%d properties, %d Gaussians, all float32 equal' % (len(names), n))
        info = [c for c in ply.comments if c.startswith('apcg_splat ')]
        meta = json.loads(info[0][len('apcg_splat '):]) if len(info) == 1 else {}
        nat = json.loads([c for c in PlyData.read(str(native / ('export_web.ply' if grey else 'train.ply'))).comments
                          if c.startswith('apcg_splat ')][0][len('apcg_splat '):])
        if grey:
            cam_err = max(abs(a - b) for key in ('position', 'target')
                          for a, b in zip(meta['camera'][key], nat['camera'][key]))
            meta = {**meta, 'camera': None}
            nat = {**nat, 'camera': None}
            check('JS start camera = native (Python json)', cam_err < 1e-4, '%.2e m' % cam_err)
        check('apcg_splat header of JS %s = native (Python json)' % name, meta == nat)

    a, b = load_ply(js / 'train.ply', 'cpu'), load_ply(native / 'train.ply', 'cpu')
    check('render_ply.load_ply: JS train.ply = native', all(torch.equal(a[x], b[x]) for x in a if x != 'degree')
          and a['degree'] == b['degree'] == 3)
    a, b = load_ply(js / 'splat.ply', 'cpu'), load_ply(native / 'export_web.ply', 'cpu')
    check('render_ply.load_ply: JS splat.ply = export_web.py', all(torch.equal(a[x], b[x]) for x in a if x != 'degree'))

    from gsplat import rasterization, spherical_harmonics
    ex = json.loads((js / 'expected.json').read_text())
    W, H = ex['width'], ex['height']
    s = load_ply(js / 'splat.ply', 'cuda')
    renders = {}
    for cname, cam in ex['cameras'].items():
        campos = torch.tensor(cam['position'], dtype=torch.float32, device='cuda')
        dirs = s['means'] - campos
        val = (spherical_harmonics(3, dirs, s['sh']) + 0.5).clamp(min=0).mean(-1).cpu().numpy()
        js_val = np.array([g['value'] for g in cam['gaussians']])
        err = np.abs(val - js_val).max()
        check('%s: JS SH value = gsplat spherical_harmonics' % cname, err < 1e-5, 'max %.1e' % err)
        vm = torch.tensor(np.reshape(cam['viewmat'], (4, 4)), dtype=torch.float32, device='cuda')
        K = torch.tensor([[cam['f'], 0, W / 2], [0, cam['f'], H / 2], [0, 0, 1]], dtype=torch.float32, device='cuda')

        # the exports say rasterize_mode classic (train.py's gsplat default) and the viewer renders
        # them so; 'antialiased' is Spark's default, kept for splats without the field
        def render(sp, mode='classic'):
            img, alpha, _ = rasterization(sp['means'], torch.nn.functional.normalize(sp['quats'], dim=1),
                                          sp['scales'], sp['opacities'], sp['sh'], vm[None], K[None], W, H,
                                          sh_degree=3, near_plane=0.5, packed=False, rasterize_mode=mode)
            return img[0].mean(-1).cpu().numpy(), alpha[0, ..., 0].cpu().numpy()

        img, alpha = render(s)
        renders['%s_value' % cname], renders['%s_alpha' % cname] = img, alpha
        renders['%s_aa_value' % cname], renders['%s_aa_alpha' % cname] = render(s, 'antialiased')
        # centre pixel = alpha * value when nothing else covers it
        alone = []
        for g in cam['gaussians']:
            u, v = int(g['u']), int(g['v'])
            if 0 <= u < W and 0 <= v < H:
                alone.append(abs(img[v, u] - min(1, g['alpha'] * g['value'])))
        check('%s: gsplat centre pixels = alpha * value' % cname, np.median(alone) < 0.01,
              'median %.4f, max %.4f' % (np.median(alone), max(alone)))
        ctrl = {
            'xyzw': dict(s, quats=s['quats'][:, [1, 2, 3, 0]]),
            'shflip': dict(s, sh=s['sh'] * torch.tensor([1, -1, -1, -1, 1, 1, 1, 1, 1] + [-1] * 7,
                                                        device='cuda')[None, :, None]),
            # the viewer's -y up (no header): rotated -90 deg about x, (x, y, z) -> (x, z, -y)
            'yup': dict(s, means=s['means'][:, [0, 2, 1]] * torch.tensor([1, 1, -1], device='cuda'),
                        quats=quat_mul(torch.tensor([0.5 ** 0.5, -0.5 ** 0.5, 0, 0], device='cuda'), s['quats'])),
            'nologit': dict(s, opacities=torch.logit(s['opacities']).clamp(0, 1)),
            'linscale': dict(s, scales=torch.log(s['scales']).clamp(min=1e-3)),
        }
        spz = load_ply(js / 'spz_decoded.ply', 'cuda')
        renders['%s_spz_value' % cname], renders['%s_spz_alpha' % cname] = render(spz)
        for c, sp in ctrl.items():
            renders['%s_%s_value' % (cname, c)], renders['%s_%s_alpha' % (cname, c)] = render(sp)
    np.savez(out, **renders)
    print('wrote', out)
    print('native checks: %s' % ('all pass' if not failures else '%d FAILED' % failures))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
