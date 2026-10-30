#!/usr/bin/env python3
"""Round trips and failure handling for the radiometric archive converter."""
import hashlib
import json
import math
import os
import struct
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import thermal_to_video as thermal
import numpy as np


class ThermalArchiveTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root/'source'
        self.source.mkdir()
        self.names = ['2026-08-28_02-08-32_%s_I.bin' % ms for ms in ('5', '25', '100')]
        rng = np.random.default_rng(42)
        arrays = [np.arange(thermal.WIDTH*thermal.HEIGHT, dtype=np.uint32).astype('<u2'),
                  rng.integers(0, 65536, size=thermal.WIDTH*thermal.HEIGHT, dtype=np.uint16),
                  np.full(thermal.WIDTH*thermal.HEIGHT, 18532, dtype='<u2')]
        for n, (name, data) in enumerate(zip(self.names, arrays)):
            p = self.source/name
            p.write_bytes(data.astype('<u2').tobytes())
            os.utime(p, ns=(1700000000000000000+n, 1700000000000000000+n))
        self.video = self.root/'thermal.mkv'

    def test_bit_exact_names_times_and_metadata(self):
        thermal.encode(self.source, self.video)
        dest = self.root/'extracted'
        result = thermal.extract(self.video, dest, save_metadata=True)
        self.assertEqual(result['checksums_verified'], 3)
        self.assertEqual(sorted(p.name for p in dest.glob('*.bin')), sorted(self.names))
        for name in self.names:
            self.assertEqual((self.source/name).read_bytes(), (dest/name).read_bytes())
            self.assertEqual((self.source/name).stat().st_mtime_ns, (dest/name).stat().st_mtime_ns)
        first = json.loads((dest/self.names[0]).with_suffix('.json').read_text())
        second = json.loads((dest/self.names[1]).with_suffix('.json').read_text())
        self.assertEqual(first['capture_monotonic_us'], 1)
        self.assertEqual(second['capture_monotonic_us'], 20001)
        self.assertIsNone(first['telemetry'])
        import av
        with av.open(str(self.video)) as container:
            self.assertEqual([p.pts for p in container.demux(video=0) if p.size], [0, 20, 95])

    def test_extract_live_profile_without_archive_fields(self):
        import av
        thermal.encode(self.source, self.video)
        live = self.root/'live.mkv'
        with av.open(str(self.video)) as container, live.open('wb') as out:
            stream = container.streams.video[0]
            out.write(thermal.matroska_header(stream.codec_context.extradata, 200000000, 600))
            for packet in container.demux(stream):
                if not packet.size:
                    continue
                metadata = thermal.packet_metadata(packet)
                for key in ('original_filename', 'source_mtime_ns', 'source_sha256', 'archive_frame_count'):
                    metadata.pop(key)
                out.write(thermal.matroska_frame(bytes(packet), metadata, packet.pts))
        output = self.root/'live-frames'
        result = thermal.extract(live, output)
        self.assertEqual(result, dict(frames=3, raw_bytes=3*thermal.FRAME_BYTES, checksums_verified=0))
        for n, name in enumerate(self.names, 1):
            self.assertEqual((self.source/name).read_bytes(), (output/('%012u.bin' % n)).read_bytes())

    def test_fixed_fps_and_mtime_fallback(self):
        for n, name in enumerate(self.names):
            (self.source/name).rename(self.source/('%s.bin' % n))
        entries, times, duration, source = thermal.inputs(self.source, fps=5)
        self.assertEqual(times, [0, 200, 400])
        self.assertEqual(source, 'legacy_fixed_fps')
        self.assertEqual(thermal.inputs(self.source)[3], 'legacy_file_mtime')

    def test_wrong_size_and_no_overwrite(self):
        (self.source/'bad.bin').write_bytes(b'bad')
        with self.assertRaises(ValueError):
            thermal.encode(self.source, self.video)
        self.assertFalse(self.video.exists())
        (self.source/'bad.bin').unlink()
        self.video.write_bytes(b'keep me')
        with self.assertRaises(ValueError):
            thermal.encode(self.source, self.video)
        self.assertEqual(self.video.read_bytes(), b'keep me')
        with self.assertRaises(ValueError):
            thermal.extract(self.video, self.source)

    def test_checksum_and_truncation(self):
        thermal.encode(self.source, self.video)
        data = self.video.read_bytes()
        digest = hashlib.sha256((self.source/self.names[0]).read_bytes()).hexdigest().encode()
        self.assertIn(digest, data)
        self.video.write_bytes(data.replace(digest, b'0'*64, 1))
        output = self.root/'bad-output'
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            thermal.extract(self.video, output)
        self.assertFalse(output.exists())
        # A clean cut between complete frames is also an incomplete archive.
        at = data.rfind(bytes.fromhex('1f43b675'))
        self.assertGreater(at, 0)
        self.video.write_bytes(data[:at])
        with self.assertRaisesRegex(ValueError, 'incomplete archive'):
            thermal.extract(self.video, output)
        self.assertFalse(output.exists())
        self.assertFalse(list(self.root.glob('.bad-output.*')))

    def test_unsafe_names(self):
        for name in ('../escape.bin', '/escape.bin', 'a/b.bin', 'a\\b.bin', 'C:escape.bin', '\0.bin'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                thermal.output_name(dict(original_filename=name, frame_id=1))

    def write_tlog(self):
        from pymavlink.dialects.v20 import ardupilotmega as mavlink
        writer = mavlink.MAVLink(None, srcSystem=1, srcComponent=1)
        path = self.root / 'flight.tlog'
        start = thermal.filename_time(Path(self.names[0])) / 1000
        with path.open('wb') as dest:
            for dt in (-0.1, 0.1):
                # Deliberately reset boot time: matching must use tlog UTC.
                boot = 90000 if dt < 0 else 5
                messages = [mavlink.MAVLink_attitude_message(boot, 0.1, 0.2, 0.3, 0, 0, 0.4),
                            mavlink.MAVLink_global_position_int_message(
                                boot, -350000000, 1490000000, 600000, 50000, 100, 200, -100, 0)]
                for name, value in [('CROLL', 2), ('CPITCH', -70), ('CYAW', 179 if dt < 0 else -179),
                                    ('TMIN', 3.5), ('TMAX', 42.25), ('SIYI_LIDR', 120.5)]:
                    messages.append(mavlink.MAVLink_named_value_float_message(boot, name.encode(), value))
                for message in messages:
                    dest.write(struct.pack('>Q', round((start + dt) * 1e6)))
                    dest.write(message.pack(writer))
        return path

    def test_tlog_encode_and_update_preserve_pixels_and_timestamps(self):
        import av
        log = self.write_tlog()
        thermal.encode(self.source, self.video)
        updated = self.root / 'updated.mkv'
        result = thermal.update_metadata(self.video, updated, tlog=log)
        self.assertEqual(result['coverage']['gimbal_attitude'], 3)
        with av.open(str(self.video)) as old, av.open(str(updated)) as new:
            old_packets = [p for p in old.demux(video=0) if p.size]
            new_packets = [p for p in new.demux(video=0) if p.size]
            self.assertEqual([(p.pts, bytes(p)) for p in old_packets], [(p.pts, bytes(p)) for p in new_packets])
            metadata = thermal.packet_metadata(new_packets[0])
            snapshot = metadata['telemetry']
            self.assertEqual(snapshot['position']['alt_amsl_m'], 600)
            self.assertEqual(snapshot['position']['alt_relative_m'], 50)
            self.assertEqual(snapshot['velocity']['vn_m_s'], 1)
            self.assertAlmostEqual(snapshot['gimbal_attitude']['pitch_rad'], math.radians(-70), places=6)
            self.assertAlmostEqual(abs(snapshot['gimbal_attitude']['yaw_rad']), math.pi, places=6)
            self.assertEqual(metadata['gimbal_pose_source']['pitch']['name'], 'CPITCH')
            self.assertEqual(snapshot['named_values']['TMAX']['value'], 42.25)
            self.assertEqual(snapshot['named_values']['TMIN']['value'], 3.5)
            self.assertEqual(snapshot['named_values']['SIYI_LIDR']['value'], 120.5)
            self.assertEqual(snapshot['named_values']['TMAX']['method'], 'previous')
            self.assertEqual(snapshot['named_values']['TMAX']['age_ms'], 100)
        extracted = self.root / 'updated-raw'
        self.assertEqual(thermal.extract(updated, extracted)['checksums_verified'], 3)
        encoded = self.root / 'with-tlog.mkv'
        thermal.encode(self.source, encoded, tlog=log)
        with av.open(str(encoded)) as c:
            self.assertIsNotNone(thermal.packet_metadata(next(c.demux(video=0)))['telemetry']['gimbal_attitude'])
        with self.assertRaisesRegex(ValueError, 'already exists'):
            thermal.update_metadata(self.video, updated, tlog=log)
        # A clean packet-boundary cut must never publish a partial archive.
        data = self.video.read_bytes()
        self.video.write_bytes(data[:data.rfind(bytes.fromhex('1f43b675'))])
        broken = self.root / 'broken.mkv'
        with self.assertRaisesRegex(ValueError, 'incomplete archive'):
            thermal.update_metadata(self.video, broken, tlog=log)
        self.assertFalse(broken.exists())

    def test_camera_timestamp_with_host_timestamp_suffix(self):
        path = Path('2026-08-28_02-08-32_5_I_2026_08_28_12_08_32_359.bin')
        self.assertEqual(thermal.filename_time(path), thermal.filename_time(Path(self.names[0])))



class Record:
    """A minimal stand-in for a pymavlink dataflash record."""
    def __init__(self, TimeUS, **fields):
        self.TimeUS = TimeUS
        self.__dict__.update(fields)


class ReconstructedTelemetryTests(unittest.TestCase):
    def series(self, times, **columns):
        angle = columns.pop('_angle', ())
        return (list(times), {k: list(v) for k, v in columns.items()}, set(angle))

    def telemetry(self, **kwargs):
        # 1000 ms and 2000 ms samples; a query at 1500 ms interpolates midway.
        series = {
            'position': self.series([1000, 2000], lat_e7=[-350000000, -350002000],
                                    lon_e7=[1490000000, 1490004000],
                                    alt_amsl_m=[600.0, 620.0], alt_relative_m=[100.0, 120.0]),
            'vehicle_attitude': self.series([1000, 2000], roll_rad=[0.0, 0.2],
                                            pitch_rad=[0.0, 0.1], yaw_rad=[0.0, math.radians(10)],
                                            _angle=('yaw_rad',)),
            'gimbal_attitude': self.series([1000, 2000],
                                           roll_rad=[0.0, 0.0], pitch_rad=[math.radians(-35)]*2,
                                           yaw_rad=[math.radians(90), math.radians(90)], _angle=('yaw_rad',)),
        }
        return thermal.BinTelemetry(series, 'flight.bin', **kwargs)

    def test_interpolation_and_ages(self):
        snap = self.telemetry().sample(1500, pts_ms=1500)
        self.assertEqual(snap['schema'], 'apcg.telemetry.v1')
        self.assertEqual(snap['utc_us'], 1500000)
        self.assertEqual(snap['position']['lat_e7'], -350001000)
        self.assertAlmostEqual(snap['position']['alt_amsl_m'], 610.0)
        self.assertEqual(snap['position']['age_ms'], 500)
        self.assertAlmostEqual(snap['vehicle_attitude']['yaw_rad'], math.radians(5), places=6)
        self.assertAlmostEqual(snap['heading_rad'], math.radians(5), places=6)

    def test_out_of_range_is_null_with_clock(self):
        snap = self.telemetry().sample(5000, pts_ms=5000)
        self.assertEqual(snap['utc_us'], 5000000)
        for key in ('position', 'velocity', 'vehicle_attitude', 'gimbal_attitude', 'heading_rad'):
            self.assertIsNone(snap[key], key)

    def test_earth_yaw_is_made_vehicle_relative(self):
        # Gimbal earth yaw 90 deg minus vehicle yaw 5 deg -> 85 deg relative.
        snap = self.telemetry(gimbal_yaw_earth=True).sample(1500, pts_ms=1500)
        self.assertAlmostEqual(math.degrees(snap['gimbal_attitude']['yaw_rad']), 85, places=4)
        self.assertAlmostEqual(math.degrees(snap['gimbal_attitude']['pitch_rad']), -35, places=4)
        # Without the flag the yaw is reported as stored.
        snap = self.telemetry(gimbal_yaw_earth=False).sample(1500, pts_ms=1500)
        self.assertAlmostEqual(math.degrees(snap['gimbal_attitude']['yaw_rad']), 90, places=4)

    def test_dataflash_degrees_become_radians(self):
        # ATT angles and RATE rates (ArduPilot unit 'k') are logged in degrees.
        rows = {name: [] for name in ('POS', 'ATT', 'XKF1', 'MNT', 'RATE')}
        rows['ATT'] = [Record(1000000, Roll=0.0, Pitch=-10.0, Yaw=180.0),
                       Record(2000000, Roll=0.0, Pitch=-10.0, Yaw=180.0)]
        rows['RATE'] = [Record(1500000, P=0.0, Y=90.0)]
        built, _, _ = thermal.build_telemetry_series(rows, boot_ms=0)
        times, columns, _ = built['vehicle_rate']
        self.assertEqual(times, [1500.0])
        self.assertAlmostEqual(columns['yaw_rate_rad_s'][0], (math.pi / 2) / math.cos(math.radians(10)))
        _, columns, _ = built['vehicle_attitude']
        self.assertAlmostEqual(columns['pitch_rad'][0], math.radians(-10))
        self.assertAlmostEqual(columns['yaw_rad'][0], math.pi)
        self.assertNotIn('position', built)

    def test_body_rates_become_euler_yaw_rate(self):
        # RATE.Y is the body yaw gyro; a 60 degree bank halves its share of
        # the Euler yaw rate and the body pitch gyro contributes sin(roll).
        rows = {name: [] for name in ('POS', 'ATT', 'XKF1', 'MNT', 'RATE')}
        rows['ATT'] = [Record(1000000, Roll=0.0, Pitch=0.0, Yaw=0.0),
                       Record(3000000, Roll=60.0, Pitch=0.0, Yaw=0.0)]
        rows['RATE'] = [Record(3000000, P=0.0, Y=90.0),        # bank 60: r cos(60)
                        Record(2000000, P=90.0, Y=0.0),        # bank 30: q sin(30)
                        Record(4000000, P=0.0, Y=90.0)]        # after ATT coverage
        built, _, _ = thermal.build_telemetry_series(rows, boot_ms=0)
        times, columns, _ = built['vehicle_rate']
        self.assertEqual(times, [2000.0, 3000.0])
        self.assertAlmostEqual(columns['yaw_rate_rad_s'][0], math.pi / 4)
        self.assertAlmostEqual(columns['yaw_rate_rad_s'][1], math.pi / 4)
        rows['RATE'] = []
        self.assertNotIn('vehicle_rate', thermal.build_telemetry_series(rows, boot_ms=0)[0])

    def test_roll_wraps_through_180(self):
        # Roll interpolates along the shortest arc: 179 -> -179 passes 180,
        # where cos(roll) is -1, not through 0.
        rows = {name: [] for name in ('POS', 'ATT', 'XKF1', 'MNT', 'RATE')}
        rows['ATT'] = [Record(1000000, Roll=179.0, Pitch=0.0, Yaw=0.0),
                       Record(3000000, Roll=-179.0, Pitch=0.0, Yaw=0.0)]
        rows['RATE'] = [Record(2000000, P=0.0, Y=90.0)]
        built, _, _ = thermal.build_telemetry_series(rows, boot_ms=0)
        self.assertAlmostEqual(built['vehicle_rate'][1]['yaw_rate_rad_s'][0], -math.pi / 2)

    def test_vertical_pitch_gap_is_not_bridged(self):
        # Vertical pitch leaves the Euler yaw rate undefined, as on the camera;
        # snapshots touching that interval carry no yaw rate at all.
        rows = {name: [] for name in ('POS', 'ATT', 'XKF1', 'MNT', 'RATE')}
        rows['ATT'] = [Record(1000000, Roll=0.0, Pitch=0.0, Yaw=0.0),
                       Record(2000000, Roll=0.0, Pitch=90.0, Yaw=0.0),
                       Record(3000000, Roll=0.0, Pitch=0.0, Yaw=0.0)]
        rows['RATE'] = [Record(t, P=0.0, Y=90.0) for t in (1000000, 2000000, 3000000)]
        built, yaw_earth, source = thermal.build_telemetry_series(rows, boot_ms=0)
        telemetry = thermal.BinTelemetry(built, 'flight.bin', yaw_earth, source)
        for utc_ms in (1500, 2000, 2500):
            self.assertNotIn('yaw_rate_rad_s', telemetry.sample(utc_ms, pts_ms=utc_ms)['vehicle_attitude'], utc_ms)
        self.assertAlmostEqual(telemetry.sample(1000, pts_ms=1000)['vehicle_attitude']['yaw_rate_rad_s'], math.pi / 2, places=5)
        self.assertAlmostEqual(telemetry.sample(3000, pts_ms=3000)['vehicle_attitude']['yaw_rate_rad_s'], math.pi / 2, places=5)

    def test_gimbal_source_prefers_signal(self):
        records = [Record(t, Roll=0.0, DRoll=0.0, Pitch=0.0, DPitch=-35.0,
                          YawB=0.0, DYawB=0.0, YawE=y, DYawE=float('nan'))
                   for t, y in ((1000, 10.0), (2000, 40.0))]
        series, yaw_earth, source = thermal.build_gimbal(records, boot_ms=0, rad=math.radians)
        self.assertTrue(yaw_earth)
        self.assertEqual(source, dict(roll='actual', pitch='demanded', yaw='actual', yaw_frame='earth'))
        _, columns, _ = series
        self.assertAlmostEqual(columns['pitch_rad'][0], math.radians(-35))
        self.assertAlmostEqual(columns['yaw_rad'][1], math.radians(40))

    def test_long_gaps_are_not_interpolated(self):
        telemetry = self.telemetry()
        telemetry.max_gap_ms = 200
        self.assertIsNone(telemetry.sample(1500, 0)['gimbal_attitude'])
        self.assertIsNotNone(telemetry.sample(1000, 0)['gimbal_attitude'])
        self.assertIsNotNone(telemetry.sample(2000, 0)['gimbal_attitude'])

    def test_named_sources_gaps_invalid_values_and_yaw_wrap(self):
        named = {('CYAW', 1, 1): ([1000, 1200, 10000], [179, -179, 90]),
                 ('CYAW', 252, 231): ([1000, 1200], [0, 0]),
                 ('TMAX', 252, 231): ([1000, 1200], [20, 50]),
                 ('SIYI_LIDR', 1, 1): ([1000, 1200], [10, 0])}
        telemetry = thermal.TlogTelemetry({}, named, 'flight.tlog', max_gap_ms=2000)
        yaw = telemetry.named_sample('CYAW', 1100, angle=True)
        self.assertAlmostEqual(yaw['value'], 180)
        self.assertEqual(yaw['source']['system_id'], 1)
        self.assertEqual(telemetry.named_sample('TMAX', 1100)['value'], 20)
        self.assertEqual(telemetry.named_sample('TMAX', 1100)['age_ms'], 100)
        self.assertFalse(telemetry.named_sample('SIYI_LIDR', 1300)['valid'])
        self.assertIsNone(telemetry.named_sample('CYAW', 5000, angle=True))
        self.assertEqual(telemetry.named_sample('CYAW', 1800, angle=True)['method'], 'nearest')
        self.assertIsNone(telemetry.named_sample('TMAX', 5000))
        self.assertIsNone(telemetry.named_sample('TMAX', 500))
        # An exact finite sample survives an invalid neighbour; the interval
        # touching the invalid angle cannot supply a reconstructed pose.
        named[('CYAW', 1, 1)] = ([1000, 1200], [179, float('nan')])
        del named[('CYAW', 252, 231)]
        self.assertTrue(telemetry.named_sample('CYAW', 1000, angle=True)['valid'])
        self.assertFalse(telemetry.named_sample('CYAW', 1100, angle=True)['valid'])

    def test_temperature_log_anomaly_is_retained_and_flagged(self):
        named = {(name, 252, 231): ([1000], [value])
                 for name, value in [('TMIN', 645.21), ('TMAX', 16.79)]}
        snap = thermal.TlogTelemetry({}, named, 'flight.tlog').sample(1000, 0)
        low = snap['named_values']['TMIN']
        self.assertEqual(low['value'], 645.21)
        self.assertFalse(low['valid'])
        self.assertEqual(low['invalid_reason'], 'logged_minimum_exceeds_maximum')
        self.assertTrue(snap['named_values']['TMAX']['valid'])

    def test_measured_tlog_wins_and_old_stale_metadata_is_removed(self):
        telemetry = thermal.CombinedTelemetry()
        telemetry.tlog = thermal.TlogTelemetry({}, {
            (name, 1, 1): ([1000, 2000], [value, value])
            for name, value in [('CROLL', 0), ('CPITCH', -75), ('CYAW', 20)]}, 'flight.tlog')
        existing = self.telemetry().sample(1500, 0)
        existing['position']['age_ms'] = 700000
        result = telemetry.sample(1500, 0, existing, dict(pitch='demanded'))
        self.assertAlmostEqual(result['gimbal_attitude']['pitch_rad'], math.radians(-75), places=6)
        self.assertIsNone(result['position'])
        self.assertEqual(telemetry.gimbal_source['pitch']['name'], 'CPITCH')


if __name__ == '__main__':
    unittest.main()
