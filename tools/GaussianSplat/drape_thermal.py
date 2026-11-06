#!/usr/bin/env python3
"""Drape radiometric thermal frames onto a surface_map.py surface model.

Every surface cell is projected into each thermal frame of an sfm.py reconstruction (in the
same ENU frame as the surface), with a depth test against the surface so canopy hides the
ground behind it. Per cell it keeps the hottest view, a view-weighted mean and the number of
views; hot cells are grouped into hot spots:

  drape_thermal.py THERMALWORK RGBWORK/dense/map [--sparse MODEL] [--out DIR]

Writes OUT (default MAPDIR/thermal)/{thermal_max.tif, thermal_mean.tif, views.tif,
thermal_max.png, overlay.png, thermal.kmz, thermal.glb, hotspots.geojson, drape.json}.
"""
import argparse
import json
from pathlib import Path
import sys
import time
import zipfile

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from colormaps import colormap_lut  # noqa: E402
from splat_common import LocalFrame, fatal, import_pycolmap, triangulate_rays  # noqa: E402
from surface_map import geo_transform, heightfield_mesh, write_tif  # noqa: E402


def project(cam, pts):
    """Pixel coordinates of camera-frame points for PINHOLE / SIMPLE_RADIAL / RADIAL cameras."""
    p = cam.params
    x, y = pts[:, 0] / pts[:, 2], pts[:, 1] / pts[:, 2]
    name = cam.model.name
    if name == 'PINHOLE':
        return p[0] * x + p[2], p[1] * y + p[3]
    r2 = x * x + y * y
    if name == 'SIMPLE_RADIAL':
        d = 1 + p[3] * r2
    elif name == 'RADIAL':
        d = 1 + p[3] * r2 + p[4] * r2 * r2
    else:
        fatal('unsupported camera model %s' % name)
    return p[0] * x * d + p[1], p[0] * y * d + p[2]


def downsample(height, valid, factor):
    """Block maximum height (top surface) over factor x factor cells."""
    n = (height.shape[0] // factor) * factor
    h = np.where(valid, height, -np.inf)[:n, :n].reshape(n // factor, factor, n // factor, factor)
    h = h.max(axis=(1, 3))
    return np.where(np.isfinite(h), h, np.nan).astype(np.float32), np.isfinite(h)


def ironbow_rgba(c, lo, hi, alpha):
    lut = (colormap_lut('ironbow') * 255).astype(np.uint8)
    v = np.clip((c - lo) / (hi - lo), 0, 1)
    idx = np.nan_to_num(v * 255).astype(np.uint8)
    return np.dstack([lut[idx], alpha.astype(np.uint8)])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('thermal', type=Path, help='thermal workdir (extract_frames.py + sfm.py)')
    ap.add_argument('mapdir', type=Path, help='surface_map.py output directory')
    ap.add_argument('--sparse', type=Path, help='thermal model (default THERMAL/colmap/sparse/final)')
    ap.add_argument('--out', type=Path, help='output directory (default MAPDIR/thermal)')
    ap.add_argument('--factor', type=int, default=2, help='thermal grid = surface cells x this')
    ap.add_argument('--max-range', type=float, default=500.0, help='ignore views further than this (m)')
    ap.add_argument('--min-obs', type=int, default=50, help='skip frames with fewer SfM observations')
    ap.add_argument('--hot-c', type=float, default=60.0, help='hot spot threshold (C)')
    ap.add_argument('--mesh-step', type=int, default=2, help='3D mesh vertex every this many cells')
    ap.add_argument('--ray-px', type=float, default=15.0,
                    help='rays within this many pixels of a triangulated hot spot belong to it')
    ap.add_argument('--hot-radius', type=float, default=2.0,
                    help='hot views agree if within this distance of each other (m)')
    ap.add_argument('--min-hot-views', type=int, default=3, help='a hot cell needs this many hot views')
    ap.add_argument('--min-hot-fraction', type=float, default=0.2,
                    help='and this fraction of the views that see it')
    ap.add_argument('--min-views', type=int, default=3, help='leave cells seen by fewer views blank')
    ap.add_argument('--no-level', dest='level', action='store_false',
                    help='do not correct per-frame temperature offsets')
    ap.add_argument('--edge', type=int, default=8, help='ignore this many pixels at the frame edges')
    args = ap.parse_args()

    from scipy.ndimage import map_coordinates
    pycolmap = import_pycolmap()
    surf = np.load(args.mapdir / 'surface.npz')
    height, valid = downsample(surf['height'], surf['valid'], args.factor)
    cell = float(surf['cell']) * args.factor
    lo = surf['lo']
    origin = surf['origin']
    local = LocalFrame(*origin)
    rows, cols = height.shape
    jj, ii = np.meshgrid(np.arange(cols), np.arange(rows))
    sel = valid.ravel()
    pts = np.stack([lo[0] + (jj + 0.5) * cell, lo[1] + (ii + 0.5) * cell, height], -1).reshape(-1, 3)[sel]
    m = len(pts)
    cell_ids = np.flatnonzero(sel)  # grid index of each surface cell
    rad = max(0, int(round(args.hot_radius / cell)))
    disk = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * rad + 1, 2 * rad + 1))
    print('%d surface cells of %.2f m' % (m, cell), flush=True)

    info = json.loads((args.thermal / 'frames.json').read_text())
    frames = {f['name']: f for f in info['frames']}
    rec = pycolmap.Reconstruction(str(args.sparse or args.thermal / 'colmap' / 'sparse' / 'final'))
    images = sorted((im for im in rec.images.values()
                     if im.name in frames and im.num_points3D >= args.min_obs), key=lambda im: im.name)
    if not images:
        fatal('no usable thermal frames')
    def sample(im):
        """Visible surface cells of a frame, their temperatures and weights."""
        cam = rec.cameras[im.camera_id]
        W, H = cam.width, cam.height
        T = im.cam_from_world().matrix()
        pc = pts @ T[:, :3].T + T[:, 3]
        z = pc[:, 2]
        near = np.flatnonzero((z > 1) & (z < args.max_range))
        if not len(near):
            return None
        u, v = project(cam, pc[near])
        e = args.edge
        inside = (u >= e) & (u < W - e) & (v >= e) & (v < H - e)
        near, u, v, zn = near[inside], u[inside], v[inside], z[near][inside]
        if not len(near):
            return None
        # depth test on a coarse buffer, two cells per block so the surface covers it without gaps;
        # tolerance grows with the depth spread a block covers at grazing angles
        ray = pts[near] - im.projection_center()
        dist = np.linalg.norm(ray, axis=1)
        sin_el = (-ray[:, 2] / dist).clip(0.1, 1)
        block = max(1, int(np.ceil(2 * cam.params[0] * cell / np.median(zn))))
        bw, bh = W // block + 1, H // block + 1
        bi = (v // block).astype(np.int64) * bw + (u // block).astype(np.int64)
        zbuf = np.full(bw * bh, np.inf, np.float32)
        np.minimum.at(zbuf, bi, zn)
        seen = zn <= zbuf[bi] + 1.0 + 0.01 * zn + 2 * cell / sin_el
        near, u, v, dist, sin_el = near[seen], u[seen], v[seen], dist[seen], sin_el[seen]
        fr = frames[im.name]
        raw = cv2.imread(str(args.thermal / 'thermal' / im.name), cv2.IMREAD_UNCHANGED)
        c = raw.astype(np.float32) * fr['temperature_scale_k'] + fr['temperature_offset_k'] - 273.15
        val = map_coordinates(c, [v, u], order=1, mode='nearest')
        # closer, steeper views count more; frame edges are feathered so footprints do not show
        edge = np.minimum(np.minimum(u, W - u), np.minimum(v, H - v)) / (0.15 * W)
        return near, val, (sin_el / dist) * edge.clip(0.05, 1)

    t0 = time.time()
    offsets = np.zeros(len(images))
    if args.level:
        # per-frame temperature offsets (radiometric drift) from overlapping views of the same cells
        ks, cs, vs, ws = [], [], [], []
        for k, im in enumerate(images):
            r = sample(im)
            if r is None:
                continue
            near, val, w = r
            sub = (near % 7 == 0) & (val < args.hot_c)
            ks.append(np.full(sub.sum(), k)), cs.append(near[sub]), vs.append(val[sub]), ws.append(w[sub])
        ks, cs, vs, ws = (np.concatenate(a) for a in (ks, cs, vs, ws))
        for _ in range(10):
            corr = vs - offsets[ks]
            mean = np.bincount(cs, ws * corr, minlength=m) / np.maximum(np.bincount(cs, ws, minlength=m), 1e-9)
            d = vs - mean[cs]
            multi = np.bincount(cs, minlength=m)[cs] > 1
            for k in range(len(images)):
                sel_k = (ks == k) & multi
                if sel_k.sum() > 100:
                    offsets[k] = np.median(d[sel_k])
            offsets -= np.median(offsets)
        print('frame temperature offsets: median |offset| %.2f C, 90%% %.2f C, max %.2f C (%.0f s)' % (
            np.median(np.abs(offsets)), np.percentile(np.abs(offsets), 90), np.abs(offsets).max(), time.time() - t0),
            flush=True)

    cmax = np.full(m, -np.inf, np.float32)
    wsum = np.zeros(m, np.float32)
    csum = np.zeros(m, np.float32)
    views = np.zeros(m, np.int32)
    hot_views = np.zeros(m, np.int32)
    rays = []  # hottest pixel of each hot frame, for 3D hot spots independent of the surface
    for k, im in enumerate(images):
        fr = frames[im.name]
        if fr.get('maximum_c', -1e9) - offsets[k] >= args.hot_c:
            raw = cv2.imread(str(args.thermal / 'thermal' / im.name), cv2.IMREAD_UNCHANGED)
            y, x = np.unravel_index(np.argmax(raw), raw.shape)
            cam = rec.cameras[im.camera_id]
            pose = im.cam_from_world().inverse()
            d = pose.rotation.matrix() @ np.r_[cam.cam_from_img(np.array([x, y], float)), 1.0]
            rays.append((pose.translation, d / np.linalg.norm(d), float(raw.max()) * fr['temperature_scale_k']
                         + fr['temperature_offset_k'] - 273.15 - offsets[k], cam.params[0]))
        r = sample(im)
        if r is None:
            continue
        near, val, w = r
        val = val - offsets[k]
        cmax[near] = np.maximum(cmax[near], val)
        wsum[near] += w
        csum[near] += w * val
        views[near] += 1
        if args.hot_radius > 0:  # a view counts as hot near any cell it saw hot, allowing for pose error
            g = np.zeros(rows * cols, np.uint8)
            g[cell_ids[near[val >= args.hot_c]]] = 1
            if g.any():
                g = cv2.dilate(g.reshape(rows, cols), disk).ravel()[cell_ids]
                hot_views += g.astype(np.int32)
        else:
            hot_views[near] += val >= args.hot_c
        if (k + 1) % 50 == 0:
            print('  %d/%d frames, %.0f s' % (k + 1, len(images), time.time() - t0), flush=True)
    print('%d frames draped in %.0f s; %.0f%% of cells seen' % (len(images), time.time() - t0, 100 * (views > 0).mean()))

    def grid(a, fill=np.nan):
        g = np.full(rows * cols, fill, np.float32)
        g[sel] = a
        return g.reshape(rows, cols)
    seen = views >= args.min_views
    gmax = grid(np.where(seen, cmax, np.nan))
    gmean = grid(np.where(seen, csum / np.maximum(wsum, 1e-9), np.nan))
    gviews = grid(views, 0)
    ghot = grid(hot_views, 0)

    out = args.out or args.mapdir / 'thermal'
    out.mkdir(parents=True, exist_ok=True)
    tf = geo_transform(local, lo, cell, (rows, cols))
    write_tif(out / 'thermal_max.tif', np.flipud(gmax), tf, nodata=np.nan)
    write_tif(out / 'thermal_mean.tif', np.flipud(gmean), tf, nodata=np.nan)
    write_tif(out / 'views.tif', np.flipud(gviews).astype(np.int16), tf)

    # colour: mean temperature for the background, the hottest view where it is hot
    finite = np.isfinite(gmean)
    t_lo = float(np.percentile(gmean[finite], 1))
    t_hi = max(float(np.nanmax(gmax)), args.hot_c)
    hot0 = (ghot >= args.min_hot_views) & (ghot >= args.min_hot_fraction * np.maximum(gviews, 1)) & \
        (np.nan_to_num(gmax, nan=-1e9) >= args.hot_c)
    show = np.where(hot0, gmax, gmean)
    # terrain spread over most of the palette, hotter than that compressed into the top
    t_bg = float(np.percentile(gmean[finite], 99.5))

    def norm(t):
        t = np.asarray(t, np.float64)
        cool = 0.7 * np.clip((t - t_lo) / max(t_bg - t_lo, 1e-3), 0, 1)
        warm = 0.7 + 0.3 * np.log1p(np.clip(t - t_bg, 0, None) / 8) / np.log1p(max(t_hi - t_bg, 1e-3) / 8)
        return np.where(t <= t_bg, cool, warm)
    rgba = ironbow_rgba(norm(show), 0, 1, np.where(finite, 255, 0))
    cv2.imwrite(str(out / 'thermal_max.png'), np.flipud(rgba[..., [2, 1, 0, 3]]))
    # hot where several views agree, so a single misprojected fire pixel is not a hot spot
    hot = (ghot >= args.min_hot_views) & (ghot >= args.min_hot_fraction * np.maximum(gviews, 1)) & \
        (np.nan_to_num(gmax, nan=-1e9) >= args.hot_c)
    overlay = ironbow_rgba(norm(gmax), 0, 1, np.where(hot, 255, 0))
    cv2.imwrite(str(out / 'overlay.png'), np.flipud(overlay[..., [2, 1, 0, 3]]))

    # hot spots
    # cells within --hot-radius of each other form one hot spot
    n_lab, lab, stats, _ = cv2.connectedComponentsWithStats(cv2.dilate(hot.astype(np.uint8), disk), connectivity=8)
    lab[~hot] = 0
    feats = []
    for i in range(1, n_lab):
        mask = lab == i
        if not mask.any():
            continue
        peak = float(np.nanmax(gmax[mask]))
        r, c = np.unravel_index(np.nanargmax(np.where(mask, gmax, -np.inf)), mask.shape)
        enu = [lo[0] + (c + 0.5) * cell, lo[1] + (r + 0.5) * cell, float(height[r, c])]
        lat, lon, alt = local.to_geodetic(enu)
        feats.append(dict(type='Feature', geometry=dict(type='Point', coordinates=[lon, lat, alt]),
                          properties=dict(peak_c=round(peak, 1), hot_views=int(ghot[r, c]), area_m2=round(float(mask.sum()) * cell * cell, 1),
                                          views=int(gviews[r, c]), enu=[round(x, 2) for x in enu])))
    feats.sort(key=lambda f: -f['properties']['peak_c'])
    (out / 'hotspots.geojson').write_text(json.dumps(dict(type='FeatureCollection', features=feats), indent=1))

    # 3D hot spots: rays from the hottest pixel of each hot frame, grouped greedily
    sources = []
    left = list(range(len(rays)))
    while len(left) >= args.min_hot_views:
        X, _ = triangulate_rays([rays[j][0] for j in left], [rays[j][1] for j in left], rays[left[0]][3])
        err = np.array([np.linalg.norm((np.eye(3) - np.outer(rays[j][1], rays[j][1])) @ (X - rays[j][0]))
                        / np.linalg.norm(X - rays[j][0]) * rays[j][3] for j in left])
        member = [j for j, e in zip(left, err) if e < args.ray_px]
        if len(member) < args.min_hot_views:
            break
        X, med = triangulate_rays([rays[j][0] for j in member], [rays[j][1] for j in member], rays[member[0]][3])
        lat, lon, alt = local.to_geodetic(X)
        sources.append(dict(type='Feature', geometry=dict(type='Point', coordinates=[lon, lat, alt]),
                            properties=dict(kind='triangulated', peak_c=round(max(rays[j][2] for j in member), 1),
                                            views=len(member), ray_error_px=round(med, 1),
                                            enu=[round(float(v), 2) for v in X])))
        left = [j for j in left if j not in member]
    for f in sources:
        print('triangulated hot spot: %.0f C from %d views, ray error %.1f px, ENU %s' % (
            f['properties']['peak_c'], f['properties']['views'], f['properties']['ray_error_px'],
            f['properties']['enu']))
    for f in feats:
        f['properties']['kind'] = 'surface'
    feats = sources + feats
    (out / 'hotspots.geojson').write_text(json.dumps(dict(type='FeatureCollection', features=feats), indent=1))

    # Google Earth overlay: the ENU grid is aligned with latitude/longitude
    north, west, _ = local.to_geodetic([lo[0], lo[1] + rows * cell, 0])
    south, east, _ = local.to_geodetic([lo[0] + cols * cell, lo[1], 0])
    placemarks = ''.join(
        '<Placemark><name>%.0f C</name><description>%s, %d views</description>'
        '<Point><coordinates>%.8f,%.8f,0</coordinates></Point></Placemark>' % (
            f['properties']['peak_c'], f['properties']['kind'], f['properties']['views'],
            *f['geometry']['coordinates'][:2]) for f in feats[:20])
    kml = ('<?xml version="1.0" encoding="UTF-8"?><kml xmlns="http://www.opengis.net/kml/2.2"><Document>'
           '<name>Thermal map</name><GroundOverlay><name>Apparent temperature</name><Icon><href>thermal.png</href></Icon>'
           '<LatLonBox><north>%.8f</north><south>%.8f</south><east>%.8f</east><west>%.8f</west></LatLonBox>'
           '</GroundOverlay>%s</Document></kml>' % (north, south, east, west, placemarks))
    ok, png = cv2.imencode('.png', np.flipud(rgba[..., [2, 1, 0, 3]]))
    with zipfile.ZipFile(out / 'thermal.kmz', 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('doc.kml', kml)
        z.writestr('thermal.png', png.tobytes())

    tex = np.flipud(rgba[..., :3]).copy()
    faces = heightfield_mesh(np.nan_to_num(height), valid, lo, cell, args.mesh_step, tex, out / 'thermal.glb')
    (out / 'drape.json').write_text(json.dumps(dict(
        frames=len(images), cell_m=cell, colour_range_c=[t_lo, t_bg, t_hi], hot_c=args.hot_c,
        north=north, south=south, east=east, west=west, hotspots=len(feats), mesh_faces=faces), indent=1))
    print('%d hot spots above %.0f C%s; wrote %s' % (
        len(feats), args.hot_c, (' (hottest %.0f C)' % feats[0]['properties']['peak_c']) if feats else '', out))


if __name__ == '__main__':
    main()
