#!/usr/bin/env python3
"""G2 / stage 5 acceptance: compare a sparse model with the native reference model.

Criteria (PLAN.md G2): camera position difference median <= 0.3 m and 95th percentile <= 1 m;
rotation difference median <= 0.1 deg and 95th percentile <= 0.3 deg; mean reprojection error
within 10%; observations per image, track lengths and triangulation angles comparable (here:
medians within 10% and mean observations per image within 10%). Images are matched by name.
"""
import argparse
import json
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from splat_common import import_pycolmap  # noqa: E402


def stats(rec):
    pycolmap = import_pycolmap()
    imgs = {im.name: im for im in rec.images.values()}
    centres = {n: im.projection_center() for n, im in imgs.items()}
    obs = {n: im.num_points3D for n, im in imgs.items()}
    tl, ang = [], []
    for p in rec.points3D.values():
        ids = [el.image_id for el in p.track.elements]
        tl.append(len(ids))
        cs = np.array([rec.images[i].projection_center() for i in sorted(set(ids))[:12]])
        rays = cs - p.xyz
        rays /= np.linalg.norm(rays, axis=1, keepdims=True)
        cosm = np.clip(rays @ rays.T, -1, 1).min()
        ang.append(np.degrees(np.arccos(cosm)))
    cam = next(iter(rec.cameras.values()))
    return {'imgs': imgs, 'centres': centres, 'obs': obs, 'track': np.array(tl), 'angle': np.array(ang),
            'reproj': rec.compute_mean_reprojection_error(), 'points': rec.num_points3D(),
            'observations': int(sum(tl)), 'camera': cam.params.tolist(), 'model': cam.model.name}


def rot_angle(Ra, Rb):
    c = (np.trace(Ra @ Rb.T) - 1) / 2
    return np.degrees(np.arccos(np.clip(c, -1, 1)))


def summary(x):
    x = np.asarray(x, float)
    return {'mean': float(x.mean()), 'median': float(np.median(x)), 'p5': float(np.percentile(x, 5)),
            'p95': float(np.percentile(x, 95)), 'max': float(x.max())}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('reference')
    ap.add_argument('model')
    ap.add_argument('--json', default=None)
    args = ap.parse_args()
    pycolmap = import_pycolmap()
    A, B = stats(pycolmap.Reconstruction(args.reference)), stats(pycolmap.Reconstruction(args.model))
    names = sorted(set(A['imgs']) & set(B['imgs']))
    dpos = [np.linalg.norm(A['centres'][n] - B['centres'][n]) for n in names]
    drot = [rot_angle(A['imgs'][n].cam_from_world().rotation.matrix(), B['imgs'][n].cam_from_world().rotation.matrix())
            for n in names]
    oa = np.array([A['obs'][n] for n in names], float)
    ob = np.array([B['obs'][n] for n in names], float)
    res = {
        'images': {'reference': len(A['imgs']), 'model': len(B['imgs']), 'common': len(names)},
        'position_m': summary(dpos), 'rotation_deg': summary(drot),
        'points': {'reference': A['points'], 'model': B['points']},
        'observations': {'reference': A['observations'], 'model': B['observations']},
        'obs_per_image': {'reference': summary(oa), 'model': summary(ob),
                          'ratio_median': float(np.median(ob / np.maximum(oa, 1))),
                          'ratio_p5': float(np.percentile(ob / np.maximum(oa, 1), 5)),
                          'ratio_p95': float(np.percentile(ob / np.maximum(oa, 1), 95))},
        'track_length': {'reference': summary(A['track']), 'model': summary(B['track'])},
        'tri_angle_deg': {'reference': summary(A['angle']), 'model': summary(B['angle'])},
        'reproj_px': {'reference': A['reproj'], 'model': B['reproj']},
        'camera': {'reference': A['camera'], 'model': B['camera'], 'model_name': [A['model'], B['model']]},
    }
    rel = lambda a, b: abs(b - a) / abs(a)
    checks = {
        'all images': len(names) == len(A['imgs']) == len(B['imgs']),
        'position median <= 0.3 m': res['position_m']['median'] <= 0.3,
        'position p95 <= 1 m': res['position_m']['p95'] <= 1.0,
        'rotation median <= 0.1 deg': res['rotation_deg']['median'] <= 0.1,
        'rotation p95 <= 0.3 deg': res['rotation_deg']['p95'] <= 0.3,
        'reprojection within 10%': rel(A['reproj'], B['reproj']) <= 0.10,
        'obs/image mean within 10%': rel(oa.mean(), ob.mean()) <= 0.10,
        'track length median within 10%': rel(np.median(A['track']), np.median(B['track'])) <= 0.10,
        'track length mean within 10%': rel(A['track'].mean(), B['track'].mean()) <= 0.10,
        'tri angle median within 10%': rel(np.median(A['angle']), np.median(B['angle'])) <= 0.10,
    }
    checks = {k: bool(v) for k, v in checks.items()}
    res['checks'] = checks
    res['pass'] = all(checks.values())
    print(json.dumps(res, indent=1))
    if args.json:
        Path(args.json).write_text(json.dumps(res, indent=1))
    print('PASS' if res['pass'] else 'FAIL: ' + ', '.join(k for k, v in checks.items() if not v))
    sys.exit(0 if res['pass'] else 1)


if __name__ == '__main__':
    main()
