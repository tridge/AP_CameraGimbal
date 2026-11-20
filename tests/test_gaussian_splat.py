#!/usr/bin/env python3
"""GaussianSplat tools: terrain lookups, ray/terrain intersection and thermal colouring."""
import io
import math
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import urllib.error
import zipfile

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 'GaussianSplat'))
import splat_common  # noqa: E402
from splat_common import EARTH_RADIUS, LocalFrame  # noqa: E402

LAT0, LON0, HOME = -35.3, 149.1, 500.0


def sloped(slope):
    """Terrain rising `slope` m per m north of the origin, HOME at the origin."""
    def height(lat, lon):
        return HOME + slope * math.radians(lat - LAT0) * EARTH_RADIUS
    return height


N_CELLS = 3600  # SRTM1


def grid_terrain(node):
    """Bilinear terrain over SRTM1 grid nodes; node(i, j) is the height at lon i/N_CELLS, lat j/N_CELLS,
    NaN for a void. A cell with a void corner is None (NaN for arrays), as terrain_height."""
    def height(lat, lon):
        x, y = np.asarray(lon) * N_CELLS, np.asarray(lat) * N_CELLS
        i, j = np.floor(x), np.floor(y)
        fx, fy = x - i, y - j
        h = (node(i, j) * (1 - fx) * (1 - fy) + node(i + 1, j) * fx * (1 - fy)
             + node(i, j + 1) * (1 - fx) * fy + node(i + 1, j + 1) * fx * fy)
        if np.ndim(h):
            return h
        return None if math.isnan(h) else float(h)
    return height


def brute_first_hit(c, a, local, terrain, max_range=5000.0, step=0.01):
    """Ray distance to the first sample at or below the terrain."""
    t = np.arange(0, max_range, step)
    p = c[:, None] + a[:, None] * t
    lat = local.lat + np.degrees(p[1] / EARTH_RADIUS)
    lon = local.lon + np.degrees(p[0] / (EARTH_RADIUS * local.coslat))
    g = terrain(lat, lon)
    below = p[2] - (np.where(np.isnan(g), HOME, g) - HOME) <= 0  # voids are the takeoff-height plane
    return t[np.argmax(below)] if below.any() else None


class RayGroundHit(unittest.TestCase):
    def setUp(self):
        import extract_ts_frames
        self.m = extract_ts_frames
        self.local = LocalFrame(LAT0, LON0, 0.0)

    def hit(self, slope, a, c=(0.0, 0.0, 100.0)):
        a = np.array(a, float) / np.linalg.norm(a)
        c = np.array(c, float)
        with mock.patch.object(self.m, 'terrain_height', sloped(slope)), \
                mock.patch.object(self.m, 'terrain_cells_per_degree', lambda lat, lon: N_CELLS):
            return self.m.ray_ground_hit(c, a, self.local, HOME), c, a

    def check(self, slope, a):
        hit, c, a = self.hit(slope, a)
        # analytic: c_z + a_z t = slope (c_n + a_n t)
        t = (slope * c[1] - c[2]) / (a[2] - slope * a[1])
        self.assertIsNotNone(hit)
        self.assertGreater(t, 0)
        np.testing.assert_allclose(hit, c + a * t, atol=0.01)

    def test_flat(self):
        self.check(0.0, (0, 1, -0.25))
        self.check(0.0, (0.1, 0.1, -1))

    def test_shallow_up_slope(self):
        self.check(0.15, (0, 1, -0.25))
        # three fixed-point steps put this one behind and above the camera
        self.check(0.3, (0, 1, -0.25))

    def test_shallow_down_slope(self):
        # nearly parallel to the falling ground: meets it about 2 km out
        self.check(-0.2, (0, 1, -0.25))

    def test_steep(self):
        self.check(0.3, (0, 0.2, -1))
        self.check(-0.3, (0, -0.2, -1))

    def test_no_crossing(self):
        # ground falls away faster than the ray: no hit in front of the camera
        hit, _, _ = self.hit(-0.3, (0, 1, -0.25))
        self.assertIsNone(hit)

    def grid_hit(self, terrain, c, a):
        with mock.patch.object(self.m, 'terrain_height', terrain), \
                mock.patch.object(self.m, 'terrain_cells_per_degree', lambda lat, lon: N_CELLS):
            return self.m.ray_ground_hit(c, a, self.local, HOME)

    def test_thin_ridge(self):
        # one row of nodes 82 m up, slopes ~2.7 either side: the ray passes 0.5 m under the crest,
        # inside the ridge for under half a metre, then meets the flat ground far beyond
        jc = math.floor(LAT0 * N_CELLS) + 10
        terrain = grid_terrain(lambda i, j: HOME + 82.0 * (j == jc))
        north = math.radians(jc / N_CELLS - LAT0) * EARTH_RADIUS
        c = np.array([0.0, 0.0, 100.0])
        a = np.array([0.0, north, 81.5 - 100.0])
        a /= np.linalg.norm(a)
        hit = self.grid_hit(terrain, c, a)
        self.assertIsNotNone(hit)
        self.assertLess(abs(hit[1] - north), 1.0)
        self.assertAlmostEqual(np.linalg.norm(hit - c), brute_first_hit(c, a, self.local, terrain), delta=0.02)

    def test_rough_terrain(self):
        # crossings inside cells (a bilinear cell is quadratic along a diagonal) match a dense march
        def node(i, j):
            return HOME + 60.0 * (np.sin(i * 12.9898 + j * 78.233) * 43758.5453 % 1.0)
        terrain = grid_terrain(node)
        rng = np.random.default_rng(3)
        c = np.array([0.0, 0.0, 100.0])
        for _ in range(25):
            az, el = rng.uniform(0, 2 * math.pi), rng.uniform(0.2, 1.4)
            a = np.array([math.cos(el) * math.sin(az), math.cos(el) * math.cos(az), -math.sin(el)])
            hit = self.grid_hit(terrain, c, a)
            ref = brute_first_hit(c, a, self.local, terrain, max_range=1000.0)
            self.assertIsNotNone(hit)
            self.assertAlmostEqual(np.linalg.norm(hit - c), ref, delta=0.02)
            self.assertAlmostEqual(hit[2], terrain(*self.local.to_geodetic(hit)[:2]) - HOME, delta=1e-6)

    def test_void_edges(self):
        # a 40 m plateau with voids: each void cell is the takeoff-height plane, and no quadratic
        # mixes plateau and fallback samples
        def node(i, j):
            void = (np.sin(i * 3.1 + j * 7.7) * 1000.0 % 1.0) < 0.3
            return np.where(void, np.nan, HOME + 40.0 + 0.0 * i)
        terrain = grid_terrain(node)
        rng = np.random.default_rng(5)
        c = np.array([0.0, 0.0, 100.0])
        for _ in range(25):
            az, el = rng.uniform(0, 2 * math.pi), rng.uniform(0.1, 1.4)
            a = np.array([math.cos(el) * math.sin(az), math.cos(el) * math.cos(az), -math.sin(el)])
            hit = self.grid_hit(terrain, c, a)
            ref = brute_first_hit(c, a, self.local, terrain, max_range=1500.0)
            self.assertIsNotNone(hit)
            self.assertAlmostEqual(np.linalg.norm(hit - c), ref, delta=0.02)
            g = terrain(*self.local.to_geodetic(hit - a * 1e-6)[:2])
            # on the plateau, on the fallback plane, or at the plateau's wall at a void edge
            self.assertTrue(abs(hit[2] - 40.0) < 1e-6 or abs(hit[2]) < 1e-6 or g is None, hit)

    def test_missing_start_tile(self):
        # the camera's tile is unavailable, the one it looks into has terrain 20 m up
        local = LocalFrame(-35.0005, LON0, 0.0)

        def terrain(lat, lon):
            return None if lat < -35.0 else HOME + 20.0
        c = np.array([0.0, 0.0, 100.0])
        a = np.array([0.0, 1.0, -0.5])
        a /= np.linalg.norm(a)
        with mock.patch.object(self.m, 'terrain_height', terrain), \
                mock.patch.object(self.m, 'terrain_cells_per_degree',
                                  lambda lat, lon: None if lat < -35.0 else N_CELLS):
            hit = self.m.ray_ground_hit(c, a, local, HOME)
        np.testing.assert_allclose(hit, [0, 160, 20], atol=1e-6)

    def test_no_terrain(self):
        c = np.array([0.0, 0.0, 100.0])
        a = np.array([0.0, 0.6, -0.8])
        np.testing.assert_allclose(self.m.ray_ground_hit(c, a, self.local, None), [0, 75, 0], atol=1e-9)

    def test_camera_below_ground(self):
        hit, _, _ = self.hit(0.0, (0, 0, -1), c=(0.0, 0.0, -5.0))
        self.assertIsNone(hit)


class TakeoffReference(unittest.TestCase):
    """extract_ts_frames: KLV has no relative altitude, so takeoff is the first record."""
    OFFSET = 8.0  # GPS altitude above SRTM on the ground

    def setUp(self):
        import extract_ts_frames
        self.m = extract_ts_frames

    def ref(self, heights, terrain=None, use_terrain=True, takeoff_alt=None, lookup=None):
        terrain = terrain or sloped(0.0)
        recs = [dict(lat=LAT0 + 0.001 * k, lon=LON0, alt=terrain(LAT0 + 0.001 * k, LON0) + self.OFFSET + h)
                for k, h in enumerate(heights)]
        with mock.patch.object(self.m, 'terrain_height', lookup or terrain), mock.patch('builtins.print') as out:
            return self.m.takeoff_reference(recs, use_terrain, takeoff_alt), recs, out

    def test_ground_start(self):
        (alt, srtm), recs, out = self.ref([0, 100, 120, 0.5])
        self.assertEqual((alt, srtm), (recs[0]['alt'], HOME))
        out.assert_not_called()

    def test_hilltop_start(self):
        # takes off on a slope and lands 50 m lower, with SRTM 10 m high at the takeoff: still the start
        hill = sloped(-0.45)

        def terrain(lat, lon):
            return hill(lat, lon) + (10.0 if lat == LAT0 else 0.0)
        (alt, srtm), recs, out = self.ref([0, 100, 120, 0], terrain=terrain)
        self.assertEqual((alt, srtm), (recs[0]['alt'], HOME + 10.0))
        out.assert_not_called()

    def test_low_flight(self):
        # flown at 20 m throughout: the first record, no warning
        (alt, srtm), recs, out = self.ref([20, 22, 21])
        self.assertEqual((alt, srtm), (recs[0]['alt'], HOME))
        out.assert_not_called()

    def test_airborne_start_warns(self):
        (alt, srtm), recs, out = self.ref([100, 120, 0])
        self.assertEqual((alt, srtm), (recs[0]['alt'], HOME))
        self.assertIn('--takeoff-alt', out.call_args[0][0])

    def test_takeoff_alt(self):
        (alt, srtm), _, _ = self.ref([100, 120, 0], takeoff_alt=512.5)
        self.assertEqual((alt, srtm), (512.5, 512.5))
        (alt, srtm), _, _ = self.ref([100, 120, 0], use_terrain=False, takeoff_alt=512.5)
        self.assertEqual((alt, srtm), (512.5, None))

    def test_no_terrain(self):
        (alt, srtm), recs, _ = self.ref([80, 100, 2], use_terrain=False)
        self.assertEqual((alt, srtm), (recs[0]['alt'], None))

    def test_start_tile_unavailable(self):
        (alt, srtm), recs, _ = self.ref([0, 100], lookup=lambda lat, lon: None)
        self.assertEqual((alt, srtm), (recs[0]['alt'], recs[0]['alt']))


class SrtmReference(unittest.TestCase):
    def setUp(self):
        import extract_frames
        self.m = extract_frames

    def pos(self, rel, lat=LAT0):
        return dict(lat_e7=int(lat * 1e7), lon_e7=int(LON0 * 1e7), alt_amsl_m=600.0 + rel, alt_relative_m=rel)

    def test_on_ground(self):
        with mock.patch.object(self.m, 'terrain_height', sloped(0.1)):
            ref = self.m.srtm_reference([self.pos(80), self.pos(0.4, LAT0 + 0.001)], 600.0)
        self.assertAlmostEqual(ref, sloped(0.1)(LAT0 + 0.001, LON0), places=3)

    def test_airborne_start(self):
        # recording starts at 80 m: the terrain under it is not takeoff height
        with mock.patch.object(self.m, 'terrain_height', sloped(0.1)), mock.patch('builtins.print'):
            ref = self.m.srtm_reference([self.pos(80, LAT0 + 0.01), self.pos(120)], 600.0)
        self.assertEqual(ref, 600.0)


def hgt_zip(values):
    b = io.BytesIO()
    with zipfile.ZipFile(b, 'w') as z:
        z.writestr('S36E149.hgt', np.array(values, '>i2').tobytes())
    return b.getvalue()


class FailingRead(io.BytesIO):
    def read(self, *args):
        raise OSError('connection reset')


class SrtmTile(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.patches = [mock.patch.object(splat_common, 'SRTM_CACHE', Path(self.tmp.name)),
                        mock.patch.dict(splat_common._srtm_tiles, clear=True),
                        mock.patch('builtins.print')]
        for p in self.patches:
            p.start()
        self.zip = Path(self.tmp.name) / 'S36E149.hgt.zip'

    def tearDown(self):
        for p in self.patches:
            p.stop()
        self.tmp.cleanup()

    def leftovers(self):
        return [p.name for p in Path(self.tmp.name).iterdir() if p.name.endswith('.part')]

    def test_failure_cached(self):
        fake = mock.Mock(side_effect=urllib.error.URLError('offline'))
        with mock.patch('urllib.request.urlopen', fake):
            self.assertIsNone(splat_common.terrain_height(-35.5, 149.5))
            self.assertIsNone(splat_common.terrain_height(-35.6, 149.6))
        self.assertEqual(fake.call_count, 1)

    def test_bad_zip_refetched(self):
        self.zip.write_bytes(b'truncated')
        fake = mock.Mock(side_effect=lambda *a, **k: io.BytesIO(hgt_zip([[10, 10, 10]] * 3)))
        with mock.patch('urllib.request.urlopen', fake):
            self.assertAlmostEqual(splat_common.terrain_height(-35.5, 149.5), 10.0)
        self.assertEqual(fake.call_count, 1)
        self.assertTrue(zipfile.is_zipfile(self.zip))
        self.assertEqual(self.leftovers(), [])

    def test_bad_zip_replaced_not_unlinked(self):
        # another process may have just published a good tile there: replace, never unlink
        self.zip.write_bytes(b'truncated')
        fake = mock.Mock(side_effect=lambda *a, **k: io.BytesIO(hgt_zip([[10, 10], [10, 10]])))
        real_unlink = os.unlink
        unlinked = []

        def unlink(p, *a, **k):
            unlinked.append(Path(p))
            return real_unlink(p, *a, **k)
        with mock.patch('urllib.request.urlopen', fake), mock.patch('os.unlink', unlink), \
                mock.patch.object(Path, 'unlink', lambda p, *a, **k: unlink(p)):
            self.assertAlmostEqual(splat_common.terrain_height(-35.5, 149.5), 10.0)
        self.assertNotIn(self.zip, unlinked)

    def test_failed_connect_closes_fd(self):
        fds = len(os.listdir('/proc/self/fd'))
        fake = mock.Mock(side_effect=urllib.error.URLError('offline'))
        with mock.patch('urllib.request.urlopen', fake):
            for _ in range(5):
                with self.assertRaises(urllib.error.URLError):
                    splat_common._srtm_download('S36E149.hgt', self.zip)
        self.assertEqual(len(os.listdir('/proc/self/fd')), fds)
        self.assertEqual(self.leftovers(), [])

    def test_published_mode(self):
        fake = mock.Mock(side_effect=lambda *a, **k: io.BytesIO(hgt_zip([[1, 2], [3, 4]])))
        umask = os.umask(0o022)
        try:
            with mock.patch('urllib.request.urlopen', fake):
                splat_common._srtm_download('S36E149.hgt', self.zip)
        finally:
            os.umask(umask)
        self.assertEqual(self.zip.stat().st_mode & 0o777, 0o644)

    def test_bad_zip_twice(self):
        self.zip.write_bytes(b'truncated')
        fake = mock.Mock(side_effect=lambda *a, **k: io.BytesIO(b'still not a zip'))
        with mock.patch('urllib.request.urlopen', fake):
            self.assertIsNone(splat_common.terrain_height(-35.5, 149.5))
            self.assertIsNone(splat_common.terrain_height(-35.5, 149.5))
        self.assertEqual(fake.call_count, 1)

    def test_failed_transfer_not_published(self):
        fake = mock.Mock(side_effect=lambda *a, **k: FailingRead())
        with mock.patch('urllib.request.urlopen', fake):
            self.assertIsNone(splat_common.terrain_height(-35.5, 149.5))
        self.assertFalse(self.zip.exists())
        self.assertEqual(self.leftovers(), [])

    def test_private_part_files(self):
        names = []
        real = tempfile.mkstemp

        def mkstemp(*a, **k):
            fd, name = real(*a, **k)
            names.append(name)
            return fd, name
        fake = mock.Mock(side_effect=lambda *a, **k: io.BytesIO(hgt_zip([[1, 2], [3, 4]])))
        with mock.patch('urllib.request.urlopen', fake), mock.patch('tempfile.mkstemp', mkstemp):
            splat_common._srtm_download('S36E149.hgt', self.zip)
            splat_common._srtm_download('S36E149.hgt', self.zip)
        self.assertEqual(len(set(names)), 2)
        self.assertTrue(all(Path(n).parent == Path(self.tmp.name) for n in names))
        self.assertTrue(zipfile.is_zipfile(self.zip))


class ThermalColours(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.environ.setdefault('TORCH_CUDA_ARCH_LIST', '8.0')  # keep the import off the GPU
        try:
            import torch
            import view_splat
        except ImportError as e:
            raise unittest.SkipTest('needs torch: %s' % e)
        from colormaps import colormap_lut
        cls.torch, cls.v = torch, view_splat
        cls.lut = torch.tensor(colormap_lut('gloryhot'), dtype=torch.float32)

    def rgb(self, temps, *args, **kw):
        return self.v.thermal_rgb(self.torch.tensor(temps), self.lut, *args, **kw)

    def grey(self, level):
        return [level * 200 / 255] * 3

    def test_auto_grey_below_knee(self):
        lo, hi, bg = 10.0, 60.0, 30.0
        rgb = self.rgb([10.0, 20.0, 30.0, 60.0], lo, hi, bg, True)
        np.testing.assert_allclose(rgb[0], self.grey(0), atol=1e-6)
        np.testing.assert_allclose(rgb[1], self.grey(0.5), atol=1e-6)
        np.testing.assert_allclose(rgb[2], self.grey(1), atol=1e-6)
        np.testing.assert_allclose(rgb[3], self.lut[255])

    def test_auto_colour_on_log_curve(self):
        lo, hi, bg = 10.0, 60.0, 30.0
        t = 40.0
        x = math.log1p((t - bg) / 8) / math.log1p((hi - bg) / 8)
        rgb = self.rgb([t], lo, hi, bg, True)
        np.testing.assert_allclose(rgb[0], self.lut[int(x * 255)])

    def test_manual_grey_below_lo(self):
        rgb = self.rgb([0.0, 5.0, 10.0, 35.0], 10.0, 60.0, grey=True, gmin=0.0)
        np.testing.assert_allclose(rgb[0], self.grey(0), atol=1e-6)
        np.testing.assert_allclose(rgb[1], self.grey(0.5), atol=1e-6)
        np.testing.assert_allclose(rgb[2], self.grey(1), atol=1e-6)
        np.testing.assert_allclose(rgb[3], self.lut[127])

    def test_manual_default_floor(self):
        # no scene low below lo: grey over the 10 C below it
        rgb = self.rgb([5.0], 10.0, 60.0, grey=True, gmin=20.0)
        np.testing.assert_allclose(rgb[0], self.grey(0.5), atol=1e-6)

    def test_no_grey(self):
        lo, hi, bg = 10.0, 60.0, 30.0
        rgb = self.rgb([20.0, 60.0], lo, hi, bg)
        np.testing.assert_allclose(rgb[0], self.lut[int(0.35 * 255)])
        np.testing.assert_allclose(rgb[1], self.lut[255])
        rgb = self.rgb([35.0], lo, hi)
        np.testing.assert_allclose(rgb[0], self.lut[127])


if __name__ == '__main__':
    unittest.main()
