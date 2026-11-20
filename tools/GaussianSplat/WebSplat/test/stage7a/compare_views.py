#!/usr/bin/env python3
"""Stage 7a: the viewer's screenshots and temperature readouts (viewer_check.mjs) against gsplat
renders of the same PLY at the same cameras (check_native.py), decoded as the viewer decodes
(channel mean, log1p inverse, whitehot over the fixed range, times alpha).
  - every PLY screenshot matches the correct gsplat render (classic, as the exports say; a meta
    without rasterize_mode: antialiased), and each negative control (quaternion read x,y,z,w; SH
    direction reversed; -y up; opacity without logit; linear scales; the other render mode) is
    clearly worse: this is what checks quaternion layout, SH direction, axes/up, opacity and scale encoding
  - the readout at each Gaussian centre equals the gsplat-rendered temperature there (PLY), or the
    temperature predicted from the decoded SPZ parameters (SPZ)
  - embedded viewer = open-a-file viewer, pixel for pixel; site page = ?url SPZ page
  compare_views.py JS GSPLAT.npz VIEWS      (exit 1 on failure)
"""
import json
from pathlib import Path
import sys

import cv2
import numpy as np

failures = 0
# image tolerances (8-bit grey, over pixels covered in either image): mean abs difference and
# 99.9th percentile
MEAN_TOL, P999_TOL = 1.5, 16
# readouts (shown to 0.1 C): the scaled value within 0.005 (measured on the tiny model: 0.0030 PLY,
# 0.0041 SPZ; Spark's packing and the half-float target), plus the display rounding
READOUT_DV, READOUT_ROUND_C = 0.005, 0.06
CONTROL_FACTOR = 3  # a negative control must be this much worse, on the image or the readouts


def check(name, ok, detail=''):
    global failures
    print('%s %s%s' % ('PASS' if ok else 'FAIL', name, (': ' + detail) if detail else ''))
    failures += not ok


def main():
    js, npz, views = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    meta = json.loads((js / 'meta.json').read_text())
    ex = json.loads((js / 'expected.json').read_text())
    res = json.loads((views / 'viewer.json').read_text())
    r = np.load(npz)
    gs = Path(__file__).resolve().parents[3]
    lut = np.array(json.loads((gs / 'viewer' / 'public' / 'colormaps.json').read_text())['whitehot'])[:, 0]
    s = meta['temperature_scale']
    norm = np.log1p((s['t1_c'] - s['t0_c']) / s['k_c'])
    lo, hi = meta['display_range_c']

    def to_c(v):
        return s['t0_c'] + s['k_c'] * np.expm1(np.clip(v, 0, 1) * norm)

    def expected_image(value, alpha):
        x = np.clip((to_c(value) - lo) / max(hi - lo, 0.1), 0, 1) * 255
        i = np.clip(np.floor(x).astype(int), 0, 254)
        g = lut[i] + (lut[i + 1] - lut[i]) * (x - i)
        return np.clip(g * np.clip(alpha, 0, 1), 0, 1) * 255

    def shot(name):
        return cv2.imread(str(views / (name + '.png')), cv2.IMREAD_GRAYSCALE).astype(np.float64)

    def slope(v):  # dT/dv
        return s['k_c'] * norm * np.exp(np.clip(v, 0, 1) * norm)

    def max_dv(temps, value, rows):
        out = 0
        for t, g in zip(temps, rows):
            v = np.log1p(max(t - s['t0_c'], 0) / s['k_c']) / norm
            out = max(out, abs(v - value[int(g['v']), int(g['u'])]))
        return out

    def readout_errors(temps, value, rows):
        errs, tols = [], []
        for t, g in zip(temps, rows):
            v = value[int(g['v']), int(g['u'])]
            errs.append(abs(t - float(to_c(v))) if t is not None else np.inf)
            tols.append(READOUT_ROUND_C + READOUT_DV * slope(v))
        return np.array(errs), np.array(tols)

    def image_error(img, value, alpha, extra=None):
        e = expected_image(value, alpha)
        mask = (e > 4) | (img > 4)
        if extra is not None:
            mask |= extra > 4
        d = np.abs(img - e)[mask]
        return d.mean(), np.percentile(d, 99.9), ((e > 8) & (img > 8)).sum() / max(((e > 8) | (img > 8)).sum(), 1)

    report = {}
    cases = {'file_start', 'spz_start', 'site_start', 'embed_start', 'embed_back', 'embed_legacy_back',
             'embed_spz_start', 'spz_embedded_start', 'file_spz_start'}
    check('every viewer case present', set(res['cases']) == cases, 'got %s' % sorted(res['cases']))
    for name, case in res['cases'].items():
        cam = case['camera']
        img = shot(name)
        rows = ex['cameras'][cam]['gaussians']
        temps = case['temps']
        if len(temps) != len(rows) or any(t is None for t in temps):
            check('%s: one readout at each of the %d Gaussian centres' % (name, len(rows)), False,
                  '%d readouts, %d missing' % (len(temps), sum(t is None for t in temps)))
            continue
        legacy = case.get('mode') == 'antialiased'  # no rasterize_mode: Spark's default
        ref = '%s_spz' % cam if case.get('spz') else '%s_aa' % cam if legacy else cam
        value, alpha = r[ref + '_value'], r[ref + '_alpha']
        what = 'decoded SPZ' if case.get('spz') else 'PLY, %s' % ('antialiased' if legacy else 'classic')
        errs, tols = readout_errors(temps, value, rows)
        check('%s: readouts at the Gaussian centres = gsplat (%s)' % (name, what),
              bool(np.all(errs <= tols)), 'max %.2f C (tolerance there %.2f C), max scaled-value error %.4f'
              % (errs.max(), tols[errs.argmax()], max_dv(temps, value, rows)))
        mean, p999, iou = image_error(img, value, alpha)
        check('%s: screenshot = gsplat render' % name, mean < MEAN_TOL and p999 < P999_TOL and iou > 0.9,
              'mean %.3f, p99.9 %.1f, coverage IoU %.3f' % (mean, p999, iou))
        rep = dict(readout_max_c=float(errs.max()), mean=mean, p999=p999, iou=iou)
        if case.get('spz'):
            d = [abs(t - g['centreC']) for t, g in zip(temps, rows)]
            rep['spz_vs_ply_c'] = d
            print('  %s: SPZ vs PLY temperature at the centres: max %.2f C, median %.2f C'
                  % (name, max(d), float(np.median(d))))
            report[name] = rep
            continue
        controls = ['xyzw', 'shflip', 'yup', 'nologit', 'linscale'] if not legacy else []
        # the render mode shows on the thin disc from the back (from the start view the modes agree
        # within the readout tolerance)
        if cam == 'back':
            controls.append('classic' if legacy else 'aa')
        rep['controls'] = {}
        for c in controls:
            key = cam if c == 'classic' else '%s_%s' % (cam, c)
            cv, ca = r[key + '_value'], r[key + '_alpha']
            cm = image_error(img, cv, ca, expected_image(value, alpha))[0]
            ce, _ = readout_errors(temps, cv, rows)
            img_bad = cm > CONTROL_FACTOR * mean and cm > 2
            read_bad = ce.max() > CONTROL_FACTOR * errs.max() and ce.max() > 1
            rep['controls'][c] = dict(image_mean=cm, readout_max_c=float(ce.max()))
            check('%s: negative control %s rejected' % (name, c), img_bad or read_bad,
                  'image %.2f vs %.2f, readout %.2f C vs %.2f C' % (cm, mean, ce.max(), errs.max()))
        report[name] = rep
    a, b = shot('file_start'), shot('embed_start')
    check('embedded viewer = open-a-file viewer', np.abs(a - b).max() <= 1, 'max %.0f' % np.abs(a - b).max())
    a, b = shot('spz_start'), shot('site_start')
    check('site page = ?url SPZ page', np.abs(a - b).max() <= 1, 'max %.0f' % np.abs(a - b).max())
    a, b = shot('spz_start'), shot('embed_spz_start')
    check('embedded SPZ = ?url SPZ page', np.abs(a - b).max() <= 1, 'max %.0f' % np.abs(a - b).max())
    for name in ('spz_embedded_start', 'file_spz_start'):  # metadata from the SPZ's gzip header
        b = shot(name)
        check('%s = ?url SPZ page with meta.json' % name, np.abs(a - b).max() <= 1, 'max %.0f' % np.abs(a - b).max())
    (views / 'compare.json').write_text(json.dumps(report, indent=1))
    print('view checks: %s' % ('all pass' if not failures else '%d FAILED' % failures))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
