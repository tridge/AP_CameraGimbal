#!/usr/bin/env python3
"""Check fixture timing and bounded work after large presentation-time jumps."""
from fractions import Fraction
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import av
import numpy as np
import terrain_video

terrain_video.av = av


class Fixtures(unittest.TestCase):
    def test_catchup(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'video.h264'
            with av.open(str(path), 'w', format='h264') as output:
                stream = output.add_stream('libx264', rate=20)
                stream.width, stream.height, stream.pix_fmt = 64, 48, 'yuv420p'
                stream.options = {'preset': 'ultrafast', 'tune': 'zerolatency'}
                for index in range(20):
                    pixels = np.full((48, 64, 3), 10 * index, dtype=np.uint8)
                    frame = av.VideoFrame.from_ndarray(pixels, format='rgb24')
                    frame.pts, frame.time_base = index, Fraction(1, 20)
                    for packet in stream.encode(frame):
                        output.mux(packet)
                for packet in stream.encode():
                    output.mux(packet)
            with av.open(str(path)) as source:
                expected = [f.to_ndarray(format='rgb24') for f in source.decode(video=0)]
            self.assertEqual(len(expected), 20)
            counts = {'decoded': 0, 'converted': 0}
            real_open = av.open

            class Frame:
                def __init__(self, frame):
                    self.frame = frame

                def to_ndarray(self, **kwargs):
                    counts['converted'] += 1
                    return self.frame.to_ndarray(**kwargs)

            class Container:
                def __init__(self, name):
                    self.source = real_open(name)
                    self.streams, self.format = self.source.streams, self.source.format

                def decode(self, **kwargs):
                    for frame in self.source.decode(**kwargs):
                        counts['decoded'] += 1
                        # Fail promptly if the old catch-up loop is restored.
                        if counts['decoded'] > 60:
                            raise AssertionError('Decoded more than three fixture loops for one frame')
                        yield Frame(frame)

                def close(self):
                    self.source.close()

            with patch.object(av, 'open', Container):
                # Cover large jumps before and after discovering the loop length.
                for times in ((3600.75, 3601.0, 7200.15), (0, .05, .95, 1, 3600.75, 3600.75, 7200.15)):
                    fixture = terrain_video.Fixture(str(path))
                    try:
                        self.assertEqual(fixture.rate, 20)
                        for seconds in times:
                            counts.update(decoded=0, converted=0)
                            actual = fixture.at(seconds)
                            np.testing.assert_array_equal(actual, expected[int(seconds * 20) % 20])
                            self.assertLessEqual(counts['converted'], 1)
                    finally:
                        fixture.close()


if __name__ == '__main__':
    unittest.main()
