#!/usr/bin/env python3
"""Stage 7 held-out evaluation of the three forms of a splat, scored separately with G1's
metrics.py (imported, train.py's definitions) at the frozen held-out views of DATASET (g1.json,
gt.npy):
  training  the trainer's own renders: Brush eval renders (a directory of NAME.f32, as G1's
            browser runs write), or train.py's splat.pt rendered with its emission lobe (gain 1)
  ply       the exported splat.ply (greyscale SH), rendered as the viewer decodes (render_ply.py)
  clamped   splat.ply with SPZ's range limits applied (decode_spz.mjs), rendered the same way
  spz       the exported splat.spz decoded to a PLY (decode_spz.mjs), rendered the same way
and the step errors: training -> PLY (lobe projection, greyscale; G1 criterion 9's PSNR and peak),
PLY -> clamped (SPZ's value ranges: SH within [-1, 1], colour within its byte) and clamped -> SPZ
(quantisation).
  evaluate.py DATASET OUT.json --training DIR|splat.pt --ply splat.ply --clamped-ply c.ply
             --spz-ply spz.ply [--require-export]
--require-export: fail unless training -> PLY meets criterion 9 (>= 45 dB, <= 1 C).
--require-training-ply DB C: the same with other bounds (run13's lobe projection: 50 dB, 0.5 C).
Exit 1 on a failed requirement or invalid input.
"""
import argparse
import json
from pathlib import Path
import sys

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2]))  # tools/GaussianSplat
sys.path.insert(0, str(HERE.parent / 'g1'))
from splat_common import setup_cuda_env  # noqa: E402

setup_cuda_env()
import metrics  # noqa: E402
from render_ply import load_ply, render  # noqa: E402

# clamped -> SPZ: quantisation only (SPZ's SH steps are 1/16 and 1/8 of the coefficient range,
# so hot peaks move by a few C); the clamping loss is reported, not bounded
SPZ_MIN_PSNR_DB = 33.0
SPZ_MAX_PEAK_DIFF_C = 5.0


def render_pt(path, meta, vms, K):
    """train.py's checkpoint with its lobe (source gain 1), as train.py renders values."""
    import torch
    from gsplat import rasterization
    from splat_model import view_values
    ck = torch.load(path, map_location='cuda', weights_only=False)
    p = {k: v.cuda().float() for k, v in ck['params'].items()}
    q = torch.nn.functional.normalize(p['quats'], dim=1)
    out = []
    for vm in vms:
        campos = torch.linalg.inv(vm)[:3, 3][None]
        v = view_values(p, campos, ck['sh_degree'])[..., None]
        img, _, _ = rasterization(p['means'], q, torch.exp(p['scales']), torch.sigmoid(p['opacities']), v,
                                  vm[None], K[None], meta['width'], meta['height'], packed=False, near_plane=1.0)
        out.append(img[0, ..., 0].clamp(0, 1).cpu().numpy())
    return np.stack(out).astype(np.float32)


def psnr_peak(a, b, to_c):
    rows = []
    for x, y in zip(a.astype(np.float64), b.astype(np.float64)):
        rows.append(dict(psnr=float(-10 * np.log10(max(np.mean((x - y) ** 2), 1e-10))),
                         peak_diff_c=float(abs(to_c(x.max()) - to_c(y.max())))))
    return dict(min_psnr=min(r['psnr'] for r in rows), max_peak_diff_c=max(r['peak_diff_c'] for r in rows),
                views=rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dataset', type=Path)
    ap.add_argument('out', type=Path)
    ap.add_argument('--training', type=Path, required=True)
    ap.add_argument('--ply', type=Path, required=True)
    ap.add_argument('--clamped-ply', type=Path, required=True)
    ap.add_argument('--spz-ply', type=Path, required=True)
    ap.add_argument('--require-export', action='store_true')
    ap.add_argument('--require-training-ply', nargs=2, type=float, metavar=('DB', 'C'),
                    help='fail unless training -> PLY has min PSNR >= DB and max peak difference <= C')
    a = ap.parse_args()
    import torch
    meta = json.loads((a.dataset / 'g1.json').read_text())
    idx = {n: i for i, n in enumerate(meta['names'])}
    vms = torch.tensor(np.array(meta['viewmats'])[[idx[n] for n in meta['test']]], dtype=torch.float32,
                       device='cuda')
    K = torch.tensor(meta['K'], dtype=torch.float32, device='cuda')
    work = a.out.with_suffix('')
    work.mkdir(parents=True, exist_ok=True)
    names = np.array(meta['test'])
    with torch.no_grad():
        if a.training.suffix == '.pt':
            np.savez(work / 'training.npz', names=names, renders=render_pt(a.training, meta, vms, K))
            training = work / 'training.npz'
        else:
            training = a.training
        for key, ply in (('ply', a.ply), ('clamped', a.clamped_ply), ('spz', a.spz_ply)):
            s = load_ply(ply, 'cuda')
            np.savez(work / ('%s.npz' % key), names=names, renders=render(s, vms, K, meta['width'], meta['height']))
    forms = {'training': training, 'ply': work / 'ply.npz', 'clamped': work / 'clamped.npz',
             'spz': work / 'spz.npz'}
    res = {k: metrics.score(v, a.dataset) for k, v in forms.items()}
    to_c = metrics.Scale(meta['temperature_scale']).inverse
    load = {k: metrics.load_renders(v, meta) for k, v in forms.items()}
    steps = {'training_to_ply': psnr_peak(load['training'], load['ply'], to_c),
             'ply_to_clamped': psnr_peak(load['ply'], load['clamped'], to_c),
             'clamped_to_spz': psnr_peak(load['clamped'], load['spz'], to_c),
             'ply_to_spz': psnr_peak(load['ply'], load['spz'], to_c)}
    report = dict(scores={k: v['summary'] for k, v in res.items()}, steps=steps,
                  views={k: v['views'] for k, v in res.items()})
    a.out.write_text(json.dumps(report, indent=1))
    print('%-9s %9s %9s %12s %12s %8s' % ('form', 'cold dB', 'hot dB', 'hot peak C', 'cold peak C', 'hot IoU'))
    for k, v in res.items():
        s = v['summary']
        print('%-9s %9.2f %9.2f %12.2f %12.2f %8.3f' % (k, s['cold']['psnr_mean'], s['hot']['psnr_mean'],
                                                       s['hot']['abs_peak_error_median_c'],
                                                       s['cold']['abs_peak_error_median_c'], s['hot']['hot_iou_mean']))
    for k, v in steps.items():
        print('%s: min PSNR %.1f dB, max peak difference %.2f C' % (k, v['min_psnr'], v['max_peak_diff_c']))
    failed = 0
    if a.require_export:
        t = steps['training_to_ply']
        ok = t['min_psnr'] >= 45 and t['max_peak_diff_c'] <= 1.0
        print('%s training -> PLY meets G1 criterion 9 (>= 45 dB, <= 1 C)' % ('PASS' if ok else 'FAIL'))
        failed += not ok
    if a.require_training_ply:
        t = steps['training_to_ply']
        db, c = a.require_training_ply
        ok = t['min_psnr'] >= db and t['max_peak_diff_c'] <= c
        print('%s training -> PLY within >= %.0f dB, <= %.2f C (%.1f dB, %.2f C)'
              % ('PASS' if ok else 'FAIL', db, c, t['min_psnr'], t['max_peak_diff_c']))
        failed += not ok
    s = steps['clamped_to_spz']
    ok = s['min_psnr'] >= SPZ_MIN_PSNR_DB and s['max_peak_diff_c'] <= SPZ_MAX_PEAK_DIFF_C
    print('%s clamped -> SPZ (quantisation) within >= %.0f dB, <= %.0f C' % ('PASS' if ok else 'FAIL',
                                                                           SPZ_MIN_PSNR_DB, SPZ_MAX_PEAK_DIFF_C))
    failed += not ok
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
