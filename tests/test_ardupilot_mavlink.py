#!/usr/bin/env python3
"""Test MT11 MAVLink services through ArduPilot AP_Networking backends."""

import math
import os
import pathlib
import signal
import socket
import struct
import subprocess
import sys
import time
from types import SimpleNamespace

if os.environ.get("MAVPROXY_REPO"):
    sys.path.insert(0, str(pathlib.Path(os.environ["MAVPROXY_REPO"]).resolve()))

os.environ.setdefault("MAVLINK20", "1")

from pymavlink import mavutil  # noqa: E402
from pymavlink.quaternion import Quaternion  # noqa: E402


CAMERA_COMPONENT = mavutil.mavlink.MAV_COMP_ID_CAMERA
GIMBAL_COMPONENT = mavutil.mavlink.MAV_COMP_ID_GIMBAL
HOME_ALTITUDE_AMSL = 584.0


class MAVProxyTestConsole:
    def set_status(self, _name, _text, **_kwargs):
        pass


class MAVProxyTestFunctions:
    def say(self, _message):
        pass

    def get_mav_param(self, _name, default=None):
        return default


class MAVProxyTestState:
    def __init__(self, connection):
        self.command_map = {}
        self.completions = {}
        self.completion_functions = {}
        self.public_modules = {}
        self.multi_instance = {}
        self.instance_count = {}
        self.settings = SimpleNamespace(target_system=1, target_component=1)
        self.console = MAVProxyTestConsole()
        self.status = SimpleNamespace(logdir=".")
        self.functions = MAVProxyTestFunctions()
        self.connection = connection

    def master(self):
        return self.connection

    def module(self, _name):
        return None


def reserve_port(socktype):
    sock = socket.socket(socket.AF_INET, socktype)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
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


def connect_mavlink(port, process):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"ArduCopter exited with status {process.returncode}")
        try:
            connection = mavutil.mavlink_connection(
                f"tcp:127.0.0.1:{port}", source_system=255,
                source_component=mavutil.mavlink.MAV_COMP_ID_MISSIONPLANNER,
            )
            if connection.wait_heartbeat(timeout=5) is not None:
                return connection
            connection.close()
        except (ConnectionError, OSError):
            pass
        time.sleep(0.2)
    raise TimeoutError("ArduCopter MAVLink connection")


def command(connection, target_component, command_id, params=(),
            ack_component=None, timeout=12):
    values = list(params) + [0.0] * (7 - len(params))
    connection.mav.command_long_send(
        1, target_component, command_id, 0, *values[:7]
    )
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ack = connection.recv_match(type="COMMAND_ACK", blocking=True, timeout=1)
        if ack is None or ack.command != command_id:
            continue
        if ack_component is not None and ack.get_srcComponent() != ack_component:
            continue
        return ack
    raise TimeoutError(f"command {command_id}")


def request_device_message(connection, component, message_id, name, timeout=12):
    while connection.recv_match(blocking=False) is not None:
        pass
    connection.mav.command_long_send(
        1, component, mavutil.mavlink.MAV_CMD_REQUEST_MESSAGE, 0,
        message_id, 0, 0, 0, 0, 0, 0,
    )
    deadline = time.monotonic() + timeout
    response = None
    ack = None
    while time.monotonic() < deadline:
        message = connection.recv_match(blocking=True, timeout=1)
        if message is None:
            continue
        if (message.get_type() == name and
                message.get_srcComponent() == component):
            response = message
        if (message.get_type() == "COMMAND_ACK" and
                message.get_srcComponent() == component and
                message.command == mavutil.mavlink.MAV_CMD_REQUEST_MESSAGE):
            ack = message
        if response is not None and ack is not None:
            assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
            return response
    raise TimeoutError(f"device message {name}")


def request_ap_camera_information(connection, timeout=45):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        while connection.recv_match(blocking=False) is not None:
            pass
        connection.mav.command_long_send(
            1, 1, mavutil.mavlink.MAV_CMD_REQUEST_MESSAGE, 0,
            mavutil.mavlink.MAVLINK_MSG_ID_CAMERA_INFORMATION,
            0, 0, 0, 0, 0, 0,
        )
        inner = time.monotonic() + 4
        while time.monotonic() < inner:
            message = connection.recv_match(blocking=True, timeout=1)
            if message is None:
                continue
            if (message.get_type() == "CAMERA_INFORMATION" and
                    bytes(message.model_name).rstrip(b"\0") == b"MT11"):
                return message
        time.sleep(0.2)
    raise TimeoutError("AP_Camera did not relay MT11 CAMERA_INFORMATION")


def request_video_stream_information(connection, timeout=15):
    # Stream metadata belongs to the camera component. ArduPilot routes this
    # request over NET_P1; its AP_Camera backend does not relay stream metadata
    # under the flight controller's component ID.
    while connection.recv_match(blocking=False) is not None:
        pass
    deadline = time.monotonic() + timeout
    next_request = 0
    streams = {}
    ack = None
    expected_count = 0
    while time.monotonic() < deadline:
        now = time.monotonic()
        if now >= next_request:
            connection.mav.command_long_send(
                1, CAMERA_COMPONENT, mavutil.mavlink.MAV_CMD_REQUEST_MESSAGE, 0,
                mavutil.mavlink.MAVLINK_MSG_ID_VIDEO_STREAM_INFORMATION,
                0, 0, 0, 0, 0, 0,
            )
            next_request = now + 1
        message = connection.recv_match(blocking=True, timeout=1)
        if message is None:
            continue
        if (message.get_type() == "VIDEO_STREAM_INFORMATION" and
                message.get_srcComponent() == CAMERA_COMPONENT and message.stream_id > 0):
            streams[message.stream_id] = message
            expected_count = max(expected_count, message.count)
        elif (message.get_type() == "COMMAND_ACK" and
              message.get_srcComponent() == CAMERA_COMPONENT and
              message.command == mavutil.mavlink.MAV_CMD_REQUEST_MESSAGE):
            ack = message
        if (ack is not None and expected_count > 0 and
                len(streams) >= expected_count):
            assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
            return streams
    raise TimeoutError(
        f"Routed camera stream information; streams={streams}, ack={ack}"
    )


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
            # SIYI reports yaw positive to the left; return the MAVLink
            # convention (positive to the right) like the camera app does.
            return roll / 10.0, pitch / 10.0, -yaw / 10.0


def wait_attitude(siyi_port, target_pitch=None, timeout=20):
    deadline = time.monotonic() + timeout
    latest = None
    while time.monotonic() < deadline:
        try:
            latest = query_siyi_attitude(siyi_port)
        except (ConnectionError, OSError, TimeoutError):
            continue
        if target_pitch is None or abs(latest[1] - target_pitch) < 2.0:
            return latest
        time.sleep(0.1)
    raise TimeoutError(f"gimbal pitch {target_pitch}, latest={latest}")


def wait_attitude_target(siyi_port, target_pitch, target_yaw, timeout=20):
    deadline = time.monotonic() + timeout
    latest = None
    while time.monotonic() < deadline:
        try:
            latest = query_siyi_attitude(siyi_port)
        except (ConnectionError, OSError, TimeoutError):
            continue
        yaw_error = (latest[2] - target_yaw + 180.0) % 360.0 - 180.0
        if (abs(latest[1] - target_pitch) < 2.0 and
                abs(yaw_error) < 3.0):
            return latest
        time.sleep(0.1)
    raise TimeoutError(
        f"gimbal pitch/yaw {target_pitch}/{target_yaw}, latest={latest}")


def wait_vehicle_attitude(connection, timeout=10):
    while connection.recv_match(blocking=False) is not None:
        pass
    deadline = time.monotonic() + timeout
    next_request = 0.0
    while time.monotonic() < deadline:
        now = time.monotonic()
        if now >= next_request:
            connection.mav.command_long_send(
                1, 1, mavutil.mavlink.MAV_CMD_REQUEST_MESSAGE, 0,
                mavutil.mavlink.MAVLINK_MSG_ID_ATTITUDE,
                0, 0, 0, 0, 0, 0)
            next_request = now + 1.0
        message = connection.recv_match(type="ATTITUDE", blocking=True,
                                        timeout=1)
        if message is not None and message.get_srcComponent() == 1:
            return message
    raise TimeoutError("vehicle ATTITUDE")


def wait_earth_attitude(connection, siyi_port, target_pitch,
                        target_yaw_earth, timeout=20):
    deadline = time.monotonic() + timeout
    latest = None
    while time.monotonic() < deadline:
        physical = query_siyi_attitude(siyi_port)
        vehicle = wait_vehicle_attitude(connection, timeout=3)
        vehicle_yaw = math.degrees(vehicle.yaw)
        earth_error = (
            physical[2] + vehicle_yaw - target_yaw_earth + 180.0
        ) % 360.0 - 180.0
        latest = (physical, vehicle_yaw, earth_error)
        if abs(physical[1] - target_pitch) < 2.0 and abs(earth_error) < 5.0:
            return physical, vehicle_yaw
        time.sleep(0.1)
    raise TimeoutError(
        f"earth attitude {target_pitch}/{target_yaw_earth}, latest={latest}")


def wait_gimbal_earth_attitude(connection, target_pitch, target_yaw, timeout=15):
    # Check the device's authoritative frame flags and quaternion directly.
    # Its status is addressed to the autopilot, so is not routed to the GCS.
    # Upstream AP_Mount_MAVLink does not yet convert
    # earth-frame device feedback into its manager's body-frame convention.
    deadline = time.monotonic() + timeout
    latest = None
    while time.monotonic() < deadline:
        status = request_device_message(
            connection, GIMBAL_COMPONENT,
            mavutil.mavlink.MAVLINK_MSG_ID_GIMBAL_DEVICE_ATTITUDE_STATUS,
            "GIMBAL_DEVICE_ATTITUDE_STATUS", timeout=3)
        _, pitch, yaw = (math.degrees(value) for value in Quaternion(status.q).euler)
        yaw_error = (yaw - target_yaw + 180.0) % 360.0 - 180.0
        latest = (status.flags, pitch, yaw, yaw_error)
        if abs(pitch - target_pitch) < 2.0 and abs(yaw_error) < 6.0:
            assert status.flags & mavutil.mavlink.GIMBAL_DEVICE_FLAGS_YAW_LOCK
            assert status.flags & mavutil.mavlink.GIMBAL_DEVICE_FLAGS_YAW_IN_EARTH_FRAME
            assert not status.flags & mavutil.mavlink.GIMBAL_DEVICE_FLAGS_YAW_IN_VEHICLE_FRAME
            return status
        time.sleep(0.1)
    raise TimeoutError(f"gimbal earth attitude {target_pitch}/{target_yaw}, latest={latest}")


def wait_global_position(connection, timeout=10):
    while connection.recv_match(blocking=False) is not None:
        pass
    deadline = time.monotonic() + timeout
    next_request = 0.0
    while time.monotonic() < deadline:
        now = time.monotonic()
        if now >= next_request:
            connection.mav.command_long_send(
                1, 1, mavutil.mavlink.MAV_CMD_REQUEST_MESSAGE, 0,
                mavutil.mavlink.MAVLINK_MSG_ID_GLOBAL_POSITION_INT,
                0, 0, 0, 0, 0, 0)
            next_request = now + 1.0
        message = connection.recv_match(
            type="GLOBAL_POSITION_INT", blocking=True, timeout=1)
        if message is not None and message.get_srcComponent() == 1:
            return message
    raise TimeoutError("vehicle GLOBAL_POSITION_INT")


def set_roi(connection, latitude, longitude, altitude,
            frame=mavutil.mavlink.MAV_FRAME_GLOBAL, timeout=12):
    connection.mav.command_int_send(
        1, 1, frame,
        mavutil.mavlink.MAV_CMD_DO_SET_ROI_LOCATION,
        0, 0, 0.0, 0.0, 0.0, 0.0,
        round(latitude * 1.0e7), round(longitude * 1.0e7), altitude)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ack = connection.recv_match(type="COMMAND_ACK", blocking=True,
                                    timeout=1)
        if (ack is not None and
                ack.command == mavutil.mavlink.MAV_CMD_DO_SET_ROI_LOCATION and
                ack.get_srcComponent() == 1):
            return ack
    raise TimeoutError("MAV_CMD_DO_SET_ROI_LOCATION")


def write_params(path, transport, port):
    network_type = 3 if transport == "tcp" else 1
    path.write_text(
        "MNT1_TYPE 6\n"
        "CAM1_TYPE 6\n"
        "NET_ENABLE 1\n"
        f"NET_P1_TYPE {network_type}\n"
        "NET_P1_PROTOCOL 2\n"
        "NET_P1_IP0 127\n"
        "NET_P1_IP1 0\n"
        "NET_P1_IP2 0\n"
        "NET_P1_IP3 1\n"
        f"NET_P1_PORT {port}\n",
        encoding="ascii",
    )


def module_wait(connection, module, predicate, timeout=15):
    deadline = time.monotonic() + timeout
    latest = None
    while time.monotonic() < deadline:
        message = connection.recv_match(blocking=True, timeout=1)
        if message is None:
            module.idle_task()
            continue
        module.mavlink_packet(message)
        latest = message
        if predicate(message):
            return message
    raise TimeoutError(f"MAVProxy camera module response; latest={latest}")


def module_ack(connection, module, command_id, component, timeout=15):
    return module_wait(
        connection, module,
        lambda message: (message.get_type() == "COMMAND_ACK" and
                         message.command == command_id and
                         message.get_srcComponent() == component),
        timeout,
    )


def exercise_mavproxy_camera(connection, siyi_port, capture_root,
                             recording_state):
    mavproxy_repo = os.environ.get("MAVPROXY_REPO")
    if not mavproxy_repo:
        return
    repo = pathlib.Path(mavproxy_repo).resolve()
    module_path = repo / "MAVProxy/modules/mavproxy_camera/__init__.py"
    if not module_path.exists():
        raise FileNotFoundError(module_path)
    from MAVProxy.modules.mavproxy_camera import CameraModule

    module = CameraModule(MAVProxyTestState(connection))
    module.discover()
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        message = connection.recv_match(blocking=True, timeout=1)
        if message is not None:
            module.mavlink_packet(message)
        module.idle_task()
        camera = module.cameras.get((1, CAMERA_COMPONENT))
        gimbal = module.gimbals.get((1, GIMBAL_COMPONENT))
        if (camera is not None and camera.information is not None and
                len(camera.streams) >= 2 and gimbal is not None and
                gimbal.information is not None):
            break
    else:
        raise TimeoutError("MAVProxy module camera/gimbal discovery")

    camera = module.cameras[(1, CAMERA_COMPONENT)]
    assert camera.streams[1].uri.startswith("rtsp://127.0.0.1:")
    assert camera.streams[2].uri.startswith("rtsp://127.0.0.1:")
    assert 0 < camera.streams[1].hfov <= 88
    assert camera.streams[2].hfov == 24

    module.cmd_camera(["zoom", "70"])
    ack = module_ack(connection, module,
                     mavutil.mavlink.MAV_CMD_SET_CAMERA_ZOOM,
                     CAMERA_COMPONENT)
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED

    before = set(capture_root.glob("SITL_*.jpg"))
    module.cmd_camera(["photo"])
    ack = module_ack(connection, module,
                     mavutil.mavlink.MAV_CMD_IMAGE_START_CAPTURE,
                     CAMERA_COMPONENT)
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        after = set(capture_root.glob("SITL_*.jpg"))
        if len(after) > len(before):
            break
        time.sleep(0.05)
    assert len(after) > len(before)

    module._request_message(
        1, CAMERA_COMPONENT,
        mavutil.mavlink.MAVLINK_MSG_ID_CAMERA_CAPTURE_STATUS)
    status = module_wait(
        connection, module,
        lambda message: (
            message.get_type() == "CAMERA_CAPTURE_STATUS" and
            message.get_srcComponent() == CAMERA_COMPONENT))
    assert status.video_status == 0

    module.cmd_camera(["record", "toggle"])
    ack = module_ack(connection, module,
                     mavutil.mavlink.MAV_CMD_VIDEO_START_CAPTURE,
                     CAMERA_COMPONENT)
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
    assert recording_state.exists()

    # Query the camera through the routed link: upstream AP_Camera does not
    # relay the remote camera's capture status under component 1.
    module._request_message(
        1, CAMERA_COMPONENT, mavutil.mavlink.MAVLINK_MSG_ID_CAMERA_CAPTURE_STATUS)
    status = module_wait(
        connection, module,
        lambda message: (
            message.get_type() == "CAMERA_CAPTURE_STATUS" and
            message.get_srcComponent() == CAMERA_COMPONENT and
            message.video_status == 1))
    assert status.video_status == 1

    module.cmd_camera(["record", "toggle"])
    ack = module_ack(connection, module,
                     mavutil.mavlink.MAV_CMD_VIDEO_STOP_CAPTURE,
                     CAMERA_COMPONENT)
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
    assert not recording_state.exists()

    module._request_message(
        1, CAMERA_COMPONENT, mavutil.mavlink.MAVLINK_MSG_ID_CAMERA_CAPTURE_STATUS)
    status = module_wait(
        connection, module,
        lambda message: (
            message.get_type() == "CAMERA_CAPTURE_STATUS" and
            message.get_srcComponent() == CAMERA_COMPONENT and
            message.video_status == 0))
    assert status.video_status == 0

    module.cmd_camera(["source", "thermal"])
    ack = module_ack(connection, module,
                     mavutil.mavlink.MAV_CMD_SET_CAMERA_SOURCE,
                     CAMERA_COMPONENT)
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
    module._request_message(1, CAMERA_COMPONENT,
                            mavutil.mavlink.MAVLINK_MSG_ID_VIDEO_STREAM_INFORMATION,
                            1)
    stream = module_wait(
        connection, module,
        lambda message: (message.get_type() == "VIDEO_STREAM_INFORMATION" and
                         message.get_srcComponent() == CAMERA_COMPONENT and
                         message.stream_id == 1))
    assert stream.flags & mavutil.mavlink.VIDEO_STREAM_STATUS_FLAGS_THERMAL
    module.cmd_camera(["source", "rgb"])
    module_ack(connection, module, mavutil.mavlink.MAV_CMD_SET_CAMERA_SOURCE,
               CAMERA_COMPONENT)

    module.cmd_camera(["mount", "angle", "-16", "12", "body"])
    ack = module_ack(connection, module,
                     mavutil.mavlink.MAV_CMD_DO_GIMBAL_MANAGER_PITCHYAW, 1)
    assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
    wait_attitude_target(siyi_port, -16.0, 12.0)
    print("PASS MAVProxy camera module discovery and control through ArduPilot")


def run_transport(repo_root, camera_binary, gimbal_script, arducopter,
                  output, transport):
    runtime = output / transport
    run = runtime / "run"
    capture_root = runtime / "capture"
    record_root = runtime / "record"
    for directory in (run, capture_root, record_root):
        directory.mkdir(parents=True, exist_ok=True)
    camera_ready = run / "camera.ready"
    gimbal_ready = run / "gimbal.ready"
    recording_state = run / "recording.state"
    for path in (camera_ready, gimbal_ready, recording_state):
        path.unlink(missing_ok=True)
    for path in capture_root.glob("SITL_*.jpg"):
        path.unlink()

    network_port = reserve_port(
        socket.SOCK_STREAM if transport == "tcp" else socket.SOCK_DGRAM
    )
    gimbal_port = reserve_port(socket.SOCK_DGRAM)
    siyi_port = reserve_port(socket.SOCK_STREAM)
    mavlink_port = reserve_port(socket.SOCK_STREAM)
    params = runtime / "ardupilot.parm"
    write_params(params, transport, network_port)
    log_paths = [run / "gimbal.log", run / "camera.log", run / "arducopter.log"]
    logs = []
    processes = []
    failed = True
    connection = None
    device_connection = None
    try:
        gimbal_log = log_paths[0].open("w", encoding="utf-8")
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
            "CAMERA_APP_MAVLINK_TCP_PORT": str(network_port if transport == "tcp" else 0),
            "CAMERA_APP_MAVLINK_UDP_PORT": str(network_port if transport == "udp" else 0),
            "CAMERA_APP_CONFIG": str(repo_root / "camera_app/camera.ini"),
            "CAMERA_APP_READY_PATH": str(camera_ready),
            "CAMERA_APP_RECORD_STATE": str(recording_state),
            "CAMERA_APP_RECORD_ROOT": str(record_root),
            "CAMERA_APP_CAPTURE_ROOT": str(capture_root),
            "CAMERA_APP_SITL_PHOTO": str(repo_root / "build/sitl/photo.jpg"),
        })
        camera_log = log_paths[1].open("w", encoding="utf-8")
        logs.append(camera_log)
        camera = subprocess.Popen(
            [str(camera_binary)], env=env,
            stdout=camera_log, stderr=subprocess.STDOUT,
        )
        processes.append(camera)
        wait_path(camera_ready, camera)

        copter_log = log_paths[2].open("w", encoding="utf-8")
        logs.append(copter_log)
        instance = os.getpid() % 50 + (50 if transport == "tcp" else 110)
        copter = subprocess.Popen(
            [str(arducopter), "--wipe", "--model", "quad", "--speedup", "1",
            "--home", f"-35.363262,149.165237,{HOME_ALTITUDE_AMSL:g},90",
             "--instance", str(instance),
             "--serial0", f"tcp:{mavlink_port}",
             "--serial1", "none", "--serial2", "none",
             "--defaults", str(params)],
            cwd=runtime, stdout=copter_log, stderr=subprocess.STDOUT,
        )
        processes.append(copter)
        connection = connect_mavlink(mavlink_port, copter)
        device_connection = mavutil.mavlink_connection(
            f"{'tcp' if transport == 'tcp' else 'udpout'}:127.0.0.1:{network_port}",
            source_system=255,
            source_component=mavutil.mavlink.MAV_COMP_ID_MISSIONPLANNER,
        )

        camera_info = request_ap_camera_information(connection)
        assert bytes(camera_info.vendor_name).rstrip(b"\0") == b"ArduPilot"
        assert camera_info.flags & mavutil.mavlink.CAMERA_CAP_FLAGS_CAPTURE_IMAGE
        streams = request_video_stream_information(connection)
        # MISB is primary; raw thermal keeps ID 3, with APCG previews at 4/5.
        assert set(streams) == {1, 2, 3, 4, 5}, streams
        assert all(stream.count == 5 for stream in streams.values()), streams
        assert streams[1].uri.startswith("rtsp://127.0.0.1:")
        assert streams[1].uri.endswith("/video1.misb")
        assert streams[2].uri.startswith("rtsp://127.0.0.1:")
        assert streams[2].uri.endswith("/video2.misb")
        assert streams[2].flags & mavutil.mavlink.VIDEO_STREAM_STATUS_FLAGS_THERMAL
        assert streams[3].type == 200, streams[3]
        assert streams[3].uri.startswith("http://127.0.0.1:")
        assert streams[3].uri.endswith("/thermal.mkv")
        assert streams[3].flags & mavutil.mavlink.VIDEO_STREAM_STATUS_FLAGS_THERMAL
        assert (streams[3].resolution_h, streams[3].resolution_v) == (640, 512)
        for source, compatibility in ((1, 4), (2, 5)):
            preview = streams[compatibility]
            assert preview.uri == streams[source].uri.removesuffix('.misb'), preview
            assert preview.type == streams[source].type
            assert (preview.resolution_h, preview.resolution_v, preview.hfov, preview.encoding) == (
                streams[source].resolution_h, streams[source].resolution_v,
                streams[source].hfov, streams[source].encoding)

        gimbal_info = request_device_message(
            connection, GIMBAL_COMPONENT,
            mavutil.mavlink.MAVLINK_MSG_ID_GIMBAL_DEVICE_INFORMATION,
            "GIMBAL_DEVICE_INFORMATION",
        )
        assert gimbal_info.model_name.rstrip("\0") == "MT11"
        assert (gimbal_info.cap_flags2 &
                mavutil.mavlink.GIMBAL_DEVICE_CAP_FLAGS_CAN_POINT_LOCATION_GLOBAL)

        initial = wait_attitude(siyi_port)
        ack = command(
            connection, 1,
            mavutil.mavlink.MAV_CMD_DO_GIMBAL_MANAGER_PITCHYAW,
            [-8.0, 0.0, math.nan, math.nan, 0.0, 0.0, 0.0],
            ack_component=1,
        )
        assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
        final = wait_attitude(siyi_port, -8.0)
        assert abs(final[1] - initial[1]) > 3.0

        # Exercise the same earth-frame path used by an ROI.  ArduPilot sends
        # an earth-locked quaternion, the camera app converts it into the
        # MT11's vehicle-frame SIYI angle. Check physical pointing and the
        # device's earth-frame feedback independently.
        target_yaw_earth = 70.0
        ack = command(
            connection, 1,
            mavutil.mavlink.MAV_CMD_DO_GIMBAL_MANAGER_PITCHYAW,
            [-18.0, target_yaw_earth, math.nan, math.nan,
             mavutil.mavlink.GIMBAL_MANAGER_FLAGS_YAW_LOCK, 0.0, 0.0],
            ack_component=1,
        )
        assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
        physical_attitude, vehicle_yaw = wait_earth_attitude(
            connection, siyi_port, -18.0, target_yaw_earth)
        earth_error = (
            physical_attitude[2] + vehicle_yaw - target_yaw_earth + 180.0
        ) % 360.0 - 180.0
        assert abs(earth_error) < 6.0, (
            physical_attitude, vehicle_yaw, target_yaw_earth)
        wait_gimbal_earth_attitude(device_connection, -18.0, target_yaw_earth)

        # Exercise native geographic targeting from the operator-facing map
        # ROI command. Use GLOBAL AMSL: upstream AP_Mount_MAVLink does not yet
        # convert relative-home locations to AMSL when forwarding them to a
        # gimbal, and the MT11 only accepts AMSL. The camera owns the continuously
        # updated bearing/elevation loop.  Put the ROI 100m north and 50m below
        # the stationary SITL vehicle for earth yaw 0 and pitch about -26.6deg.
        position = wait_global_position(connection)
        roi_latitude = position.lat * 1.0e-7 + 100.0 / 111319.5
        roi_longitude = position.lon * 1.0e-7
        roi_altitude = position.alt * 1.0e-3 - 50.0
        ack = set_roi(
            connection, roi_latitude, roi_longitude,
            roi_altitude, mavutil.mavlink.MAV_FRAME_GLOBAL)
        assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
        roi_pitch = math.degrees(math.atan2(-50.0, 100.0))
        wait_earth_attitude(connection, siyi_port, roi_pitch, 0.0)
        wait_gimbal_earth_attitude(device_connection, roi_pitch, 0.0)

        ack = command(
            connection, 1, mavutil.mavlink.MAV_CMD_SET_CAMERA_ZOOM,
            [mavutil.mavlink.ZOOM_TYPE_RANGE, 50.0, 1.0], ack_component=1,
        )
        assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
        time.sleep(0.3)
        settings = request_device_message(
            connection, CAMERA_COMPONENT,
            mavutil.mavlink.MAVLINK_MSG_ID_CAMERA_SETTINGS,
            "CAMERA_SETTINGS",
        )
        assert abs(settings.zoomLevel - 50.0) < 0.2

        ack = command(
            connection, 1, mavutil.mavlink.MAV_CMD_SET_CAMERA_FOCUS,
            [mavutil.mavlink.FOCUS_TYPE_AUTO, 0.0, 1.0], ack_component=1,
        )
        assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED

        ack = command(
            connection, 1, mavutil.mavlink.MAV_CMD_SET_CAMERA_FOCUS,
            [mavutil.mavlink.FOCUS_TYPE_RANGE, 35.0, 1.0], ack_component=1,
        )
        assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
        time.sleep(0.3)
        settings = request_device_message(
            connection, CAMERA_COMPONENT,
            mavutil.mavlink.MAVLINK_MSG_ID_CAMERA_SETTINGS,
            "CAMERA_SETTINGS",
        )
        assert abs(settings.focusLevel - 35.0) < 0.2

        ack = command(
            connection, 1, mavutil.mavlink.MAV_CMD_IMAGE_START_CAPTURE,
            [1.0, 0.0, 1.0], ack_component=1,
        )
        assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
        expected = [capture_root / f"SITL_000001_{suffix}.jpg" for suffix in "CZI"]
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and not all(path.exists() for path in expected):
            time.sleep(0.1)
        assert all(path.stat().st_size > 0 for path in expected)

        ack = command(
            connection, 1, mavutil.mavlink.MAV_CMD_VIDEO_START_CAPTURE,
            [1.0], ack_component=1,
        )
        assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not recording_state.exists():
            time.sleep(0.05)
        assert recording_state.exists()
        ack = command(
            connection, 1, mavutil.mavlink.MAV_CMD_VIDEO_STOP_CAPTURE,
            [1.0], ack_component=1,
        )
        assert ack.result == mavutil.mavlink.MAV_RESULT_ACCEPTED
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and recording_state.exists():
            time.sleep(0.05)
        assert not recording_state.exists()

        exercise_mavproxy_camera(connection, siyi_port, capture_root,
                                 recording_state)

        print(f"PASS ArduPilot NET {transport.upper()}: MAVLink mount and camera")
        failed = False
    finally:
        if device_connection is not None:
            device_connection.close()
        if connection is not None:
            connection.close()
        for process in reversed(processes):
            terminate(process)
        for log in logs:
            log.close()
        if failed:
            for path in log_paths:
                print(f"\n--- {path} ---", file=sys.stderr)
                if path.exists():
                    print(path.read_text(errors="replace"), file=sys.stderr)


def main():
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: test_ardupilot_mavlink.py CAMERA_APP GIMBAL_SIM "
            "ARDUCOPTER OUTPUT_DIRECTORY"
        )
    repo_root = pathlib.Path(__file__).resolve().parent.parent
    camera_binary = pathlib.Path(sys.argv[1]).resolve()
    gimbal_script = pathlib.Path(sys.argv[2]).resolve()
    arducopter = pathlib.Path(sys.argv[3]).resolve()
    output = pathlib.Path(sys.argv[4]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    for transport in ("tcp", "udp"):
        run_transport(repo_root, camera_binary, gimbal_script, arducopter,
                      output, transport)
    print("PASS ArduPilot AP_Mount_MAVLink/AP_Camera_MAVLinkCamV2 over NET")


if __name__ == "__main__":
    main()
