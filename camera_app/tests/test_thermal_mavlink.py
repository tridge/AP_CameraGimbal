#!/usr/bin/env python3
"""Verify thermal capability, requests and streaming over the real MAVLink server."""
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from test_mavlink_integration import (
    command, mavutil, request_message, reserve_tcp_udp_port, terminate, wait_path,
)

M = mavutil.mavlink
CAMERA = 101  # Exercise the configured identity rather than assuming camera 100.
THERMAL = M.MAVLINK_MSG_ID_CAMERA_THERMAL_RANGE


def interval(link, value, stream=0, device=0, result=M.MAV_RESULT_ACCEPTED):
    ack, _ = command(link, CAMERA, M.MAV_CMD_SET_MESSAGE_INTERVAL,
                     [THERMAL, value, stream, device])
    assert ack.result == result, ack


def drain(link):
    while link.recv_match(blocking=False) is not None:
        pass


def samples(link, count):
    found = []
    deadline = time.monotonic() + count * 2 + 2
    while len(found) < count and time.monotonic() < deadline:
        m = link.recv_match(type='CAMERA_THERMAL_RANGE', blocking=True, timeout=1)
        if m is not None:
            assert (m.get_srcSystem(), m.get_srcComponent(), m.camera_device_id) == (1, CAMERA, 0), m
            found.append(m)
    assert len(found) == count, found
    return found


def check_sample(m, stream, rotated=True):
    assert m.stream_id == stream, m
    assert math.isclose(m.max, 42.0, abs_tol=0.001), m
    assert math.isclose(m.min, 12.0, abs_tol=0.001), m
    for actual, expected in ((m.max_point_x, 100 / 639), (m.max_point_y, 200 / 511),
                             (m.min_point_x, 10 / 639), (m.min_point_y, 20 / 511)):
        assert math.isclose(actual, 1-expected if rotated else expected, abs_tol=1e-6), m


def check(link, thermal):
    info = request_message(link, CAMERA, M.MAVLINK_MSG_ID_CAMERA_INFORMATION, 'CAMERA_INFORMATION')
    assert bool(info.flags & M.CAMERA_CAP_FLAGS_HAS_THERMAL_RANGE) == thermal, info
    for stream in (1, 2):
        for kind in ('VIDEO_STREAM_INFORMATION', 'VIDEO_STREAM_STATUS'):
            status = request_message(link, CAMERA, getattr(M, 'MAVLINK_MSG_ID_' + kind), kind, stream)
            assert bool(status.flags & M.VIDEO_STREAM_STATUS_FLAGS_THERMAL_RANGE_ENABLED) == (
                thermal and stream == 2), status
    if not thermal:
        ack, _ = command(link, CAMERA, M.MAV_CMD_REQUEST_MESSAGE, [THERMAL])
        assert ack.result == M.MAV_RESULT_UNSUPPORTED, ack
        interval(link, 200000, result=M.MAV_RESULT_UNSUPPORTED)
        assert link.recv_match(type='CAMERA_THERMAL_RANGE', blocking=True, timeout=.4) is None
        return

    check_sample(samples(link, 1)[0], 2)  # Default streaming works without a subscription.
    interval(link, -1)
    drain(link)
    assert link.recv_match(type='CAMERA_THERMAL_RANGE', blocking=True, timeout=.4) is None
    for selection in (0, 2):
        report = request_message(link, CAMERA, THERMAL, 'CAMERA_THERMAL_RANGE', selection)
        check_sample(report, 2)
    ack, _ = command(link, CAMERA, M.MAV_CMD_REQUEST_MESSAGE, [THERMAL, 1])
    assert ack.result == M.MAV_RESULT_UNSUPPORTED, ack
    for selection, device in ((-1, 0), (.5, 0), (3, 0), (math.nan, 0), (2, 1)):
        ack, _ = command(link, CAMERA, M.MAV_CMD_REQUEST_MESSAGE, [THERMAL, selection, device])
        assert ack.result == M.MAV_RESULT_DENIED, ack
    for value, selection, device in ((math.nan, 0, 0), (math.inf, 0, 0), (-2, 0, 0),
                                     (-.5, 0, 0), (1e12, 0, 0), (200000, -.5, 0),
                                     (200000, 3, 0), (200000, 0, 1)):
        interval(link, value, selection, device, result=M.MAV_RESULT_DENIED)
    interval(link, 200000, 1, result=M.MAV_RESULT_UNSUPPORTED)
    interval(link, 1000000, 2)
    slow = samples(link, 3)
    assert all(b.time_boot_ms - a.time_boot_ms >= 990 for a, b in zip(slow, slow[1:])), slow
    interval(link, 0, 2)  # Restore the 5 Hz default.
    normal = samples(link, 4)
    assert all(190 <= b.time_boot_ms - a.time_boot_ms < 700
               for a, b in zip(normal, normal[1:])), normal
    interval(link, 1, 2)  # Clamp to the thermal frame rate, never a busy loop.
    fast = samples(link, 4)
    assert all(b.time_boot_ms - a.time_boot_ms >= 39 for a, b in zip(fast, fast[1:])), fast
    interval(link, -1, 2)
    drain(link)
    assert link.recv_match(type='CAMERA_THERMAL_RANGE', blocking=True, timeout=.4) is None

    ack, _ = command(link, CAMERA, M.MAV_CMD_SET_CAMERA_SOURCE, [0, 2])
    assert ack.result == M.MAV_RESULT_ACCEPTED, ack
    report = request_message(link, CAMERA, THERMAL, 'CAMERA_THERMAL_RANGE')
    check_sample(report, 1)
    for stream in (1, 2):
        status = request_message(link, CAMERA, M.MAVLINK_MSG_ID_VIDEO_STREAM_STATUS,
                                 'VIDEO_STREAM_STATUS', stream)
        assert bool(status.flags & M.VIDEO_STREAM_STATUS_FLAGS_THERMAL_RANGE_ENABLED) == (stream == 1), status
    interval(link, 200000, 0)
    for report in samples(link, 2):
        check_sample(report, 1)
    interval(link, -1)


def check_measurement_edges(link, fixture):
    interval(link, -1)  # Isolate requested samples from queued broadcasts.
    for rotated in (False, True):
        fixture.write_text(f'70000 -4000 0 0 639 511 {time.monotonic_ns() // 1000} {int(rotated)}')
        report = request_message(link, CAMERA, THERMAL, 'CAMERA_THERMAL_RANGE')
        assert report.max == 700 and report.min == -40, report
        assert report.max_point_x == report.max_point_y == int(rotated), report
        assert report.min_point_x == report.min_point_y == int(not rotated), report
    interval(link, 0)
    time.sleep(.4)  # The frozen frame must expire while the app remains alive.
    drain(link)
    assert link.recv_match(type='CAMERA_THERMAL_RANGE', blocking=True, timeout=.4) is None
    ack, _ = command(link, CAMERA, M.MAV_CMD_REQUEST_MESSAGE, [THERMAL])
    assert ack.result == M.MAV_RESULT_TEMPORARILY_REJECTED, ack
    fixture.write_text(f'70000 -4000 0 0 639 511 {time.monotonic_ns() // 1000} 0')
    report = request_message(link, CAMERA, THERMAL, 'CAMERA_THERMAL_RANGE')
    assert report.min == -40  # Publishing recovers on a fresh frame.
    interval(link, -1)


def main():
    if len(sys.argv) != 4 or sys.argv[3] not in ('thermal', 'rgb'):
        raise SystemExit('usage: test_thermal_mavlink.py CAMERA_APP GIMBAL_SIM thermal|rgb')
    binary, simulator = (str(Path(arg).resolve()) for arg in sys.argv[1:3])
    thermal = sys.argv[3] == 'thermal'
    vendor_port, mav_port, gimbal_port = [reserve_tcp_udp_port() for _ in range(3)]
    with tempfile.TemporaryDirectory(prefix='thermal-mavlink-') as directory:
        root = Path(directory)
        config, ready, gimbal_ready = (root / name for name in ('camera.ini', 'ready', 'gimbal.ready'))
        config.write_text('[mavlink]\nsystem_id=1\ncamera_component_id=101\n')
        env = dict(os.environ, CAMERA_APP_UART=f'udp://127.0.0.1:{gimbal_port}',
                   CAMERA_APP_PORT=str(vendor_port), CAMERA_APP_CONFIG=str(config),
                   CAMERA_APP_MAVLINK_TCP_PORT=str(mav_port), CAMERA_APP_MAVLINK_UDP_PORT=str(mav_port),
                   CAMERA_APP_READY_PATH=str(ready), CAMERA_APP_LOG_ROOT=str(root / 'logs'),
                   CAMERA_APP_CAPTURE_ROOT=str(root / 'capture'), CAMERA_APP_RECORD_ROOT=str(root / 'record'))
        fixture = root / 'thermal.range'
        fixture.write_text(f'4200 1200 100 200 10 20 {time.monotonic_ns() // 1000} 1')
        camera = gimbal = link = udp = None
        with (root / 'test.log').open('w+') as log:
            try:
                gimbal = subprocess.Popen([sys.executable, simulator, '--port', str(gimbal_port),
                    '--ready-file', str(gimbal_ready)], stdout=log, stderr=log)
                wait_path(gimbal_ready, gimbal)
                camera = subprocess.Popen([binary], env=env, stdout=log, stderr=log)
                wait_path(ready, camera)
                link = mavutil.mavlink_connection(f'tcp:127.0.0.1:{mav_port}',
                                                  source_system=42, source_component=190)
                check(link, thermal)
                udp = mavutil.mavlink_connection(f'udpout:127.0.0.1:{mav_port}',
                                                 source_system=43, source_component=190)
                if thermal:
                    report = request_message(udp, CAMERA, THERMAL, 'CAMERA_THERMAL_RANGE')
                    check_sample(report, 1)
                else:
                    ack, _ = command(udp, CAMERA, M.MAV_CMD_REQUEST_MESSAGE, [THERMAL])
                    assert ack.result == M.MAV_RESULT_UNSUPPORTED, ack
                if thermal:
                    link.close(); link = None
                    udp.close(); udp = None
                    terminate(camera); camera = None
                    ready.unlink(missing_ok=True)
                    env['CAMERA_APP_TEST_THERMAL_RANGE'] = str(fixture)
                    camera = subprocess.Popen([binary], env=env, stdout=log, stderr=log)
                    wait_path(ready, camera)
                    link = mavutil.mavlink_connection(f'tcp:127.0.0.1:{mav_port}', source_system=42, source_component=190)
                    check_measurement_edges(link, fixture)
                print(f'PASS {Path(binary).name}: thermal={thermal}, capability, requests, rotation, signed range, freshness and TCP/UDP')
            except BaseException:
                log.flush()
                log.seek(0)
                print(log.read())
                raise
            finally:
                for connection in (link, udp):
                    if connection is not None:
                        connection.close()
                terminate(camera)
                terminate(gimbal)


if __name__ == '__main__':
    main()
