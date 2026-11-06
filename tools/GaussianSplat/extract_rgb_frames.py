#!/usr/bin/env python3
"""Extract RGB video frames with telemetry priors for structure from motion.

For day flights the camera's RGB lens shares the gimbal with the thermal camera and has far
more image texture, so SfM on RGB frames gives better poses, which transfer_poses.py then maps
onto the thermal frames. Frames are written in the local ENU frame of an existing thermal
workdir (from extract_frames.py) and restricted to the time span of its frames:

  extract_rgb_frames.py VIDEO.mp4 RGBWORK --reference THERMALWORK --bin FLIGHT.BIN

Per-frame position/attitude come from the dataflash log via tools/thermal_to_video.py. The
video's first frame time defaults to its modification time minus its duration.
"""
import argparse
import datetime
import json
from pathlib import Path
import sys

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))  # tools/, for thermal_to_video
from splat_common import LocalFrame, camera_rotation, fatal, intrinsics  # noqa: E402


def parse_utc(text):
    t = datetime.datetime.fromisoformat(text.replace('Z', '+00:00'))
    if t.tzinfo is None:
        t = t.replace(tzinfo=datetime.timezone.utc)
    return t.timestamp()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('video', type=Path)
    ap.add_argument('workdir', type=Path)
    ap.add_argument('--reference', type=Path, required=True,
                    help='thermal workdir whose frames.json gives the local frame, target and time span')
    ap.add_argument('--bin', dest='bin_log', type=Path, required=True, help='ArduPilot dataflash log')
    ap.add_argument('--start-utc', help='UTC of the first video frame, ISO 8601 (default: mtime minus duration)')
    ap.add_argument('--time-offset', type=float, default=0.0, help='seconds added to the video start time')
    ap.add_argument('--fps', type=float, default=3.0, help='frames kept per second of video')
    ap.add_argument('--margin', type=float, default=5.0, help='seconds kept beyond the thermal frames\' span')
    ap.add_argument('--hfov', type=float, default=84.5, help='horizontal FOV (default: ZT30 wide lens)')
    ap.add_argument('--width', type=int, default=0, help='resize to this width (default: native)')
    ap.add_argument('--gimbal-yaw-offset', type=float, default=0.0, help='subtracted from SIAT yaw')
    ap.add_argument('--min-depression', type=float, default=0.0,
                    help='drop frames whose view axis is less than this far below the horizon (deg), '
                         'e.g. 10 to skip far hills and sky on a transit; frames without attitude are kept')
    args = ap.parse_args()

    import av
    import cv2
    import thermal_to_video as thermal

    ref = json.loads((args.reference / 'frames.json').read_text())
    origin = ref['origin']
    local = LocalFrame(origin['lat'], origin['lon'], origin['alt_amsl'])
    utcs = [f['utc_us'] / 1e6 for f in ref['frames'] if f.get('utc_us')]
    if not utcs:
        fatal('reference frames carry no UTC')
    t0, t1 = min(utcs) - args.margin, max(utcs) + args.margin

    container = av.open(str(args.video))
    stream = container.streams.video[0]
    stream.thread_type = 'AUTO'
    duration = float(stream.duration * stream.time_base) if stream.duration else container.duration / 1e6
    if args.start_utc:
        start = parse_utc(args.start_utc)
    else:
        start = args.video.stat().st_mtime - duration
    start += args.time_offset
    origin_pts = stream.start_time or 0
    print('video start %s UTC, keeping %.0f..%.0f s of %.0f s at %g fps' % (
        datetime.datetime.fromtimestamp(start, datetime.timezone.utc).isoformat(timespec='milliseconds'),
        t0 - start, t1 - start, duration, args.fps), flush=True)

    telemetry = thermal.load_bin_telemetry(args.bin_log, 2000, {'source': 'siat', 'yaw_offset_deg': args.gimbal_yaw_offset})
    images = args.workdir / 'images'
    images.mkdir(parents=True, exist_ok=True)
    frames, K, next_t, skipped, level = [], None, t0, 0, 0
    for frame in container.decode(stream):
        if frame.pts is None:
            continue
        media_s = float((frame.pts - origin_pts) * stream.time_base)
        utc = start + media_s
        if utc > t1:
            break
        if utc < next_t:
            continue
        next_t = max(next_t + 1.0 / args.fps, utc)
        snap = telemetry.sample(utc * 1000.0, media_s * 1000.0)
        pos = snap.get('position')
        if not pos or not snap.get('vehicle_attitude'):
            skipped += 1
            continue
        rotation = camera_rotation(snap) if snap.get('gimbal_attitude') else None
        if rotation is not None and np.degrees(np.arcsin(np.clip(-rotation[2, 2], -1, 1))) < args.min_depression:
            level += 1
            continue
        img = frame.to_ndarray(format='bgr24')
        if args.width and args.width != img.shape[1]:
            img = cv2.resize(img, (args.width, round(img.shape[0] * args.width / img.shape[1])), interpolation=cv2.INTER_AREA)
        if K is None:
            K = intrinsics(img.shape[1], img.shape[0], args.hfov)
        name = '%06d.jpg' % round(media_s * 1000)  # video milliseconds
        cv2.imwrite(str(images / name), img, [cv2.IMWRITE_JPEG_QUALITY, 95])
        centre = local.to_enu(pos['lat_e7'] / 1e7, pos['lon_e7'] / 1e7, pos['alt_amsl_m'])
        frames.append(dict(index=len(frames), name=name, utc_us=round(utc * 1e6), media_s=media_s,
                           position=centre.tolist(), rotation=rotation.tolist() if rotation is not None else None))
        if len(frames) % 200 == 0:
            print('  %d frames, video %.0f s' % (len(frames), media_s), flush=True)
    if not frames:
        fatal('no frames in the reference time span')
    info = dict(source=str(args.video.resolve()), kind='rgb', width=int(round(2 * K[0, 2])),
                height=int(round(2 * K[1, 2])), hfov_deg=args.hfov, K=K.tolist(),
                video_start_utc=start, origin=origin, target_enu=ref['target_enu'], frames=frames)
    (args.workdir / 'frames.json').write_text(json.dumps(info, indent=1))
    no_rot = sum(f['rotation'] is None for f in frames)
    print('wrote %d frames to %s (%d without gimbal attitude, %d skipped without position, %d near the horizon)' % (
        len(frames), args.workdir, no_rot, skipped, level))


if __name__ == '__main__':
    main()
