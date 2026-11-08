#!/usr/bin/env python3
"""Prepare an OpenDroneMap project from extract_rgb_frames.py + sfm.py output.

Frames are chosen from the refined COLMAP poses: looking at least --min-pitch below the
horizon with the view axis meeting the ground within --radius of the site centre; the
blurriest are dropped and the rest thinned to --max-images. Each kept frame gets EXIF
camera tags (exiftool), a mask hiding aircraft structure (dark blobs entering from the frame
edge: propeller blades, antenna), and a line in geo.txt with its telemetry position. Also
writes cameras.json (the COLMAP calibration as an OpenSfM perspective camera) and a site
boundary:

  odm_prep.py RGBWORK ODMROOT/project --centre E,N,U
"""
import argparse
import datetime
import json
import math
import shutil
from pathlib import Path
import subprocess
import sys

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dense_rgb import ground_level  # noqa: E402
from splat_common import LocalFrame, fatal, import_pycolmap  # noqa: E402

MAKE, MODEL = 'SIYI', 'ZT30 wide'


def aircraft_mask(img, dark, min_frac, grow):
    """255 = keep, 0 = aircraft: dark blobs entering from the top of the frame (closed so rolling-shutter
    striped propeller blades are solid), grown into the adjoining grey, unsaturated structure."""
    small = cv2.resize(img, None, fx=0.25, fy=0.25, interpolation=cv2.INTER_AREA)
    hsv = cv2.cvtColor(small, cv2.COLOR_BGR2HSV)
    sat, val = hsv[..., 1], hsv[..., 2]
    seed = cv2.morphologyEx((val < dark).astype(np.uint8), cv2.MORPH_CLOSE, np.ones((7, 7), np.uint8))
    n, lab, stats, _ = cv2.connectedComponentsWithStats(seed, connectivity=8)
    h, w = seed.shape
    bad = np.zeros_like(seed)
    for i in range(1, n):
        x, y, bw, bh, area = stats[i]
        # the camera hangs below the aircraft: its structure enters from the top, or a side near the top
        if area >= min_frac * h * w and (y == 0 or ((x == 0 or x + bw == w) and y < h // 3)):
            bad[lab == i] = 1
    if bad.any():
        grey = ((sat < 40) & (val < 170)).astype(np.uint8)
        k = np.ones((3, 3), np.uint8)
        for _ in range(12):  # geodesic growth into grey pixels next to the blob
            bad = np.maximum(bad, cv2.dilate(bad, k) & grey)
        bad = cv2.dilate(bad * 255, np.ones((2 * grow + 1, 2 * grow + 1), np.uint8))
    keep = cv2.resize(255 - bad * (255 if bad.max() == 1 else 1), (img.shape[1], img.shape[0]),
                      interpolation=cv2.INTER_NEAREST)
    return keep, float((bad > 0).mean())


def sharpness(img):
    gray = cv2.cvtColor(cv2.resize(img, None, fx=0.5, fy=0.5, interpolation=cv2.INTER_AREA), cv2.COLOR_BGR2GRAY)
    return float(cv2.Laplacian(gray, cv2.CV_32F).var())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('rgb', type=Path, help='RGB workdir (frames.json, images/, colmap/sparse/final)')
    ap.add_argument('project', type=Path, help='ODM project directory to create')
    ap.add_argument('--sparse', type=Path, help='COLMAP model (default RGB/colmap/sparse/final)')
    ap.add_argument('--centre', type=lambda s: np.array([float(v) for v in s.split(',')]),
                    help='site centre E,N,U (m, frames.json ENU; default: its target)')
    ap.add_argument('--telemetry', action='store_true',
                    help='choose frames by telemetry attitude instead of an RGB SfM model')
    ap.add_argument('--camera-from', type=Path, help='COLMAP model with the RADIAL lens calibration (--telemetry)')
    ap.add_argument('--ground-z', type=float, help='ground height in the ENU frame (default: from the SfM points, '
                    'or the site centre height with --telemetry)')
    ap.add_argument('--radius', type=float, default=400.0, help='view axis must meet the ground this close (m)')
    ap.add_argument('--boundary-radius', type=float, default=300.0, help='ODM output boundary radius (m)')
    ap.add_argument('--min-pitch', type=float, default=30.0, help='view axis at least this far below horizontal (deg)')
    ap.add_argument('--max-images', type=int, default=600)
    ap.add_argument('--drop-blurry', type=float, default=0.1, help='drop this fraction of the least sharp frames')
    ap.add_argument('--max-masked', type=float, default=0.15, help='drop frames with more than this fraction masked')
    ap.add_argument('--dark', type=int, default=50, help='aircraft mask: grey level below this')
    ap.add_argument('--h-accuracy', type=float, default=3.0, help='GPS horizontal accuracy for geo.txt (m)')
    ap.add_argument('--v-accuracy', type=float, default=5.0, help='GPS vertical accuracy for geo.txt (m)')
    args = ap.parse_args()

    pycolmap = import_pycolmap()
    info = json.loads((args.rgb / 'frames.json').read_text())
    frames = {f['name']: f for f in info['frames']}
    if args.centre is None:
        args.centre = np.array(info['target_enu'], dtype=float)
    if args.telemetry and args.ground_z is None:
        args.ground_z = float(args.centre[2])  # the extract tools put the target on the terrain
    if args.telemetry:
        # before (or without) RGB SfM: telemetry poses, lens calibration from another model of the same camera
        if not args.camera_from:
            fatal('--telemetry needs --camera-from MODEL for the lens calibration')
        cam = list(pycolmap.Reconstruction(str(args.camera_from)).cameras.values())[0]
        ground_z = args.ground_z
        poses = [(n, np.array(f['position']), np.array(f['rotation'])) for n, f in frames.items()
                 if f.get('rotation') is not None]
    else:
        rec = pycolmap.Reconstruction(str(args.sparse or args.rgb / 'colmap' / 'sparse' / 'final'))
        cams = list(rec.cameras.values())
        if len(cams) != 1:
            fatal('expected one camera')
        cam = cams[0]
        ground_z = ground_level(rec, args.centre, args.radius) if args.ground_z is None else args.ground_z
        poses = [(im.name, im.projection_center(), im.cam_from_world().inverse().rotation.matrix())
                 for im in rec.images.values() if im.name in frames]
    if cam.model.name != 'RADIAL':
        fatal('expected a RADIAL camera')

    cand = []
    for name, c, rot in poses:
        axis = rot[:, 2]
        pitch = math.degrees(math.asin(-axis[2]))
        if pitch < args.min_pitch:
            continue
        hit = c + axis * (ground_z - c[2]) / axis[2]
        if np.linalg.norm(hit[:2] - args.centre[:2]) < args.radius:
            cand.append(name)
    cand.sort()
    print('%d frames look at the site from %.0f deg or more below horizontal' % (len(cand), args.min_pitch), flush=True)

    imgdir = args.project / 'images'
    imgdir.mkdir(parents=True, exist_ok=True)
    scored = []
    for name in cand:
        img = cv2.imread(str(args.rgb / 'images' / name))
        keep, frac = aircraft_mask(img, args.dark, 0.002, 6)
        scored.append((name, sharpness(img), frac, keep))
    sharp = np.array([s[1] for s in scored])
    cut = np.quantile(sharp, args.drop_blurry) if args.drop_blurry > 0 else -1
    good = [s for s in scored if s[1] >= cut and s[2] <= args.max_masked]
    print('dropped %d blurry and %d mostly masked frames' % (
        sum(s[1] < cut for s in scored), sum(s[1] >= cut and s[2] > args.max_masked for s in scored)), flush=True)
    if args.max_images and len(good) > args.max_images:
        good = [good[i] for i in np.linspace(0, len(good) - 1, args.max_images).round().astype(int)]
    if len(good) < 20:
        fatal('only %d frames left' % len(good))

    origin = info['origin']
    local = LocalFrame(origin['lat'], origin['lon'], origin['alt_amsl'])
    geo = ['EPSG:4326']
    argfile = []
    masked = 0
    for name, _, frac, keep in good:
        dst = imgdir / name
        shutil.copyfile(args.rgb / 'images' / name, dst)  # a copy: exiftool edits it in place
        if frac > 0:
            cv2.imwrite(str(imgdir / (Path(name).stem + '_mask.png')), keep)
            masked += 1
        f = frames[name]
        lat, lon, alt = local.to_geodetic(f['position'])
        # unknown attitude: nan, so ODM does not treat oblique frames as nadir
        geo.append('%s %.9f %.9f %.3f nan nan nan %g %g' % (name, lon, lat, alt, args.h_accuracy, args.v_accuracy))
        t = datetime.datetime.fromtimestamp(f['utc_us'] / 1e6, datetime.timezone.utc)
        argfile += ['-q', '-q', '-overwrite_original', '-Make=%s' % MAKE, '-Model=%s' % MODEL,
                    '-FocalLengthIn35mmFormat=%d' % round(cam.params[0] / cam.width * 36),
                    '-DateTimeOriginal=%s' % t.strftime('%Y:%m:%d %H:%M:%S'),
                    '-SubSecTimeOriginal=%03d' % (t.microsecond // 1000), str(dst), '-execute']
    (args.project / 'exif.args').write_text('\n'.join(argfile) + '\n')
    subprocess.run(['exiftool', '-q', '-q', '-@', str(args.project / 'exif.args')], check=True)
    (args.project / 'exif.args').unlink()
    (args.project / 'geo.txt').write_text('\n'.join(geo) + '\n')

    # OpenSfM camera id as ODM builds it: lower-cased, focal ratio from the 35 mm EXIF, cut to 6 chars
    focal35 = round(cam.params[0] / cam.width * 36)
    cam_id = ('v2 %s %s %d %d perspective %s' % (MAKE, MODEL, cam.width, cam.height, str(float(focal35 / 36))[:6])).lower()
    p = cam.params
    (args.project / 'cameras.json').write_text(json.dumps({cam_id: dict(
        projection_type='perspective', width=cam.width, height=cam.height,
        focal=p[0] / max(cam.width, cam.height), k1=p[3], k2=p[4])}, indent=1))

    ring = []
    for a in np.linspace(0, 2 * math.pi, 73):
        lat, lon, _ = local.to_geodetic([args.centre[0] + args.boundary_radius * math.cos(a),
                                         args.centre[1] + args.boundary_radius * math.sin(a), 0])
        ring.append([lon, lat])
    (args.project / 'boundary.geojson').write_text(json.dumps(dict(type='FeatureCollection', features=[dict(
        type='Feature', properties={}, geometry=dict(type='Polygon', coordinates=[ring]))])))
    (args.project / 'prep.json').write_text(json.dumps(dict(
        centre_enu=args.centre.tolist(), radius_m=args.radius, boundary_radius_m=args.boundary_radius,
        min_pitch_deg=args.min_pitch, ground_z=ground_z, camera_id=cam_id, origin=origin,
        frames=[s[0] for s in good]), indent=1))
    print('wrote %d frames (%d with aircraft masks) to %s; camera id "%s"' % (len(good), masked, args.project, cam_id))


if __name__ == '__main__':
    main()
