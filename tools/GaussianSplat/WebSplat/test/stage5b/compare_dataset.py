#!/usr/bin/env python3
"""Compare a browser stage 5b dataset (CONTRACTS.md 5b) with dump_scene.py's dump of train.py's scene
loading for the same model and options. Exits 1 if any check fails, 2 on unusable input.

  compare_dataset.py NATIVE_DIR BROWSER_DIR [--frozen] [--json OUT.json]

Tolerances (stated, notes/stage5b.md):
- views, order, splits, hot flags, registered/skipped: identical; --frozen: the 13 frozen holdouts
- K, size: identical; viewmats: identical float32 values
- temperature scale t0/t1/k: |diff| <= 1e-6 C; hot_c: <= 1e-9 C; v_hot: <= 1e-12
- undistorted Celsius (when both dumped): |diff| <= 1e-4 C at every pixel
- targets (float16): at most 1 float16 ulp anywhere, and at most 1e-5 of pixels differing
- bounds z: <= 1e-9 m; ceiling grid: identical float32
- initial points: same count, same positions (float32), values |diff| <= 1e-6
- hot spot: same frame count; position within 1e-3 m of both native hot spots (load_scene's and
  main()'s re-located one); median ray error within 1e-3 px
- extent within 1e-9 relative; learning rates, means schedule, DefaultStrategy fields and
  scene_scale identical (1e-12 relative); training options identical to the native options
- schema: required fields, scale kind, undistortion record, dtypes, shapes and file sizes, split
  and hot enums, frame/image_id per view, rejection reason per view and the pose-offset limit,
  complete option key sets, init constants, near plane, bounds-pruning frequency, v_hot
"""
import argparse
import json
from pathlib import Path
import sys

import numpy as np

SCENE_OPTIONS = ['max_images', 'hot_delta', 'min_elevation', 'min_obs', 'max_pose_offset', 'holdout',
                 'hot_holdout', 'k', 't0', 't1', 'radius', 'height_margin', 'ceiling_cell', 'ceiling_margin']
OUTPUT_ONLY = {'colormap'}  # train.py options that only affect its output files
FROZEN = dict(cold='00015 00035 00066 00086 00115 00135 00167 00187'.split(),
              hot='00009 00050 00096 00140 00149'.split())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('native', type=Path)
    ap.add_argument('browser', type=Path)
    ap.add_argument('--frozen', action='store_true', help='also require the 13 frozen reference holdouts')
    ap.add_argument('--json', type=Path)
    a = ap.parse_args()
    try:
        N = json.loads((a.native / 'dataset.json').read_text())
        B = json.loads((a.browser / 'dataset.json').read_text())
    except (OSError, ValueError) as e:
        print('unusable input: %s' % e)
        sys.exit(2)
    if B.get('format') != 'websplat.dataset' or B.get('version') != 1:
        print('browser dataset: format %s version %s' % (B.get('format'), B.get('version')))
        sys.exit(2)
    checks, info = [], {}

    def check(name, ok, detail=''):
        checks.append(dict(name=name, ok=bool(ok), detail=detail))
        print('%s %s%s' % ('PASS' if ok else 'FAIL', name, ': ' + detail if detail else ''))

    rel = lambda x, y: abs(x - y) / max(abs(y), 1e-300)  # noqa: E731
    W, H, n = B.get('width'), B.get('height'), len(B.get('images', []))

    # schema: required fields, enums, dtypes and shapes
    need = ['width', 'height', 'K', 'undistortion', 'scale', 'hot_c', 'v_hot', 'images', 'skipped', 'bounds',
            'hotspot', 'extent_m', 'lr', 'training', 'targets', 'points', 'options']
    miss = [k for k in need if k not in B]
    check('required fields present', not miss, ','.join(miss))
    if miss:
        print('FAILED: incomplete dataset')
        sys.exit(1)
    check('scale kind log1p', B['scale'].get('kind') == 'log1p', str(B['scale'].get('kind')))
    distorted = N['camera']['model'] == 'SIMPLE_RADIAL'
    u = B['undistortion']
    check('undistortion record matches the camera',
          (u is None) if not distorted else (isinstance(u, dict) and u.get('from', {}).get('model') == 'SIMPLE_RADIAL'
                                              and u['from'].get('params') == N['camera']['params']
                                              and u.get('method') == N['init']['undistortion_method']),
          json.dumps(u)[:120])
    tg, pt = B['targets'], B['points']
    check('targets dtype f16, shape [n, H, W]', tg.get('dtype') == 'f16' and tg.get('shape') == [n, H, W]
          and (a.browser / tg.get('file', '')).is_file() and (a.browser / tg['file']).stat().st_size == 2 * n * W * H,
          json.dumps(tg))
    pfile = a.browser / pt.get('file', '')
    check('points dtype f32, shape [count, 4]', pt.get('dtype') == 'f32' and pt.get('shape') == [pt.get('count'), 4]
          and pfile.is_file() and pfile.stat().st_size == 16 * pt.get('count', -1), json.dumps(pt))
    cf = B['bounds'].get('ceiling', {})
    check('ceiling file [n, n] f32', (a.browser / cf.get('file', '')).is_file()
          and (a.browser / cf['file']).stat().st_size == 4 * cf.get('n', -1) ** 2, json.dumps(cf))
    bad = [im.get('name') for im in B['images'] if im.get('split') not in ('train', 'test')
           or not isinstance(im.get('hot'), bool) or len(im.get('viewmat', [])) != 16]
    check('views: split train|test, hot boolean, 16-value viewmat', not bad, ' '.join(map(str, bad[:5])))
    if [im['name'] for im in N['images']] == [im.get('name') for im in B['images']]:
        bad = [b['name'] for nv, b in zip(N['images'], B['images'])
               if b.get('frame') != nv['frame'] or b.get('image_id') != nv['image_id']]
        check('views: frame (frames.json position) and image_id', not bad, '%d wrong %s' % (len(bad), bad[:3]))
    sk = B['skipped']
    reasons = ('elevation', 'min_obs', 'pose_offset', 'budget')
    bad = {r: (sk.get(r), N['skipped'][r]) for r in reasons if sorted(sk.get(r) or []) != N['skipped'][r]}
    check('rejection reasons per view (elevation, observations, pose offset, budget)',
          not bad and set(k for k in sk if isinstance(sk[k], list)) == set(reasons),
          str({r: len(N['skipped'][r]) for r in reasons}) + (' differ: %s' % str(bad)[:200] if bad else ''))
    lim = sk.get('pose_offset_limit_m')
    check('pose offset limit', isinstance(lim, (int, float)) and rel(lim, N['skipped']['pose_offset_limit_m']) <= 1e-9,
          '%r vs %r' % (lim, N['skipped']['pose_offset_limit_m']))
    nn = [im['name'] for im in N['images']]
    bn = [im['name'] for im in B['images']]
    check('chosen views identical (names, order)', nn == bn, '%d native, %d browser' % (len(nn), len(bn)))
    same = nn == bn
    nt = sorted(im['name'] for im in N['images'] if im['split'] == 'test')
    bt = sorted(im['name'] for im in B['images'] if im['split'] == 'test')
    check('holdouts identical', nt == bt, ' '.join(bt))
    nh = sorted(im['name'] for im in N['images'] if im['hot'])
    bh = sorted(im['name'] for im in B['images'] if im['hot'])
    check('hot views identical', nh == bh, '%d hot' % len(bh))
    if a.frozen:
        want = sorted(n + '.png' for n in FROZEN['cold'] + FROZEN['hot'])
        check('frozen reference holdouts', bt == want and {n + '.png' for n in FROZEN['hot']} <= set(bh), ' '.join(want))
    bskip = set(sum((v for k, v in B['skipped'].items() if isinstance(v, list)), []))
    check('skipped = registered - chosen', bskip == set(N['registered']) - set(nn),
          '%d skipped: %s' % (len(bskip), {k: len(v) for k, v in B['skipped'].items() if isinstance(v, list)}))
    check('image size identical', (N['width'], N['height']) == (B['width'], B['height']))
    check('K identical (refined camera)', np.array_equal(np.array(N['K']), np.array(B['K'])), str(B['K']))
    if same:
        vn = np.array([im['viewmat'] for im in N['images']], np.float32)
        vb = np.array([im['viewmat'] for im in B['images']], np.float64)
        d = float(np.abs(vb - vn.astype(np.float64)).max())
        check('viewmats identical (float32)', d == 0, 'max |diff| %.3g' % d)
    for k, tol in (('t0_c', 1e-6), ('t1_c', 1e-6), ('k_c', 0)):
        d = abs(B['scale'][k] - N['scale'][k])
        check('scale %s' % k, d <= tol, 'browser %r native %r' % (B['scale'][k], N['scale'][k]))
    check('hot_c', abs(B['hot_c'] - N['hot_c']) <= 1e-9, '%r vs %r' % (B['hot_c'], N['hot_c']))
    check('v_hot', abs(B['v_hot'] - N['v_hot']) <= 1e-12, '%r vs %r' % (B['v_hot'], N['v_hot']))

    if same and N.get('celsius') and (a.browser / 'celsius.f32').exists():
        cn = np.fromfile(a.native / 'celsius.f32', np.float32)
        cb = np.fromfile(a.browser / 'celsius.f32', np.float32)
        if cn.size == cb.size == n * W * H:
            d = np.abs(cn - cb)
            info['celsius'] = dict(max=float(d.max()), differing=int((d > 0).sum()))
            check('undistorted Celsius within 1e-4 C', d.max() <= 1e-4,
                  'max |diff| %.3g C, %d of %d pixels differ' % (d.max(), (d > 0).sum(), d.size))
        else:
            check('undistorted Celsius sizes', False, '%d vs %d' % (cn.size, cb.size))
    if same:
        tn = np.fromfile(a.native / 'targets.f16', np.uint16)
        tb = np.fromfile(a.browser / B['targets']['file'], np.uint16)
        if tn.size != n * W * H or tb.size != n * W * H:
            check('targets size', False, '%d / %d values, expected %d' % (tn.size, tb.size, n * W * H))
        else:
            ulp = np.abs(tn.astype(np.int32) - tb.astype(np.int32))  # non-negative halves
            dv = np.abs(tn.view(np.float16).astype(np.float64) - tb.view(np.float16).astype(np.float64))
            frac = float((ulp > 0).mean())
            info['targets'] = dict(max_ulp=int(ulp.max()), differing=int((ulp > 0).sum()), max_abs=float(dv.max()))
            check('targets within 1 float16 ulp, <= 1e-5 of pixels differ', ulp.max() <= 1 and frac <= 1e-5,
                  'max %d ulp (%.3g), %d of %d pixels differ' % (ulp.max(), dv.max(), (ulp > 0).sum(), ulp.size))

    for k in ('z_min', 'z_max'):
        check('bounds %s' % k, abs(B['bounds'][k] - N['bounds'][k]) <= 1e-9, '%r vs %r' % (B['bounds'][k], N['bounds'][k]))
    check('bounds centre, radius', B['bounds']['centre'] == N['bounds']['centre'] and B['bounds']['radius'] == N['bounds']['radius'])
    cnb, cbb = N['bounds']['ceiling'], B['bounds']['ceiling']
    gn = np.fromfile(a.native / 'ceiling.f32', np.float32)
    gb = np.fromfile(a.browser / cbb['file'], np.float32)
    check('canopy ceiling identical', cnb['origin'] == cbb['origin'] and cnb['cell'] == cbb['cell'] and
          cnb['n'] == cbb['n'] and gn.shape == gb.shape and np.array_equal(gn, gb),
          '%dx%d cells, median %.2f m' % (cbb['n'], cbb['n'], float(np.median(gb))))

    pn = np.load(a.native / 'points64.npy')
    pb = np.fromfile(a.browser / B['points']['file'], np.float32).reshape(-1, 4)
    check('initial point count', len(pn) == len(pb) == B['points']['count'], '%d native, %d browser' % (len(pn), len(pb)))
    if len(pn) == len(pb):
        xn = pn[:, :3].astype(np.float32)
        # match by position; points at the same position (the model has some) by value
        vn32 = pn[:, 3].astype(np.float32)
        on = np.lexsort([vn32, xn[:, 2], xn[:, 1], xn[:, 0]])
        ob = np.lexsort([pb[:, 3], pb[:, 2], pb[:, 1], pb[:, 0]])
        info['duplicate_positions'] = int(len(xn) - len(np.unique(xn, axis=0)))
        dpos = float(np.abs(xn[on] - pb[ob, :3]).max())
        dval = float(np.abs(pn[on, 3] - pb[ob, 3].astype(np.float64)).max())
        info['points'] = dict(max_pos=dpos, max_val=dval)
        check('initial point positions identical', dpos == 0, 'max |diff| %.3g m' % dpos)
        exact = int((pn[on, 3].astype(np.float32) != pb[ob, 3]).sum())
        check('initial point values within 1e-6', dval <= 1e-6,
              'max |diff| %.3g, %d differ from the native float64 value rounded to float32' % (dval, exact))

    hn, hp, hb = N['hotspot'], N['hotspot_post'], B['hotspot']
    if hn is None or hb is None:
        check('hot spot', hn is None and hb is None, 'native %s browser %s' % (hn, hb))
    else:
        d1 = float(np.linalg.norm(np.subtract(hb['enu'], hn['enu'])))
        d2 = float(np.linalg.norm(np.subtract(hb['enu'], hp['enu'])))
        info['hotspot'] = dict(vs_load_scene_m=d1, vs_relocated_m=d2)
        check('hot spot re-located with refined poses', hb['frames'] == hn['frames'] and max(d1, d2) <= 1e-3 and
              abs(hb['median_ray_error_px'] - hn['median_ray_error_px']) <= 1e-3,
              'ENU %s, %d frames, %.3g m from load_scene, %.3g m from the re-located one'
              % (np.round(hb['enu'], 3).tolist(), hb['frames'], d1, d2))

    T = B['training']
    check('extent', rel(B['extent_m'], N['extent_m']) <= 1e-9, '%r vs %r' % (B['extent_m'], N['extent_m']))
    lr_ok = set(T['lr']) == set(N['lr']) and all(rel(T['lr'][k], N['lr'][k]) <= 1e-12 for k in N['lr'])
    check('learning rates', lr_ok and B['lr'] == T['lr'], json.dumps(T['lr']))
    check('Adam eps', all(v == T['adam_eps'] for v in N['adam_eps'].values()), str(T['adam_eps']))
    check('means LR schedule', rel(T['means_lr_gamma'], N['means_gamma'] if 'means_gamma' in N else N['means_lr_gamma']) <= 1e-12,
          '%r' % T['means_lr_gamma'])
    sd = {k: (v, N['strategy'].get(k)) for k, v in T['strategy'].items() if v != N['strategy'].get(k)}
    check('densification and pruning (DefaultStrategy)', not sd and set(T['strategy']) == set(N['strategy']), str(sd))
    check('scene_scale', T['scene_scale'] == N['scene_scale'], str(T['scene_scale']))
    od = {k: (v, N['options'].get(k)) for k, v in {**B['options'], **T['options']}.items() if v != N['options'].get(k)}
    check('options identical', not od, str(od))
    want = set(N['options']) - OUTPUT_ONLY
    keys_ok = (sorted(B['options']) == sorted(SCENE_OPTIONS) and not set(B['options']) & set(T['options'])
               and set(B['options']) | set(T['options']) == want)
    check('complete option key sets (scene, training)', keys_ok,
          'missing %s, extra %s' % (sorted(want - set(B['options']) - set(T['options'])),
                                    sorted((set(B['options']) | set(T['options'])) - want)))
    ini, nini = T.get('init', {}), N['init']
    check('initialisation constants and seed',
          all(k in ini and ini[k] == nini[k] for k in ('opacity', 'knn', 'min_scale', 'quats', 'seed')),
          '%s vs %s' % (ini, nini))
    check('SH degree schedule', T.get('sh_degree_schedule') == nini['sh_degree_schedule'],
          '%s vs %s' % (T.get('sh_degree_schedule'), nini['sh_degree_schedule']))
    check('near plane', T.get('near_plane') == nini['near_plane'], str(T.get('near_plane')))
    check('bounds pruning every refine_every steps',
          T.get('prune_outside_bounds_every') == N['strategy']['refine_every'] == T['strategy']['refine_every'],
          str(T.get('prune_outside_bounds_every')))
    check('training hot threshold v_hot', T.get('v_hot') == B['v_hot'] and abs(T.get('v_hot', -1) - N['v_hot']) <= 1e-12,
          str(T.get('v_hot')))

    ok = all(c['ok'] for c in checks)
    if a.json:
        a.json.write_text(json.dumps(dict(ok=ok, checks=checks, info=info), indent=1))
    print('%s: %d checks, %d failed' % ('ALL PASS' if ok else 'FAILED', len(checks), sum(not c['ok'] for c in checks)))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
