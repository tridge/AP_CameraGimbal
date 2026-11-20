#!/usr/bin/env python3
"""Check pinhole geometry, bounded regions and identical hardware/SITL pixels."""
import ctypes as C
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'sitl'))
from image_controls import apply_overlay  # noqa: E402
from terrain_video import set_camera_fov  # noqa: E402


class Line(C.Structure):
    _fields_ = [(n, C.c_int) for n in ('x0', 'y0', 'x1', 'y1')] + [('region', C.c_uint), ('dashed', C.c_bool)]


class Bitmap(C.Structure):
    _fields_ = [(n, C.c_uint) for n in ('x', 'y', 'width', 'height')] + [('pixels', C.POINTER(C.c_uint16))]


class Overlays(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='overlay-test-')
        cls.addClassCleanup(cls.tmp.cleanup)
        path = Path(cls.tmp.name) / 'overlay.so'
        # Query the native capacities before allocating anything passed to C.
        # Tracking added lines/regions; stale ctypes arrays corrupt Python's heap.
        layout_source = path.with_suffix('.cpp')
        layout_source.write_text('''#include "camera_app/overlay.h"
extern "C" void overlay_test_layout(unsigned out[5]) {
    out[0] = CA_OVERLAY_LINES;
    out[1] = CA_OVERLAY_REGIONS;
    out[2] = sizeof(struct ca_overlay_geometry);
    out[3] = sizeof(struct ca_overlay_line);
    out[4] = sizeof(struct ca_overlay_bitmap);
}
''')
        subprocess.run(['c++', '-std=gnu++17', '-Wno-missing-field-initializers', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror', '-std=gnu++17',
                        '-I'+str(ROOT/'include'), '-I'+str(ROOT/'camera_app/include'),
                        '-DAPCAM_TARGET='+getattr(cls, 'target', 'APCAM_TARGET_MT11'), str(ROOT/'camera_app/src/media/overlay.cpp'),
                        str(layout_source), '-lm', '-o', str(path)], check=True)
        cls.lib = C.CDLL(str(path))
        cls.lib.overlay_test_layout.argtypes = [C.POINTER(C.c_uint)]
        cls.lib.overlay_test_layout.restype = None
        layout = (C.c_uint * 5)()
        cls.lib.overlay_test_layout(layout)
        class Geometry(C.Structure):
            _fields_ = [('lines', Line * layout[0]), ('count', C.c_uint), ('scale', C.c_float)]
        assert list(layout)[2:] == [C.sizeof(Geometry), C.sizeof(Line), C.sizeof(Bitmap)], 'ctypes overlay ABI mismatch'
        cls.Geometry = Geometry
        cls.Bitmaps = Bitmap * layout[1]
        cls.lib.ca_overlay_geometry.argtypes = [C.POINTER(Geometry), C.c_uint, C.c_uint, C.c_bool, C.c_bool, C.c_float]
        cls.lib.ca_overlay_geometry.restype = None
        cls.lib.ca_overlay_geometry_fov.argtypes = [C.POINTER(Geometry), C.c_uint, C.c_uint, C.c_bool, C.c_bool, C.c_float, C.c_float, C.c_float]
        cls.lib.ca_overlay_geometry_fov.restype = None
        cls.lib.ca_overlay_tracking.argtypes = [C.POINTER(Geometry), C.c_uint, C.c_uint, C.POINTER(C.c_float)]
        cls.lib.ca_overlay_tracking.restype = None
        cls.lib.ca_overlay_bitmaps.argtypes = [C.POINTER(Bitmap), C.c_uint, C.c_uint, C.POINTER(Geometry)]
        cls.lib.ca_overlay_free.argtypes = [C.POINTER(Bitmap)]
        cls.lib.ca_overlay_free.restype = None

    def geometry(self, w=1280, h=720, cross=True, box=True, fov=88):
        g = self.Geometry()
        self.lib.ca_overlay_geometry(C.byref(g), w, h, cross, box, fov)
        return g

    def test_pinhole_and_clipping(self):
        g = self.geometry(cross=False)
        self.assertEqual(g.count, 4)
        top, bottom = g.lines[:2]
        self.assertAlmostEqual((top.x1-top.x0)/(bottom.y0-top.y0), 640/512, delta=.01)
        narrow = self.geometry(cross=False, fov=10)
        self.assertEqual(narrow.count, 0)
        for invalid in (float('nan'), 0, -1, 180):
            self.assertEqual(self.geometry(cross=False, fov=invalid).count, 0)
        self.assertEqual(self.geometry(cross=False, box=False).count, 0)

    def test_cross_proportions(self):
        for width, height in ((1280, 720), (1920, 1080), (2560, 1440), (3840, 2160)):
            g = self.geometry(width, height, box=False)
            self.assertEqual(g.count, 4)
            self.assertAlmostEqual(g.scale, height / 720, places=5)
            for line in g.lines[:g.count]:
                for coordinate, centre, fraction in (
                        (line.x0, width // 2, 5 / 720),
                        (line.y0, height // 2, 5 / 720),
                        (line.x1, width // 2, 20 / 720),
                        (line.y1, height // 2, 20 / 720)):
                    self.assertAlmostEqual(abs(coordinate - centre), height * fraction, delta=.5)

    def test_terrain_thermal_projection(self):
        try:
            import vtk
        except ImportError:
            self.skipTest('VTK is optional for the basic SITL video tests')
        camera = vtk.vtkCamera()
        camera.SetPosition(0, 0, 0)
        camera.SetFocalPoint(0, 0, -1)
        camera.SetClippingRange(.1, 1000)
        set_camera_fov(camera, 24.2, 16/9, 640/512)
        matrix = camera.GetCompositeProjectionTransformMatrix(16/9, -1, 1)
        half_w = np.tan(np.radians(24.2)/2)*10
        edge = matrix.MultiplyPoint((half_w, half_w*512/640, -10, 1))
        np.testing.assert_allclose(np.array(edge[:2])/edge[3], [1, 1], atol=1e-6)
        # Switching back to RGB restores its ordinary output aspect.
        set_camera_fov(camera, 88, 16/9)
        self.assertFalse(camera.GetUseExplicitAspectRatio())
        matrix = camera.GetCompositeProjectionTransformMatrix(16/9, -1, 1)
        edge = matrix.MultiplyPoint((np.tan(np.radians(44))*10, 0, -10, 1))
        self.assertAlmostEqual(edge[0]/edge[3], 1)

    def test_hardware_and_sitl_pixels(self):
        for w, h in ((1280, 720), (1920, 1080), (3840, 2160), (1280, 1024)):
            for fov in (88, 45, 31.3613561, 24.2, 10):
                with self.subTest(size=(w, h), fov=fov):
                    g = self.geometry(w, h, fov=fov)
                    source = np.full((h, w, 3), 110, dtype=np.uint8)
                    expected = source.copy()
                    bitmaps = self.Bitmaps()
                    self.assertEqual(self.lib.ca_overlay_bitmaps(bitmaps, w, h, C.byref(g)), 0)
                    try:
                        total = 0
                        for b in bitmaps:
                            if not b.pixels:
                                continue
                            self.assertLessEqual(b.x+b.width, w)
                            self.assertLessEqual(b.y+b.height, h)
                            bits = np.ctypeslib.as_array(b.pixels, shape=(b.width*b.height,)).reshape(b.height, b.width)
                            opaque = bits & 0x8000 != 0
                            view = expected[b.y:b.y+b.height, b.x:b.x+b.width]
                            view[opaque] = np.where(bits[opaque] & 0x7fff, 255, 0)[:, None]
                            total += b.width*b.height*2
                        self.assertLess(total, w*h//3, 'Must use strips, not a full frame bitmap')
                        overlay = {'scale': g.scale, 'lines': [[line.x0, line.y0, line.x1, line.y1, line.dashed] for line in g.lines[:g.count]]}
                        actual = apply_overlay(source, overlay)
                        np.testing.assert_array_equal(actual, expected)
                        self.assertTrue(np.all(source == 110), 'Do not corrupt cached images or other streams')
                        self.assertTrue(np.all(actual[h//2, w//2] == 110), 'Cross has an open centre')
                    finally:
                        self.lib.ca_overlay_free(bitmaps)

    def test_tracking_regions(self):
        g = self.geometry()
        self.lib.ca_overlay_tracking(C.byref(g), 1280, 720, (C.c_float * 4)(.1, .1, .3, .3))
        self.assertEqual(g.count, 12)
        self.assertEqual([line.region for line in g.lines[8:12]], [5, 6, 7, 8])
        bitmaps = self.Bitmaps()
        self.assertEqual(self.lib.ca_overlay_bitmaps(bitmaps, 1280, 720, C.byref(g)), 0)
        try:
            self.assertTrue(all(b.pixels for b in bitmaps))
            for b in bitmaps[5:9]:
                self.assertLessEqual(b.x+b.width, 1280)
                self.assertLessEqual(b.y+b.height, 720)
                pixels = np.ctypeslib.as_array(b.pixels, shape=(b.width*b.height,))
                self.assertIn(0x821f, pixels)
        finally:
            self.lib.ca_overlay_free(bitmaps)
        self.assertTrue(all(not b.pixels for b in bitmaps))


class ZT30ThermalFOV(unittest.TestCase):
    target = 'APCAM_TARGET_ZT30'
    setUpClass = classmethod(Overlays.setUpClass.__func__)
    geometry = Overlays.geometry

    def test_assumed_mt11_thermal_box(self):
        # ZT30 uses the requested MT11 24.2 degree thermal FOV and 640x512
        # aspect. Its boundary is visible in both full-width RGB lens views.
        for fov in (84.5, 58.1):
            g = self.geometry(cross=False, fov=fov)
            self.assertEqual(g.count, 4)
            top, bottom = g.lines[:2]
            expected_width = 1280*np.tan(np.radians(24.2)/2)/np.tan(np.radians(fov)/2)
            self.assertAlmostEqual(top.x1-top.x0, expected_width, delta=1)
            self.assertAlmostEqual((top.x1-top.x0)/(bottom.y0-top.y0), 640/512, delta=.01)
            self.assertEqual(self.geometry(cross=True, fov=fov).count, 8)
        # At full optical zoom the thermal boundary surrounds the RGB image.
        self.assertEqual(self.geometry(cross=False, fov=2.14).count, 0)
        self.assertEqual(self.geometry(cross=True, fov=2.14).count, 4)

    def test_unknown_thermal_box_is_absent(self):
        # Keep explicit unknown/invalid geometry coverage for native callers.
        for thermal_fov in (float('nan'), 0, -1, 180):
            for cross in (False, True):
                g = self.Geometry()
                self.lib.ca_overlay_geometry_fov(C.byref(g), 1280, 720, cross, True, 84.5, thermal_fov, 640/512)
                self.assertEqual(g.count, 4 if cross else 0)


if __name__ == '__main__':
    unittest.main()
