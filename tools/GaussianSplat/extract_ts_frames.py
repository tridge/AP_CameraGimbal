#!/usr/bin/env python3
"""Extract frames with pose priors from an MPEG-TS carrying MISB ST 0601 KLV.

For recordings made by the camera's own recorder (or tools/video_to_ts.py): each frame is
paired with the KLV cell of the same PTS, giving sensor position, platform attitude, sensor
relative angles and field of view. Frames are written in a local ENU frame about the mean
position with the ground at takeoff height, ready for sfm.py and train_rgb.py:

  extract_ts_frames.py RECORDING.ts WORKDIR [--fps 3]
"""
import argparse
import json
import math
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from splat_common import R_BODY_CAM, R_ENU_NED, LocalFrame, fatal, intrinsics, rot_x, rot_y, rot_z  # noqa: E402

UAS_KEY = bytes.fromhex('060e2b34020b01010e01030101000000')


def s(b, span):
    return int.from_bytes(b, 'big', signed=True) * span / (2 ** (8 * len(b) - 1) - 1)


def u(b, span):
    return int.from_bytes(b, 'big') * span / (2 ** (8 * len(b)) - 1)


def parse_klv(data):
    i = data.find(UAS_KEY[:4])
    if i < 0:
        return None
    k = data[i:]
    if len(k) < 17:
        return None
    at = 17 if k[16] < 128 else 17 + (k[16] & 127)
    items = {}
    while at < len(k) - 1:
        tag, n = k[at], k[at + 1]
        items[tag] = k[at + 2:at + 2 + n]
        at += 2 + n
    if not all(t in items for t in (13, 14, 15)):
        return None
    rec = dict(utc_us=int.from_bytes(items[2], 'big') if 2 in items else None,
               lat=s(items[13], 90), lon=s(items[14], 180), alt=u(items[15], 19900) - 900)
    if 5 in items:
        rec['yaw'] = u(items[5], 360)
        rec['pitch'] = s(items[90], 90) if 90 in items else (s(items[6], 20) if 6 in items else 0.0)
        rec['roll'] = s(items[91], 90) if 91 in items else (s(items[7], 50) if 7 in items else 0.0)
    if 18 in items and 19 in items:
        rec['saz'], rec['sel'] = u(items[18], 360), s(items[19], 180)
        rec['sroll'] = u(items[20], 360) if 20 in items else 0.0
    if 16 in items:
        rec['hfov'] = u(items[16], 180)
    return rec


def camera_rotation(k):
    """World (ENU) from camera: platform yaw/pitch/roll then sensor relative azimuth/elevation/roll."""
    if 'yaw' not in k or 'saz' not in k:
        return None
    body = rot_z(math.radians(k['yaw'])) @ rot_y(math.radians(k['pitch'])) @ rot_x(math.radians(k['roll']))
    sensor = rot_z(math.radians(k['saz'])) @ rot_y(math.radians(k['sel'])) @ rot_x(math.radians(k['sroll']))
    return R_ENU_NED @ body @ sensor @ R_BODY_CAM


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ts', type=Path)
    ap.add_argument('workdir', type=Path)
    ap.add_argument('--fps', type=float, default=3.0, help='frames kept per second')
    ap.add_argument('--hfov', type=float, help='override the KLV horizontal FOV (deg)')
    ap.add_argument('--skip-start', type=float, default=0.0, help='seconds dropped at the start (e.g. on the ground)')
    ap.add_argument('--skip-end', type=float, default=0.0, help='seconds dropped at the end')
    ap.add_argument('--min-height', type=float, default=5.0, help='drop frames lower than this above the start (m)')
    args = ap.parse_args()

    import av
    import cv2
    c = av.open(str(args.ts))
    vs, ds = c.streams.video[0], c.streams.data[0]
    klv = {}
    for p in c.demux(ds):
        if p.pts is None:
            continue
        rec = parse_klv(bytes(p))
        if rec:
            klv[int(p.pts)] = rec
    if not klv:
        fatal('no ST 0601 KLV in %s' % args.ts)
    klv_tb = ds.time_base
    c.close()
    keys = np.array(sorted(klv))
    first = klv[keys[0]]
    ground = first['alt']  # takeoff height
    c = av.open(str(args.ts))
    vs = c.streams.video[0]
    vs.thread_type = 'AUTO'
    t0 = vs.start_time or 0
    duration = float(c.duration / 1e6) if c.duration else None
    images = args.workdir / 'images'
    images.mkdir(parents=True, exist_ok=True)
    raw, next_t, K = [], 0.0, None
    for frame in c.decode(vs):
        if frame.pts is None:
            continue
        ts = float((frame.pts - t0) * vs.time_base)
        if ts < args.skip_start or (duration and args.skip_end and ts > duration - args.skip_end) or ts < next_t:
            continue
        j = int(np.searchsorted(keys, frame.pts))
        j = min(max(j, 0), len(keys) - 1)
        if j > 0 and abs(keys[j - 1] - frame.pts) < abs(keys[j] - frame.pts):
            j -= 1
        k = klv[keys[j]]
        if abs(float((keys[j] - frame.pts) * klv_tb)) > 0.2 or k['alt'] - ground < args.min_height:
            continue
        next_t = ts + 1.0 / args.fps
        img = frame.to_ndarray(format='bgr24')
        if K is None:
            K = intrinsics(img.shape[1], img.shape[0], args.hfov or k.get('hfov') or 70.0)
        name = '%06d.jpg' % round(ts * 1000)
        cv2.imwrite(str(images / name), img, [cv2.IMWRITE_JPEG_QUALITY, 95])
        raw.append((name, ts, k))
    if not raw:
        fatal('no frames kept')
    lat0 = float(np.mean([k['lat'] for _, _, k in raw]))
    lon0 = float(np.mean([k['lon'] for _, _, k in raw]))
    local = LocalFrame(lat0, lon0, ground)
    frames = []
    for name, ts, k in raw:
        rot = camera_rotation(k)
        frames.append(dict(index=len(frames), name=name, utc_us=k['utc_us'], media_s=ts,
                           position=local.to_enu(k['lat'], k['lon'], k['alt']).tolist(),
                           rotation=rot.tolist() if rot is not None else None))
    # target: where the view axes meet the ground, robustly
    hits = []
    for f in frames:
        if f['rotation'] is None:
            continue
        c_, a = np.array(f['position']), np.array(f['rotation'])[:, 2]
        if a[2] < -0.2:
            hits.append(c_ + a * (-c_[2] / a[2]))
    target = np.median(hits, axis=0) if hits else np.array([0.0, 0.0, 0.0])
    info = dict(source=str(args.ts.resolve()), kind='rgb', width=int(round(2 * K[0, 2])), height=int(round(2 * K[1, 2])),
                hfov_deg=float(2 * math.degrees(math.atan(K[0, 2] / K[0, 0]))), K=K.tolist(),
                origin=dict(lat=lat0, lon=lon0, alt_amsl=ground, frame='ENU, up from takeoff altitude'),
                target_enu=target.tolist(), frames=frames)
    (args.workdir / 'frames.json').write_text(json.dumps(info, indent=1))
    heights = [f['position'][2] for f in frames]
    print('wrote %d frames to %s; heights %.0f..%.0f m above takeoff; target ENU %s' % (
        len(frames), args.workdir, min(heights), max(heights), np.round(target, 1)))


if __name__ == '__main__':
    main()
