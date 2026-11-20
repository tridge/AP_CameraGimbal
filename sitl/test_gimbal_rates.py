#!/usr/bin/env python3
"""Regression checks for measured MT11 rate response and BIN curve extraction."""
import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

from gimbal_sim import Gimbal, siyi_frame
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_gimbal_rates import samples
from mt11_rate_sweep import Vendor


class RateResponse(unittest.TestCase):
    def test_mt11_angle_slew_uses_full_hardware_speed(self):
        now = [0.0]
        with patch('gimbal_sim.time.monotonic', side_effect=lambda: now[0]):
            gimbal = Gimbal(1, 'mt11')
            gimbal.pitch = -60
            gimbal.target = (90, 0)
            now[0] = .1
            gimbal.update()
            self.assertAlmostEqual(gimbal.yaw, 10)
            self.assertAlmostEqual(gimbal.pitch, -50)
            self.assertAlmostEqual(gimbal.yaw_rate, 100)
            self.assertAlmostEqual(gimbal.pitch_rate, 100)

    def test_a8_measured_response(self):
        gimbal = Gimbal(2, 'a8')
        for axis in ('pitch', 'yaw'):
            for command in range(-5, 6):
                self.assertEqual(gimbal.vendor_rate_response(axis, command), 0)
        for axis, command, speed in [('pitch', 6, 4.3), ('pitch', -20, -14.6),
                                     ('pitch', 100, 74), ('yaw', -100, -65),
                                     ('yaw', 100, 89), ('yaw', -10, -7.6),
                                     ('yaw', 10, 8.6)]:
            self.assertAlmostEqual(gimbal.vendor_rate_response(axis, command), speed)

    def test_zt30_measured_manual_response(self):
        gimbal = Gimbal(1, 'zt30')
        for axis in ('pitch', 'yaw'):
            self.assertEqual(gimbal.properties['sim_' + axis + '_rate_curve'],
                             gimbal.properties['vendor_' + axis + '_rate_curve'])
        for axis in ('pitch', 'yaw'):
            for command in range(-4, 5):
                self.assertEqual(gimbal.vendor_rate_response(axis, command), 0)
        # The calibrated web30deg/s requests use raw24 yaw and25 pitch.
        for axis, command, speed in [('yaw', 5, 1.6), ('pitch', -5, -1.3),
                                     ('yaw', 24, 29.58), ('yaw', -24, -29.3),
                                     ('pitch', 25, 30.5), ('pitch', -25, -30.4),
                                     ('yaw', 45, 59.5), ('pitch', -45, -59.4)]:
            self.assertAlmostEqual(gimbal.vendor_rate_response(axis, command), speed)

    def test_a8_inverted_sweep_coordinates(self):
        with patch('mt11_rate_sweep.socket.socket') as factory:
            sock = factory.return_value
            reply = siyi_frame(2, 801, 13, struct.pack('<6h', 100, -1700, 0, 20, -30, 0))
            sock.recv.side_effect = [BlockingIOError(), reply]
            vendor = Vendor('127.0.0.1', 37260, 'a8', True)
            self.assertEqual(vendor.attitude(), (10, -10, 0, 2, 3, 0))

    def test_sweep_discards_queued_feedback_without_requiring_sequence_echo(self):
        with patch('mt11_rate_sweep.socket.socket') as factory:
            sock = factory.return_value
            stale = siyi_frame(2, 800, 13, struct.pack('<6h', 0, -800, 0, 0, 0, 0))
            fresh = siyi_frame(2, 801, 13, struct.pack('<6h', -100, -450, 0, 0, 0, 0))
            sock.recv.side_effect = [stale, BlockingIOError(), fresh]
            vendor = Vendor('127.0.0.1', 37260)
            self.assertEqual(vendor.attitude()[:2], (10.0, -45.0))

    def test_measured_threshold_on_both_axes(self):
        for orientation in (1, 2):
            for command in (-7, -6, -5, -1, 0, 1, 5, 6, 7):
                now = [0.0]
                with patch('gimbal_sim.time.monotonic', side_effect=lambda: now[0]):
                    gimbal = Gimbal(orientation, 'mt11')
                    gimbal.properties = dict(gimbal.properties, sim_rate_time_constant=0)
                    gimbal.pitch = -45
                    gimbal.handle_siyi(siyi_frame(1, 1, 7, struct.pack('<bb', command, command)))
                    for tick in range(1, 5):
                        now[0] = tick * .25
                        gimbal.update()
                    expected = command if abs(command) >= 6 else 0
                    self.assertAlmostEqual(gimbal.pitch, -45 + expected)
                    self.assertAlmostEqual(gimbal.yaw, expected)
                    gimbal.handle_siyi(siyi_frame(1, 2, 7, b'\0\0'))
                    now[0] += .25
                    gimbal.update()
                    self.assertAlmostEqual(gimbal.pitch, -45 + expected)
                    self.assertAlmostEqual(gimbal.yaw, expected)

    def test_continuous_yaw_and_bounded_camera(self):
        for backend, initial, expected in [('mt11', 179, -178.5), ('a8', 134, 135)]:
            now = [0.0]
            with patch('gimbal_sim.time.monotonic', side_effect=lambda: now[0]):
                gimbal = Gimbal(1, backend)
                gimbal.properties = dict(gimbal.properties, sim_rate_time_constant=0)
                gimbal.yaw = initial
                gimbal.handle_siyi(siyi_frame(1, 1, 7, struct.pack('<bb', 10, 0)))
                now[0] = .25
                gimbal.update()
                self.assertAlmostEqual(gimbal.yaw, expected)

    def test_rate_response_has_onset_and_stopping_lag(self):
        now = [0.0]
        with patch('gimbal_sim.time.monotonic', side_effect=lambda: now[0]):
            gimbal = Gimbal(1, 'mt11')
            gimbal.pitch = -45
            gimbal.handle_siyi(siyi_frame(1, 1, 7, struct.pack('<bb', 20, 20)))
            now[0] = .04
            gimbal.update()
            self.assertGreater(gimbal.yaw_rate, 10)
            self.assertLess(gimbal.yaw_rate, 15)
            self.assertGreater(gimbal.yaw, .2)
            self.assertLess(gimbal.yaw, .5)
            before_stop = gimbal.yaw
            gimbal.handle_siyi(siyi_frame(1, 2, 7, b'\0\0'))
            for _ in range(4):
                now[0] += .25
                gimbal.update()
            self.assertGreater(gimbal.yaw, before_stop)
            self.assertAlmostEqual(gimbal.yaw, .8, places=5)
            self.assertAlmostEqual(gimbal.pitch, -44.2, places=5)
            self.assertAlmostEqual(gimbal.yaw_rate, 0, places=5)

    def test_analyzer_uses_angles_and_handles_yaw_wrap(self):
        def fmt(ident, length, name, form, fields):
            return struct.pack('<BBBBB4s16s64s', 0xa3, 0x95, 128, ident, length,
                               name.encode(), form.encode(), fields.encode())
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'sweep.BIN'
            with path.open('wb') as out:
                out.write(fmt(129, 43, 'GIMB', 'QQffffff', 'TimeUS,SampleUS,Roll,Pitch,Yaw,RollRate,PitchRate,YawRate'))
                out.write(fmt(130, 32, 'GCMD', 'QBffffi', 'TimeUS,Mode,Pitch,Yaw,WireP,WireY,Result'))
                out.write(b'\xa3\x95\x82' + struct.pack('<QBffffi', 1000000, 2, 0, 0, 5, 6, 0))
                for n in range(50):
                    timestamp = 1000000 + n * 50000
                    yaw = (179 + n * .3 + 180) % 360 - 180
                    out.write(b'\xa3\x95\x81' + struct.pack('<QQffffff', timestamp, timestamp,
                              0, -45, yaw, math.nan, math.nan, math.nan))
            data = list(samples(path))
            self.assertTrue(data)
            for axis, command, rate, *_ in data:
                self.assertEqual(command, 6 if axis == 'Yaw' else 5)
                self.assertAlmostEqual(rate, 6 if axis == 'Yaw' else 0, places=3)


if __name__ == '__main__':
    unittest.main()
