#!/usr/bin/env python3
"""Bring an OpenDroneMap result into our ENU frame and align the thermal model to it.

1. Camera centres of the frames shared by ODM's reconstruction (opensfm/
   reconstruction.topocentric.json about reference_lla.json) and our RGB COLMAP model give a
   robust similarity transform from our ENU frame onto ODM's georeferencing.
2. That transform is applied to the thermal COLMAP model (poses and points).
3. ODM's DSM and orthophoto are resampled onto a north-up grid in our ENU frame and written in
   surface_map.py's layout, so drape_thermal.py can use them:

  odm_import.py ODMPROJECT RGBWORK THERMALWORK OUTDIR
  drape_thermal.py THERMALWORK OUTDIR --sparse OUTDIR/thermal_model
"""
import argparse
import json
import math
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from splat_common import LocalFrame, fatal, import_pycolmap  # noqa: E402
from surface_map import geo_transform, heightfield_mesh, write_tif  # noqa: E402

WGS84_A, WGS84_E2 = 6378137.0, 6.69437999014e-3


def ecef(lat, lon, alt):
    la, lo = math.radians(lat), math.radians(lon)
    n = WGS84_A / math.sqrt(1 - WGS84_E2 * math.sin(la) ** 2)
    return np.array([(n + alt) * math.cos(la) * math.cos(lo), (n + alt) * math.cos(la) * math.sin(lo),
                     (n * (1 - WGS84_E2) + alt) * math.sin(la)])


def ecef_to_lla(x, y, z):
    lon = math.atan2(y, x)
    p = math.hypot(x, y)
    lat = math.atan2(z, p * (1 - WGS84_E2))
    for _ in range(6):
        n = WGS84_A / math.sqrt(1 - WGS84_E2 * math.sin(lat) ** 2)
        alt = p / math.cos(lat) - n
        lat = math.atan2(z, p * (1 - WGS84_E2 * n / (n + alt)))
    return math.degrees(lat), math.degrees(lon), alt


def topocentric_to_lla(enu, ref):
    """OpenSfM topocentric (ENU tangent plane at ref) to geodetic."""
    la, lo = math.radians(ref['latitude']), math.radians(ref['longitude'])
    r = np.array([[-math.sin(lo), -math.sin(la) * math.cos(lo), math.cos(la) * math.cos(lo)],
                  [math.cos(lo), -math.sin(la) * math.sin(lo), math.cos(la) * math.sin(lo)],
                  [0, math.cos(la), math.sin(la)]])
    return ecef_to_lla(*(ecef(ref['latitude'], ref['longitude'], ref['altitude']) + r @ np.asarray(enu)))


def umeyama(src, dst):
    """Similarity (s, R, t) with dst ~ s R src + t."""
    ms, md = src.mean(0), dst.mean(0)
    a, b = src - ms, dst - md
    u, d, vt = np.linalg.svd(b.T @ a / len(src))
    sgn = np.eye(3)
    if np.linalg.det(u @ vt) < 0:
        sgn[2, 2] = -1
    R = u @ sgn @ vt
    s = np.trace(np.diag(d) @ sgn) / (a ** 2).sum(1).mean()
    return s, R, md - s * R @ ms


def robust_sim3(src, dst, iters=5):
    keep = np.ones(len(src), bool)
    for _ in range(iters):
        s, R, t = umeyama(src[keep], dst[keep])
        res = np.linalg.norm(dst - (s * src @ R.T + t), axis=1)
        thr = max(3 * 1.4826 * np.median(res[keep]), 0.05)
        keep = res < thr
    return s, R, t, res, keep


def odm_centres(project):
    recs = json.loads((project / 'opensfm' / 'reconstruction.topocentric.json').read_text())
    ref = json.loads((project / 'opensfm' / 'reference_lla.json').read_text())
    rec = max(recs, key=lambda r: len(r['shots']))
    out = {}
    for name, shot in rec['shots'].items():
        rv = np.array(shot['rotation'])
        theta = np.linalg.norm(rv)
        k = rv / theta if theta > 0 else rv
        K = np.array([[0, -k[2], k[1]], [k[2], 0, -k[0]], [-k[1], k[0], 0]])
        R = np.eye(3) + math.sin(theta) * K + (1 - math.cos(theta)) * K @ K  # world -> camera
        out[name] = -R.T @ np.array(shot['translation'])
    return out, ref, len(recs)


def resample(path, transform, shape, bands, resampling):
    import rasterio
    from rasterio.warp import reproject
    with rasterio.open(path) as src:
        idx = bands or list(range(1, src.count + 1))
        out = np.zeros((len(idx),) + shape, np.float32)
        for k, b in enumerate(idx):
            dst = np.full(shape, np.nan, np.float32)
            reproject(rasterio.band(src, b), dst, dst_transform=transform, dst_crs='EPSG:4326',
                      resampling=resampling, dst_nodata=np.nan,
                      src_nodata=src.nodata if src.nodata is not None else None)
            out[k] = dst
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('project', type=Path, help='ODM project (odm_prep.py output after ODM ran)')
    ap.add_argument('rgb', type=Path, help='RGB workdir (frames.json, colmap/sparse/final)')
    ap.add_argument('thermal', type=Path, help='thermal workdir whose model is aligned')
    ap.add_argument('out', type=Path)
    ap.add_argument('--rgb-sparse', type=Path)
    ap.add_argument('--thermal-sparse', type=Path, help='default THERMAL/colmap/sparse/final')
    ap.add_argument('--cell', type=float, default=0.25)
    ap.add_argument('--ortho', type=Path, help='orthophoto to use (default: ODM\'s; e.g. ortho_mosaic.py output)')
    ap.add_argument('--radius', type=float, help='grid radius (default: the ODM boundary radius)')
    ap.add_argument('--mesh-step', type=int, default=4)
    ap.add_argument('--max-dev', type=float, default=25.0,
                    help='drop DSM cells this far from the median of the surrounding 14 m (m)')
    ap.add_argument('--smooth', type=float, default=2.0, help='median filter the DSM over this size (m, 0 = off)')
    ap.add_argument('--local-window', type=float, default=0.0,
                    help='align each thermal pose using RGB frames within this many seconds (0 = global only)')
    args = ap.parse_args()

    from rasterio.enums import Resampling
    pycolmap = import_pycolmap()
    prep = json.loads((args.project / 'prep.json').read_text())
    origin = prep['origin']
    local = LocalFrame(origin['lat'], origin['lon'], origin['alt_amsl'])
    args.out.mkdir(parents=True, exist_ok=True)

    # 1. similarity from our ENU onto ODM's georeferenced cameras
    odm, ref, n_recs = odm_centres(args.project)
    rgb = pycolmap.Reconstruction(str(args.rgb_sparse or args.rgb / 'colmap' / 'sparse' / 'final'))
    ours = {im.name: im.projection_center() for im in rgb.images.values()}
    names = sorted(set(odm) & set(ours))
    if len(names) < 10:
        fatal('only %d frames shared with the ODM reconstruction' % len(names))
    src = np.array([ours[n] for n in names])
    dst = np.array([local.to_enu(*topocentric_to_lla(odm[n], ref)) for n in names])
    raw = np.linalg.norm(dst - src, axis=1)
    s, R, t, res, keep = robust_sim3(src, dst)
    ang = math.degrees(math.acos(np.clip((np.trace(R) - 1) / 2, -1, 1)))
    print('ODM: %d reconstruction(s), %d frames in the largest, %d shared with our RGB model' % (
        n_recs, len(odm), len(names)))
    print('before alignment: camera centres differ by median %.2f m; similarity: scale %.5f, rotation %.3f deg, '
          'shift %s m; residual median %.2f m, 90%% %.2f m (%d/%d inliers)' % (
              np.median(raw), s, ang, np.round(t, 2), np.median(res[keep]), np.percentile(res[keep], 90),
              keep.sum(), len(keep)))
    geo = {}
    for line in (args.project / 'geo.txt').read_text().splitlines()[1:]:
        p = line.split()
        geo[p[0]] = local.to_enu(float(p[2]), float(p[1]), float(p[3]))
    gps = np.array([np.linalg.norm(dst[i] - geo[n]) for i, n in enumerate(names) if n in geo])
    print('ODM camera centres vs telemetry GPS: median %.2f m, 90%% %.2f m' % (np.median(gps), np.percentile(gps, 90)))

    # 2. thermal model into the ODM-aligned frame
    th = pycolmap.Reconstruction(str(args.thermal_sparse or args.thermal / 'colmap' / 'sparse' / 'final'))
    th.transform(pycolmap.Sim3d(s, pycolmap.Rotation3d(R), t))  # points and a first pass at the poses
    # each thermal camera is then shifted by the remaining offset of the RGB frames near it in
    # time, so a slow warp between the two reconstructions does not move it on the ground
    rgb_t = {f['name']: f['utc_us'] / 1e6 for f in json.loads((args.rgb / 'frames.json').read_text())['frames']}
    th_t = {f['name']: f['utc_us'] / 1e6 for f in json.loads((args.thermal / 'frames.json').read_text())['frames']}
    shared_t = np.array([rgb_t[nm] for nm in names])
    g_src = src * s @ R.T + t  # our RGB centres after the global similarity
    moved = []
    for im in th.images.values():
        if im.name not in th_t:
            continue
        near = np.flatnonzero(np.abs(shared_t - th_t[im.name]) <= args.local_window) if args.local_window > 0 else []
        near = near[keep[near]] if len(near) else near
        if len(near) < 6:
            continue
        # local shift only: a similarity fitted to a short, nearly straight leg is ill-conditioned
        shift = np.median(dst[near] - g_src[near], axis=0)
        pose = im.cam_from_world()
        c_old = im.projection_center()
        c_new = c_old + shift
        r_cw = pose.rotation.matrix()
        th.frame(im.frame_id).rig_from_world = pycolmap.Rigid3d(pycolmap.Rotation3d(r_cw), -r_cw @ c_new)
        moved.append(np.linalg.norm(c_new - c_old))
    if moved:
        print('local alignment moved %d thermal cameras by median %.2f m, 90%% %.2f m' % (
            len(moved), np.median(moved), np.percentile(moved, 90)))
    (args.out / 'thermal_model').mkdir(exist_ok=True)
    th.write(str(args.out / 'thermal_model'))
    (args.out / 'alignment.json').write_text(json.dumps(dict(
        scale=s, rotation=R.tolist(), translation=t.tolist(), rotation_deg=ang, shared_frames=len(names),
        inliers=int(keep.sum()), residual_median_m=float(np.median(res[keep])),
        residual_p90_m=float(np.percentile(res[keep], 90)), odm_vs_gps_median_m=float(np.median(gps))), indent=1))

    # 3. DSM and orthophoto onto our ENU grid (north-up raster; our arrays have row 0 south)
    c = np.array(prep['centre_enu'][:2])
    r = args.radius or prep['boundary_radius_m']
    lo = c - r
    n = int(np.ceil(2 * r / args.cell))
    tf = geo_transform(local, lo, args.cell, (n, n))
    dsm = resample(args.project / 'odm_dem' / 'dsm.tif', tf, (n, n), [1], Resampling.bilinear)[0]
    ortho = resample(args.ortho or args.project / 'odm_orthophoto' / 'odm_orthophoto.tif', tf, (n, n), None,
                     Resampling.average)
    height = np.flipud(dsm) - origin['alt_amsl']
    valid = np.isfinite(height)
    if args.smooth > 0 and valid.any():  # stereo noise in canopy shows as spikes in 3D
        from scipy.ndimage import distance_transform_edt, gaussian_filter, median_filter
        _, (ii, jj) = distance_transform_edt(~valid, return_indices=True)
        k = max(3, int(round(args.smooth / args.cell)) | 1)
        filled = height[ii, jj]
        # drop cells far from the broad local median: isolated walls of bad stereo at the edges
        broad = median_filter(filled[::8, ::8], size=7)
        broad = np.kron(broad, np.ones((8, 8)))[:filled.shape[0], :filled.shape[1]]
        valid &= np.abs(filled - broad) < args.max_dev
        _, (ii, jj) = distance_transform_edt(~valid, return_indices=True)
        height = gaussian_filter(median_filter(filled[ii, jj], size=k), sigma=k / 4)
        height[~valid] = np.nan
    rgbo = np.flipud(np.moveaxis(np.nan_to_num(ortho[:3]), 0, -1)).clip(0, 255).astype(np.uint8)
    if len(ortho) > 3:
        valid &= np.flipud(np.nan_to_num(ortho[3])) > 0
    print('surface: %dx%d cells of %.2f m, %.0f%% covered' % (n, n, args.cell, 100 * valid.mean()))
    write_tif(args.out / 'dsm.tif', np.flipud(np.where(valid, height + origin['alt_amsl'], -9999)).astype(np.float32),
              tf, nodata=-9999)
    alpha = (valid * 255).astype(np.uint8)
    write_tif(args.out / 'ortho.tif', np.flipud(np.dstack([rgbo, alpha])), tf)
    import cv2
    cv2.imwrite(str(args.out / 'ortho.png'), np.flipud(np.dstack([rgbo[..., ::-1], alpha])))
    np.savez_compressed(args.out / 'surface.npz', height=np.nan_to_num(height).astype(np.float32), valid=valid,
                        lo=lo, cell=args.cell, origin=[origin['lat'], origin['lon'], origin['alt_amsl']])
    faces = heightfield_mesh(np.nan_to_num(height), valid, lo, args.cell, args.mesh_step, np.flipud(rgbo),
                             args.out / 'surface.glb')
    print('wrote %s (mesh %d triangles)' % (args.out, faces))


if __name__ == '__main__':
    main()
