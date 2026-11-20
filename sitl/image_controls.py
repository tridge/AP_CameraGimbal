"""Deterministic visual approximations of camera ISP and thermal controls.

These model control direction and interaction, not a particular sensor's
radiometry, exposure calibration or lens mechanics. All arrays use RGB order.
"""
from functools import lru_cache
import math
import struct

import cv2
import numpy as np


# Palette numbers are the public camera parameter values (1 is reserved).
# RGB anchor colours approximate the named palettes, not vendor lookup tables.
PALETTES = {
    0: [(0, 0, 0), (255, 255, 255)],
    2: [(0, 0, 0), (110, 65, 35), (255, 235, 190)],
    3: [(0, 0, 0), (65, 0, 110), (190, 20, 60), (255, 150, 0), (255, 255, 230)],
    4: [(0, 0, 100), (0, 180, 255), (0, 230, 70), (255, 255, 0), (255, 0, 0)],
    5: [(0, 0, 0), (0, 90, 30), (170, 255, 180)],
    6: [(0, 0, 60), (80, 0, 170), (0, 200, 150), (230, 255, 120)],
    7: [(0, 0, 0), (140, 0, 0), (255, 80, 0), (255, 255, 140)],
    8: [(0, 15, 0), (30, 110, 25), (170, 190, 45), (255, 255, 180)],
    9: [(0, 0, 0), (0, 0, 200), (0, 255, 255), (255, 255, 0), (255, 0, 0), (255, 255, 255)],
    10: [(255, 255, 255), (0, 0, 0)],
    11: [(0, 0, 0), (95, 95, 95), (190, 190, 190), (255, 50, 0), (255, 255, 0)],
}


class ImageControls:
    def __init__(self):
        self.luts = {}
        for palette, anchors in PALETTES.items():
            x = np.linspace(0, 255, len(anchors))
            self.luts[palette] = np.stack([np.interp(np.arange(256), x, np.array(anchors)[:, c])
                                          for c in range(3)], axis=1).astype(np.uint8).reshape(256, 1, 3)
        self.matrix_key = None
        self.exposure = None

    @staticmethod
    def synthetic_thermal(image):
        """Map rendered RGB luminance to 15..45 C, stored as Kelvin * 64.

        This is a deterministic visual simulation, not measured terrain
        temperature. Keep fractional luminance until uint16 quantisation.
        """
        luminance = image.astype(np.float32) @ np.array([.299, .587, .114], dtype=np.float32)
        return np.rint((273.15 + 15 + luminance * (30 / 255)) * 64).astype('<u2')

    def thermal_display(self, samples, settings):
        """Apply display gain and palette to a copy of native samples."""
        gray = np.rint(np.clip((samples.astype(np.float32) / 64 - 273.15 - 15) * (255 / 30),
                              0, 255)).astype(np.uint8)
        return self.thermal_palette(gray, settings)

    def thermal_palette(self, gray, settings):
        if settings.get('thermal_gain', 1) == 0:
            gray = cv2.convertScaleAbs(gray, alpha=0.5, beta=64)
        return cv2.applyColorMap(gray, self.luts.get(settings.get('thermal_palette', 0), self.luts[0]))

    def apply(self, image, settings, thermal=False):
        if thermal:
            gray = cv2.cvtColor(image, cv2.COLOR_RGB2GRAY)
            return self.thermal_palette(gray, settings)

        brightness = (settings.get('brightness', 50) - 50) * 2.55
        contrast = settings.get('contrast', 50) / 50
        saturation = settings.get('saturation', 50) / 50
        gain = 2 ** (settings.get('exposure', 0) / 3)
        iso = settings.get('iso', 0)
        shutter = settings.get('shutter', 0)
        gain *= 2 ** max(0, iso - 1)
        if shutter:
            gain *= 100 / (30, 50, 100, 250, 500, 750, 1000, 2000)[shutter - 1]
        gray = cv2.cvtColor(image[::8, ::8], cv2.COLOR_RGB2GRAY)
        metering = settings.get('metering', 0)
        if metering and (not iso or not shutter):
            # Match the selected region to the average-metered reference.
            # Subsampling bounds the work even with a 4K recording stream.
            h, w = gray.shape
            fraction = 0.5 if metering == 1 else 0.15
            half_h, half_w = max(1, round(h * fraction / 2)), max(1, round(w * fraction / 2))
            region = gray[max(0, h // 2 - half_h):h // 2 + half_h,
                          max(0, w // 2 - half_w):w // 2 + half_w]
            gain *= np.clip(gray.mean() / max(1, region.mean()), 0.25, 4)
        # Describe the actual visual model, not fictional hardware feedback.
        # There is no closed-loop AE/convergence model yet. Its effective sensor
        # shutter/gain and exposure-stage luminance are nevertheless measurable.
        sensor_gain = 2 ** max(0, iso - 1)
        shutter_us = 1000000 / (30, 50, 100, 250, 500, 750, 1000, 2000)[shutter - 1] if shutter else 10000
        isp_gain = gain / (sensor_gain * shutter_us / 10000)
        mode = (1 if shutter else 2) if iso else (3 if shutter else 0)
        self.exposure = (shutter_us, sensor_gain, 1.0, isp_gain,
                         float(np.clip(gray.astype(np.float32) * gain, 0, 255).mean()), math.nan, math.nan)
        self.exposure_mode = mode
        balance = ((1, 1, 1), (1.04, 1, .96), (1.15, 1, .85),
                   (1.08, .90, 1.12), (.78, 1, 1.25))[settings.get('white_balance', 0)]
        key = (brightness, contrast, saturation, gain, balance)
        if key != self.matrix_key:
            matrix = np.tile(np.array([.299, .587, .114]) * (1 - saturation), (3, 1))
            matrix += np.eye(3) * saturation
            matrix *= (gain * contrast * np.array(balance))[:, None]
            self.matrix = np.column_stack((matrix, np.full(3, 127.5 * (1 - contrast) + brightness))).astype(np.float32)
            self.matrix_key = key
        if key != (0, 1, 1, 1, (1, 1, 1)):
            image = cv2.transform(image, self.matrix)  # saturates, never folds negative pixels
        defocus = settings.get('defocus', 0)
        if defocus > 0:
            sigma = defocus * max(1, image.shape[1] / 240)
            h, w = image.shape[:2]
            reduction = max(1, int(sigma / 2))
            if reduction > 1:
                image = cv2.resize(image, (max(1, w // reduction), max(1, h // reduction)),
                                   interpolation=cv2.INTER_AREA)
                sigma /= reduction
            kernel = min(31, 2 * math.ceil(2 * sigma) + 1)
            image = cv2.GaussianBlur(image, (kernel, kernel), sigma)
            if reduction > 1:
                image = cv2.resize(image, (w, h), interpolation=cv2.INTER_LINEAR)
        return image

    def exposure_packet(self):
        """Little-endian ca_exposure IPC; C supplies sample time and lens ID."""
        valid = 0x9f if self.exposure is not None else 0
        values = self.exposure or (math.nan,) * 7
        return struct.pack('<QBBHBBi7f', 0, 0, 1, valid,
                           self.exposure_mode if valid else 255, 0, 0, *values)


@lru_cache(maxsize=16)
def _overlay_pixels(width, height, scale, lines):
    # Cache sparse pixels until geometry changes (e.g. zoom/resolution). Never
    # rasterize the dashed box in the per-frame Python loop.
    pixels = {}
    # Match hardware region stacking: cross first, then each box edge.
    groups = [[line for line in lines if not line[4]]]
    groups.extend([line] for line in lines if line[4])
    for group in groups:
        for color, radius in ((0, int(2 * scale + .5)), (255, int(scale + .5) - 1)):
            for x0, y0, x1, y1, dashed in group:
                n = max(abs(x1 - x0), abs(y1 - y0))
                for k in range(n + 1):
                    if dashed and k // int(12 * scale + .5) % 2:
                        continue
                    x = x0 + (int((x1 - x0) * k / n) if n else 0)
                    y = y0 + (int((y1 - y0) * k / n) if n else 0)
                    for row in range(max(0, y-radius), min(height, y+radius+1)):
                        for col in range(max(0, x-radius), min(width, x+radius+1)):
                            pixels[row*width+col] = color
    return np.array(list(pixels), dtype=np.intp), np.array(list(pixels.values()), dtype=np.uint8)


def apply_overlay(image, overlay):
    """Apply the hardware renderer's geometry after ISP effects, before encoding.

    The scene caches clean images for other streams and still captures; encoders
    consume their arrays on background threads, so never alter the source array.
    """
    lines = overlay.get('lines', [])
    if not lines:
        return image
    indices, values = _overlay_pixels(image.shape[1], image.shape[0], overlay['scale'],
                                      tuple(tuple(line) for line in lines))
    if not len(indices):
        return image
    image = image.copy()
    image.reshape(-1, 3)[indices] = values[:, None]
    return image
