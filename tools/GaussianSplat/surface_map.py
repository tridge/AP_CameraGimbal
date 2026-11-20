#!/usr/bin/env python3
"""Surface model and orthophoto from a dense_rgb.py point cloud.

Grids the fused points in the local ENU frame (which is aligned with latitude/longitude),
taking a high percentile of point heights per cell as the top surface and the mean point
colour as the orthophoto; small holes are filled from neighbouring cells:

  surface_map.py RGBWORK [--cell 0.25]

Writes RGBWORK/dense/map/{dsm.tif, ortho.tif, ortho.png, surface.npz, surface.glb}: GeoTIFFs
in WGS84 (DSM heights AMSL), a textured heightfield mesh for 3D viewing, and surface.npz
(the ENU grid) for drape_thermal.py.
"""
import argparse
import json
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from splat_common import LocalFrame, fatal  # noqa: E402


def read_points(path):
    from plyfile import PlyData
    v = PlyData.read(str(path))['vertex']
    xyz = np.stack([v['x'], v['y'], v['z']], axis=1).astype(np.float64)
    rgb = np.stack([v['red'], v['green'], v['blue']], axis=1).astype(np.float32)
    return xyz, rgb


def grid_points(xyz, rgb, lo, cell, shape, pct):
    """Per-cell height percentile and mean colour; NaN where empty."""
    ij = np.floor((xyz[:, :2] - lo) / cell).astype(np.int64)
    ok = (ij[:, 0] >= 0) & (ij[:, 0] < shape[1]) & (ij[:, 1] >= 0) & (ij[:, 1] < shape[0])
    ij, z, rgb = ij[ok], xyz[ok, 2], rgb[ok]
    key = ij[:, 1] * shape[1] + ij[:, 0]  # row = north index, col = east index
    order = np.lexsort((z, key))
    key, z, rgb = key[order], z[order], rgb[order]
    starts = np.flatnonzero(np.r_[True, key[1:] != key[:-1]])
    counts = np.diff(np.r_[starts, len(key)])
    height = np.full(shape[0] * shape[1], np.nan, np.float32)
    height[key[starts]] = z[starts + ((counts - 1) * pct).astype(np.int64)]
    colour = np.zeros((shape[0] * shape[1], 3), np.float32)
    for c in range(3):
        colour[key[starts], c] = np.add.reduceat(rgb[:, c], starts) / counts
    n = np.zeros(shape[0] * shape[1], np.int32)
    n[key[starts]] = counts
    return height.reshape(shape), colour.reshape(shape + (3,)), n.reshape(shape)


def remove_spikes(height, size, max_dev):
    """Drop cells far from the local median (isolated stereo outliers)."""
    from scipy.ndimage import distance_transform_edt, median_filter
    empty = np.isnan(height)
    _, (ii, jj) = distance_transform_edt(empty, return_indices=True)
    med = median_filter(height[ii, jj], size=size, mode='nearest')
    bad = ~empty & (np.abs(height - med) > max_dev)
    out = height.copy()
    out[bad] = np.nan
    return out, int(bad.sum())


def fill_holes(height, colour, max_cells):
    """Nearest-neighbour fill of holes up to max_cells from data; returns filled arrays and a validity mask."""
    from scipy.ndimage import distance_transform_edt
    empty = np.isnan(height)
    d, (ii, jj) = distance_transform_edt(empty, return_indices=True)
    valid = d <= max_cells
    h = np.where(valid, height[ii, jj], np.nan)
    c = np.where(valid[..., None], colour[ii, jj], 0)
    return h.astype(np.float32), c, valid


def geo_transform(local, lo, cell, shape):
    """Affine for a north-up WGS84 raster whose row 0 is the northern edge."""
    from rasterio.transform import Affine
    lat0, lon0, _ = local.to_geodetic([lo[0], lo[1] + shape[0] * cell, 0])
    lat1, lon1, _ = local.to_geodetic([lo[0] + shape[1] * cell, lo[1], 0])
    return Affine((lon1 - lon0) / shape[1], 0, lon0, 0, (lat1 - lat0) / shape[0], lat0)


def write_tif(path, data, transform, nodata=None):
    import rasterio
    data = np.asarray(data)
    bands = data[None] if data.ndim == 2 else np.moveaxis(data, -1, 0)
    with rasterio.open(path, 'w', driver='GTiff', width=bands.shape[2], height=bands.shape[1],
                       count=bands.shape[0], dtype=bands.dtype, crs='EPSG:4326', transform=transform,
                       nodata=nodata, compress='deflate') as f:
        f.write(bands)


def heightfield_mesh(height, valid, lo, cell, step, texture, path):
    """Textured triangle mesh of the surface, every step cells, written as GLB."""
    import trimesh
    from PIL import Image
    h = height[::step, ::step]
    ok = valid[::step, ::step]
    rows, cols = h.shape
    jj, ii = np.meshgrid(np.arange(cols), np.arange(rows))
    verts = np.stack([lo[0] + (jj + 0.5) * cell * step, lo[1] + (ii + 0.5) * cell * step,
                      np.nan_to_num(h)], axis=-1).reshape(-1, 3)
    uv = np.stack([(jj + 0.5) / cols, (ii + 0.5) / rows], axis=-1).reshape(-1, 2)
    idx = np.arange(rows * cols).reshape(rows, cols)
    a, b, c, d = idx[:-1, :-1], idx[:-1, 1:], idx[1:, :-1], idx[1:, 1:]
    quad_ok = (ok[:-1, :-1] & ok[:-1, 1:] & ok[1:, :-1] & ok[1:, 1:]).ravel()
    faces = np.concatenate([np.stack([a, b, d], -1).reshape(-1, 3)[quad_ok],
                            np.stack([a, d, c], -1).reshape(-1, 3)[quad_ok]])
    img = Image.fromarray(np.ascontiguousarray(texture))
    # glTF is y-up: ENU (e, n, u) -> (e, u, -n); texture is north-up, uv v=0 at the south edge
    mesh = trimesh.Trimesh(verts[:, [0, 2, 1]] * [1, 1, -1], faces, process=False,
                           visual=trimesh.visual.TextureVisuals(uv=uv, image=img))
    mesh.remove_unreferenced_vertices()
    mesh.export(str(path))
    return len(mesh.faces)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('workdir', type=Path, help='RGB workdir with dense/fused.ply from dense_rgb.py')
    ap.add_argument('--dense', type=Path, help='dense workspace (default WORKDIR/dense)')
    ap.add_argument('--cell', type=float, default=0.25, help='grid cell (m)')
    ap.add_argument('--percentile', type=float, default=0.9, help='height percentile per cell (top surface)')
    ap.add_argument('--fill', type=float, default=3.0, help='fill holes up to this far from data (m)')
    ap.add_argument('--spike', type=float, default=10.0, help='drop cells this far from the 5x5 median height (m)')
    ap.add_argument('--mesh-step', type=int, default=4, help='mesh vertex every this many cells')
    args = ap.parse_args()

    dense = args.dense or args.workdir / 'dense'
    site = json.loads((dense / 'site.json').read_text())
    origin = json.loads((args.workdir / 'frames.json').read_text())['origin']
    local = LocalFrame(origin['lat'], origin['lon'], origin['alt_amsl'])
    out = dense / 'map'
    out.mkdir(exist_ok=True)

    xyz, rgb = read_points(dense / 'fused.ply')
    if len(xyz) < 1000:
        fatal('only %d fused points' % len(xyz))
    c, r = np.array(site['centre_enu'][:2]), site['radius_m']
    lo = c - r
    n = int(np.ceil(2 * r / args.cell))
    shape = (n, n)
    height, colour, counts = grid_points(xyz, rgb, lo, args.cell, shape, args.percentile)
    print('%d points, %d of %d cells with data' % (len(xyz), np.isfinite(height).sum(), n * n), flush=True)
    height, spikes = remove_spikes(height, 5, args.spike)
    height, colour, valid = fill_holes(height, colour, args.fill / args.cell)
    print('removed %d spikes; %.0f%% of the site covered after filling' % (spikes, 100 * valid.mean()), flush=True)

    # rasters: row 0 north
    tf = geo_transform(local, lo, args.cell, shape)
    write_tif(out / 'dsm.tif', np.flipud(np.where(valid, height + origin['alt_amsl'], -9999)).astype(np.float32),
              tf, nodata=-9999)
    ortho = np.clip(colour, 0, 255).astype(np.uint8)
    alpha = (valid * 255).astype(np.uint8)
    write_tif(out / 'ortho.tif', np.flipud(np.dstack([ortho, alpha])), tf)
    import cv2
    cv2.imwrite(str(out / 'ortho.png'), np.flipud(np.dstack([ortho[..., ::-1], alpha])))
    np.savez_compressed(out / 'surface.npz', height=height, valid=valid, lo=lo, cell=args.cell,
                        origin=[origin['lat'], origin['lon'], origin['alt_amsl']])
    faces = heightfield_mesh(height, valid, lo, args.cell, args.mesh_step, np.flipud(ortho), out / 'surface.glb')
    lat_n, lon_w, _ = local.to_geodetic([lo[0], lo[1] + n * args.cell, 0])
    lat_s, lon_e, _ = local.to_geodetic([lo[0] + n * args.cell, lo[1], 0])
    (out / 'map.json').write_text(json.dumps(dict(
        cell_m=args.cell, size=n, enu_lo=lo.tolist(), north=lat_n, south=lat_s, west=lon_w, east=lon_e,
        ground_z=site['ground_z'], alt_amsl_origin=origin['alt_amsl'], mesh_faces=faces), indent=1))
    print('wrote %s (%dx%d cells of %.2f m, mesh %d triangles)' % (out, n, n, args.cell, faces))


if __name__ == '__main__':
    main()
