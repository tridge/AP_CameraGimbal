#!/usr/bin/env python3
"""G1 held-out metrics, one definition for every candidate (train.py's, extended).
Exits nonzero on invalid input or any failed criterion.

  metrics.py score RENDERS DATASET OUT.json     RENDERS: .npz from render_ply.py, a directory of
                                                16-bit PNG renders (Brush eval output, value*65535)
                                                or of NAME.f32 renders (browser worker)
  metrics.py compare CANDIDATE.json BASELINE.json [BASELINE2.json ...]   criteria 1-6 of notes/g1.md
  metrics.py export BRUSH_RENDERS PLY_RENDERS DATASET   criterion 9 (Brush render vs exported PLY)
"""
import argparse
import json
from pathlib import Path
import sys

import numpy as np


class Scale:  # train.py TempScale
    def __init__(self, s):
        self.t0, self.t1, self.k = s['t0_c'], s['t1_c'], s['k_c']
        self.norm = np.log1p((self.t1 - self.t0) / self.k)

    def inverse(self, v):
        return self.t0 + self.k * np.expm1(np.asarray(v, np.float64) * self.norm)


def fail(msg):
    sys.exit('metrics: ' + msg)


def check_run(path):
    """Refuse renders of a run its driver declared invalid (run.json valid: false)."""
    for d in (path.parent, path.parent.parent):
        run = d / 'run.json'
        if run.exists() and json.loads(run.read_text()).get('valid') is False:
            fail('%s belongs to an invalid run (%s)' % (path, run))


def load_renders(path, meta):
    """Held-out renders [views, H, W] in g1.json test order, validated: exactly the frozen views
    (by name, each once), the dataset's dimensions, finite values.
    PATH: .npz from render_ply.py (names + renders), a directory of NAME.f32 (browser worker), or a
    directory holding one 16-bit gray NAME (any extension .png) per view (native Brush eval)."""
    path = Path(path)
    if not path.exists():
        fail('%s does not exist' % path)
    check_run(path)
    test, H, W = list(meta['test']), meta['height'], meta['width']
    if path.suffix == '.npz':
        z = np.load(path)
        names = [str(n) for n in z['names']]
        if names != test:
            fail('%s has views %s, expected %s' % (path, names, test))
        imgs = z['renders']
    elif path.suffix == '.npy':
        fail('%s: bare .npy has no view names; use render_ply.py .npz output' % path)
    else:
        import cv2
        raw = {p.name[:-4]: p for p in path.rglob('*.f32')}
        png = {p.name: p for p in path.rglob('*.png')}
        found = raw or png
        if raw and png:
            fail('%s mixes .f32 and .png renders' % path)
        if sorted(found) != sorted(test) or len(list(path.rglob('*.f32' if raw else '*.png'))) != len(test):
            fail('%s has views %s, expected exactly %s' % (path, sorted(found), test))
        out = []
        for n in test:
            if raw:
                a = np.fromfile(found[n], '<f4')
                if a.size != H * W:
                    fail('%s: %d values, expected %dx%d' % (found[n], a.size, W, H))
                out.append(a.reshape(H, W))
            else:
                im = cv2.imread(str(found[n]), cv2.IMREAD_UNCHANGED)
                if im is None or im.dtype != np.uint16 or im.ndim != 2:
                    fail('%s is not a 16-bit gray render' % found[n])
                out.append(im.astype(np.float32) / 65535)
        imgs = np.stack(out)
    if imgs.shape != (len(test), H, W):
        fail('%s: renders %s, expected %s' % (path, imgs.shape, (len(test), H, W)))
    if not np.isfinite(imgs).all():
        fail('%s: non-finite render values' % path)
    return imgs


def score(renders, dataset):
    import cv2
    meta = json.loads((Path(dataset) / 'g1.json').read_text())
    gt_all = np.load(Path(dataset) / 'gt.npy', mmap_mode='r')
    idx = {n: i for i, n in enumerate(meta['names'])}
    if gt_all.shape != (len(meta['names']), meta['height'], meta['width']):
        fail('gt.npy shape %s does not match g1.json' % (gt_all.shape,))
    if len(set(meta['test'])) != len(meta['test']) or not set(meta['test']) <= set(idx):
        fail('g1.json test views are not unique views of the dataset')
    pred_all = load_renders(renders, meta)
    to_c = Scale(meta['temperature_scale']).inverse
    v_hot, hot_c = meta['v_hot'], meta['hot_c']
    hot_names = set(meta['hot'])
    rows = dict(hot=[], cold=[])
    kernel = np.ones((5, 5), np.uint8)
    for n, pred in zip(meta['test'], pred_all):
        gt = np.asarray(gt_all[idx[n]], np.float64)
        pred = np.clip(pred.astype(np.float64), 0, 1)
        mse = max(float(np.mean((pred - gt) ** 2)), 1e-10)
        m_gt, m_pr = gt > v_hot, pred > v_hot
        near_true = cv2.dilate(m_gt.astype(np.uint8), kernel) > 0
        true_peak, peak = float(to_c(gt.max())), float(to_c(pred.max()))
        rows['hot' if n in hot_names else 'cold'].append(dict(
            name=n, psnr=-10 * np.log10(mse), true_peak_c=true_peak, rendered_peak_c=peak,
            abs_peak_error_c=abs(peak - true_peak),
            hot_iou=float((m_gt & m_pr).sum() / max(1, (m_gt | m_pr).sum())),
            false_hot=bool(n not in hot_names and peak > hot_c),
            false_hot_pixels=int((m_pr & ~near_true).sum())))
    summary = {}
    for kind, r in rows.items():
        e = [x['abs_peak_error_c'] for x in r]
        summary[kind] = dict(views=len(r), psnr_mean=float(np.mean([x['psnr'] for x in r])),
                             psnr_min=float(np.min([x['psnr'] for x in r])),
                             abs_peak_error_median_c=float(np.median(e)), abs_peak_error_max_c=float(np.max(e)),
                             abs_peak_error_p90_c=float(np.percentile(e, 90)),
                             hot_iou_mean=float(np.mean([x['hot_iou'] for x in r])),
                             false_hot_views=int(sum(x['false_hot'] for x in r)),
                             false_hot_pixels=int(sum(x['false_hot_pixels'] for x in r)))
    return dict(summary=summary, views=rows, renders=str(renders))


def criteria(cand, bases):
    """notes/g1.md tolerances v2 as [(criterion, name, [(statistic, value, bound, '>=' or '<=')])]:
    every bound against the worse of the baseline runs; cold PSNR and hot IoU on means only."""
    c = cand['summary']
    views = lambda r: {k: sorted(x['name'] for x in r['views'][k]) for k in ('hot', 'cold')}  # noqa: E731
    for b in bases:
        if views(b) != views(cand):
            fail('candidate and baseline were scored on different views: %s vs %s' % (views(cand), views(b)))
    worst = lambda kind, key, f: f(b['summary'][kind][key] for b in bases)  # noqa: E731
    view = {}
    for b in bases:
        for k in b['views']:
            for x in b['views'][k]:
                view.setdefault(x['name'], []).append(x)
    hot_worst_view = min(x['psnr'] - min(v['psnr'] for v in view[x['name']]) for x in cand['views']['hot'])
    fp = lambda s_: s_['hot']['false_hot_pixels'] + s_['cold']['false_hot_pixels']  # noqa: E731
    return [
        (1, 'cold PSNR', [('mean dB', c['cold']['psnr_mean'], worst('cold', 'psnr_mean', min) - 1.0, '>=')]),
        (2, 'hot PSNR', [('mean dB', c['hot']['psnr_mean'], worst('hot', 'psnr_mean', min) - 1.0, '>='),
                         ('worst view vs baseline dB', hot_worst_view, -2.0, '>=')]),
        (3, 'abs peak error hot', [
            ('median C', c['hot']['abs_peak_error_median_c'], worst('hot', 'abs_peak_error_median_c', max) + 2.0, '<='),
            ('max C', c['hot']['abs_peak_error_max_c'], worst('hot', 'abs_peak_error_max_c', max) + 5.0, '<=')]),
        (4, 'abs peak error cold', [
            ('median C', c['cold']['abs_peak_error_median_c'], worst('cold', 'abs_peak_error_median_c', max) + 0.5, '<='),
            ('max C', c['cold']['abs_peak_error_max_c'], worst('cold', 'abs_peak_error_max_c', max) + 2.0, '<=')]),
        (5, 'hot IoU', [('mean', c['hot']['hot_iou_mean'], worst('hot', 'hot_iou_mean', min) - 0.05, '>=')]),
        (6, 'false hot', [('cold views', c['cold']['false_hot_views'], worst('cold', 'false_hot_views', max), '<='),
                          ('pixels', fp(c), max(fp(x['summary']) for x in bases) + 50, '<=')]),
    ]


def within(value, bound, op):
    return value >= bound if op == '>=' else value <= bound


def compare(cand, bases):
    res = []
    for num, name, stats in criteria(cand, bases):
        ok = all(within(v, b, op) for _, v, b, op in stats)
        detail = ', '.join('%s %.3f (%s %.3f)' % (k, v, op, b) for k, v, b, op in stats)
        res.append(dict(criterion=num, name=name, pass_=ok, detail=detail))
    return res


def export_check(brush, ply, dataset):
    meta = json.loads((Path(dataset) / 'g1.json').read_text())
    to_c = Scale(meta['temperature_scale']).inverse
    a, b = load_renders(brush, meta).astype(np.float64), load_renders(ply, meta).astype(np.float64)
    if a.shape != b.shape:
        fail('export check: %s vs %s renders' % (a.shape, b.shape))
    rows = []
    for n, x, y in zip(meta['test'], a, b):
        rows.append(dict(name=n, psnr=float(-10 * np.log10(max(np.mean((x - y) ** 2), 1e-10))),
                         peak_diff_c=float(abs(to_c(x.max()) - to_c(y.max())))))
    ok = all(r['psnr'] >= 45 and r['peak_diff_c'] <= 1.0 for r in rows)
    return dict(pass_=ok, min_psnr=min(r['psnr'] for r in rows), max_peak_diff_c=max(r['peak_diff_c'] for r in rows),
                views=rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('score')
    s.add_argument('renders')
    s.add_argument('dataset')
    s.add_argument('out', type=Path)
    c = sub.add_parser('compare')
    c.add_argument('candidate', type=Path)
    c.add_argument('baseline', type=Path, nargs='+')
    e = sub.add_parser('export')
    e.add_argument('brush')
    e.add_argument('ply')
    e.add_argument('dataset')
    a = ap.parse_args()
    if a.cmd == 'score':
        r = score(a.renders, a.dataset)
        a.out.write_text(json.dumps(r, indent=1))
        print(json.dumps(r['summary'], indent=1))
    elif a.cmd == 'compare':
        res = compare(json.loads(a.candidate.read_text()), [json.loads(b.read_text()) for b in a.baseline])
        for r in res:
            print('%d %-24s %s  %s' % (r['criterion'], r['name'], 'PASS' if r['pass_'] else 'FAIL', r['detail']))
        sys.exit(0 if all(r['pass_'] for r in res) else 1)
    else:
        r = export_check(a.brush, a.ply, a.dataset)
        print(json.dumps({k: v for k, v in r.items() if k != 'views'}, indent=1))
        sys.exit(0 if r['pass_'] else 1)


if __name__ == '__main__':
    main()
