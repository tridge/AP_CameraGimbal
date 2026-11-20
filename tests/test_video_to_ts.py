#!/usr/bin/env python3
"""MPEG-TS remux with synchronous ST 0601 KLV, in the camera recorder's layout."""
from fractions import Fraction
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import thermal_to_video as thermal
import video_to_ts

START_MS = 1730936828900.0  # 2024-11-06T23:47:08.900Z


def make_clip(path, codec, frames=12):
    import av
    import numpy as np
    with av.open(str(path), 'w') as container:
        stream = container.add_stream({'hevc': 'libx265', 'h264': 'libx264'}[codec], rate=30)
        stream.width, stream.height, stream.pix_fmt = 64, 48, 'yuv420p'
        stream.options = {'x265-params': 'log-level=none:keyint=5:bframes=0'} if codec == 'hevc' \
            else {'g': '5', 'bf': '0'}
        for n in range(frames):
            frame = av.VideoFrame.from_ndarray(np.full((48, 64, 3), n * 20, np.uint8), format='rgb24')
            frame.pts, frame.time_base = n, Fraction(1, 30)
            container.mux(stream.encode(frame))
        container.mux(stream.encode())


def transport(data):
    """PMT entries and PES packets per PID."""
    assert len(data) % 188 == 0
    types, descriptors, pes, continuity = {}, {}, {}, {}
    for at in range(0, len(data), 188):
        p = data[at:at+188]
        assert p[0] == 0x47
        pid, control = (p[1] & 31) << 8 | p[2], (p[3] >> 4) & 3
        off = 4 + (1 + p[4] if control & 2 else 0)
        if not control & 1:
            continue
        if pid in continuity:
            assert p[3] & 15 == (continuity[pid] + 1) & 15, pid
        continuity[pid] = p[3] & 15
        payload = p[off:]
        if pid in (0, video_to_ts.PMT_PID):
            section = payload[1:]
            size = ((section[1] & 15) << 8 | section[2]) + 3
            assert video_to_ts.crc32_mpeg(section[:size]) == 0
            if pid == video_to_ts.PMT_PID:
                i = 12
                while i < size - 4:
                    stream = (section[i+1] & 31) << 8 | section[i+2]
                    n = (section[i+3] & 15) << 8 | section[i+4]
                    types[stream], descriptors[stream] = section[i], section[i+5:i+5+n]
                    i += 5 + n
            continue
        if p[1] & 0x40:
            pes.setdefault(pid, []).append(bytearray())
        pes[pid][-1] += payload
    return types, descriptors, pes


def pes_pts(p):
    t = p[9:14]
    return ((t[0] >> 1) & 7) << 30 | (t[1] << 7 | t[2] >> 1) << 15 | (t[3] << 7 | t[4] >> 1)


class VideoToTsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        series = {
            'position': ([START_MS, START_MS + 1000],
                         {'lat_e7': [-353632610, -353632610], 'lon_e7': [1491652300, 1491652400],
                          'alt_amsl_m': [600.0, 610.0], 'alt_relative_m': [100.0, 110.0]}, set()),
            'vehicle_attitude': ([START_MS, START_MS + 1000],
                                 {'roll_rad': [0.0, 0.0], 'pitch_rad': [0.1, 0.1], 'yaw_rad': [1.0, 1.0]},
                                 {'yaw_rad'}),
            'gimbal_attitude': ([START_MS, START_MS + 1000],
                                {'roll_rad': [0.0, 0.0], 'pitch_rad': [-0.5, -0.5], 'yaw_rad': [0.2, 0.2]},
                                {'yaw_rad'}),
        }
        original = thermal.load_bin_telemetry
        thermal.load_bin_telemetry = lambda *args: thermal.BinTelemetry(series, 'flight.bin')
        self.addCleanup(setattr, thermal, 'load_bin_telemetry', original)

    def check(self, codec):
        import av
        clip = self.root/('clip-%s.mp4' % codec)
        make_clip(clip, codec)
        with av.open(str(clip)) as container:
            duration = float(container.streams.video[0].duration * container.streams.video[0].time_base)
        end = int((START_MS + duration * 1000) * 1e6)
        os.utime(clip, ns=(end, end))
        output = self.root/('clip-%s.ts' % codec)
        counts = video_to_ts.convert(clip, output, self.root/'flight.bin')
        self.assertEqual(counts['frames'], 12)
        self.assertEqual(counts['klv'], 12)
        types, descriptors, pes = transport(output.read_bytes())
        self.assertEqual(types, {0x101: 0x24 if codec == 'hevc' else 0x1b, 0x102: 0x15})
        self.assertIn(b'\x26\x09\x01\x00\xffKLVA\x00\x0f', descriptors[0x102])
        video = [pes_pts(p) for p in pes[0x101]]
        self.assertEqual(video, [round(n * 3000) for n in range(12)])
        for n, p in enumerate(pes[0x102]):
            self.assertEqual(p[3], 0xfc)
            self.assertEqual(pes_pts(p), video[n])
            cell = p[9+p[8]:]
            self.assertEqual((cell[0], cell[1], cell[2]), (0, n, 0xdf))
            klv = bytes(cell[5:5+int.from_bytes(cell[3:5], 'big')])
            self.assertEqual(klv[:16], thermal.UAS_LOCAL_SET_KEY)
            self.assertAlmostEqual(int.from_bytes(klv[19:27], 'big') / 1000, START_MS + n * 100 / 3, delta=1)
            self.assertIn(bytes((16, 2)), klv)
        with av.open(str(output)) as container:
            frames = list(container.decode(video=0))
        self.assertEqual(len(frames), 12)
        self.assertEqual(int(frames[-1].to_ndarray(format='rgb24').mean() / 20 + .5), 11)
        with self.assertRaises(ValueError):
            video_to_ts.convert(clip, output, self.root/'flight.bin')

    def test_hevc(self):
        self.check('hevc')

    def test_h264(self):
        self.check('h264')

    def test_start_time(self):
        clip = self.root/'clip.mp4'
        clip.write_bytes(b'')
        os.utime(clip, ns=(1730936158820000000, 1730936158820000000))
        start, source = video_to_ts.recording_start(clip, 1729.902)
        self.assertEqual(source, 'mtime - duration')
        self.assertAlmostEqual(start, 1730934428918.0, places=3)
        self.assertEqual(video_to_ts.recording_start(clip, 10, '2024-11-06T23:47:08.9Z', 1.5)[0],
                         START_MS + 1500)
        self.assertEqual(video_to_ts.parse_utc('2024-11-07T10:47:08.9+11:00'), START_MS)


if __name__ == '__main__':
    unittest.main()
