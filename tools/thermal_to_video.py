#!/usr/bin/env python3
"""Losslessly convert legacy 640x512 gray16le thermal files to APCG Matroska.

Encode:  thermal_to_video.py CAPTURE_DIRECTORY OUTPUT.mkv
Extract: thermal_to_video.py --extract INPUT.mkv OUTPUT_DIRECTORY
Update:  thermal_to_video.py --update-metadata INPUT.mkv OUTPUT.mkv --tlog FLIGHT.tlog
Requires numpy and PyAV >=18.1. Existing outputs are never overwritten.

Old captures carry only a timestamp, so their per-frame telemetry is null.
Pass --bin FLIGHT.bin (an ArduPilot dataflash log) to reconstruct each frame's
position, velocity, and vehicle and gimbal attitude from the log, matched to the
frame's absolute UTC time, filling the apcg.telemetry.v1 snapshot. This also
requires pymavlink. --tlog also supplies measured SIYI CROLL/CPITCH/CYAW (degrees,
level roll/pitch, vehicle-relative yaw), TMIN/TMAX (Celsius) and SIYI_LIDR (metres).
Tlog timestamps are reception UTC, not the frequently reused time_boot_ms.
"""
import argparse
import bisect
import datetime
from fractions import Fraction
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import struct
import tempfile
import time

WIDTH, HEIGHT = 640, 512
FRAME_BYTES = WIDTH * HEIGHT * 2
SCHEMA = 'apcg.thermal.v1'
ADDITION_TYPE = 0x41504347  # Experimental APCG FourCC, matching firmware.
TELEMETRY_SCHEMA = 'apcg.telemetry.v1'
GPS_UNIX_EPOCH = 315964800  # 1980-01-06 UTC, in Unix seconds
GPS_LEAP_SECONDS = 18       # GPS minus UTC; constant since 2017-01
# Milliseconds are an integer, not a decimal fraction: _13 means 13 ms.
FILENAME_TIME = re.compile(r'^(\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2})_(\d{1,3})(?:_\d+)?_I'
                           r'(?:_\d{4}_\d{2}_\d{2}_\d{2}_\d{2}_\d{2}_\d{1,3})?\.bin$')


def uint(value):
    return value.to_bytes(max(1, (value.bit_length() + 7) // 8), 'big')


def element(tag, payload):
    """EBML element with a known length (all-ones lengths are reserved)."""
    size = len(payload)
    length = next(n for n in range(1, 9) if size < (1 << (7*n)) - 1)
    return uint(tag) + ((1 << (7*length)) | size).to_bytes(length, 'big') + payload


def integer(tag, value):
    return element(tag, uint(value))


def text(tag, value):
    return element(tag, value.encode('utf-8'))


def matroska_header(extra, frame_duration_ns, duration_ms):
    """Same FFV1 track and BlockAdditionMapping as thermal_matroska.h."""
    ebml = b''.join(integer(tag, value) for tag, value in
                    ((0x4286, 1), (0x42F7, 1), (0x42F2, 4), (0x42F3, 8)))
    ebml += text(0x4282, 'matroska') + integer(0x4287, 4) + integer(0x4285, 2)
    segment = bytes.fromhex('1853806701ffffffffffffff')  # unknown Segment length
    info = integer(0x2AD7B1, 1000000)  # 1 ms timebase
    info += text(0x4D80, 'AP_CameraGimbal') + text(0x5741, 'thermal_to_video.py')
    info += element(0x4489, struct.pack('>d', duration_ms))
    track = b''.join(integer(tag, value) for tag, value in
                     ((0xD7, 1), (0x73C5, 1), (0x83, 1), (0x9C, 0), (0x55EE, 2)))
    track += integer(0x23E383, frame_duration_ns)
    track += text(0x86, 'V_FFV1') + element(0x63A2, extra)
    video = integer(0xB0, WIDTH) + integer(0xBA, HEIGHT)
    video += element(0x55B0, integer(0x55B2, 16))
    track += element(0xE0, video)
    mapping = integer(0x41F0, 2) + text(0x41A4, 'apcg.thermal.v1 JSON')
    mapping += integer(0x41E7, ADDITION_TYPE)
    track += element(0x41E4, mapping)
    return element(0x1A45DFA3, ebml) + segment + element(0x1549A966, info) + element(0x1654AE6B, element(0xAE, track))


def matroska_frame(encoded, metadata, pts_ms):
    block = element(0xA1, b'\x81\0\0\0' + encoded)
    extra = json.dumps(metadata, separators=(',', ':'), allow_nan=False).encode('utf-8')
    additions = element(0x75A1, element(0xA6, integer(0xEE, 2) + element(0xA5, extra)))
    return element(0x1F43B675, integer(0xE7, pts_ms) + element(0xA0, block + additions))


def filename_time(path):
    match = FILENAME_TIME.fullmatch(path.name)
    if not match:
        return None
    date = datetime.datetime.strptime(match[1], '%Y-%m-%d_%H-%M-%S')
    delta = date - datetime.datetime(1970, 1, 1)
    return (delta.days*86400 + delta.seconds)*1000 + int(match[2])


def _wrap_pi(angle):
    return (angle + math.pi) % (2 * math.pi) - math.pi


def _wrap_2pi(angle):
    return angle % (2 * math.pi)


def _lerp_angle(a, b, fraction):
    """Interpolate along the shortest arc, so a 359 -> 1 degree step is +2."""
    return a + _wrap_pi(b - a) * fraction


class BinTelemetry:
    """Reconstructs apcg.telemetry.v1 snapshots from a dataflash log, indexed by
    absolute UTC milliseconds. Each field is linearly interpolated between the
    bracketing log samples; yaw and heading follow the shortest arc. age_ms is
    the distance to the nearest real sample. A time outside a series' coverage
    yields null for that field, so frames captured before the log started (or
    after it ended) keep a reconstructed clock with a null pose."""

    def __init__(self, series, source_name, gimbal_yaw_earth=False, gimbal_source=None,
                 max_gap_ms=2000):
        self.series = series  # name -> (times, {column: values}, angle_columns)
        self.source_name = source_name
        # This gimbal log carries earth-referenced yaw; sample() subtracts the
        # vehicle yaw to report the schema's vehicle-relative angle.
        self.gimbal_yaw_earth = gimbal_yaw_earth
        # Which MNT field each gimbal axis came from (actual vs demanded).
        self.gimbal_source = gimbal_source
        self.max_gap_ms = max_gap_ms

    def _interp(self, name, utc_ms):
        entry = self.series.get(name)
        if entry is None:
            return None, None
        times, columns, angle_columns = entry
        if not times or utc_ms < times[0] or utc_ms > times[-1]:
            return None, None
        hi = min(bisect.bisect_right(times, utc_ms), len(times) - 1)
        lo = max(0, hi - 1)
        span = times[hi] - times[lo]
        fraction = (utc_ms - times[lo]) / span if span else 0.0
        if 0 < fraction < 1 and span > self.max_gap_ms:
            return None, None
        out = {}
        for column, values in columns.items():
            a, b = values[lo], values[hi]
            # An exact hit is the sample itself, even beside a NaN neighbour.
            if fraction <= 0:
                out[column] = a
            elif fraction >= 1:
                out[column] = b
            else:
                out[column] = _lerp_angle(a, b, fraction) if column in angle_columns \
                    else a + (b - a) * fraction
        return out, int(round(min(utc_ms - times[lo], times[hi] - utc_ms)))

    def sample(self, utc_ms, pts_ms):
        snapshot = {'schema': TELEMETRY_SCHEMA, 'pts90k': int(round(pts_ms * 90)),
                    'utc_us': int(round(utc_ms * 1000))}
        position, age = self._interp('position', utc_ms)
        snapshot['position'] = position and {
            'lat_e7': int(round(position['lat_e7'])), 'lon_e7': int(round(position['lon_e7'])),
            'alt_amsl_m': round(position['alt_amsl_m'], 3),
            'alt_relative_m': round(position['alt_relative_m'], 3), 'age_ms': age}
        velocity, age = self._interp('velocity', utc_ms)
        snapshot['velocity'] = velocity and {
            'vn_m_s': round(velocity['vn_m_s'], 3), 've_m_s': round(velocity['ve_m_s'], 3),
            'vd_m_s': round(velocity['vd_m_s'], 3), 'age_ms': age}
        attitude, age = self._interp('vehicle_attitude', utc_ms)
        heading = None
        if attitude is None:
            snapshot['vehicle_attitude'] = None
        else:
            entry = {'roll_rad': round(attitude['roll_rad'], 6),
                     'pitch_rad': round(attitude['pitch_rad'], 6),
                     'yaw_rad': round(_wrap_2pi(attitude['yaw_rad']), 6)}
            rate, _ = self._interp('vehicle_rate', utc_ms)
            if rate is not None and math.isfinite(rate['yaw_rate_rad_s']):
                entry['yaw_rate_rad_s'] = round(rate['yaw_rate_rad_s'], 6)
            entry['age_ms'] = age
            snapshot['vehicle_attitude'] = entry
            heading = entry['yaw_rad']
        gimbal, age = self._interp('gimbal_attitude', utc_ms)
        if gimbal is None:
            snapshot['gimbal_attitude'] = None
        else:
            entry = {'roll_rad': round(gimbal['roll_rad'], 6),
                     'pitch_rad': round(gimbal['pitch_rad'], 6)}
            yaw = gimbal['yaw_rad']
            if self.gimbal_yaw_earth:
                # Earth-referenced yaw needs the vehicle yaw to become relative.
                vehicle_yaw = heading
                if vehicle_yaw is None:
                    reference, _ = self._interp('vehicle_attitude', utc_ms)
                    vehicle_yaw = reference['yaw_rad'] if reference else None
                yaw = None if vehicle_yaw is None else yaw - vehicle_yaw
            if yaw is not None:
                entry['yaw_rad'] = round(_wrap_pi(yaw), 6)
            entry['age_ms'] = age
            snapshot['gimbal_attitude'] = entry
        snapshot['heading_rad'] = None if heading is None else round(heading, 6)
        snapshot['zoom'] = None      # RGB zoom is not recorded in the log
        snapshot['hfov_deg'] = None  # thermal HFOV is the outer frame field
        return snapshot


def load_bin_telemetry(bin_path, max_gap_ms=2000):
    """Read the pose time-series a dataflash log needs to reconstruct telemetry.

    GPS week/ms give absolute UTC against each record's boot-relative TimeUS, so
    every message maps to UTC and to the thermal frames' filename timestamps."""
    try:
        from pymavlink import mavutil
    except ImportError as error:
        raise RuntimeError("Install pymavlink for --bin: python3 -m pip install pymavlink") from error
    if not bin_path.is_file():
        raise ValueError('dataflash log not found: %s' % bin_path)
    rows = {name: [] for name in ('POS', 'ATT', 'XKF1', 'MNT', 'RATE')}
    boot_offsets = []
    log = mavutil.mavlink_connection(str(bin_path))
    while True:
        message = log.recv_match(type=['GPS'] + list(rows))
        if message is None:
            break
        kind = message.get_type()
        if kind == 'GPS':
            if getattr(message, 'GWk', 0) > 0:
                utc_s = (GPS_UNIX_EPOCH + message.GWk * 604800 +
                         message.GMS / 1000.0 - GPS_LEAP_SECONDS)
                boot_offsets.append(utc_s - message.TimeUS / 1e6)
            continue
        if kind == 'XKF1' and getattr(message, 'C', 0) != 0:
            continue  # primary EKF lane only
        if kind == 'MNT' and getattr(message, 'I', 0) != 0:
            continue  # first mount only
        rows[kind].append(message)
    if not boot_offsets:
        raise ValueError('%s has no GPS UTC; cannot align frame timestamps' % bin_path.name)
    boot_ms = statistics.median(boot_offsets) * 1000.0
    built, gimbal_yaw_earth, gimbal_source = build_telemetry_series(rows, boot_ms)
    log.close()
    return BinTelemetry(built, bin_path.name, gimbal_yaw_earth, gimbal_source, max_gap_ms)


def euler_yaw_rate_series(rate_records, attitude, boot_ms):
    """Euler yaw rate from body pitch/yaw gyro rates, as the camera computes
    it from ATTITUDE: (q sin(roll) + r cos(roll)) / cos(pitch). Samples outside
    the attitude coverage are dropped; near vertical pitch the rate is NaN so
    interpolation cannot bridge the undefined interval."""
    if attitude is None or not rate_records:
        return None
    att_times, att, _ = attitude
    times, values = [], []
    for record in sorted(rate_records, key=lambda r: r.TimeUS):
        t = boot_ms + record.TimeUS / 1000.0
        if t < att_times[0] or t > att_times[-1]:
            continue
        hi = min(bisect.bisect_right(att_times, t), len(att_times) - 1)
        lo = max(0, hi - 1)
        span = att_times[hi] - att_times[lo]
        fraction = (t - att_times[lo]) / span if span else 0.0
        roll = _lerp_angle(att['roll_rad'][lo], att['roll_rad'][hi], fraction)
        pitch = att['pitch_rad'][lo] + (att['pitch_rad'][hi] - att['pitch_rad'][lo]) * fraction
        cos_pitch = math.cos(pitch)
        times.append(t)
        values.append((math.radians(record.P) * math.sin(roll) +
                       math.radians(record.Y) * math.cos(roll)) / cos_pitch
                      if abs(cos_pitch) > 0.01 else math.nan)
    return (times, {'yaw_rate_rad_s': values}, set()) if times else None


def build_telemetry_series(rows, boot_ms):
    """Map dataflash rows (degrees) to the telemetry series (radians, ms).

    RATE logs body gyro rates; the schema's yaw_rate_rad_s is the Euler (earth
    frame) yaw rate the camera receives live, so it is derived from the pitch
    and yaw gyro rates with roll and pitch interpolated from ATT."""
    def series(name, columns, angle_columns=()):
        records = rows[name]
        if not records:
            return None
        ordered = sorted(records, key=lambda record: record.TimeUS)
        times = [boot_ms + record.TimeUS / 1000.0 for record in ordered]
        data = {column: [read(record) for record in ordered] for column, read in columns.items()}
        return (times, data, set(angle_columns))

    rad = math.radians
    built = {
        'position': series('POS', {
            'lat_e7': lambda r: r.Lat * 1e7, 'lon_e7': lambda r: r.Lng * 1e7,
            'alt_amsl_m': lambda r: r.Alt, 'alt_relative_m': lambda r: r.RelHomeAlt}),
        'velocity': series('XKF1', {
            'vn_m_s': lambda r: r.VN, 've_m_s': lambda r: r.VE, 'vd_m_s': lambda r: r.VD}),
        'vehicle_attitude': series('ATT', {
            'roll_rad': lambda r: rad(r.Roll), 'pitch_rad': lambda r: rad(r.Pitch),
            'yaw_rad': lambda r: rad(r.Yaw)}, angle_columns=('yaw_rad',)),
    }
    built['vehicle_rate'] = euler_yaw_rate_series(rows['RATE'], built['vehicle_attitude'], boot_ms)
    gimbal, gimbal_yaw_earth, gimbal_source = build_gimbal(rows['MNT'], boot_ms, rad)
    if gimbal is not None:
        built['gimbal_attitude'] = gimbal
    return ({name: s for name, s in built.items() if s is not None},
            gimbal_yaw_earth, gimbal_source)


def build_gimbal(records, boot_ms, rad):
    """Reconstruct gimbal attitude from an MNT stream. Backends differ in which
    fields they fill: prefer the reported (actual) angle per axis, else the
    demanded angle, and prefer vehicle-relative yaw, else earth-referenced yaw
    (converted to vehicle-relative when sampled). Returns the series, whether its
    yaw is earth-referenced, and the chosen source per axis."""
    if not records:
        return None, False, None

    def finite(read):
        values = [read(r) for r in records]
        return [v for v in values if v == v]

    def informative(read):
        # A backend leaves an unreported axis at a flat zero; a held command
        # (e.g. a constant -35 pitch) is still real, so accept any non-zero.
        values = finite(read)
        return bool(values) and (max(abs(v) for v in values) > 1e-3 or
                                 max(values) - min(values) > 1e-3)

    def field(name):
        return lambda r: getattr(r, name, float('nan'))

    def choose(actual, demanded):
        if informative(field(actual)):
            return actual, 'actual'
        if informative(field(demanded)):
            return demanded, 'demanded'
        return actual, 'actual'  # nothing logged on this axis; report zero

    roll_col, roll_src = choose('Roll', 'DRoll')
    pitch_col, pitch_src = choose('Pitch', 'DPitch')
    yaw_earth = False
    for column, source, earth in (('YawB', 'actual', False), ('DYawB', 'demanded', False),
                                  ('YawE', 'actual', True), ('DYawE', 'demanded', True)):
        if informative(field(column)):
            yaw_col, yaw_src, yaw_earth = column, source, earth
            break
    else:
        yaw_col, yaw_src = 'YawB', 'actual'
    read = {'roll_rad': field(roll_col), 'pitch_rad': field(pitch_col), 'yaw_rad': field(yaw_col)}
    usable = [r for r in records if all(v == v for v in (read[c](r) for c in read))]
    if not usable:
        return None, False, None
    usable.sort(key=lambda r: r.TimeUS)
    times = [boot_ms + r.TimeUS / 1000.0 for r in usable]
    data = {column: [rad(get(r)) for r in usable] for column, get in read.items()}
    source = dict(roll=roll_src, pitch=pitch_src, yaw=yaw_src,
                  yaw_frame='earth' if yaw_earth else 'vehicle')
    return (times, data, {'yaw_rad'}), yaw_earth, source


class TlogTelemetry(BinTelemetry):
    """Reception-time telemetry; keep named-value senders separate when interpolating.

    Angles use bounded interpolation. Temperatures and range use the last actual
    measurement, with its timestamp and age, rather than invented intermediate
    measurements. No boot-clock alignment is attempted: SIYI's local MAVProxy
    messages reuse the most recent ATTITUDE timestamp and the aircraft may reboot.
    """

    def __init__(self, series, named, source_name, max_gap_ms=2000):
        super().__init__(series, source_name, max_gap_ms=max_gap_ms)
        self.named = named

    def named_sample(self, name, utc_ms, angle=False):
        candidates = []
        for (key, sysid, compid), (times, values) in self.named.items():
            if key != name or not times:
                continue
            hi = bisect.bisect_right(times, utc_ms)
            lo = hi - 1
            method = 'previous'
            if angle and 0 < hi < len(times) and times[hi] - times[lo] <= self.max_gap_ms:
                span = times[hi] - times[lo]
                fraction = (utc_ms - times[lo]) / span
                value = values[lo] if fraction == 0 else math.degrees(
                    _lerp_angle(math.radians(values[lo]), math.radians(values[hi]), fraction))
                nearest = lo if utc_ms - times[lo] <= times[hi] - utc_ms else hi
                age = abs(utc_ms - times[nearest])
                method = 'linear_shortest_arc'
            elif angle:
                nearest = min((i for i in (lo, hi) if 0 <= i < len(times)),
                              key=lambda i: abs(times[i] - utc_ms))
                age = abs(times[nearest] - utc_ms)
                # At the edge of a long outage, only a nearby real sample is useful.
                if age > self.max_gap_ms / 2:
                    continue
                value = values[nearest]
                method = 'nearest'
            else:
                if lo < 0:
                    continue
                nearest = lo
                age = utc_ms - times[lo]
                value = values[lo]
            if age > self.max_gap_ms:
                continue
            valid = math.isfinite(value)
            if name in ('SIYI_LIDR', 'RFND') and valid:
                valid = value > 0
            record = dict(value=value if math.isfinite(value) else None,
                          unit='deg' if angle else 'm' if name in ('SIYI_LIDR', 'RFND') else 'degC',
                          age_ms=round(age), sample_utc_us=round(times[nearest] * 1000),
                          valid=valid, method=method,
                          source=dict(log=self.source_name, message='NAMED_VALUE_FLOAT',
                                      name=name, system_id=sysid, component_id=compid,
                                      timestamp_basis='tlog_reception_utc'))
            candidates.append((not valid, age, sysid, compid, record))
        return min(candidates, key=lambda c: c[:4])[-1] if candidates else None

    def sample(self, utc_ms, pts_ms):
        snapshot = super().sample(utc_ms, pts_ms)
        axes = {axis: self.named_sample(name, utc_ms, angle=True)
                for axis, name in (('roll', 'CROLL'), ('pitch', 'CPITCH'), ('yaw', 'CYAW'))}
        self.gimbal_source = None
        if all(record is not None and record['valid'] for record in axes.values()):
            snapshot['gimbal_attitude'] = {
                axis + '_rad': round(_wrap_pi(math.radians(record['value'])), 6)
                for axis, record in axes.items()}
            snapshot['gimbal_attitude']['age_ms'] = max(r['age_ms'] for r in axes.values())
            self.gimbal_source = {axis: dict(record['source'], method=record['method'],
                                            age_ms=record['age_ms'], sample_utc_us=record['sample_utc_us'])
                                  for axis, record in axes.items()}
            self.gimbal_source['yaw_frame'] = 'vehicle'
        snapshot['named_values'] = {name: self.named_sample(name, utc_ms)
                                   for name in ('TMIN', 'TMAX', 'SIYI_LIDR', 'RFND')}
        low, high = (snapshot['named_values'][key] for key in ('TMIN', 'TMAX'))
        if (low and high and low['valid'] and high['valid'] and
                low['source']['system_id'] == high['source']['system_id'] and
                low['source']['component_id'] == high['source']['component_id'] and
                abs(low['sample_utc_us'] - high['sample_utc_us']) < 50000 and low['value'] > high['value']):
            # Old SIYI readers decoded negative int16 temperatures as uint16.
            # Preserve the logged value, but don't label e.g. 645 C as valid.
            low['valid'] = False
            low['invalid_reason'] = 'logged_minimum_exceeds_maximum'
        return snapshot


def load_tlog_telemetry(path, max_gap_ms=2000):
    try:
        from pymavlink import mavutil
    except ImportError as error:
        raise RuntimeError('Install pymavlink for --tlog: python3 -m pip install pymavlink') from error
    if not path.is_file():
        raise ValueError('tlog not found: %s' % path)
    log = mavutil.mavlink_connection(str(path))
    named, vehicle = {}, {}
    print('Reading telemetry from %s' % path, flush=True)
    try:
        while True:
            message = log.recv_match(type=['NAMED_VALUE_FLOAT', 'ATTITUDE', 'GLOBAL_POSITION_INT'])
            if message is None:
                break
            kind = message.get_type()
            stamp = message._timestamp * 1000
            if not math.isfinite(stamp) or stamp < 946684800000:
                continue  # a timestamped tlog with an absolute UTC clock is required
            if kind == 'NAMED_VALUE_FLOAT':
                name = message.name
                if isinstance(name, bytes):
                    name = name.decode('ascii', errors='replace')
                name = name.rstrip('\0')
                if name in ('CROLL', 'CPITCH', 'CYAW', 'TMIN', 'TMAX', 'SIYI_LIDR', 'RFND'):
                    key = (name, message.get_srcSystem(), message.get_srcComponent())
                    named.setdefault(key, []).append((stamp, message.value))
            else:
                key = (message.get_srcSystem(), message.get_srcComponent())
                vehicle.setdefault(key, {}).setdefault(kind, []).append((stamp, message))
    finally:
        log.close()

    def ordered(records):
        # A duplicate UTC stamp uses the last received value. Sorting also handles
        # small reception-clock regressions without making bisect unreliable.
        return sorted(dict(records).items())

    named = {key: tuple(zip(*ordered(records))) for key, records in named.items()}
    # Never combine vehicle poses from different aircraft/components. Choose the
    # sender with the most position samples (ATTITUDE count breaks ties).
    senders = [key for key, rows in vehicle.items() if rows.get('GLOBAL_POSITION_INT')]
    if len({key[0] for key in senders}) > 1:
        raise ValueError('tlog contains multiple aircraft; supply a log for one aircraft')
    rows = vehicle[max(vehicle, key=lambda k: (len(vehicle[k].get('GLOBAL_POSITION_INT', [])),
                                              len(vehicle[k].get('ATTITUDE', []))))] if vehicle else {}
    series = {}

    def add(name, kind, readers, angles=()):
        records = ordered(rows.get(kind, []))
        usable = [(t, [read(m) for read in readers.values()]) for t, m in records]
        if name != 'vehicle_rate':
            usable = [(t, v) for t, v in usable if all(math.isfinite(x) for x in v)]
        if usable:
            series[name] = ([t for t, _ in usable],
                            {key: [v[i] for _, v in usable] for i, key in enumerate(readers)}, set(angles))

    add('position', 'GLOBAL_POSITION_INT', {
        'lat_e7': lambda m: m.lat, 'lon_e7': lambda m: m.lon,
        'alt_amsl_m': lambda m: m.alt / 1000, 'alt_relative_m': lambda m: m.relative_alt / 1000})
    add('velocity', 'GLOBAL_POSITION_INT', {
        'vn_m_s': lambda m: m.vx / 100, 've_m_s': lambda m: m.vy / 100, 'vd_m_s': lambda m: m.vz / 100})
    add('vehicle_attitude', 'ATTITUDE', {
        'roll_rad': lambda m: m.roll, 'pitch_rad': lambda m: m.pitch, 'yaw_rad': lambda m: m.yaw}, ('yaw_rad',))
    add('vehicle_rate', 'ATTITUDE', {
        'yaw_rate_rad_s': lambda m: ((m.pitchspeed * math.sin(m.roll) + m.yawspeed * math.cos(m.roll)) /
                                   math.cos(m.pitch)) if abs(math.cos(m.pitch)) >= 1e-3 else float('nan')})
    if not named and not series:
        raise ValueError('tlog has no usable timestamped camera or vehicle telemetry')
    return TlogTelemetry(series, named, path.name, max_gap_ms)


class CombinedTelemetry:
    """Prefer dataflash vehicle pose and measured tlog gimbal pose, per frame."""

    def __init__(self, bin_log=None, tlog=None, max_gap_ms=2000):
        self.bin = load_bin_telemetry(bin_log, max_gap_ms) if bin_log else None
        self.tlog = load_tlog_telemetry(tlog, max_gap_ms) if tlog else None
        self.source_name = ', '.join(str(p) for p in (bin_log, tlog) if p)
        self.max_gap_ms = max_gap_ms
        self.gimbal_source = None

    def sample(self, utc_ms, pts_ms, existing=None, gimbal_source=None):
        base = self.bin.sample(utc_ms, pts_ms) if self.bin else dict(existing or {})
        self.gimbal_source = self.bin.gimbal_source if self.bin else gimbal_source
        other = self.tlog.sample(utc_ms, pts_ms) if self.tlog else {}
        base.update(schema=TELEMETRY_SCHEMA, utc_us=round(utc_ms * 1000), pts90k=round(pts_ms * 90))
        for key in ('position', 'velocity', 'vehicle_attitude', 'gimbal_attitude'):
            value = base.get(key)
            if value is not None and value.get('age_ms', float('inf')) > self.max_gap_ms:
                value = None
            if key == 'gimbal_attitude' and value is not None and not all(
                    axis + '_rad' in value for axis in ('roll', 'pitch', 'yaw')):
                value = None
            if (key == 'gimbal_attitude' and other.get(key)) or value is None:
                value = other.get(key)
                if key == 'gimbal_attitude':
                    self.gimbal_source = self.tlog.gimbal_source if self.tlog and value else None
            base[key] = value
        base['heading_rad'] = base['vehicle_attitude']['yaw_rad'] if base['vehicle_attitude'] else None
        base.setdefault('zoom', None)
        base.setdefault('hfov_deg', None)
        if self.tlog:
            base['named_values'] = other['named_values']
        return base


def inputs(directory, fps=None):
    if not directory.is_dir():
        raise ValueError('encoding input must be a directory')
    files = list(directory.glob('*.bin'))
    if not files:
        raise ValueError('no .bin thermal frames found')
    stamps = [filename_time(p) for p in files]
    use_names = all(t is not None for t in stamps)
    entries = []
    for path, stamp in zip(files, stamps):
        info = path.stat()
        if not path.is_file() or info.st_size != FRAME_BYTES:
            raise ValueError('%s: expected %u bytes, got %u' % (path, FRAME_BYTES, info.st_size))
        entries.append((stamp if use_names else info.st_mtime_ns//1000000, path, info.st_mtime_ns))
    entries.sort(key=lambda row: (row[0], row[1].name))
    first = entries[0][0]
    timestamps = [round(i*1000/fps) if fps else stamp-first for i, (stamp, _, _) in enumerate(entries)]
    differences = [b-a for a, b in zip(timestamps, timestamps[1:]) if b>a]
    interval_ms = 1000/fps if fps else statistics.median(differences) if differences else 200
    source = 'legacy_fixed_fps' if fps else 'legacy_filename' if use_names else 'legacy_file_mtime'
    return entries, timestamps, max(1, round(interval_ms*1000000)), source


def metadata_for_frame(path, data, pixels, number, count, pts_ms, mtime_ns, timing, rotation,
                       telemetry=None, gimbal_source=None):
    lo, hi = int(pixels.min()), int(pixels.max())
    extra = dict(gimbal_pose_source=gimbal_source) if telemetry is not None and gimbal_source else {}
    return dict(schema=SCHEMA, frame_id=number, capture_monotonic_us=pts_ms*1000+1,
                timestamp_source=timing, capture_clock='reconstructed_relative', simulated=False,
                width=WIDTH, height=HEIGHT, pixel_format='gray16le', bits_per_sample=16,
                temperature_scale_k=1/64, temperature_offset_k=0, gain=-1,
                minimum_raw=lo, maximum_raw=hi, minimum_c=lo/64-273.15, maximum_c=hi/64-273.15,
                rotation_deg=rotation, hfov_deg=24.2, calibration='nominal_pinhole',
                altitude_datum='AMSL', gimbal_frame='roll_pitch_level_yaw_vehicle', telemetry=telemetry,
                original_filename=path.name, source_mtime_ns=mtime_ns,
                source_sha256=hashlib.sha256(data).hexdigest(), archive_frame_count=count, **extra)


def dependencies():
    try:
        import av
        import numpy as np
    except ImportError as error:
        raise RuntimeError("Install dependencies: python3 -m pip install 'av>=18.1' numpy") from error
    return av, np


def encode(directory, output, fps=None, rotation=180, bin_log=None, tlog=None, max_gap_ms=2000):
    av, np = dependencies()
    entries, timestamps, duration_ns, timing = inputs(directory, fps)
    if output.exists():
        raise ValueError('output already exists: %s' % output)
    telemetry = CombinedTelemetry(bin_log, tlog, max_gap_ms) if bin_log or tlog else None
    codec = av.CodecContext.create('ffv1', 'w')
    codec.width, codec.height = WIDTH, HEIGHT
    codec.pix_fmt = 'gray16le'
    codec.time_base = Fraction(1, 1000)
    codec.gop_size, codec.thread_count = 1, 2
    codec.flags |= av.codec.context.Flags.global_header
    codec.options = {'level': '3', 'coder': '1', 'slicecrc': '1'}
    codec.open()
    if not codec.extradata:
        raise RuntimeError('FFV1 encoder did not supply CodecPrivate')
    output.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='.'+output.name+'.', suffix='.tmp', dir=output.parent)
    temporary = Path(name)
    start = last_report = time.monotonic()
    print('Encoding %u frames (%s timing%s) to %s' %
          (len(entries), timing,
           ', telemetry from %s' % telemetry.source_name if telemetry else '', output), flush=True)
    try:
        with os.fdopen(fd, 'wb') as dest:
            dest.write(matroska_header(codec.extradata, duration_ns, timestamps[-1]+duration_ns/1000000))
            for number, ((stamp, path, mtime_ns), pts) in enumerate(zip(entries, timestamps), 1):
                data = path.read_bytes()
                if len(data) != FRAME_BYTES or path.stat().st_mtime_ns != mtime_ns:
                    raise ValueError('input changed during encoding: %s' % path)
                pixels = np.frombuffer(data, dtype='<u2').reshape(HEIGHT, WIDTH)
                frame = av.VideoFrame.from_ndarray(pixels.astype(np.uint16, copy=False), format='gray16le')
                frame.pts, frame.time_base = pts, codec.time_base
                packets = codec.encode(frame)
                if len(packets) != 1 or not packets[0].is_keyframe:
                    raise RuntimeError('expected one independently coded FFV1 packet per frame')
                snapshot = telemetry.sample(stamp, pts) if telemetry else None
                gimbal_source = telemetry.gimbal_source if telemetry else None
                metadata = metadata_for_frame(path, data, pixels, number, len(entries), pts, mtime_ns,
                                              timing, rotation, snapshot, gimbal_source)
                dest.write(matroska_frame(bytes(packets[0]), metadata, pts))
                now = time.monotonic()
                if now-last_report >= 10 or number == len(entries):
                    print('  %u/%u frames, %.1f frames/s, %.1f MB written' %
                          (number, len(entries), number/(now-start), dest.tell()/1e6), flush=True)
                    last_report = now
            if codec.encode(None):
                raise RuntimeError('unexpected delayed FFV1 packets')
            dest.flush()
            os.fsync(dest.fileno())
        # Publish without replacing another process's output.
        os.link(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)
    raw_size, compressed = len(entries)*FRAME_BYTES, output.stat().st_size
    result = dict(frames=len(entries), raw_bytes=raw_size, matroska_bytes=compressed,
                  compression_ratio=raw_size/compressed, saving_percent=100*(1-compressed/raw_size))
    print('Encoded %u frames: %s raw bytes -> %s Matroska bytes; %.3f:1, %.2f%% smaller' %
          (len(entries), format(raw_size, ','), format(compressed, ','), result['compression_ratio'], result['saving_percent']), flush=True)
    return result


def packet_metadata(packet):
    if not hasattr(packet, 'get_sidedata'):
        raise RuntimeError('PyAV >=18.1 with packet side-data access is required')
    data = bytes(packet.get_sidedata('matroska_block_additional'))
    if not 8 < len(data) <= 65544 or int.from_bytes(data[:8], 'big') != ADDITION_TYPE:
        raise ValueError('missing or invalid APCG frame metadata')
    m = json.loads(data[8:])
    if not isinstance(m, dict) or m.get('schema') != SCHEMA:
        raise ValueError('unsupported thermal metadata schema')
    if (m.get('width'), m.get('height'), m.get('pixel_format'), m.get('bits_per_sample')) != (WIDTH, HEIGHT, 'gray16le', 16):
        raise ValueError('unsupported radiometric format')
    if type(m.get('frame_id')) is not int or m['frame_id'] < 1:
        raise ValueError('invalid frame ID')
    return m


def update_metadata(source, output, bin_log=None, tlog=None, max_gap_ms=2000):
    """Copy encoded FFV1 packets verbatim, replacing only their telemetry JSON."""
    av, _ = dependencies()
    if not source.is_file():
        raise ValueError('metadata update input must be a Matroska file')
    if output.exists():
        raise ValueError('output already exists: %s' % output)
    if not bin_log and not tlog:
        raise ValueError('metadata update needs --bin or --tlog')
    telemetry = CombinedTelemetry(bin_log, tlog, max_gap_ms)
    output.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='.' + output.name + '.', suffix='.tmp', dir=output.parent)
    temporary = Path(name)
    count, expected_count = 0, None
    coverage = {k: 0 for k in ('position', 'vehicle_attitude', 'gimbal_attitude', 'TMIN', 'TMAX', 'SIYI_LIDR')}
    start = last_report = time.monotonic()
    try:
        with os.fdopen(fd, 'wb') as dest, av.open(str(source)) as container:
            if len(container.streams.video) != 1 or container.streams.video[0].codec_context.name != 'ffv1':
                raise ValueError('expected one FFV1 video track')
            if len(container.streams) != 1:
                raise ValueError('metadata update only supports a single-track thermal archive')
            stream = container.streams.video[0]
            if not container.duration or not stream.codec_context.extradata:
                raise ValueError('missing archive duration or FFV1 CodecPrivate')
            duration_ns = round(1e9 / stream.average_rate) if stream.average_rate else 200000000
            dest.write(matroska_header(stream.codec_context.extradata, duration_ns, container.duration / 1000))
            for packet in container.demux(stream):
                if not packet.size:
                    continue
                if packet.is_corrupt or not packet.is_keyframe or packet.pts is None:
                    raise ValueError('expected intact independently coded FFV1 packets with timestamps')
                pts = packet.pts * packet.time_base * 1000
                if pts.denominator != 1 or pts < 0:
                    raise ValueError('expected nonnegative millisecond archive timestamps')
                pts = int(pts)
                metadata = packet_metadata(packet)
                total = metadata.get('archive_frame_count')
                if count == 0:
                    expected_count = total
                if (type(total) is not int or total < 1 or total != expected_count or
                        metadata['frame_id'] != count + 1):
                    raise ValueError('missing or inconsistent archive frame count/order')
                existing = metadata.get('telemetry')
                stamp_us = existing.get('utc_us') if isinstance(existing, dict) else None
                if not isinstance(stamp_us, (int, float)) or not math.isfinite(stamp_us) or stamp_us <= 0:
                    stamp = filename_time(Path(output_name(metadata)))
                    if stamp is None:
                        raise ValueError('frame has no absolute UTC timestamp')
                else:
                    stamp = stamp_us / 1000
                snapshot = telemetry.sample(stamp, pts, existing, metadata.get('gimbal_pose_source'))
                metadata['telemetry'] = snapshot
                if telemetry.gimbal_source:
                    metadata['gimbal_pose_source'] = telemetry.gimbal_source
                else:
                    metadata.pop('gimbal_pose_source', None)
                dest.write(matroska_frame(bytes(packet), metadata, pts))
                count += 1
                for key in coverage:
                    value = snapshot.get(key) if key in snapshot else snapshot.get('named_values', {}).get(key)
                    if value is not None and value.get('valid', True):
                        coverage[key] += 1
                now = time.monotonic()
                if now - last_report >= 10:
                    print('  %u frames updated, %.1f MB written' % (count, dest.tell()/1e6), flush=True)
                    last_report = now
            if not count or count != expected_count:
                raise ValueError('incomplete archive: read %u of %s frames' % (count, expected_count))
            dest.flush()
            os.fsync(dest.fileno())
        os.link(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)
    result = dict(frames=count, coverage=coverage, matroska_bytes=output.stat().st_size)
    print('Updated %u frames without re-encoding: %s' % (count, json.dumps(result)), flush=True)
    return result


def output_name(metadata):
    name = metadata.get('original_filename', '%012u.bin' % metadata['frame_id'])
    if (not isinstance(name, str) or not name or name in ('.', '..') or
            '/' in name or '\\' in name or '\0' in name or ':' in name or not name.endswith('.bin')):
        raise ValueError('unsafe original filename in thermal metadata')
    return name


def extract(source, output, save_metadata=False):
    av, _ = dependencies()
    if not source.is_file():
        raise ValueError('extraction input must be a Matroska file')
    if output.exists():
        raise ValueError('output directory already exists: %s' % output)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix='.'+output.name+'.', dir=output.parent))
    count = checked = 0
    expected_count = None
    start = last_report = time.monotonic()
    print('Extracting %s to %s' % (source, output), flush=True)
    try:
        with av.open(str(source)) as container:
            streams = container.streams.video
            if len(streams) != 1 or streams[0].codec_context.name != 'ffv1':
                raise ValueError('expected one FFV1 video track')
            stream = streams[0]
            stream.codec_context.options = {'err_detect': 'crccheck+explode'}
            if (stream.width, stream.height) != (WIDTH, HEIGHT):
                raise ValueError('unexpected image dimensions')
            for packet in container.demux(stream):
                if not packet.size:
                    continue
                if packet.is_corrupt:
                    raise ValueError('corrupt video packet')
                m = packet_metadata(packet)
                total = m.get('archive_frame_count')
                if count == 0:
                    expected_count = total
                if total != expected_count or (total is not None and (type(total) is not int or total < 1)):
                    raise ValueError('inconsistent archive frame count')
                if total is not None and m['frame_id'] != count+1:
                    raise ValueError('missing, duplicate or out-of-order archive frame')
                frames = packet.decode()
                if len(frames) != 1 or frames[0].format.name != 'gray16le' or (frames[0].width, frames[0].height) != (WIDTH, HEIGHT):
                    raise ValueError('decoder did not return one native gray16le frame')
                pixels = frames[0].to_ndarray()
                data = pixels.astype('<u2', copy=False).tobytes()
                digest = m.get('source_sha256')
                if digest is not None:
                    if digest != hashlib.sha256(data).hexdigest():
                        raise ValueError('raw pixel checksum mismatch at frame %u' % m['frame_id'])
                    checked += 1
                if m.get('minimum_raw') != int(pixels.min()) or m.get('maximum_raw') != int(pixels.max()):
                    raise ValueError('decoded pixels do not match frame range')
                dest = temporary/output_name(m)
                with dest.open('xb') as f:
                    f.write(data)
                mtime = m.get('source_mtime_ns')
                if mtime is not None:
                    if type(mtime) is not int:
                        raise ValueError('invalid original file timestamp')
                    os.utime(dest, ns=(mtime, mtime))
                if save_metadata:
                    with dest.with_suffix('.json').open('x') as f:
                        json.dump(m, f, indent=2, allow_nan=False)
                        f.write('\n')
                count += 1
                now = time.monotonic()
                if now-last_report >= 10:
                    print('  %u frames extracted, %.1f frames/s' % (count, count/(now-start)), flush=True)
                    last_report = now
        if not count or (expected_count is not None and count != expected_count):
            raise ValueError('incomplete archive: extracted %u of %s frames' % (count, expected_count))
        if output.exists():
            raise ValueError('output directory appeared during extraction')
        temporary.rename(output)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    print('Extracted %u frames (%s raw bytes); %u SHA-256 checks passed' %
          (count, format(count*FRAME_BYTES, ','), checked), flush=True)
    return dict(frames=count, raw_bytes=count*FRAME_BYTES, checksums_verified=checked)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--extract', action='store_true', help='extract raw .bin files instead of encoding')
    parser.add_argument('--update-metadata', action='store_true',
                        help='update an existing thermal archive without re-encoding its FFV1 video')
    parser.add_argument('--metadata', action='store_true', help='also extract per-frame .json sidecars')
    parser.add_argument('--fps', type=float, help='override capture timing with a fixed playback frame rate')
    parser.add_argument('--rotation', type=int, choices=(0, 180),
                        help='display rotation metadata; raw pixels are never rotated (default: 180)')
    parser.add_argument('--bin', dest='bin_log', type=Path,
                        help='ArduPilot dataflash log to reconstruct per-frame telemetry from')
    parser.add_argument('--tlog', type=Path,
                        help='timestamped MAVLink log for measured SIYI attitude, temperatures and range')
    parser.add_argument('--max-telemetry-gap', type=float, default=2.0, metavar='SECONDS',
                        help='maximum interpolation gap / named measurement age (default: 2 seconds)')
    args = parser.parse_args()
    if args.fps is not None and (not math.isfinite(args.fps) or not 0 < args.fps <= 1000):
        parser.error('--fps must be finite and between 0 and 1000')
    if args.extract and args.fps is not None:
        parser.error('--fps is only for encoding')
    if args.extract and args.bin_log is not None:
        parser.error('--bin is only for encoding')
    if args.extract and (args.tlog is not None or args.update_metadata):
        parser.error('--tlog and --update-metadata cannot be used with --extract')
    if args.update_metadata and args.fps is not None:
        parser.error('--fps cannot be used with --update-metadata')
    if args.rotation is not None and (args.extract or args.update_metadata):
        parser.error('--rotation is only for encoding raw frames')
    if not math.isfinite(args.max_telemetry_gap) or args.max_telemetry_gap <= 0:
        parser.error('--max-telemetry-gap must be finite and positive')
    if not args.extract and args.metadata:
        parser.error('--metadata is only for extraction')
    try:
        if args.extract:
            extract(args.input, args.output, args.metadata)
        elif args.update_metadata:
            update_metadata(args.input, args.output, args.bin_log, args.tlog, args.max_telemetry_gap * 1000)
        else:
            encode(args.input, args.output, args.fps, args.rotation if args.rotation is not None else 180,
                   args.bin_log, args.tlog,
                   args.max_telemetry_gap * 1000)
    except (OSError, ValueError, RuntimeError, EOFError) as error:
        parser.exit(1, 'thermal_to_video: %s\n' % error)


if __name__ == '__main__':
    main()
