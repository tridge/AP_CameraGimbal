"""Shared helpers for the thermal Gaussian splat tools."""
import json
import math
import os
from pathlib import Path
import site
import sys

import numpy as np

EARTH_RADIUS = 6378137.0
# OpenCV camera axes (x right, y down, z forward) expressed in body FRD axes
R_BODY_CAM = np.array([[0, 0, 1], [1, 0, 0], [0, 1, 0]], dtype=float)
R_ENU_NED = np.array([[0, 1, 0], [1, 0, 0], [0, 0, -1]], dtype=float)
# the viewer's circular trim, this fraction of the radius beyond which training prunes Gaussians
TRIM_FRACTION = 0.85


def trim_info(bounds):
    """apcg_splat trim fields from training bounds {centre: [x, y], radius}: the fringe just inside
    the pruning radius is the poorly constrained part."""
    if not bounds or not bounds.get('radius'):
        return {}
    return dict(trim_centre=[float(c) for c in bounds['centre'][:2]],
                trim_radius=TRIM_FRACTION * float(bounds['radius']))


def setup_cuda_env():
    """Point torch extensions at the pip CUDA toolkit (nvidia/cu13) if CUDA_HOME is unset."""
    # the venv's ninja is needed for the JIT build even when the venv is not activated
    bindir = str(Path(sys.executable).parent)
    if bindir not in os.environ.get('PATH', '').split(os.pathsep):
        os.environ['PATH'] = bindir + os.pathsep + os.environ.get('PATH', '')
    if os.environ.get('CUDA_HOME'):
        return
    for base in site.getsitepackages() + [site.getusersitepackages()]:
        home = Path(base) / 'nvidia' / 'cu13'
        if (home / 'bin' / 'nvcc').exists():
            os.environ['CUDA_HOME'] = str(home)
            os.environ['PATH'] = str(home / 'bin') + os.pathsep + os.environ.get('PATH', '')
            break
    if 'TORCH_CUDA_ARCH_LIST' not in os.environ:
        try:
            import torch
            major, minor = torch.cuda.get_device_capability()
            os.environ['TORCH_CUDA_ARCH_LIST'] = '%d.%d' % (major, minor)
        except Exception:
            pass


def import_pycolmap():
    """Import pycolmap-cuda12, first loading the CUDA 12 runtime from the venv's nvidia wheels: the
    extension finds libcudart.so.12 only via the system library path otherwise."""
    import ctypes
    for base in site.getsitepackages() + [site.getusersitepackages()]:
        for lib in ('cuda_runtime/lib/libcudart.so.12', 'curand/lib/libcurand.so.10'):
            p = Path(base) / 'nvidia' / lib
            if p.exists():
                try:
                    ctypes.CDLL(str(p), mode=ctypes.RTLD_GLOBAL)
                except OSError:
                    pass
    import pycolmap
    return pycolmap


def read_mkv_metadata(path):
    """Yield (index, packet, metadata dict) for each video packet of an APCG thermal mkv."""
    import av
    container = av.open(str(path))
    stream = container.streams.video[0]
    index = 0
    for packet in container.demux(stream):
        if packet.size == 0:
            continue
        data = bytes(packet.get_sidedata('matroska_block_additional'))
        yield index, packet, json.loads(data[8:])
        index += 1
    container.close()


def decode_frames(path, wanted):
    """Yield (index, metadata, uint16 HxW array) for frame indices in the set `wanted`."""
    import av
    container = av.open(str(path))
    stream = container.streams.video[0]
    index = 0
    last = max(wanted) if wanted else -1
    for packet in container.demux(stream):
        if packet.size == 0:
            continue
        if index in wanted:
            data = bytes(packet.get_sidedata('matroska_block_additional'))
            meta = json.loads(data[8:])
            for frame in packet.decode():
                yield index, meta, frame.to_ndarray(format='gray16le')
        index += 1
        if index > last:
            break
    container.close()


def rot_x(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])


def rot_y(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def rot_z(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])


class LocalFrame:
    """Flat-earth ENU frame about an origin; adequate over a few km."""

    def __init__(self, lat, lon, alt):
        self.lat, self.lon, self.alt = lat, lon, alt
        self.coslat = math.cos(math.radians(lat))

    def to_enu(self, lat, lon, alt):
        n = math.radians(lat - self.lat) * EARTH_RADIUS
        e = math.radians(lon - self.lon) * EARTH_RADIUS * self.coslat
        return np.array([e, n, alt - self.alt])

    def to_geodetic(self, enu):
        lat = self.lat + math.degrees(enu[1] / EARTH_RADIUS)
        lon = self.lon + math.degrees(enu[0] / (EARTH_RADIUS * self.coslat))
        return lat, lon, self.alt + enu[2]


# SRTM1 tiles from ArduPilot's terrain server, in MAVProxy's tile cache
SRTM_URL = 'https://terrain.ardupilot.org/SRTM1/'
SRTM_CACHE = Path(os.environ.get('SRTM_CACHE', Path.home() / '.tilecache' / 'SRTM1'))
_srtm_tiles = {}  # not thread-safe: the tools look terrain up from one thread


def _srtm_download(name, path):
    """Fetch a tile into a private temp file, then publish it atomically over path."""
    import shutil
    import tempfile
    import urllib.request
    SRTM_CACHE.mkdir(parents=True, exist_ok=True)
    fd, part = tempfile.mkstemp(prefix=name + '.', suffix='.part', dir=SRTM_CACHE)
    try:
        with os.fdopen(fd, 'wb') as f:
            with urllib.request.urlopen(SRTM_URL + name + '.zip', timeout=60) as r:
                shutil.copyfileobj(r, f)
            umask = os.umask(0)
            os.umask(umask)
            os.fchmod(f.fileno(), 0o644 & ~umask)  # mkstemp makes it 0600
        os.replace(part, path)
    except BaseException:
        os.unlink(part)
        raise


def _srtm_tile(lat_i, lon_i):
    """A tile's heights, or None if it can't be had; failures are cached so each tile is tried once a run."""
    import zipfile
    name = '%s%02d%s%03d.hgt' % ('N' if lat_i >= 0 else 'S', abs(lat_i), 'E' if lon_i >= 0 else 'W', abs(lon_i))
    if name not in _srtm_tiles:
        path = SRTM_CACHE / (name + '.zip')
        _srtm_tiles[name] = None
        try:
            for attempt in range(2):
                if attempt or not path.exists():
                    _srtm_download(name, path)  # replaces a corrupt cached zip
                try:
                    with zipfile.ZipFile(path) as z:
                        data = z.read(next(n for n in z.namelist() if n.endswith('.hgt')))
                    break
                except zipfile.BadZipFile:
                    if attempt:
                        raise
            n = math.isqrt(len(data) // 2)
            _srtm_tiles[name] = np.frombuffer(data, '>i2').reshape(n, n)
        except Exception as e:
            print('terrain height unavailable for %s: %s' % (name, e))
    return _srtm_tiles[name]


def terrain_cells_per_degree(lat, lon):
    """Grid cells per degree of the SRTM tile holding (lat, lon); None if it can't be had.
    Grid nodes are at whole multiples of 1/cells degrees of latitude and longitude."""
    a = _srtm_tile(math.floor(lat), math.floor(lon))
    return None if a is None else a.shape[0] - 1


def terrain_height(lat, lon):
    """SRTM ground height (m AMSL), bilinear; None if the tile can't be had or the point is a void."""
    lat_i, lon_i = math.floor(lat), math.floor(lon)
    a = _srtm_tile(lat_i, lon_i)
    if a is None:
        return None
    n = a.shape[0] - 1
    y, x = (lat_i + 1 - lat) * n, (lon - lon_i) * n  # row 0 is the north edge
    y0, x0 = min(int(y), n - 1), min(int(x), n - 1)
    fy, fx = y - y0, x - x0
    q = a[y0:y0 + 2, x0:x0 + 2].astype(float)
    if (q < -1000).any():
        return None
    return float(q[0, 0] * (1 - fx) * (1 - fy) + q[0, 1] * fx * (1 - fy) + q[1, 0] * (1 - fx) * fy + q[1, 1] * fx * fy)


def camera_rotation(telemetry):
    """World(ENU)-from-camera rotation from APCG telemetry (gimbal roll/pitch level, yaw vehicle-relative)."""
    g = telemetry.get('gimbal_attitude') or {}
    yaw = telemetry['vehicle_attitude']['yaw_rad'] + g.get('yaw_rad', 0.0)
    r_ned_body = rot_z(yaw) @ rot_y(g.get('pitch_rad', 0.0)) @ rot_x(g.get('roll_rad', 0.0))
    return R_ENU_NED @ r_ned_body @ R_BODY_CAM


def intrinsics(width, height, hfov_deg):
    f = (width / 2) / math.tan(math.radians(hfov_deg) / 2)
    return np.array([[f, 0, width / 2], [0, f, height / 2], [0, 0, 1.0]])


def triangulate_rays(centres, dirs, focal, iterations=5):
    """Robust least-squares intersection of rays; returns (point, median error in pixels)."""
    weights = np.ones(len(centres))
    for _ in range(iterations):
        A = np.zeros((3, 3))
        b = np.zeros(3)
        for w, c, d in zip(weights, centres, dirs):
            P = np.eye(3) - np.outer(d, d)
            A += w * P
            b += w * P @ c
        X = np.linalg.solve(A, b)
        err = np.array([np.linalg.norm((np.eye(3) - np.outer(d, d)) @ (X - c)) / np.linalg.norm(X - c)
                        for c, d in zip(centres, dirs)]) * focal
        scale = max(np.median(err), 5.0)
        weights = 1 / (1 + (err / (2 * scale)) ** 2)
    return X, float(np.median(err))


def raw_to_celsius(raw, meta):
    scale = meta.get('temperature_scale_k', 1 / 64)
    offset = meta.get('temperature_offset_k', 0)
    return raw.astype(np.float32) * scale + offset - 273.15


def upright(image, meta):
    """Rotate a frame by its display rotation so it is upright."""
    k = (meta.get('rotation_deg', 0) // 90) % 4
    return np.rot90(image, k) if k else image


def fatal(message):
    print(message, file=sys.stderr)
    sys.exit(1)
