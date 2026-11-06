#!/usr/bin/env python3
"""Remux an H.264/H.265 camera recording to MPEG-TS with per-frame MISB ST 0601 KLV.

Usage: video_to_ts.py INPUT.mp4 OUTPUT.ts --bin FLIGHT.bin [--start-utc TIME]
Requires PyAV >=18.1 and pymavlink. Existing outputs are never overwritten.

The video packets are copied unchanged (converted to Annex B, with parameter
sets before each random access point). Each frame gets a synchronous KLV cell
with the frame's PTS, in the layout of the camera's own MISB recorder
(camera_app/src/recording/misb_ts.cpp): video PID 0x101, metadata PID 0x102,
stream_type 0x15, stream_id 0xfc, ST 1402 metadata descriptors. Telemetry is
reconstructed from the dataflash log by thermal_to_video.py.

The recording's UTC start defaults to its modification time minus its duration
(SIYI cameras write no creation time and close the file when recording stops);
--start-utc sets it explicitly and --time-offset adjusts either.
"""
import argparse
import datetime
import math
import os
import re
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import thermal_to_video as thermal  # noqa: E402

VIDEO_PID, KLV_PID, PMT_PID = 0x101, 0x102, 0x1000
ZT30_WIDE_HFOV = 84.5  # SIYI published wide-lens HFOV, packaging/zt30/NATIVE_SETTINGS.md
KLV_DESCRIPTORS = bytes((
    0x05, 4, *b'KLVA',
    0x26, 9, 0x01, 0x00, 0xff, *b'KLVA', 0, 0x0f,
    0x27, 9, 0xc0, 0x02, 0x80, 0xc0, 0x00, 0x04, 0xc0, 0x00, 0x00,
))
CLOCK_MASK = (1 << 33) - 1


def crc32_mpeg(data):
    value = 0xffffffff
    for byte in data:
        value ^= byte << 24
        for _ in range(8):
            value = ((value << 1) ^ (0x04c11db7 if value & 0x80000000 else 0)) & 0xffffffff
    return value


def nal_type(codec, nal):
    return (nal[0] >> 1) & 63 if codec == 'hevc' else nal[0] & 31


def split_annexb(data):
    """NAL units of an Annex B byte stream, without start codes."""
    nals, at, start = [], 0, None
    while True:
        found = data.find(b'\0\0\1', at)
        if found < 0:
            break
        if start is not None:
            end = found
            while end > start and data[end-1] == 0:
                end -= 1
            nals.append(data[start:end])
        start = at = found + 3
    if start is not None and start < len(data):
        nals.append(data[start:])
    return [n for n in nals if n]


def split_lengths(data, size):
    nals, at = [], 0
    while at + size <= len(data):
        n = int.from_bytes(data[at:at+size], 'big')
        at += size
        if at + n > len(data):
            raise ValueError('truncated NAL unit in video packet')
        nals.append(data[at:at+n])
        at += n
    return nals


def parameter_sets(codec, extradata):
    """(NAL length size or 0 for Annex B, parameter set NALs) from hvcC/avcC extradata."""
    extradata = bytes(extradata or b'')
    if not extradata or extradata.startswith((b'\0\0\1', b'\0\0\0\1')):
        return 0, split_annexb(extradata)
    nals = []
    if codec == 'hevc':
        size, at = (extradata[21] & 3) + 1, 23
        for _ in range(extradata[22]):
            count = int.from_bytes(extradata[at+1:at+3], 'big')
            at += 3
            for _ in range(count):
                n = int.from_bytes(extradata[at:at+2], 'big')
                nals.append(extradata[at+2:at+2+n])
                at += 2 + n
    else:
        size, at = (extradata[4] & 3) + 1, 6
        for count in (extradata[5] & 31, None):
            if count is None:
                count, at = extradata[at], at + 1
            for _ in range(count):
                n = int.from_bytes(extradata[at:at+2], 'big')
                nals.append(extradata[at+2:at+2+n])
                at += 2 + n
    return size, nals


def access_unit(codec, nals, headers):
    """Annex B access unit as the camera writes it: AUD, then parameter sets at
    random access points (unless already in band), then the packet's NALs."""
    types = [nal_type(codec, n) for n in nals]
    if codec == 'hevc':
        key, aud, skip, ps = any(16 <= t <= 21 for t in types), b'\0\0\0\1\x46\x01\x50', 35, (32, 33, 34)
    else:
        key, aud, skip, ps = 5 in types, b'\0\0\0\1\x09\xf0', 9, (7, 8)
    # parameter sets in dependency order (VPS, SPS, PPS): in-band ones where present, otherwise from
    # the extradata at random access points
    sets = []
    for p in ps:
        inband = [n for n, t in zip(nals, types) if t == p]
        sets += inband or ([n for n in headers if nal_type(codec, n) == p] if key else [])
    out = [aud] + [b'\0\0\0\1' + n for n in sets]
    out += [b'\0\0\0\1' + n for n, t in zip(nals, types) if t != skip and t not in ps]
    return b''.join(out), key


def timestamp(prefix, value):
    value &= CLOCK_MASK
    return bytes((prefix | ((value >> 29) & 0x0e) | 1, (value >> 22) & 255,
                  ((value >> 14) & 0xfe) | 1, (value >> 7) & 255, ((value << 1) & 0xfe) | 1))


def pcr_field(value):
    value &= CLOCK_MASK
    return bytes((value >> 25 & 255, value >> 17 & 255, value >> 9 & 255, value >> 1 & 255,
                  ((value & 1) << 7) | 0x7e, 0))


class TransportWriter:
    """One program of video plus synchronous KLV, as misb_ts.cpp."""

    def __init__(self, out, codec):
        self.out, self.codec = out, codec
        self.continuity = [0, 0, 0, 0]  # PAT, PMT, video, KLV
        self.sequence = 0
        self.last_clock = None

    def _counter(self, index):
        value = self.continuity[index]
        self.continuity[index] = (value + 1) & 15
        return value

    def _section(self, pid, index, data):
        data = bytearray(data)
        length = len(data) + 1
        data[1], data[2] = 0xb0 | (length >> 8), length & 255
        data += crc32_mpeg(data).to_bytes(4, 'big')
        packet = bytes((0x47, 0x40 | (pid >> 8), pid & 255, 0x10 | self._counter(index), 0)) + data
        return packet + b'\xff' * (188 - len(packet))

    def _pes(self, pid, index, stream_id, payload, pts, dts, key, pcr):
        if dts is None or dts == pts:
            fields = timestamp(0x20, pts)
            flags = 0x80
        else:
            fields = timestamp(0x30, pts) + timestamp(0x10, dts)
            flags = 0xc0
        size = 0 if stream_id == 0xe0 else len(payload) + 3 + len(fields)
        data = bytes((0, 0, 1, stream_id, size >> 8, size & 255, 0x84, flags, len(fields))) + fields + payload
        view, parts, at, total = memoryview(data), [], 0, len(data)
        while at < total:
            first = at == 0
            with_pcr = first and pcr is not None
            n = min(total - at, 176 if with_pcr else 184)
            b1, b2, cc = (pid >> 8) | (0x40 if first else 0), pid & 255, self._counter(index)
            if n < 184:
                stuffing = 183 - n
                head = bytes((0x47, b1, b2, 0x30 | cc, stuffing))
                if stuffing:
                    field = bytes(((0x10 if with_pcr else 0) | (0x40 if key and first else 0),))
                    if with_pcr:
                        field += pcr_field(pcr)
                    head += field + b'\xff' * (stuffing - len(field))
                parts.append(head)
            else:
                parts.append(bytes((0x47, b1, b2, 0x10 | cc)))
            parts.append(view[at:at+n])
            at += n
        return b''.join(parts)

    def write(self, au, pts, dts, key, klv):
        clock = pts if dts is None else dts
        pmt = bytes((2, 0, 0, 0, 1, 0xc1, 0, 0, 0xe0 | VIDEO_PID >> 8, VIDEO_PID & 255, 0xf0, 0,
                     0x24 if self.codec == 'hevc' else 0x1b, 0xe0 | VIDEO_PID >> 8, VIDEO_PID & 255, 0xf0, 0,
                     0x15, 0xe0 | KLV_PID >> 8, KLV_PID & 255, 0xf0, len(KLV_DESCRIPTORS))) + KLV_DESCRIPTORS
        parts = [self._section(0, 0, (0, 0, 0, 0, 1, 0xc1, 0, 0, 0, 1, 0xe0 | PMT_PID >> 8, PMT_PID & 255)),
                 self._section(PMT_PID, 1, pmt)]
        # PCR-only packets keep the PCR interval within 20 ms.
        if self.last_clock is not None and 0 < clock - self.last_clock < 90000:
            fill = self.last_clock + 1800
            while fill + 1800 < clock:
                packet = bytes((0x47, VIDEO_PID >> 8, VIDEO_PID & 255,
                                0x20 | ((self.continuity[2] - 1) & 15), 183, 0x10)) + pcr_field(fill)
                parts.append(packet + b'\xff' * (188 - len(packet)))
                fill += 1800
        if klv:
            cell = bytes((0, self.sequence, 0xdf, len(klv) >> 8, len(klv) & 255)) + klv
            self.sequence = (self.sequence + 1) & 255
            parts.append(self._pes(KLV_PID, 3, 0xfc, cell, pts, None, True, None))
        parts.append(self._pes(VIDEO_PID, 2, 0xe0, au, pts, dts, key, clock))
        self.out.write(b''.join(parts))
        self.last_clock = clock


def parse_utc(value):
    """ISO 8601 time; a time without a zone is UTC. Returns Unix milliseconds."""
    # Python 3.10 only accepts 3 or 6 fractional digits
    value = re.sub(r'(\.\d{1,6})\d*', lambda m: m.group(1).ljust(7, '0'), value.replace('Z', '+00:00'), count=1)
    date = datetime.datetime.fromisoformat(value)
    if date.tzinfo is None:
        date = date.replace(tzinfo=datetime.timezone.utc)
    return date.timestamp() * 1000.0


def format_utc(utc_ms):
    return datetime.datetime.fromtimestamp(utc_ms / 1000.0, datetime.timezone.utc).isoformat(timespec='milliseconds')


def recording_start(path, duration_s, start_utc=None, offset_s=0.0):
    """Start UTC ms: explicit, else file modification time minus duration."""
    if start_utc is not None:
        base, source = parse_utc(start_utc), 'explicit'
    else:
        base, source = path.stat().st_mtime_ns / 1e6 - duration_s * 1000.0, 'mtime - duration'
    return base + offset_s * 1000.0, source


def convert(source, output, bin_log, start_utc=None, offset_s=0.0, hfov=ZT30_WIDE_HFOV,
            model='SIYI ZT30 wide', max_gap_ms=2000, gimbal=None):
    av, _ = thermal.dependencies()
    if output.exists():
        raise ValueError('output already exists: %s' % output)
    with av.open(str(source)) as container:
        stream = container.streams.video[0]
        codec = stream.codec_context.name
        if codec not in ('hevc', 'h264'):
            raise ValueError('%s: unsupported video codec %s' % (source, codec))
        width, height = stream.codec_context.width, stream.codec_context.height
        duration_s = float(stream.duration * stream.time_base) if stream.duration else \
            container.duration / 1e6
    start_ms, start_source = recording_start(source, duration_s, start_utc, offset_s)
    print('%s: %s %dx%d, %.3f s, start %s (%s, offset %+.3f s)' % (
        source.name, codec, width, height, duration_s, format_utc(start_ms), start_source, offset_s), flush=True)
    telemetry = thermal.load_bin_telemetry(bin_log, max_gap_ms, gimbal)
    partial = output.with_name('.%s.partial' % output.name)
    counts = dict(frames=0, klv=0, position=0, vehicle_attitude=0, gimbal_attitude=0)
    origin = None
    out = open(partial, 'xb', buffering=1 << 22)  # raises if another conversion owns it
    try:
        with av.open(str(source)) as container, out:
            stream = container.streams.video[0]
            origin = stream.start_time  # media time of the first frame, which start_utc refers to
            length_size, headers = parameter_sets(codec, stream.codec_context.extradata)
            writer = TransportWriter(out, codec)
            started = False
            for packet in container.demux(stream):
                if packet.pts is None or not packet.size:
                    continue
                data = bytes(packet)
                nals = split_lengths(data, length_size) if length_size else split_annexb(data)
                au, key = access_unit(codec, nals, headers)
                if not started and not key:
                    continue
                started = True
                origin = packet.pts if origin is None else origin
                seconds = float((packet.pts - origin) * packet.time_base)
                pts = round(seconds * 90000)
                dts = None if packet.dts is None or packet.dts == packet.pts else \
                    round(float((packet.dts - origin) * packet.time_base) * 90000)
                pts_ms = seconds * 1000.0
                snapshot = telemetry.sample(start_ms + pts_ms, pts_ms)
                klv = thermal.misb_klv(snapshot, hfov, width, height, model)
                writer.write(au, pts, dts, key, klv)
                counts['frames'] += 1
                counts['klv'] += klv is not None
                for name in ('position', 'vehicle_attitude', 'gimbal_attitude'):
                    counts[name] += snapshot.get(name) is not None
        os.link(partial, output)  # fails rather than replacing an existing file
    finally:
        out.close()
        partial.unlink(missing_ok=True)
    print('%s: %d frames, %d KLV, position %d, vehicle attitude %d, gimbal attitude %d, %.1f MB' % (
        output.name, counts['frames'], counts['klv'], counts['position'], counts['vehicle_attitude'],
        counts['gimbal_attitude'], output.stat().st_size / 1e6), flush=True)
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--bin', dest='bin_log', type=Path, required=True,
                        help='ArduPilot dataflash log to reconstruct per-frame telemetry from')
    parser.add_argument('--start-utc', metavar='TIME',
                        help='UTC of the first frame, ISO 8601 (default: modification time minus duration)')
    parser.add_argument('--time-offset', type=float, default=0.0, metavar='SECONDS',
                        help='added to the recording start time (default: 0)')
    parser.add_argument('--hfov', type=float, default=ZT30_WIDE_HFOV, metavar='DEGREES',
                        help='horizontal FOV for KLV tags 16/17, 0 to omit (default: %g, ZT30 wide)' % ZT30_WIDE_HFOV)
    parser.add_argument('--model', default='SIYI ZT30 wide', help='KLV image source sensor (tag 11)')
    parser.add_argument('--max-telemetry-gap', type=float, default=2.0, metavar='SECONDS',
                        help='maximum interpolation gap (default: 2 seconds)')
    parser.add_argument('--gimbal-source', choices=('mnt', 'siat'), default='mnt',
                        help='dataflash gimbal attitude: MNT, or SIYI_control.lua SIAT (default: mnt)')
    parser.add_argument('--gimbal-yaw-offset', type=float, default=0.0, metavar='DEGREES',
                        help='subtracted from SIAT yaw to make it nose-relative (default: 0)')
    args = parser.parse_args()
    if not math.isfinite(args.time_offset):
        parser.error('--time-offset must be finite')
    if not math.isfinite(args.hfov) or not 0 <= args.hfov < 180:
        parser.error('--hfov must be between 0 and 180 degrees')
    if not math.isfinite(args.max_telemetry_gap) or args.max_telemetry_gap <= 0:
        parser.error('--max-telemetry-gap must be finite and positive')
    if args.gimbal_yaw_offset and args.gimbal_source != 'siat':
        parser.error('--gimbal-yaw-offset applies to --gimbal-source siat')
    if args.start_utc is not None:
        try:
            parse_utc(args.start_utc)
        except ValueError:
            parser.error('--start-utc must be an ISO 8601 time')
    gimbal = dict(source=args.gimbal_source, yaw_offset_deg=args.gimbal_yaw_offset)
    try:
        convert(args.input, args.output, args.bin_log, args.start_utc, args.time_offset,
                args.hfov or None, args.model, args.max_telemetry_gap * 1000, gimbal)
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(1, 'video_to_ts: %s\n' % error)


if __name__ == '__main__':
    main()
