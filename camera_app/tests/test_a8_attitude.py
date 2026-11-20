#!/usr/bin/env python3
"""Verify that inverted A8 attitude is mount-relative on SIYI and MAVLink."""

import math
import os
import pathlib
import pty
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

os.environ.setdefault("MAVLINK20", "1")

from pymavlink import mavutil  # noqa: E402
from pymavlink.quaternion import Quaternion  # noqa: E402


def crc16(data):
    value = 0
    for byte in data:
        value ^= byte << 8
        for _ in range(8):
            value = (((value << 1) ^ 0x1021) & 0xFFFF
                     if value & 0x8000 else (value << 1) & 0xFFFF)
    return value


def crc8(data):
    value = 0
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = ((value >> 1) ^ 0x8C) if value & 1 else value >> 1
    return value


def siyi(control, sequence, opcode, payload=b""):
    frame = (struct.pack("<BBBHHB", 0x55, 0x66, control, len(payload),
                         sequence, opcode) + payload)
    return frame + struct.pack("<H", crc16(frame))


def a8_frame(flags, sequence, subcommand, payload=b""):
    header = bytearray([
        0xAA, flags, 0x02, len(payload), 0,
        sequence & 0xFF, sequence >> 8, 0x2E, 0x2C, 0x6B, subcommand,
    ])
    header[4] = crc8(header[:4])
    frame = bytes(header) + payload
    return frame + struct.pack("<H", crc16(frame))


def read_a8_frames(fd, count, timeout=3.0):
    data = bytearray()
    frames = []
    deadline = time.monotonic() + timeout
    while len(frames) < count and time.monotonic() < deadline:
        readable, _, _ = select.select([fd], [], [], deadline - time.monotonic())
        if not readable:
            break
        data.extend(os.read(fd, 4096))
        while len(data) >= 13:
            if data[0] != 0xAA:
                del data[0]
                continue
            total = 13 + data[3]
            if len(data) < total:
                break
            frame = bytes(data[:total])
            del data[:total]
            assert frame[4] == crc8(frame[:4])
            assert struct.unpack_from("<H", frame, total - 2)[0] == crc16(frame[:-2])
            frames.append(frame)
    return frames


def reserve_port(socktype):
    sock = socket.socket(socket.AF_INET, socktype)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def wait_path(path, process, timeout=8.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        if process.poll() is not None:
            raise AssertionError(f"camera app exited {process.returncode}")
        time.sleep(0.02)
    raise TimeoutError(path)


def terminate(process):
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def check_mount_commands(master, mavlink, client, inverted):
    """Check actual MCU wire commands against hardware-observed A8 signs."""
    def next_command(opcode):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            for frame in read_a8_frames(master, 1, deadline - time.monotonic()):
                if frame[10] == 0x16 and frame[18] == opcode:
                    return frame[19:-4]  # Strip private and public headers/CRCs.
        raise AssertionError(f'No SIYI command {opcode:#x} received')

    # MAVProxy's device command (also the output of the FC gimbal manager).
    for pitch, yaw in ((10, 20), (-20, -30)):
        q = Quaternion([0, math.radians(pitch), math.radians(yaw)]).q
        mavlink.mav.gimbal_device_set_attitude_send(
            1, mavutil.mavlink.MAV_COMP_ID_GIMBAL, 0, q,
            math.nan, math.nan, math.nan)
        expected = (yaw * 10, -pitch * 10) if inverted else (-yaw * 10, pitch * 10)
        actual = struct.unpack('<hh', next_command(0x0e))
        assert actual == expected, (inverted, pitch, yaw, actual, expected)

    # Rate and angle commands use different conventions in the A8 controller.
    mavlink.mav.gimbal_device_set_attitude_send(
        1, mavutil.mavlink.MAV_COMP_ID_GIMBAL, 0, [math.nan] * 4,
        math.nan, math.radians(6), math.radians(-12))
    actual_rate = struct.unpack('<bb', next_command(0x07))
    # Calibrated A8 rate curves map -12 deg/s yaw and +6 deg/s pitch.
    assert actual_rate == (-16, 8), actual_rate

    # Public SIYI commands retain vendor signs, bypassing MAVLink transforms.
    payload = struct.pack('<hh', 200, -100)
    client.send(siyi(1, 30, 0x0e, payload))
    assert next_command(0x0e) == payload
    client.send(siyi(1, 31, 0x07, struct.pack('<bb', -20, 10)))
    actual_rate = struct.unpack('<bb', next_command(0x07))
    assert actual_rate == (-20, 10), actual_rate


def test_upright_yaw(binary):
    """Check both A8 yaw protocols in an explicitly upright mounting."""
    master, slave = pty.openpty()
    siyi_port = reserve_port(socket.SOCK_DGRAM)
    mavlink_port = reserve_port(socket.SOCK_STREAM)
    with tempfile.TemporaryDirectory() as directory:
        temporary = pathlib.Path(directory)
        ready = temporary / "ready"
        config = temporary / "camera.ini"
        config.write_text(
            "[general]\ntimezone = UTC\n"
            "[mount]\norientation = upright\n"
            "[uart]\nprotocol = none\n"
            "[mavlink]\nsystem_id = 1\n",
            encoding="ascii",
        )
        env = os.environ.copy()
        env["CAMERA_APP_READY_PATH"] = str(ready)
        env["CAMERA_APP_MAVLINK_TCP_PORT"] = str(mavlink_port)
        env["CAMERA_APP_MAVLINK_UDP_PORT"] = "0"
        process = subprocess.Popen(
            [binary, "--backend", "a8", "--uart", os.ttyname(slave),
             "--port", str(siyi_port), "--config", str(config)],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            wait_path(ready, process)
            assert len(read_a8_frames(master, 4)) == 4

            client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            client.settimeout(2)
            client.connect(("127.0.0.1", siyi_port))
            request = siyi(1, 20, 0x0D)
            client.send(request)
            tunneled = read_a8_frames(master, 1)
            assert tunneled[0][10] == 0x16

            # SIYI clients retain vendor signs; MAVLink is normalized separately.
            siyi_raw = struct.pack("<hhhhhh", -122, 100, 0, -3, 0, 0)
            reply = siyi(2, 101, 0x0D, siyi_raw)
            os.write(master, a8_frame(0x0A, 60, 0x16, reply))
            corrected = client.recv(2048)
            assert struct.unpack_from("<hhhhhh", corrected, 8) == (
                -122, 100, 0, -3, 0, 0
            )

            mavlink = mavutil.mavlink_connection(
                f"tcp:127.0.0.1:{mavlink_port}", source_system=250)
            mavlink.mav.heartbeat_send(
                mavutil.mavlink.MAV_TYPE_GCS,
                mavutil.mavlink.MAV_AUTOPILOT_INVALID,
                0, 0, mavutil.mavlink.MAV_STATE_ACTIVE,
            )
            # Upright private 0x50 yaw is already positive right.
            private_raw = struct.pack("<hhh", 122, 100, 0)
            os.write(master, a8_frame(0x0A, 61, 0x50, private_raw))
            status = mavlink.recv_match(
                type="GIMBAL_DEVICE_ATTITUDE_STATUS", blocking=True, timeout=3)
            assert status is not None
            roll, pitch, yaw = [math.degrees(value)
                                for value in Quaternion(status.q).euler]
            assert abs(roll) < 0.1
            assert abs(pitch - 10.0) < 0.1
            assert abs(yaw - 12.2) < 0.1
            assert abs(math.degrees(status.angular_velocity_z) - 0.3) < 0.1
            check_mount_commands(master, mavlink, client, inverted=False)
            mavlink.close()
            client.close()
        finally:
            terminate(process)
            stdout, stderr = process.communicate(timeout=1)
            os.close(master)
            os.close(slave)
            if process.returncode != 0:
                raise AssertionError(
                    f"camera app exited {process.returncode}\n{stdout}\n{stderr}")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_a8_attitude.py CAMERA_APP_A8_HOST")

    master, slave = pty.openpty()
    siyi_port = reserve_port(socket.SOCK_DGRAM)
    mavlink_port = reserve_port(socket.SOCK_STREAM)
    with tempfile.TemporaryDirectory() as directory:
        temporary = pathlib.Path(directory)
        ready = temporary / "ready"
        config = temporary / "camera.ini"
        config.write_text(
            "[general]\ntimezone = UTC\n"
            "[mount]\norientation = auto\n"
            "[uart]\nprotocol = none\n"
            "[mavlink]\nsystem_id = 1\n",
            encoding="ascii",
        )
        env = os.environ.copy()
        env["CAMERA_APP_READY_PATH"] = str(ready)
        env["CAMERA_APP_MAVLINK_TCP_PORT"] = str(mavlink_port)
        env["CAMERA_APP_MAVLINK_UDP_PORT"] = "0"
        process = subprocess.Popen(
            [sys.argv[1], "--backend", "a8", "--uart", os.ttyname(slave),
             "--port", str(siyi_port), "--config", str(config)],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            wait_path(ready, process)
            startup = read_a8_frames(master, 4)
            assert [(frame[1], frame[10]) for frame in startup] == [
                (0x09, 0x14), (0x09, 0x15), (0x09, 0x17), (0x08, 0x0D),
            ]

            # The startup mounting reply can be lost. Auto mode must retry it.
            retry = read_a8_frames(master, 1, timeout=2.0)
            assert len(retry) == 1
            assert (retry[0][1], retry[0][10]) == (0x09, 0x17)

            # Even with no mounting reply, an out-of-range raw pitch uniquely
            # identifies the A8's inverted-mount attitude representation.
            private_raw = struct.pack("<hhh", 3478, 1577, 0)
            os.write(master, a8_frame(0x0A, 50, 0x50, private_raw))

            # A delayed stale startup reply must not undo the inference.
            os.write(master, a8_frame(0x0A, 51, 0x17, b"\x01"))

            client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            client.settimeout(2)
            client.connect(("127.0.0.1", siyi_port))

            client.send(siyi(1, 6, 0x0A))
            mounting_status = client.recv(2048)
            assert mounting_status[7] == 0x0A
            assert mounting_status[13] == 2

            request = siyi(1, 7, 0x0D)
            client.send(request)
            tunneled = read_a8_frames(master, 1)
            assert tunneled[0][10] == 0x16 and tunneled[0][11:-2] == request

            # An inverted camera pitched physically up by 22.3 degrees is
            # reported by this A8 controller as +157.7 degrees.
            # In an inverted mounting SIYI yaw/rate are already positive right.
            # Private sub-command 0x50 above reports the same -12.2 degree yaw
            # as 347.8 degrees, which must be wrapped rather than reversed.
            siyi_raw = struct.pack("<hhhhhh", -122, 1577, 0, -3, 4, -2)
            reply = siyi(2, 100, 0x0D, siyi_raw)
            os.write(master, a8_frame(0x0A, 52, 0x16, reply))
            corrected = client.recv(2048)
            reply_crc = struct.unpack_from("<H", corrected,
                                           len(corrected) - 2)[0]
            assert reply_crc == crc16(corrected[:-2])
            assert struct.unpack_from("<hhhhhh", corrected, 8) == (
                -122, 1577, 0, -3, 4, -2
            )

            mavlink = mavutil.mavlink_connection(
                f"tcp:127.0.0.1:{mavlink_port}", source_system=250)
            mavlink.mav.heartbeat_send(
                mavutil.mavlink.MAV_TYPE_GCS,
                mavutil.mavlink.MAV_AUTOPILOT_INVALID,
                0, 0, mavutil.mavlink.MAV_STATE_ACTIVE,
            )
            os.write(master, a8_frame(0x0A, 53, 0x50, private_raw))
            status = mavlink.recv_match(
                type="GIMBAL_DEVICE_ATTITUDE_STATUS", blocking=True, timeout=3)
            assert status is not None
            roll, pitch, yaw = [math.degrees(value)
                                for value in Quaternion(status.q).euler]
            assert abs(roll) < 0.1
            assert abs(pitch - 22.3) < 0.1
            assert abs(yaw + 12.2) < 0.1
            assert abs(math.degrees(status.angular_velocity_y) + 0.4) < 0.1
            assert abs(math.degrees(status.angular_velocity_z) + 0.3) < 0.1
            check_mount_commands(master, mavlink, client, inverted=True)
            mavlink.close()
            client.close()
        finally:
            terminate(process)
            stdout, stderr = process.communicate(timeout=1)
            os.close(master)
            os.close(slave)
            if process.returncode != 0:
                raise AssertionError(
                    f"camera app exited {process.returncode}\n{stdout}\n{stderr}")
            assert "inferred inverted mounting from raw gimbal pitch" in stderr

    test_upright_yaw(sys.argv[1])
    print("PASS A8 upright/inverted attitude and angle/rate commands on SIYI and MAVLink")


if __name__ == "__main__":
    main()
