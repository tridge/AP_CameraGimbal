#!/usr/bin/env python3
"""Extract the frames of an APCG thermal mkv that see a target, with telemetry pose priors.

Writes WORKDIR/images (8-bit contrast-enhanced, for feature matching),
WORKDIR/thermal (16-bit raw radiometric PNG, for training) and WORKDIR/frames.json
(local ENU frame about the target, per-frame camera position/rotation priors).
"""
import argparse
import json
import math
from pathlib import Path
import sys

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from splat_common import (LocalFrame, camera_rotation, decode_frames, fatal, intrinsics,
                          raw_to_celsius, read_mkv_metadata, triangulate_rays)


def rotate(image, degrees):
    k = (degrees // 90) % 4
    return np.ascontiguousarray(np.rot90(image, k)) if k else image


def feature_image(celsius, clahe):
    lo, hi = np.percentile(celsius, (1, 99))
    # clip hot features so they don't flatten the background texture
    v = np.clip((celsius - lo) / max(hi - lo, 0.5), 0, 1)
    return clahe.apply((v * 255).astype(np.uint8))


def is_blank(raw, meta):
    """Flat frames some cameras emit (e.g. during shutter calibration), sometimes with one hot pixel."""
    p1, p99 = np.percentile(raw, [1, 99])
    return (p99 - p1) * meta.get('temperature_scale_k', 1 / 64) < 0.5


def frame_pose(meta, local):
    t = meta.get('telemetry')
    if not t or not t.get('position') or not t.get('vehicle_attitude') or not t.get('gimbal_attitude'):
        return None
    p = t['position']
    if p.get('alt_amsl_m') is None:
        return None
    centre = local.to_enu(p['lat_e7'] / 1e7, p['lon_e7'] / 1e7, p['alt_amsl_m'])
    return centre, camera_rotation(t)


def project(K, centre, R, point):
    x = R.T @ (point - centre)
    if x[2] <= 0:
        return None
    uv = K @ x
    return uv[:2] / uv[2], x[2]


def locate_hotspot(path, metas, local, K, rotation, min_c, start, end):
    """Triangulate the hottest pixel's ray over the frames whose maximum exceeds min_c."""
    hot = {i for i, m in enumerate(metas) if start <= i <= end and m.get('maximum_c', -300) >= min_c}
    centres, dirs = [], []
    Ki = np.linalg.inv(K)
    for i, meta, raw in decode_frames(path, hot):
        pose = frame_pose(meta, local)
        if pose is None or is_blank(raw, meta):
            continue
        img = rotate(raw, rotation)
        y, x = np.unravel_index(np.argmax(img), img.shape)
        d = pose[1] @ Ki @ np.array([x, y, 1.0])
        centres.append(pose[0])
        dirs.append(d / np.linalg.norm(d))
    if len(centres) < 5:
        return None, len(centres), None
    X, err = triangulate_rays(centres, dirs, K[0, 0])
    return X, len(centres), err


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mkv', type=Path)
    ap.add_argument('workdir', type=Path)
    ap.add_argument('--lat', type=float, required=True, help='target latitude')
    ap.add_argument('--lon', type=float, required=True, help='target longitude')
    ap.add_argument('--target-alt', type=float, default=None,
                    help='target height above takeoff (m); default: locate hotspot, else 0')
    ap.add_argument('--no-locate-hotspot', action='store_true',
                    help='do not move the target to the triangulated hottest point')
    ap.add_argument('--hotspot-min-c', type=float, default=60.0,
                    help='frames with a maximum above this are used to locate the hotspot')
    ap.add_argument('--hotspot-max-error', type=float, default=60.0,
                    help='reject the hotspot if its median ray error exceeds this (px)')
    ap.add_argument('--rotation', type=int, default=None,
                    help='rotate raw frames by this many degrees (default: mkv rotation_deg; '
                         'use 0 for ZT30 captures encoded with the MT11 default of 180)')
    ap.add_argument('--hfov', type=float, default=None, help='horizontal FOV (default: from metadata)')
    ap.add_argument('--margin', type=float, default=0.1,
                    help='target must project inside the image shrunk by this fraction per side')
    ap.add_argument('--radius', type=float, default=None,
                    help='keep frames whose view axis meets the ground within this of the target (m), '
                         'instead of frames that see the target (for maps)')
    ap.add_argument('--start', type=int, default=0, help='first frame index considered')
    ap.add_argument('--end', type=int, default=None, help='last frame index considered')
    ap.add_argument('--step', type=int, default=1, help='keep every Nth selected frame')
    ap.add_argument('--max-range', type=float, default=2000, help='maximum camera to target distance (m)')
    ap.add_argument('--max-median-dev', type=float, default=15.0,
                    help='skip frames whose median temperature differs from the typical frame by more (C)')
    args = ap.parse_args()

    if not args.mkv.exists():
        fatal('no such file: %s' % args.mkv)
    metas = [m for _, _, m in read_mkv_metadata(args.mkv)]
    if not metas:
        fatal('no frames in %s' % args.mkv)
    first = metas[0]
    width, height = first['width'], first['height']
    rotation = first.get('rotation_deg', 0) if args.rotation is None else args.rotation
    if rotation % 180:
        width, height = height, width
    hfov = args.hfov or first.get('hfov_deg') or 24.2
    K = intrinsics(width, height, hfov)

    # takeoff altitude: median of AMSL minus relative altitude
    homes = [m['telemetry']['position']['alt_amsl_m'] - m['telemetry']['position']['alt_relative_m']
             for m in metas if m.get('telemetry') and m['telemetry'].get('position')
             and m['telemetry']['position'].get('alt_relative_m') is not None]
    if not homes:
        fatal('no frames carry position telemetry')
    home_alt = float(np.median(homes))
    local = LocalFrame(args.lat, args.lon, home_alt)
    target = np.array([0.0, 0.0, args.target_alt or 0.0])
    end = len(metas) - 1 if args.end is None else min(args.end, len(metas) - 1)
    if not args.no_locate_hotspot:
        X, n, err = locate_hotspot(args.mkv, metas, local, K, rotation, args.hotspot_min_c, args.start, end)
        if X is not None and err > args.hotspot_max_error:
            print('hotspot rays disagree (median %.0f px from %d frames): check --rotation and the gimbal '
                  'yaw convention; keeping the given position' % (err, n))
        elif X is not None:
            print('hotspot from %d frames at ENU %s m (median ray error %.0f px)' % (n, X.round(1), err))
            if args.target_alt is not None:
                X[2] = args.target_alt
            if np.linalg.norm(X[:2]) < 300:
                target = X
            else:
                print('hotspot is far from the given position, ignoring it')
    # re-centre the local frame on the target, keeping takeoff altitude as the vertical datum
    t_lat, t_lon, _ = local.to_geodetic(target)
    local = LocalFrame(t_lat, t_lon, home_alt)
    target_up = float(target[2])
    print('target %.7f %.7f, %.1f m above takeoff (%.1f m AMSL)' % (t_lat, t_lon, target_up, home_alt + target_up))

    selected = []
    mx, my = args.margin * width, args.margin * height
    point = np.array([0.0, 0.0, target_up])
    for i in range(args.start, end + 1):
        pose = frame_pose(metas[i], local)
        if pose is None:
            continue
        if args.radius is not None:
            c, axis = pose[0], pose[1][:, 2]
            if axis[2] < -0.1 and np.linalg.norm((c + axis * (target_up - c[2]) / axis[2] - point)[:2]) < args.radius:
                selected.append(i)
            continue
        p = project(K, pose[0], pose[1], point)
        if p is None:
            continue
        (u, v), depth = p
        if mx <= u < width - mx and my <= v < height - my and depth < args.max_range:
            selected.append(i)
    selected = selected[::max(1, args.step)]
    if len(selected) < 3:
        fatal('only %d frames see the target' % len(selected))
    print('%d of %d frames %s (frames %d..%d)' % (len(selected), len(metas),
        'look within %.0f m of the target' % args.radius if args.radius is not None else 'see the target',
        selected[0], selected[-1]))

    images = args.workdir / 'images'
    thermal = args.workdir / 'thermal'
    images.mkdir(parents=True, exist_ok=True)
    thermal.mkdir(parents=True, exist_ok=True)
    clahe = cv2.createCLAHE(clipLimit=3.0, tileGridSize=(8, 8))
    frames = []
    tmin, tmax = [], []
    # drop blank frames and frames whose radiometry has drifted (the whole scene offset by tens of
    # degrees, e.g. before a camera recalibrates), judged by the median scene temperature
    blank, medians = [], {}
    for i, meta, raw in decode_frames(args.mkv, set(selected)):
        if is_blank(raw, meta):
            blank.append(i)
        else:
            medians[i] = float(np.median(raw_to_celsius(raw, meta)))
    typical = float(np.median(list(medians.values())))
    drifted = sorted(i for i, m in medians.items() if abs(m - typical) > args.max_median_dev)
    keep = set(medians) - set(drifted)
    for i, meta, raw in decode_frames(args.mkv, keep):
        raw = rotate(raw, rotation)
        celsius = raw_to_celsius(raw, meta)
        name = '%05d.png' % i
        cv2.imwrite(str(images / name), feature_image(celsius, clahe))
        cv2.imwrite(str(thermal / name), raw)
        centre, R = frame_pose(meta, local)
        tmin.append(float(np.percentile(celsius, 1)))
        tmax.append(float(celsius.max()))
        frames.append(dict(index=i, name=name, utc_us=meta['telemetry'].get('utc_us'),
                           position=centre.tolist(), rotation=R.tolist(),
                           minimum_c=float(celsius.min()), maximum_c=float(celsius.max()),
                           temperature_scale_k=meta.get('temperature_scale_k', 1 / 64),
                           temperature_offset_k=meta.get('temperature_offset_k', 0)))
    info = dict(source=str(args.mkv.resolve()), width=width, height=height, hfov_deg=hfov,
                K=K.tolist(), rotation_deg=rotation,
                origin=dict(lat=t_lat, lon=t_lon, alt_amsl=home_alt, frame='ENU, up from takeoff altitude'),
                target_enu=[0.0, 0.0, target_up],
                temperature_c=dict(p1=float(np.median(tmin)), max=float(np.max(tmax))),
                frames=frames)
    (args.workdir / 'frames.json').write_text(json.dumps(info, indent=1))
    if blank:
        print('skipped %d blank frames: %s' % (len(blank), ', '.join(map(str, blank))))
    if drifted:
        print('skipped %d frames with median temperature more than %.0f C from the typical %.1f C: %s' % (
            len(drifted), args.max_median_dev, typical, ', '.join(map(str, drifted))))
    print('wrote %d frames to %s' % (len(frames), args.workdir))


if __name__ == '__main__':
    main()
