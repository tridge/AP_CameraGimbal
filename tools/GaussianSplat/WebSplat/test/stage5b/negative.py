#!/usr/bin/env python3
"""Negative controls: compare_dataset.py must fail (exit 1) on each perturbed copy of a passing
browser dataset (including codex's round 1 corruptions, REVIEW-stage5b.md).
  negative.py NATIVE_DIR BROWSER_DIR REJECT_NATIVE_DIR REJECT_BROWSER_DIR WORK_DIR
The REJECT pair must have views skipped for elevation and observations (the relabelling control)."""
import json
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np

native, browser, rnative, rbrowser, work = map(Path, sys.argv[1:6])
here = Path(__file__).resolve().parent


def js(fn):
    def apply(d):
        j = json.loads((d / 'dataset.json').read_text())
        fn(j)
        (d / 'dataset.json').write_text(json.dumps(j))
    apply.__name__ = fn.__name__
    return apply


def target2ulp(d):
    t = np.fromfile(d / 'targets.f16', np.uint16)
    t[123456] += 2
    t.tofile(d / 'targets.f16')


def point_value(d):
    p = np.fromfile(d / 'points.f32', np.float32)
    p[7] += 1e-5
    p.tofile(d / 'points.f32')


@js
def holdout_flip(j):
    j['images'][1]['split'] = 'test' if j['images'][1]['split'] == 'train' else 'train'


@js
def means_lr(j):
    j['training']['lr']['means'] *= 1.001
    j['lr'] = j['training']['lr']


@js
def t0(j):
    j['scale']['t0_c'] += 1e-4


@js
def wrong_frame(j):
    for im in j['images']:
        im.update(frame=999999, image_id=999999)


@js
def wrong_frame_one(j):
    j['images'][5]['frame'] += 1


@js
def wrong_init(j):
    j['training']['init'] = {'opacity': 0.99, 'knn': 1, 'min_scale': 1000, 'quats': 'zero'}


@js
def wrong_near_plane(j):
    j['training']['near_plane'] = 100000


@js
def wrong_prune_frequency(j):
    j['training']['prune_outside_bounds_every'] = 0


@js
def wrong_training_v_hot(j):
    j['training']['v_hot'] = 1


@js
def scale_kind(j):
    j['scale']['kind'] = 'linear'


@js
def no_undistortion(j):
    j['undistortion'] = None


@js
def targets_dtype_shape(j):
    j['targets'].update(dtype='u8', shape=[1, 1, 1])


@js
def points_dtype_shape(j):
    j['points'].update(dtype='i8', shape=[1, 2])


@js
def missing_options(j):
    j['options'] = {}
    j['training']['options'] = {}


@js
def missing_one_option(j):
    del j['options']['ceiling_margin']


@js
def invalid_split(j):
    for im in j['images']:
        if im['split'] != 'test':
            im['split'] = 'invalid'


@js
def hot_not_bool(j):
    j['images'][0]['hot'] = 0


@js
def all_skips_as_elevation(j):
    j['skipped'] = {'elevation': sum([v for v in j['skipped'].values() if isinstance(v, list)], []),
                    'pose_offset_limit_m': 10000000}


@js
def relabel_rejections(j):
    s = j['skipped']
    s['budget'] = s['budget'] + s['elevation'] + s['min_obs']
    s['elevation'], s['min_obs'] = [], []


@js
def pose_limit(j):
    j['skipped']['pose_offset_limit_m'] *= 2


@js
def seed_changed(j):
    j['training']['init']['seed'] = 1


@js
def seed_deleted(j):
    del j['training']['init']['seed']


@js
def sh_schedule_replaced(j):
    j['training']['sh_degree_schedule'] = 'sh_degree'


@js
def sh_schedule_deleted(j):
    del j['training']['sh_degree_schedule']


@js
def undistortion_method(j):
    j['undistortion']['method'] = 'cv2.undistort BORDER_REFLECT'


fails = 0
controls = [(native, browser, fn) for fn in (
    target2ulp, point_value, holdout_flip, means_lr, t0, wrong_frame, wrong_frame_one, wrong_init,
    wrong_near_plane, wrong_prune_frequency, wrong_training_v_hot, scale_kind, no_undistortion,
    targets_dtype_shape, points_dtype_shape, missing_options, missing_one_option, invalid_split, hot_not_bool,
    all_skips_as_elevation, pose_limit, seed_changed, seed_deleted, sh_schedule_replaced, sh_schedule_deleted,
    undistortion_method)] + [(rnative, rbrowser, relabel_rejections), (rnative, rbrowser, all_skips_as_elevation)]
for nat, bro, fn in controls:
    d = work / fn.__name__
    shutil.rmtree(d, ignore_errors=True)
    shutil.copytree(bro, d)
    fn(d)
    r = subprocess.run([sys.executable, str(here / 'compare_dataset.py'), str(nat), str(d)],
                       capture_output=True, text=True)
    ok = r.returncode == 1
    fails += not ok
    print('%s negative control %s (%s): comparator exit %d' % ('PASS' if ok else 'FAIL', fn.__name__, nat.name,
                                                              r.returncode))
    shutil.rmtree(d)
sys.exit(1 if fails else 0)
