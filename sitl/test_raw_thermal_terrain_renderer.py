#!/usr/bin/env python3
"""Offline terrain-renderer fixture for test_raw_thermal_stream.py --terrain."""
import hashlib
from unittest import mock

import terrain_video as video


def decode(z, x, y):
    w, s, e, n = video.terrain.GlobalGeodetic(True).TileBounds(x, y, z)
    return {'bbox': (w, s, e, n),
            'verts': video.np.array([[w, s, 580], [e, s, 580], [w, n, 600], [e, n, 600]]),
            'idx': video.np.array([[0, 1, 2], [1, 3, 2]])}


def texture(mt, lock, w, s, e, n, zoom, width, priority=0):
    image = video.np.zeros((1024, 1024, 3), dtype=video.np.uint8)
    image[:] = (30, 100, 40)
    image[::2, :] = (180, 130, 30)
    return image


class CheckedScene(video.Scene):
    def thermal_metadata(self, record):
        metadata = super().thermal_metadata(record)
        # Check the full IPC/FFV1 path against the actual renderer output,
        # independently of sequence numbers and the diagnostic pattern.
        raw = self.raw_thermal(record, self.pose is not None)
        metadata['test_sha256'] = hashlib.sha256(raw.astype('<u2').tobytes()).hexdigest()
        return metadata


if __name__ == '__main__':
    video.dependencies()
    with mock.patch.object(video.terrain, 'decode_terrain', decode), \
            mock.patch.object(video.terrain, 'build_texture_image', texture), \
            mock.patch.object(video, 'Scene', CheckedScene):
        video.main()
