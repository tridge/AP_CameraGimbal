#!/usr/bin/env python3
"""Deterministic visual target projected from actual SITL gimbal feedback.

This uses the production renderer transport and encoders. The camera still
tracks pixels with dlib; no target coordinates are sent to camera-app.
"""
import math
import os
from types import SimpleNamespace
import cv2
import numpy as np
import terrain_video


class TrackingScene:
    def __init__(self, sizes):
        self.sizes = sizes
        self.controls = terrain_video.ImageControls()
        self.manager = None
        self.predictor = terrain_video.PosePredictor()
        self.tiles = SimpleNamespace(tiles_pending=lambda: 0)
        self.target_yaw = math.radians(float(os.environ.get('TRACKING_TEST_YAW', '10')))
        self.target_pitch = math.radians(float(os.environ.get('TRACKING_TEST_PITCH', '-15')))
        rng = np.random.default_rng(42)
        texture = rng.integers(30, 240, (32, 32), dtype=np.uint8)
        self.texture = cv2.cvtColor(cv2.resize(texture, (128, 128), interpolation=cv2.INTER_NEAREST), cv2.COLOR_GRAY2RGB)

    def update(self, record):
        sample = record.get('gimbal_attitude') or dict(roll_rad=0, yaw_rad=0, pitch_rad=math.radians(-20))
        # The shared renderer timestamps frames for their scheduled display,
        # so project from that pose, just as the terrain renderer does.
        lead = min(250, max(0, record.get('prediction_ms', 0)))
        sample = dict(sample, age_ms=sample.get('age_ms', 0)+lead)
        angles = self.predictor.angles('gimbal', sample, record['pts90k']/90000, .25+lead/1000)
        self.pose = dict(zip(('roll_rad', 'pitch_rad', 'yaw_rad'), angles))
        return True

    def render(self, stream, record, valid):
        w, h = self.sizes[stream]
        image = np.full((h, w, 3), 45, dtype=np.uint8)
        scale = 2 * math.tan(math.radians(record['fov'][stream]) / 2)
        yaw = self.pose['yaw_rad']
        pitch = self.pose['pitch_rad']
        cx = round(w * (.5 + math.tan(self.target_yaw - yaw) / scale))
        aspect = record['thermal_aspect'] if record['thermal'][stream] else w/h
        cy = round(h * (.5 - aspect * math.tan(self.target_pitch - pitch) / scale))
        size = max(16, round(w * .13))
        target_height = max(16, round(h*.13*aspect))
        left, top = cx-size//2, cy-target_height//2
        right, bottom = left+size, top+target_height
        x0, y0, x1, y1 = max(0, left), max(0, top), min(w, right), min(h, bottom)
        if x1 > x0 and y1 > y0:
            texture = cv2.resize(self.texture, (size, target_height), interpolation=cv2.INTER_LINEAR)
            image[y0:y1, x0:x1] = texture[y0-top:y1-top, x0-left:x1-left]
        return self.controls.apply(image, record.get('image', {}), record['thermal'][stream])

    def close(self):
        pass


if __name__ == '__main__':
    terrain_video.FixtureScene = TrackingScene
    terrain_video.main()
