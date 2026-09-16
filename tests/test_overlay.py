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


class Geometry(C.Structure):
    _fields_ = [('lines', Line * 8), ('count', C.c_uint), ('scale', C.c_uint)]


class Bitmap(C.Structure):
    _fields_ = [(n, C.c_uint) for n in ('x', 'y', 'width', 'height')] + [('pixels', C.POINTER(C.c_uint16))]


class Overlays(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='overlay-test-')
        path = Path(cls.tmp.name) / 'overlay.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror', '-std=c11',
                        '-I'+str(ROOT/'include'), '-I'+str(ROOT/'camera_app/include'),
                        '-DAPCAM_TARGET=APCAM_TARGET_MT11', str(ROOT/'camera_app/src/media/overlay.c'),
                        '-lm', '-o', str(path)], check=True)
        cls.lib = C.CDLL(str(path))
        cls.lib.ca_overlay_geometry.argtypes = [C.POINTER(Geometry), C.c_uint, C.c_uint, C.c_bool, C.c_bool, C.c_float]
        cls.lib.ca_overlay_bitmaps.argtypes = [C.POINTER(Bitmap), C.c_uint, C.c_uint, C.POINTER(Geometry)]
        cls.lib.ca_overlay_free.argtypes = [C.POINTER(Bitmap)]

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def geometry(self, w=1280, h=720, cross=True, box=True, fov=88):
        g = Geometry()
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
                    bitmaps = (Bitmap * 5)()
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


if __name__ == '__main__':
    unittest.main()
