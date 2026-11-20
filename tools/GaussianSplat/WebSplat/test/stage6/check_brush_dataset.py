#!/usr/bin/env python3
"""Check the Brush input directory written by stage 6 (brush-dataset.js) against its stage 5b
dataset and train.py's initialisation rules. Exits nonzero on any failure.

  check_brush_dataset.py PREPARE_DIR BRUSH_DIR

- images/NAME.png: 16-bit gray, decode / 65535 within half a 16-bit step (7.63e-6) of the float16
  target train.py trains on, for every pixel of every view (no gamma, no 8-bit);
- sparse/0: PINHOLE camera = dataset K; each view's cam_from_world (pycolmap) = its viewmat
  (1e-6), in dataset order, names equal;
- eval_views.txt = the 'test' split;
- init.ply: the dataset's points (same order), SH DC = (v - 0.5) / SH_C0 on all channels, f_rest 0,
  opacity logit(0.1), log scales = log(max(mean distance to the 3 nearest other points, 1e-3))
  by scipy cKDTree (k=4, train.py knn_scale) within 1e-5 relative, quaternions in [0, 1);
- bounds.json = dataset bounds, ceiling.f32 and gsplat's too-big rule from the training block.
"""
import json
from pathlib import Path
import sys

import cv2
import numpy as np


def main():
    prep, brush = Path(sys.argv[1]), Path(sys.argv[2])
    d = json.loads((prep / 'dataset.json').read_text())
    H, W, n = d['height'], d['width'], len(d['images'])
    fails = []

    def check(name, ok, detail=''):
        print('%s %s%s' % ('ok  ' if ok else 'FAIL', name, '' if ok else ': ' + detail))
        if not ok:
            fails.append(name)

    t = np.memmap(prep / d['targets']['file'], dtype='<f2', mode='r', shape=(n, H, W))
    worst = 0.0
    for i, im in enumerate(d['images']):
        png = cv2.imread(str(brush / 'images' / im['name']), cv2.IMREAD_UNCHANGED)
        if png is None or png.dtype != np.uint16 or png.shape != (H, W):
            check('target PNG %s' % im['name'], False, 'not a %dx%d 16-bit gray PNG' % (W, H))
            continue
        worst = max(worst, float(np.abs(png.astype(np.float64) / 65535 - t[i].astype(np.float64)).max()))
    check('PNG16 transport vs float16 targets (%d views, max %.3g)' % (n, worst), worst <= 0.5 / 65535 + 1e-12,
          '%.3g' % worst)

    import pycolmap
    rec = pycolmap.Reconstruction()
    rec.read_text(str(brush / 'sparse' / '0'))
    cam = list(rec.cameras.values())
    K = np.array(d['K'])
    check('PINHOLE camera = dataset K', len(cam) == 1 and cam[0].model.name == 'PINHOLE' and np.allclose(
        cam[0].params, [K[0, 0], K[1, 1], K[0, 2], K[1, 2]], rtol=0, atol=1e-9) and (cam[0].width, cam[0].height) == (W, H))
    ims = sorted(rec.images.values(), key=lambda im: im.image_id)
    names_ok = [im.name for im in ims] == [im['name'] for im in d['images']]
    err = max(float(np.abs(im.cam_from_world().matrix() - np.array(v['viewmat']).reshape(4, 4)[:3]).max())
              for im, v in zip(ims, d['images'])) if names_ok else float('inf')
    check('views in dataset order with cam_from_world = viewmat (max %.2g)' % err, names_ok and err <= 1e-6)
    ev = [ln for ln in (brush / 'eval_views.txt').read_text().split('\n') if ln]
    check('eval_views.txt = test split', ev == [im['name'] for im in d['images'] if im['split'] == 'test'])

    from plyfile import PlyData
    from scipy.spatial import cKDTree
    v = PlyData.read(str(brush / 'init.ply'))['vertex']
    pts = np.fromfile(prep / d['points']['file'], '<f4').reshape(-1, 4)
    xyz = np.stack([v['x'], v['y'], v['z']], 1)
    check('init points = dataset points, same order', len(v.data) == len(pts) and np.array_equal(xyz, pts[:, :3]))
    sh0 = (pts[:, 3].astype(np.float64) - 0.5) / 0.28209479177387814
    check('SH DC = (v - 0.5) / SH_C0 on all channels', all(np.allclose(v['f_dc_%d' % c], sh0, rtol=1e-6, atol=1e-6) for c in range(3)))
    rest = [p for p in v.data.dtype.names if p.startswith('f_rest_')]
    check('f_rest zero (%d coefficients)' % len(rest), all(not np.any(v[p]) for p in rest)
          and len(rest) == 3 * ((d['training']['options']['sh_degree'] + 1) ** 2 - 1))
    check('opacity logit(0.1)', np.allclose(v['opacity'], np.log(0.1 / 0.9), atol=1e-6))
    dist, _ = cKDTree(pts[:, :3].astype(np.float64)).query(pts[:, :3].astype(np.float64), k=4)
    ref = np.log(np.maximum(dist[:, 1:].mean(1), 1e-3))
    rel = max(float(np.abs(np.exp(v['scale_%d' % c]) / np.exp(ref) - 1).max()) for c in range(3))
    check('kNN log scales vs cKDTree (max rel %.2g)' % rel, rel <= 1e-5)
    q = np.stack([v['rot_%d' % c] for c in range(4)], 1)
    check('quaternions uniform [0, 1)', bool(((q >= 0) & (q < 1)).all()) and 0.45 < q.mean() < 0.55)

    b = json.loads((brush / 'bounds.json').read_text())
    bd = d['bounds']
    ceiling = np.fromfile(prep / bd['ceiling']['file'], '<f4')
    s = d['training']['strategy']
    check('bounds.json = dataset bounds and ceiling', b['centre'] == bd['centre'] and b['radius'] == bd['radius']
          and b['z_min'] == bd['z_min'] and b['ceiling_origin'] == bd['ceiling']['origin']
          and b['ceiling_cell'] == bd['ceiling']['cell'] and b['ceiling_n'] == bd['ceiling']['n']
          and np.array_equal(np.array(b['ceiling'], np.float32), ceiling))
    check('too-big rule = prune_scale3d * scene_scale for reset_every < step < refine_stop_iter',
          b['max_scale'] == s['prune_scale3d'] * d['training']['scene_scale']
          and b['scale_prune_from'] == s['reset_every'] + 1 and b['scale_prune_until'] == s['refine_stop_iter'])
    print('%d check(s) failed' % len(fails) if fails else 'brush dataset: PASS')
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
