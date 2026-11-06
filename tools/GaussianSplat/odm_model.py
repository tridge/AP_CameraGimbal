#!/usr/bin/env python3
"""Write OpenDroneMap's reconstruction as a COLMAP model in our local ENU frame.

ODM's bundle adjustment is constrained by the frames' GPS, so on long transit or survey flights
its cameras stay true where an unconstrained incremental SfM bends. Use the result in place of
the RGB model when transferring poses to the thermal frames and when aligning to the map:

  odm_model.py ODMPROJECT RGBWORK/colmap_warp --warp RGBWORK
  transfer_poses.py RGBWORK THERMALWORK --rgb-sparse RGBWORK/colmap_warp
  odm_import.py ODMPROJECT RGBWORK THERMALWORK OUT --rgb-sparse RGBWORK/colmap_warp ...

Without --warp only ODM's own frames are written, which is often too sparse to interpolate
thermal poses from (ODM drops frames).

The ENU frame is the one odm_prep.py recorded in prep.json, so the maps share it.
"""
import argparse
import json
import math
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from splat_common import LocalFrame, fatal  # noqa: E402
from odm_import import topocentric_to_lla  # noqa: E402


def rodrigues(rv):
    rv = np.asarray(rv, dtype=np.float64)
    theta = np.linalg.norm(rv)
    if theta == 0:
        return np.eye(3)
    k = rv / theta
    K = np.array([[0, -k[2], k[1]], [k[2], 0, -k[0]], [-k[1], k[0], 0]])
    return np.eye(3) + math.sin(theta) * K + (1 - math.cos(theta)) * K @ K


def warp(rgbwork, odm, Rotation):
    """Every frame of our RGB model, bent onto ODM's cameras.

    Each frame ODM also has gives a rotation and position correction; these are interpolated in
    time to the frames ODM dropped, after a global similarity takes out the bulk.
    """
    from scipy.ndimage import median_filter
    from scipy.spatial.transform import Slerp
    from splat_common import import_pycolmap
    from odm_import import robust_sim3
    rec = import_pycolmap().Reconstruction(str(rgbwork / 'colmap' / 'sparse' / 'final'))
    utc = {f['name']: f['utc_us'] / 1e6 for f in json.loads((rgbwork / 'frames.json').read_text())['frames']}
    ours = {}
    for im in rec.images.values():
        if im.name in utc:
            p = im.cam_from_world().inverse()
            ours[im.name] = (p.rotation.matrix(), np.asarray(p.translation))
    shared = sorted((n for n in ours if n in odm), key=utc.get)
    if len(shared) < 10:
        fatal('only %d frames shared between ODM and %s' % (len(shared), rgbwork))
    s, Rg, tg, res, keep = robust_sim3(np.array([ours[n][1] for n in shared]), np.array([odm[n][1] for n in shared]))
    shared = [n for n, k in zip(shared, keep) if k]
    t = np.array([utc[n] for n in shared])
    dR = Rotation.from_matrix([odm[n][0] @ (Rg @ ours[n][0]).T for n in shared])
    dC = np.array([odm[n][1] - (s * Rg @ ours[n][1] + tg) for n in shared])
    k = min(9, len(shared) // 2 * 2 - 1)
    dC = median_filter(dC, size=(k, 1), mode='nearest')
    rv = median_filter(dR.as_rotvec(), size=(k, 1), mode='nearest')
    slerp = Slerp(t, Rotation.from_rotvec(rv))
    print('warp from %d shared frames: residual after similarity median %.1f m; corrections median %.2f deg, %.1f m' % (
        len(shared), np.median(res[keep]), np.degrees(np.median(np.linalg.norm(rv, axis=1))),
        np.median(np.linalg.norm(dC, axis=1))))
    cam = rec.cameras[next(iter(rec.images.values())).camera_id]
    poses = []
    for name in sorted(ours, key=utc.get):
        tt = float(np.clip(utc[name], t[0], t[-1]))
        R_wc, c = ours[name]
        corr = np.array([np.interp(tt, t, dC[:, j]) for j in range(3)])
        poses.append((name, slerp([tt])[0].as_matrix() @ Rg @ R_wc, s * Rg @ c + tg + corr, 1))
    K = cam.calibration_matrix()
    cams = '1 PINHOLE %d %d %.9g %.9g %.9g %.9g\n' % (cam.width, cam.height, K[0, 0], K[1, 1], K[0, 2], K[1, 2])
    return poses, cams


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('project', type=Path, help='ODM project (odm_prep.py output after ODM ran)')
    ap.add_argument('out', type=Path, help='output COLMAP text model directory')
    ap.add_argument('--warp', type=Path, metavar='RGBWORK',
                    help='write every frame of this RGB workdir\'s model, corrected onto ODM\'s cameras')
    args = ap.parse_args()

    from scipy.spatial.transform import Rotation
    prep = json.loads((args.project / 'prep.json').read_text())
    o = prep['origin']
    local = LocalFrame(o['lat'], o['lon'], o['alt_amsl'])
    ref = json.loads((args.project / 'opensfm' / 'reference_lla.json').read_text())
    recs = json.loads((args.project / 'opensfm' / 'reconstruction.topocentric.json').read_text())
    rec = max(recs, key=lambda r: len(r['shots']))
    if not rec['shots']:
        fatal('empty ODM reconstruction')

    def to_enu(p):
        return local.to_enu(*topocentric_to_lla(p, ref))

    # the two tangent planes differ by a small rotation; take it from how ODM's axes map across
    c0 = to_enu([0.0, 0.0, 0.0])
    R_tl = np.stack([to_enu(e) - c0 for e in np.eye(3) * 100.0], axis=1) / 100.0
    u, _, vt = np.linalg.svd(R_tl)
    R_tl = u @ vt  # ODM topocentric -> our ENU

    args.out.mkdir(parents=True, exist_ok=True)
    cams, cam_ids = [], {}
    for key, c in rec['cameras'].items():
        w, h = c['width'], c['height']
        f = c.get('focal', c.get('focal_x', 0.85)) * max(w, h)
        cam_ids[key] = len(cams) + 1
        cams.append('%d PINHOLE %d %d %.9g %.9g %.9g %.9g\n' % (len(cams) + 1, w, h, f, f, w / 2, h / 2))
    (args.out / 'cameras.txt').write_text(''.join(cams))
    odm = {}  # name -> (world_from_cam rotation, centre) in our ENU
    for name, shot in rec['shots'].items():
        R_cw = rodrigues(shot['rotation'])  # topocentric -> camera
        odm[name] = (R_tl @ R_cw.T, to_enu(-R_cw.T @ np.array(shot['translation'])), cam_ids[shot['camera']])
    if args.warp:
        poses, cams = warp(args.warp, odm, Rotation)
        (args.out / 'cameras.txt').write_text(cams)
    else:
        poses = [(n, R, c, cam) for n, (R, c, cam) in sorted(odm.items())]
    lines = []
    for i, (name, R_wc, centre, cam) in enumerate(poses):
        R = R_wc.T  # our ENU -> camera
        q = Rotation.from_matrix(R).as_quat()
        t = -R @ centre
        lines.append('%d %.12g %.12g %.12g %.12g %.9g %.9g %.9g %d %s\n\n' % (
            i + 1, q[3], q[0], q[1], q[2], *t, cam, name))
    (args.out / 'images.txt').write_text(''.join(lines))
    pts = []
    for i, p in enumerate(rec.get('points', {}).values()):
        e = to_enu(p['coordinates'])
        pts.append('%d %.6f %.6f %.6f %d %d %d 0\n' % (i + 1, *e, *[int(v) for v in p['color']]))
    (args.out / 'points3D.txt').write_text(''.join(pts))
    print('wrote %d ODM cameras and %d points to %s' % (len(lines), len(pts), args.out))


if __name__ == '__main__':
    main()
