#!/usr/bin/env python3
"""Exercise the native MT11 MAVLink camera and gimbal-device services."""

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
import termios
import time

os.environ.setdefault("MAVLINK20", "1")

from pymavlink import mavutil  # noqa: E402
from pymavlink.quaternion import Quaternion  # noqa: E402


CAMERA_COMPONENT = mavutil.mavlink.MAV_COMP_ID_CAMERA
GIMBAL_COMPONENT = mavutil.mavlink.MAV_COMP_ID_GIMBAL


class FdWriter:
    def __init__(self, fd):
        self.fd = fd

    def write(self, data):
        return os.write(self.fd, data)


class FdMavlink:
    def __init__(self, fd):
        self.fd = fd
        self.mav = mavutil.mavlink.MAVLink(
            FdWriter(fd), srcSystem=44,
            srcComponent=mavutil.mavlink.MAV_COMP_ID_MISSIONPLANNER,
        )
        self.pending = []

    def recv_match(self, type=None, blocking=False, timeout=None):
        wanted = {type} if isinstance(type, str) else set(type or [])
        deadline = time.monotonic() + (timeout or 0.0)
        while True:
            while self.pending:
                message = self.pending.pop(0)
                if not wanted or message.get_type() in wanted:
                    return message
            wait = 0.0
            if blocking:
                wait = max(0.0, deadline - time.monotonic())
                if wait == 0.0:
                    return None
            readable, _, _ = select.select([self.fd], [], [], wait)
            if not readable:
                return None
            for byte in os.read(self.fd, 4096):
                message = self.mav.parse_char(bytes([byte]))
                if message is not None:
                    self.pending.append(message)

    def close(self):
        pass


def reserve_port(socktype):
    sock = socket.socket(socket.AF_INET, socktype)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def reserve_tcp_udp_port():
    tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp.bind(("127.0.0.1", 0))
    port = tcp.getsockname()[1]
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("127.0.0.1", port))
    tcp.close()
    udp.close()
    return port


def terminate(process):
    if process is None or process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def wait_path(path, process, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        if process.poll() is not None:
            raise AssertionError(f"process exited with status {process.returncode}")
        time.sleep(0.02)
    raise TimeoutError(path)


def heartbeat(connection):
    connection.mav.heartbeat_send(
        mavutil.mavlink.MAV_TYPE_GCS,
        mavutil.mavlink.MAV_AUTOPILOT_INVALID,
        0, 0, mavutil.mavlink.MAV_STATE_ACTIVE,
    )


def discover(connection):
    heartbeat(connection)
    found = set()
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline and found != {CAMERA_COMPONENT, GIMBAL_COMPONENT}:
        message = connection.recv_match(type="HEARTBEAT", blocking=True, timeout=1)
        if message is not None and message.get_srcComponent() in (
                CAMERA_COMPONENT, GIMBAL_COMPONENT):
            found.add(message.get_srcComponent())
        heartbeat(connection)
    assert found == {CAMERA_COMPONENT, GIMBAL_COMPONENT}, found


def trigger_device_information_announcement(connection, position_targeting):
    while connection.recv_match(blocking=False) is not None:
        pass
    source = (connection.mav.srcSystem, connection.mav.srcComponent)
    try:
        connection.mav.srcSystem = 1
        connection.mav.srcComponent = 1
        connection.mav.heartbeat_send(
            mavutil.mavlink.MAV_TYPE_QUADROTOR,
            mavutil.mavlink.MAV_AUTOPILOT_ARDUPILOTMEGA,
            0, 0, mavutil.mavlink.MAV_STATE_ACTIVE,
        )
    finally:
        connection.mav.srcSystem, connection.mav.srcComponent = source
    info = connection.recv_match(
        type="GIMBAL_DEVICE_INFORMATION", blocking=True, timeout=3)
    assert info is not None
    location_capability = bool(
        info.cap_flags2 &
        mavutil.mavlink.GIMBAL_DEVICE_CAP_FLAGS_CAN_POINT_LOCATION_GLOBAL)
    assert location_capability == position_targeting


def command(connection, component, command_id, params=(), response_type=None,
            timeout=8):
    while connection.recv_match(blocking=False) is not None:
        pass
    values = list(params) + [0.0] * (7 - len(params))
    connection.mav.command_long_send(1, component, command_id, 0, *values[:7])
    ack = None
    response = None
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        message = connection.recv_match(blocking=True, timeout=1)
        if message is None:
            heartbeat(connection)
            continue
        if (message.get_type() == "COMMAND_ACK" and
                message.get_srcComponent() == component and
                message.command == command_id):
            ack = message
        if (response_type is not None and message.get_type() == response_type and
                message.get_srcComponent() == component):
            response = message
        if ack is not None and (response_type is None or response is not None):
            return ack, response
    raise TimeoutError(f"command {command_id} response={response_type}")


def request_message(connection, component, message_id, message_name, instance=0):
    ack, response = command(
        connection, component, mavutil.mavlink.MAV_CMD_REQUEST_MESSAGE,
        [message_id, instance], message_name,
    )
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED, ack
    return response


def wait_attitude(connection, predicate, timeout=8):
    deadline = time.monotonic() + timeout
    latest = None
    while time.monotonic() < deadline:
        message = connection.recv_match(
            type="GIMBAL_DEVICE_ATTITUDE_STATUS", blocking=True, timeout=1
        )
        if message is None or message.get_srcComponent() != GIMBAL_COMPONENT:
            continue
        euler = Quaternion(message.q).euler
        latest = tuple(math.degrees(value) for value in euler)
        if predicate(latest):
            return latest
    raise TimeoutError(f"attitude latest={latest}")


def crc16(data):
    value = 0
    for byte in data:
        value ^= byte << 8
        for _ in range(8):
            value = (((value << 1) ^ 0x1021) & 0xFFFF
                     if value & 0x8000 else (value << 1) & 0xFFFF)
    return value


def read_exact(sock, length):
    result = bytearray()
    while len(result) < length:
        part = sock.recv(length - len(result))
        if not part:
            raise ConnectionError("SIYI connection closed")
        result.extend(part)
    return bytes(result)


def query_siyi_attitude(port):
    header = struct.pack("<BBBHHB", 0x55, 0x66, 1, 0, 0x1234, 0x0D)
    packet = header + struct.pack("<H", crc16(header))
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        sock.sendall(packet)
        while True:
            response_header = read_exact(sock, 8)
            payload_length = struct.unpack_from("<H", response_header, 3)[0]
            response = response_header + read_exact(sock, payload_length + 2)
            if response[7] != 0x0D:
                continue
            yaw, pitch, roll = struct.unpack_from("<hhh", response, 8)
            # SIYI yaw is positive left; return MAVLink-convention angles
            return roll / 10.0, pitch / 10.0, -yaw / 10.0


def wait_siyi_attitude(port, target_pitch, target_yaw, timeout=8, *, refresh=None):
    deadline = time.monotonic() + timeout
    latest = None
    while time.monotonic() < deadline:
        # Rate acquisition can outlast the vehicle telemetry freshness window.
        # Keep the fixture's stationary vehicle alive while the gimbal moves.
        if refresh is not None:
            refresh()
        try:
            latest = query_siyi_attitude(port)
        except (ConnectionError, OSError, TimeoutError):
            continue
        yaw_error = (latest[2] - target_yaw + 180.0) % 360.0 - 180.0
        if abs(latest[1] - target_pitch) < 2.0 and abs(yaw_error) < 3.0:
            return latest
        time.sleep(0.05)
    raise TimeoutError(
        f"SIYI pitch/yaw {target_pitch}/{target_yaw}, latest={latest}")


def send_vehicle_attitude(connection, yaw_degrees, yaw_rate=0.0):
    attitude = Quaternion([0.0, 0.0, math.radians(yaw_degrees)])
    source = (connection.mav.srcSystem, connection.mav.srcComponent)
    try:
        connection.mav.srcSystem = 1
        connection.mav.srcComponent = 1
        connection.mav.autopilot_state_for_gimbal_device_send(
            1, GIMBAL_COMPONENT, int(time.monotonic() * 1.0e6), attitude.q,
            0, 0.0, 0.0, 0.0, 0, yaw_rate, 0,
            mavutil.mavlink.MAV_LANDED_STATE_IN_AIR,
        )
    finally:
        connection.mav.srcSystem, connection.mav.srcComponent = source


def send_vehicle_position(connection, latitude, longitude, altitude_amsl):
    source = (connection.mav.srcSystem, connection.mav.srcComponent)
    try:
        connection.mav.srcSystem = 1
        connection.mav.srcComponent = 1
        connection.mav.global_position_int_send(
            int(time.monotonic() * 1000) & 0xFFFFFFFF,
            round(latitude * 1.0e7), round(longitude * 1.0e7),
            round(altitude_amsl * 1000.0), 0, 0, 0, 0, 0,
        )
    finally:
        connection.mav.srcSystem, connection.mav.srcComponent = source


def location_command(connection, command_id, frame, latitude=0.0,
                     longitude=0.0, altitude=0.0, timeout=8):
    while connection.recv_match(blocking=False) is not None:
        pass
    connection.mav.command_int_send(
        1, GIMBAL_COMPONENT, frame, command_id, 0, 0,
        0.0, 0.0, 0.0, 0.0,
        round(latitude * 1.0e7), round(longitude * 1.0e7), altitude,
    )
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        message = connection.recv_match(
            type="COMMAND_ACK", blocking=True, timeout=1)
        if (message is not None and
                message.get_srcComponent() == GIMBAL_COMPONENT and
                message.command == command_id):
            return message
    raise TimeoutError(f"location command {command_id}")


def wait_capture_events(connection, count, timeout=8):
    events = []
    deadline = time.monotonic() + timeout
    while len(events) < count and time.monotonic() < deadline:
        message = connection.recv_match(
            type="CAMERA_IMAGE_CAPTURED", blocking=True, timeout=1
        )
        if (message is not None and
                message.get_srcComponent() == CAMERA_COMPONENT):
            events.append(message)
    if len(events) != count:
        raise TimeoutError(f"capture events {len(events)}/{count}")
    return events


def full_capability_test(connection, siyi_port, capture_root, recording_state,
                         position_targeting):
    camera = request_message(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAVLINK_MSG_ID_CAMERA_INFORMATION,
        "CAMERA_INFORMATION",
    )
    assert bytes(camera.vendor_name).rstrip(b"\0") == b"ArduPilot"
    assert bytes(camera.model_name).rstrip(b"\0") == b"MT11"
    expected_flags = (
        mavutil.mavlink.CAMERA_CAP_FLAGS_CAPTURE_VIDEO |
        mavutil.mavlink.CAMERA_CAP_FLAGS_CAPTURE_IMAGE |
        mavutil.mavlink.CAMERA_CAP_FLAGS_HAS_MODES |
        mavutil.mavlink.CAMERA_CAP_FLAGS_CAN_CAPTURE_IMAGE_IN_VIDEO_MODE |
        mavutil.mavlink.CAMERA_CAP_FLAGS_CAN_CAPTURE_VIDEO_IN_IMAGE_MODE |
        mavutil.mavlink.CAMERA_CAP_FLAGS_HAS_BASIC_ZOOM |
        mavutil.mavlink.CAMERA_CAP_FLAGS_HAS_BASIC_FOCUS |
        mavutil.mavlink.CAMERA_CAP_FLAGS_HAS_VIDEO_STREAM |
        mavutil.mavlink.CAMERA_CAP_FLAGS_HAS_THERMAL_RANGE |
        mavutil.mavlink.CAMERA_CAP_FLAGS_HAS_IMAGE_SURVEY_MODE
    )
    assert camera.flags == expected_flags
    assert (camera.resolution_h, camera.resolution_v) == (1920, 1080)
    assert camera.gimbal_device_id == GIMBAL_COMPONENT

    gimbal = request_message(
        connection, GIMBAL_COMPONENT,
        mavutil.mavlink.MAVLINK_MSG_ID_GIMBAL_DEVICE_INFORMATION,
        "GIMBAL_DEVICE_INFORMATION",
    )
    assert gimbal.vendor_name.rstrip("\0") == "ArduPilot"
    assert gimbal.model_name.rstrip("\0") == "MT11"
    assert math.degrees(gimbal.pitch_min) == pytest_approx(-90, 0.1)
    assert math.degrees(gimbal.pitch_max) == pytest_approx(30, 0.1)
    assert gimbal.gimbal_device_id == 0
    assert gimbal.cap_flags2 & mavutil.mavlink.GIMBAL_DEVICE_CAP_FLAGS_HAS_PITCH_AXIS
    assert gimbal.cap_flags2 & mavutil.mavlink.GIMBAL_DEVICE_CAP_FLAGS_HAS_YAW_AXIS
    assert (gimbal.cap_flags2 &
            mavutil.mavlink.GIMBAL_DEVICE_CAP_FLAGS_SUPPORTS_YAW_IN_EARTH_FRAME)
    location_capability = bool(
        gimbal.cap_flags2 &
        mavutil.mavlink.GIMBAL_DEVICE_CAP_FLAGS_CAN_POINT_LOCATION_GLOBAL)
    assert location_capability == position_targeting

    # Discovery sends a GCS heartbeat.  It must not replace the autopilot as
    # the target of device status or the autopilot will consider the mount
    # unhealthy even though a GCS continues to receive forwarded status.
    status = request_message(
        connection, GIMBAL_COMPONENT,
        mavutil.mavlink.MAVLINK_MSG_ID_GIMBAL_DEVICE_ATTITUDE_STATUS,
        "GIMBAL_DEVICE_ATTITUDE_STATUS",
    )
    assert (status.target_system, status.target_component) == (1, 1)

    initial = wait_attitude(connection, lambda _value: True)
    target = Quaternion([0.0, math.radians(-12.0), math.radians(15.0)])
    connection.mav.gimbal_device_set_attitude_send(
        1, GIMBAL_COMPONENT, 0, target.q, math.nan, math.nan, math.nan
    )
    moved = wait_attitude(
        connection,
        lambda value: abs(value[1] + 12.0) < 2.0 and abs(value[2] - 15.0) < 3.0,
    )

    # The MT11/SIYI angle is vehicle-relative.  Earth-lock input must be
    # converted using AUTOPILOT_STATE_FOR_GIMBAL_DEVICE, while the MAVLink
    # feedback remains earth-relative and follows a changing vehicle yaw.
    earth_target = Quaternion(
        [0.0, math.radians(-18.0), math.radians(70.0)])
    earth_flags = (mavutil.mavlink.GIMBAL_DEVICE_FLAGS_ROLL_LOCK |
                   mavutil.mavlink.GIMBAL_DEVICE_FLAGS_PITCH_LOCK |
                   mavutil.mavlink.GIMBAL_DEVICE_FLAGS_YAW_LOCK)
    send_vehicle_attitude(connection, 40.0)
    connection.mav.gimbal_device_set_attitude_send(
        1, GIMBAL_COMPONENT, earth_flags, earth_target.q,
        math.nan, math.nan, math.nan)
    wait_siyi_attitude(siyi_port, -18.0, 30.0)
    wait_attitude(
        connection,
        lambda value: (abs(value[1] + 18.0) < 2.0 and
                       abs(value[2] - 70.0) < 3.0),
    )
    earth_status = request_message(
        connection, GIMBAL_COMPONENT,
        mavutil.mavlink.MAVLINK_MSG_ID_GIMBAL_DEVICE_ATTITUDE_STATUS,
        "GIMBAL_DEVICE_ATTITUDE_STATUS",
    )
    assert earth_status.flags & mavutil.mavlink.GIMBAL_DEVICE_FLAGS_YAW_LOCK
    assert (earth_status.flags &
            mavutil.mavlink.GIMBAL_DEVICE_FLAGS_YAW_IN_EARTH_FRAME)
    assert (earth_status.flags &
            mavutil.mavlink.GIMBAL_DEVICE_FLAGS_ACCEPTS_YAW_IN_EARTH_FRAME)
    assert not (earth_status.flags &
                mavutil.mavlink.GIMBAL_DEVICE_FLAGS_YAW_IN_VEHICLE_FRAME)

    send_vehicle_attitude(connection, 55.0)
    connection.mav.gimbal_device_set_attitude_send(
        1, GIMBAL_COMPONENT, earth_flags, earth_target.q,
        math.nan, math.nan, math.nan)
    wait_siyi_attitude(siyi_port, -18.0, 15.0)
    wait_attitude(
        connection,
        lambda value: (abs(value[1] + 18.0) < 2.0 and
                       abs(value[2] - 70.0) < 3.0),
    )

    # A native geographic target is retained and recomputed by the camera at
    # 10Hz from AMSL GLOBAL_POSITION_INT and vehicle attitude.  Relative-home
    # and terrain altitude frames are deliberately rejected for now.
    vehicle_latitude = -35.363262
    vehicle_longitude = 149.165237
    vehicle_altitude = 600.0
    roi_latitude = vehicle_latitude + 100.0 / 111319.5
    roi_altitude = vehicle_altitude - 50.0
    send_vehicle_position(connection, vehicle_latitude, vehicle_longitude,
                          vehicle_altitude)
    send_vehicle_attitude(connection, 40.0)
    ack = location_command(
        connection, mavutil.mavlink.MAV_CMD_DO_SET_ROI_LOCATION,
        mavutil.mavlink.MAV_FRAME_GLOBAL_RELATIVE_ALT,
        roi_latitude, vehicle_longitude, roi_altitude)
    assert ack.result == (mavutil.mavlink.MAV_RESULT_DENIED
                          if position_targeting
                          else mavutil.mavlink.MAV_RESULT_UNSUPPORTED)
    ack = location_command(
        connection, mavutil.mavlink.MAV_CMD_DO_SET_ROI_LOCATION,
        mavutil.mavlink.MAV_FRAME_GLOBAL,
        roi_latitude, vehicle_longitude, roi_altitude)
    assert ack.result == (mavutil.mavlink.MAV_RESULT_ACCEPTED
                          if position_targeting
                          else mavutil.mavlink.MAV_RESULT_UNSUPPORTED)
    roi_pitch = math.degrees(math.atan2(-50.0, 100.0))
    if position_targeting:
        def refresh_roi_vehicle():
            send_vehicle_position(connection, vehicle_latitude, vehicle_longitude,
                                  vehicle_altitude)
            send_vehicle_attitude(connection, 40.0)

        wait_siyi_attitude(siyi_port, roi_pitch, -40.0, refresh=refresh_roi_vehicle)
        send_vehicle_attitude(connection, 40.0)
        wait_attitude(
            connection,
            lambda value: (abs(value[1] - roi_pitch) < 2.0 and
                           abs(value[2]) < 3.0),
        )
    # Return to body-frame control for the existing rate tests.  A direct
    # attitude command must take ownership from the retained location target.
    target = Quaternion([0.0, math.radians(-12.0), math.radians(15.0)])
    connection.mav.gimbal_device_set_attitude_send(
        1, GIMBAL_COMPONENT, 0, target.q, math.nan, math.nan, math.nan
    )
    moved = wait_attitude(
        connection,
        lambda value: abs(value[1] + 12.0) < 2.0 and abs(value[2] - 15.0) < 3.0,
    )
    send_vehicle_attitude(connection, 70.0)
    time.sleep(0.4)
    held = query_siyi_attitude(siyi_port)
    assert abs(held[1] + 12.0) < 2.0 and abs(held[2] - 15.0) < 3.0

    # GLOBAL_INT is the deprecated but valid synonym for GLOBAL.  Clearing
    # the target stops the retained control loop without moving the gimbal.
    ack = location_command(
        connection, mavutil.mavlink.MAV_CMD_DO_SET_ROI_LOCATION,
        mavutil.mavlink.MAV_FRAME_GLOBAL_INT,
        roi_latitude, vehicle_longitude, roi_altitude)
    assert ack.result == (mavutil.mavlink.MAV_RESULT_ACCEPTED
                          if position_targeting
                          else mavutil.mavlink.MAV_RESULT_UNSUPPORTED)
    ack = location_command(
        connection, mavutil.mavlink.MAV_CMD_DO_SET_ROI_NONE,
        mavutil.mavlink.MAV_FRAME_GLOBAL)
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED

    connection.mav.gimbal_device_set_attitude_send(
        1, GIMBAL_COMPONENT, 0, [math.nan] * 4, 0.0,
        math.radians(12.0), 0.0,
    )
    rate_start = moved[1]
    rate_end = wait_attitude(connection, lambda value: value[1] > rate_start + 3.0)
    connection.mav.gimbal_device_set_attitude_send(
        1, GIMBAL_COMPONENT, 0, [math.nan] * 4, 0.0, 0.0, 0.0
    )
    assert initial != rate_end
    connection.mav.gimbal_device_set_attitude_send(
        1, GIMBAL_COMPONENT, mavutil.mavlink.GIMBAL_DEVICE_FLAGS_NEUTRAL,
        [math.nan] * 4, math.nan, math.nan, math.nan,
    )
    wait_attitude(
        connection,
        lambda value: abs(value[1]) < 2.0 and abs(value[2]) < 2.0,
    )

    settings = request_message(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAVLINK_MSG_ID_CAMERA_SETTINGS, "CAMERA_SETTINGS",
    )
    assert settings.zoomLevel == pytest_approx(0.0, 0.1)
    ack, _ = command(
        connection, CAMERA_COMPONENT, mavutil.mavlink.MAV_CMD_SET_CAMERA_ZOOM,
        [mavutil.mavlink.ZOOM_TYPE_RANGE, 50.0],
    )
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
    settings = request_message(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAVLINK_MSG_ID_CAMERA_SETTINGS, "CAMERA_SETTINGS",
    )
    assert settings.zoomLevel == pytest_approx(50.0, 0.2)
    ack, _ = command(connection, CAMERA_COMPONENT, mavutil.mavlink.MAV_CMD_SET_CAMERA_ZOOM,
                     [mavutil.mavlink.ZOOM_TYPE_STEP, 0.0])
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
    settings = request_message(connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAVLINK_MSG_ID_CAMERA_SETTINGS, "CAMERA_SETTINGS")
    assert settings.zoomLevel == pytest_approx(50.0, 0.2), 'zero zoom step changed zoom'
    for zoom_type, value in [(1e30, 1), (0.5, 1), (1, 2)]:
        ack, _ = command(connection, CAMERA_COMPONENT, mavutil.mavlink.MAV_CMD_SET_CAMERA_ZOOM,
                         [zoom_type, value])
        assert ack.result in (mavutil.mavlink.MAV_RESULT_DENIED, mavutil.mavlink.MAV_RESULT_UNSUPPORTED)
    ack, _ = command(
        connection, CAMERA_COMPONENT, mavutil.mavlink.MAV_CMD_SET_CAMERA_ZOOM,
        [mavutil.mavlink.ZOOM_TYPE_CONTINUOUS, 1.0],
    )
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
    time.sleep(0.5)
    command(connection, CAMERA_COMPONENT, mavutil.mavlink.MAV_CMD_SET_CAMERA_ZOOM,
            [mavutil.mavlink.ZOOM_TYPE_CONTINUOUS, 0.0])
    settings = request_message(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAVLINK_MSG_ID_CAMERA_SETTINGS, "CAMERA_SETTINGS",
    )
    assert settings.zoomLevel > 50.0

    for focus_type, expected in (
        (mavutil.mavlink.FOCUS_TYPE_AUTO, mavutil.mavlink.MAV_RESULT_ACCEPTED),
        (mavutil.mavlink.FOCUS_TYPE_CONTINUOUS, mavutil.mavlink.MAV_RESULT_ACCEPTED),
        (mavutil.mavlink.FOCUS_TYPE_RANGE, mavutil.mavlink.MAV_RESULT_ACCEPTED),
    ):
        ack, _ = command(
            connection, CAMERA_COMPONENT,
            mavutil.mavlink.MAV_CMD_SET_CAMERA_FOCUS, [focus_type, 1.0],
        )
        assert ack.result == expected
    settings = request_message(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAVLINK_MSG_ID_CAMERA_SETTINGS, "CAMERA_SETTINGS",
    )
    assert settings.focusLevel == pytest_approx(1.0, 0.1)

    ack, _ = command(
        connection, CAMERA_COMPONENT, mavutil.mavlink.MAV_CMD_SET_CAMERA_MODE,
        [0.0, mavutil.mavlink.CAMERA_MODE_VIDEO],
    )
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
    settings = request_message(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAVLINK_MSG_ID_CAMERA_SETTINGS, "CAMERA_SETTINGS",
    )
    assert settings.mode_id == mavutil.mavlink.CAMERA_MODE_VIDEO
    ack, _ = command(
        connection, CAMERA_COMPONENT, mavutil.mavlink.MAV_CMD_SET_CAMERA_MODE,
        [0.0, 9.0],
    )
    assert ack.result == mavutil.mavlink.MAV_RESULT_UNSUPPORTED

    ack, _ = command(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAV_CMD_SET_CAMERA_SOURCE,
        [0.0, mavutil.mavlink.CAMERA_SOURCE_IR, 0.0],
    )
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
    thermal_main = request_message(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAVLINK_MSG_ID_VIDEO_STREAM_INFORMATION,
        "VIDEO_STREAM_INFORMATION", 1,
    )
    assert (thermal_main.flags &
            mavutil.mavlink.VIDEO_STREAM_STATUS_FLAGS_THERMAL)
    assert (thermal_main.resolution_h, thermal_main.resolution_v) == (1280, 720)
    ack, _ = command(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAV_CMD_SET_CAMERA_SOURCE,
        [0.0, mavutil.mavlink.CAMERA_SOURCE_RGB, 0.0],
    )
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED

    image_index = 37
    ack, captured = command(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAV_CMD_IMAGE_START_CAPTURE,
        [0.0, 0.0, 1.0, image_index], "CAMERA_IMAGE_CAPTURED",
    )
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
    assert captured.image_index == image_index and captured.capture_result == 1
    expected = [capture_root / f"SITL_000001_{suffix}.jpg" for suffix in "CZI"]
    assert all(path.stat().st_size > 0 for path in expected)

    ack, captured = command(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAV_CMD_IMAGE_START_CAPTURE,
        [0.0, 0.1, 3.0, 0.0], "CAMERA_IMAGE_CAPTURED",
    )
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
    assert captured.capture_result == 1
    assert all(event.capture_result == 1
               for event in wait_capture_events(connection, 2))

    ack, captured = command(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAV_CMD_IMAGE_START_CAPTURE,
        [0.0, 0.1, 0.0, 0.0], "CAMERA_IMAGE_CAPTURED",
    )
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
    assert captured.capture_result == 1
    assert wait_capture_events(connection, 1)[0].capture_result == 1
    ack, _ = command(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAV_CMD_IMAGE_STOP_CAPTURE,
    )
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED

    status = request_message(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAVLINK_MSG_ID_CAMERA_CAPTURE_STATUS,
        "CAMERA_CAPTURE_STATUS",
    )
    assert status.image_count >= 6 and status.image_status == 0
    assert status.video_status == 0
    ack, _ = command(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAV_CMD_VIDEO_START_CAPTURE,
    )
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
    assert "recording-control-active=1" in recording_state.read_text()
    time.sleep(0.1)
    status = request_message(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAVLINK_MSG_ID_CAMERA_CAPTURE_STATUS,
        "CAMERA_CAPTURE_STATUS",
    )
    assert status.video_status == 1 and status.recording_time_ms > 0
    ack, _ = command(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAV_CMD_VIDEO_STOP_CAPTURE,
    )
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
    assert not recording_state.exists()

    storage = request_message(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAVLINK_MSG_ID_STORAGE_INFORMATION,
        "STORAGE_INFORMATION",
    )
    assert storage.status == mavutil.mavlink.STORAGE_STATUS_READY
    assert storage.type == mavutil.mavlink.STORAGE_TYPE_MICROSD
    assert storage.available_capacity > 0

    visible = request_message(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAVLINK_MSG_ID_VIDEO_STREAM_INFORMATION,
        "VIDEO_STREAM_INFORMATION", 1,
    )
    thermal = request_message(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAVLINK_MSG_ID_VIDEO_STREAM_INFORMATION,
        "VIDEO_STREAM_INFORMATION", 2,
    )
    assert visible.stream_id == 1 and visible.uri.endswith("/video1.misb")
    assert thermal.stream_id == 2 and thermal.uri.endswith("/video2.misb")
    assert visible.uri.startswith("rtsp://127.0.0.1:")
    assert thermal.uri.startswith("rtsp://127.0.0.1:")
    assert 0 < visible.hfov <= 88
    assert thermal.hfov == 24
    assert thermal.flags & mavutil.mavlink.VIDEO_STREAM_STATUS_FLAGS_THERMAL
    assert visible.encoding == mavutil.mavlink.VIDEO_STREAM_ENCODING_H264

    ack, stream_status = command(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAV_CMD_VIDEO_STOP_STREAMING,
        [2.0], "VIDEO_STREAM_STATUS",
    )
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
    assert stream_status.stream_id == 2
    assert not (stream_status.flags &
                mavutil.mavlink.VIDEO_STREAM_STATUS_FLAGS_RUNNING)
    ack, stream_status = command(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAV_CMD_VIDEO_START_STREAMING,
        [2.0], "VIDEO_STREAM_STATUS",
    )
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
    assert (stream_status.flags &
            mavutil.mavlink.VIDEO_STREAM_STATUS_FLAGS_RUNNING)

    ack, _ = command(connection, CAMERA_COMPONENT, 31000)
    assert ack.result == mavutil.mavlink.MAV_RESULT_UNSUPPORTED


def pytest_approx(expected, tolerance):
    """Tiny equality helper to keep this test dependency-free."""
    class Approx:
        def __eq__(self, actual):
            return abs(actual - expected) <= tolerance
    return Approx()


def transport_smoke(connection):
    discover(connection)
    camera = request_message(
        connection, CAMERA_COMPONENT,
        mavutil.mavlink.MAVLINK_MSG_ID_CAMERA_INFORMATION,
        "CAMERA_INFORMATION",
    )
    gimbal = request_message(
        connection, GIMBAL_COMPONENT,
        mavutil.mavlink.MAVLINK_MSG_ID_GIMBAL_DEVICE_INFORMATION,
        "GIMBAL_DEVICE_INFORMATION",
    )
    assert bytes(camera.model_name).rstrip(b"\0") == b"MT11"
    assert gimbal.model_name.rstrip("\0") == "MT11"

    # Fetch the full table over the physical UART test transport as well as
    # UDP, then check a full-width name and a persistent integer write.
    connection.mav.param_request_list_send(1, CAMERA_COMPONENT)
    parameters = {}
    deadline = time.monotonic() + 5
    count = None
    while (count is None or len(parameters) < count) and time.monotonic() < deadline:
        value = connection.recv_match(type="PARAM_VALUE", blocking=True, timeout=1)
        if value is not None:
            parameters[value.param_id] = value
            count = value.param_count
    assert count is not None and len(parameters) == count
    connection.mav.param_request_read_send(1, CAMERA_COMPONENT, b"VIDEO_MAIN_CODEC", -1)
    value = connection.recv_match(type="PARAM_VALUE", blocking=True, timeout=2)
    assert value is not None and value.param_id == "VIDEO_MAIN_CODEC"
    connection.mav.param_set_send(1, CAMERA_COMPONENT, b"IMG_BRIGHTNESS", 61,
                                  mavutil.mavlink.MAV_PARAM_TYPE_REAL32)
    value = connection.recv_match(type="PARAM_VALUE", blocking=True, timeout=2)
    assert value is not None and value.param_id == "IMG_BRIGHTNESS" and value.param_value == 61


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(
            "usage: test_mavlink_integration.py CAMERA_APP GIMBAL_SIM "
            "[enabled|disabled]")
    camera_binary = pathlib.Path(sys.argv[1]).resolve()
    gimbal_script = pathlib.Path(sys.argv[2]).resolve()
    mode = sys.argv[3] if len(sys.argv) == 4 else "enabled"
    if mode not in ("enabled", "disabled"):
        raise SystemExit("position targeting mode must be enabled or disabled")
    position_targeting = mode == "enabled"
    uart_master, uart_slave = pty.openpty()
    uart_slave_path = os.ttyname(uart_slave)
    mavlink_port = reserve_tcp_udp_port()
    siyi_port = reserve_port(socket.SOCK_STREAM)
    gimbal_port = reserve_port(socket.SOCK_DGRAM)
    processes = []
    logs = []
    with tempfile.TemporaryDirectory(prefix="mt11-mavlink-") as directory_text:
        directory = pathlib.Path(directory_text)
        ready = directory / "camera.ready"
        gimbal_ready = directory / "gimbal.ready"
        capture_root = directory / "capture"
        record_root = directory / "record"
        recording_state = directory / "recording.state"
        config_path = directory / "camera.ini"
        capture_root.mkdir()
        record_root.mkdir()
        config_path.write_text(
            "[general]\ntimezone = UTC\n"
            "[capture]\nphoto_scope = all\n"
            "[uart]\nprotocol = mavlink\n"
            f"[mavlink]\nsystem_id = 1\nposition_targeting = "
            f"{'true' if position_targeting else 'false'}\n",
            encoding="ascii",
        )
        try:
            gimbal_log = (directory / "gimbal.log").open("w", encoding="utf-8")
            logs.append(gimbal_log)
            gimbal = subprocess.Popen(
                [sys.executable, str(gimbal_script), "--port", str(gimbal_port),
                 "--ready-file", str(gimbal_ready)],
                stdout=gimbal_log, stderr=subprocess.STDOUT,
            )
            processes.append(gimbal)
            wait_path(gimbal_ready, gimbal)

            env = os.environ.copy()
            env.update({
                "CAMERA_APP_UART": f"udp://127.0.0.1:{gimbal_port}",
                "CAMERA_APP_PORT": str(siyi_port),
                "CAMERA_APP_CONFIG": str(config_path),
                "CAMERA_APP_MAVLINK_TCP_PORT": str(mavlink_port),
                "CAMERA_APP_MAVLINK_UDP_PORT": str(mavlink_port),
                "CAMERA_APP_EXTERNAL_UART": uart_slave_path,
                "CAMERA_APP_READY_PATH": str(ready),
                "CAMERA_APP_RECORD_STATE": str(recording_state),
                "CAMERA_APP_RECORD_ROOT": str(record_root),
                "CAMERA_APP_CAPTURE_ROOT": str(capture_root),
                "CAMERA_APP_SITL_PHOTO": str(camera_binary.parent / "photo.jpg"),
            })
            camera_log = (directory / "camera.log").open("w", encoding="utf-8")
            logs.append(camera_log)
            camera = subprocess.Popen(
                [str(camera_binary)], env=env,
                stdout=camera_log, stderr=subprocess.STDOUT,
            )
            processes.append(camera)
            wait_path(ready, camera)
            serial_settings = termios.tcgetattr(uart_slave)
            assert serial_settings[4] == termios.B230400
            assert serial_settings[5] == termios.B230400
            control = serial_settings[2]
            assert control & termios.CSIZE == termios.CS8
            assert not control & termios.PARENB
            assert not control & termios.CSTOPB

            serial = FdMavlink(uart_master)
            transport_smoke(serial)
            os.close(uart_master)
            uart_master = -1

            # Losing the optional UART must not take down TCP or UDP MAVLink.
            tcp = mavutil.mavlink_connection(
                f"tcp:127.0.0.1:{mavlink_port}", source_system=42,
                source_component=mavutil.mavlink.MAV_COMP_ID_MISSIONPLANNER,
            )
            discover(tcp)
            trigger_device_information_announcement(tcp, position_targeting)
            full_capability_test(tcp, siyi_port, capture_root, recording_state,
                                 position_targeting)
            tcp.close()

            udp = mavutil.mavlink_connection(
                f"udpout:127.0.0.1:{mavlink_port}", source_system=43,
                source_component=mavutil.mavlink.MAV_COMP_ID_MISSIONPLANNER,
            )
            transport_smoke(udp)
            udp.close()
        except BaseException:
            for log in logs:
                log.flush()
            for name in ("camera.log", "gimbal.log"):
                path = directory / name
                if path.exists():
                    print(f"\n--- {name} ---\n{path.read_text(errors='replace')}",
                          file=sys.stderr)
            raise
        finally:
            for process in reversed(processes):
                terminate(process)
            for log in logs:
                log.close()
            if uart_master >= 0:
                os.close(uart_master)
            os.close(uart_slave)
        assert "MAVLink UART disabled after I/O failure" in (
            directory / "camera.log"
        ).read_text(errors="replace")
    print("PASS native MAVLink camera/gimbal capabilities over TCP, UDP and UART "
          f"with position targeting {mode}")


if __name__ == "__main__":
    main()
