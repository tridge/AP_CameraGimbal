#!/usr/bin/env python3
"""Compare a WebSplat stage 2 output directory with a native extract_frames.py workdir.

Checks: same frames, hot spot (origin and target height) within --hotspot-m, intrinsics and
per-frame poses within --tol, temperatures and timestamps equal, CLAHE images and raw frames
pixel-identical.
"""
import argparse
import json
import math
from pathlib import Path
import sys

import hashlib

import cv2
import numpy as np

R = 6378137.0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('native', type=Path)
    ap.add_argument('web', type=Path)
    ap.add_argument('--tol', type=float, default=1e-6)
    ap.add_argument('--hotspot-m', type=float, default=1.0)
    ap.add_argument('--no-pixels', action='store_true')
    ap.add_argument('--hashes', type=Path, help='JSON {features: [sha256 per frame], thermal: [...]} instead of the pixel files')
    args = ap.parse_args()
    n = json.loads((args.native / 'frames.json').read_text())
    w = json.loads((args.web / 'frames.json').read_text())
    errors, report = [], {}

    def check(ok, msg):
        if not ok:
            errors.append(msg)

    for k in ('width', 'height', 'rotation_deg', 'hfov_deg'):
        check(n[k] == w[k], '%s %r != %r' % (k, n[k], w[k]))
    kd = float(np.max(np.abs(np.array(n['K']) - np.array(w['K']))))
    check(kd <= args.tol, 'K differs by %g' % kd)
    o, p = n['origin'], w['origin']
    north = math.radians(p['lat'] - o['lat']) * R
    east = math.radians(p['lon'] - o['lon']) * R * math.cos(math.radians(o['lat']))
    up = w['target_enu'][2] - n['target_enu'][2]
    hot = math.sqrt(north ** 2 + east ** 2 + up ** 2)
    report['hotspot_m'] = hot
    check(hot <= args.hotspot_m, 'hot spot %.3f m apart' % hot)
    check(o['alt_amsl'] == p['alt_amsl'], 'takeoff altitude %r != %r' % (o['alt_amsl'], p['alt_amsl']))
    ni = [f['index'] for f in n['frames']]
    wi = [f['index'] for f in w['frames']]
    check(ni == wi, 'frames differ: native-only %s web-only %s' % (sorted(set(ni) - set(wi)), sorted(set(wi) - set(ni))))
    report['frames'] = len(wi)
    for k in ('p1', 'max'):
        check(n['temperature_c'][k] == w['temperature_c'][k],
              'temperature_c.%s %r != %r' % (k, n['temperature_c'][k], w['temperature_c'][k]))
    hashes = json.loads(args.hashes.read_text()) if args.hashes else None
    if hashes is not None:
        check(len(hashes['features']) == len(hashes['thermal']) == len(w['frames']),
              'pixel files hold %d/%d frames' % (len(hashes['features']), len(hashes['thermal'])))
    sel = w.get('selection')
    check(w.get('format') == 'websplat.frames' and w.get('version') == 1 and sel is not None, 'not websplat.frames v1')
    if sel:
        report['selection'] = {k: sel[k] for k in ('considered', 'selected', 'blank', 'drifted', 'typical_median_c')}
    pos = rot = 0.0
    wf = {f['index']: f for f in w['frames']}
    mismatched_pixels = []
    hw = (w['height'], w['width'])
    if not args.no_pixels and hashes is None:
        feats = np.memmap(args.web / 'features.u8', np.uint8, 'r').reshape(-1, *hw)
        therm = np.memmap(args.web / 'thermal.u16', '<u2', 'r').reshape(-1, *hw)
        check(len(feats) == len(w['frames']) == len(therm), 'pixel files hold %d/%d frames' % (len(feats), len(therm)))
    slot = {f['index']: k for k, f in enumerate(w['frames'])}
    for f in n['frames']:
        g = wf.get(f['index'])
        if g is None:
            continue
        pos = max(pos, float(np.max(np.abs(np.array(f['position']) - np.array(g['position'])))))
        rot = max(rot, float(np.max(np.abs(np.array(f['rotation']) - np.array(g['rotation'])))))
        for k in ('name', 'utc_us', 'minimum_c', 'maximum_c', 'temperature_scale_k', 'temperature_offset_k'):
            check(f[k] == g[k], 'frame %d %s %r != %r' % (f['index'], k, f[k], g[k]))
        if args.no_pixels:
            continue
        img = cv2.imread(str(args.native / 'images' / f['name']), cv2.IMREAD_UNCHANGED)
        th = cv2.imread(str(args.native / 'thermal' / f['name']), cv2.IMREAD_UNCHANGED)
        k = slot[f['index']]
        if hashes is not None:
            if hashlib.sha256(img.tobytes()).hexdigest() != hashes['features'][k]:
                mismatched_pixels.append((f['index'], None, None))
            check(hashlib.sha256(th.astype('<u2').tobytes()).hexdigest() == hashes['thermal'][k],
                  'frame %d raw thermal differs' % f['index'])
            continue
        if not np.array_equal(img, feats[k]):
            d = np.abs(img.astype(int) - feats[k])
            mismatched_pixels.append((f['index'], int((d > 0).sum()), int(d.max())))
        check(np.array_equal(th, therm[k]), 'frame %d raw thermal differs' % f['index'])
    report['position_max_diff'] = pos
    report['rotation_max_diff'] = rot
    check(pos <= args.tol, 'positions differ by up to %g' % pos)
    check(rot <= args.tol, 'rotations differ by up to %g' % rot)
    if mismatched_pixels:
        errors.append('CLAHE images differ (frame, pixels, max): %s' % mismatched_pixels[:10])
    report['clahe_images_identical'] = not mismatched_pixels and not args.no_pixels
    for e in errors[:40]:
        print(e)
    print(json.dumps(report))
    print('stage 2 vs native: %s' % ('FAIL, %d differences' % len(errors) if errors else 'pass'))
    sys.exit(1 if errors else 0)


if __name__ == '__main__':
    main()
