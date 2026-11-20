#!/usr/bin/env python3
"""Per-view held-out metrics of train.py runs (metrics.json + scale.json): hot/cold PSNR,
absolute peak error distribution, hot-region overlap and false hot detections.
usage: metrics_table.py RUN_DIR [RUN_DIR ...]   (prints markdown)"""
import json
import sys
from pathlib import Path

import numpy as np


def summary(run):
    m = json.loads((run / 'metrics.json').read_text())
    hot_c = json.loads((run / 'scale.json').read_text())['hot_threshold_c']
    out = {}
    for kind, rows in m['views'].items():
        err = np.array([r['rendered_peak_c'] - r['true_peak_c'] for r in rows])
        out[kind] = dict(
            views=[r['name'][:5] for r in rows], psnr=[round(r['psnr'], 2) for r in rows],
            psnr_mean=float(np.mean([r['psnr'] for r in rows])),
            abs_peak_err=dict(median=float(np.median(abs(err))), mean=float(np.mean(abs(err))),
                              max=float(np.max(abs(err)))),
            peak_err=[round(float(e), 2) for e in err],
            iou=[round(r['hot_iou'], 3) for r in rows], iou_mean=float(np.mean([r['hot_iou'] for r in rows])),
            false_hot=int(sum(r['rendered_peak_c'] > hot_c for r in rows)) if kind == 'cold' else None)
    return out


def main():
    runs = [Path(p) for p in sys.argv[1:]]
    s = {r.name: summary(r) for r in runs}
    for name in s:
        sc = json.loads((Path([r for r in runs if r.name == name][0]) / 'scale.json').read_text())
        h = sc.get('hotspot') or {}
        print('%s: %d Gaussians, hot threshold %.2f C, scale %s, hot spot ENU %s (%s frames, %.2f px)' % (
            name, sc['gaussians'], sc['hot_threshold_c'], sc['temperature_scale'],
            [round(v, 3) for v in h.get('enu', [])], h.get('frames'), h.get('median_ray_error_px', 0)))
    print()
    print('| run | split | views | PSNR mean (dB) | abs peak err median / mean / max (C) | hot IoU mean | false hot views |')
    print('|---|---|---|---|---|---|---|')
    for name, d in s.items():
        for kind in ('hot', 'cold'):
            k = d[kind]
            a = k['abs_peak_err']
            print('| %s | %s | %d | %.2f | %.2f / %.2f / %.2f | %.3f | %s |' % (
                name, kind, len(k['views']), k['psnr_mean'], a['median'], a['mean'], a['max'], k['iou_mean'],
                '-' if k['false_hot'] is None else k['false_hot']))
    print()
    for kind in ('hot', 'cold'):
        views = next(iter(s.values()))[kind]['views']
        print('%s views: %s' % (kind, ' '.join(views)))
        print()
        print('| run | ' + ' | '.join(views) + ' |')
        print('|---|' + '---|' * len(views))
        for name, d in s.items():
            k = d[kind]
            assert k['views'] == views, 'holdouts differ'
            print('| %s PSNR | %s |' % (name, ' | '.join('%.2f' % v for v in k['psnr'])))
            print('| %s peak err C | %s |' % (name, ' | '.join('%+.2f' % v for v in k['peak_err'])))
            if kind == 'hot':
                print('| %s IoU | %s |' % (name, ' | '.join('%.3f' % v for v in k['iou'])))
        print()


if __name__ == '__main__':
    main()
