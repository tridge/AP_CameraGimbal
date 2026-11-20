#!/usr/bin/env python3
"""Exercise ArduPilot's SIYI mount and camera drivers against MT11 SITL."""

import math
import os
import pathlib
import signal
import socket
import struct
import subprocess
import sys
import time

os.environ.setdefault("MAVLINK20", "1")

from pymavlink import mavutil  # noqa: E402
from pymavlink.quaternion import Quaternion  # noqa: E402


def reserve_port(socktype):
    sock = socket.socket(socket.AF_INET, socktype)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


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
        chunk = sock.recv(length - len(result))
        if not chunk:
            raise ConnectionError("SIYI TCP connection closed")
        result.extend(chunk)
    return bytes(result)


def query_zoom(port):
    header = struct.pack("<BBBHHB", 0x55, 0x66, 1, 0, 0xCAFE, 0x18)
    request = header + struct.pack("<H", crc16(header))
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        sock.sendall(request)
        while True:
            response_header = read_exact(sock, 8)
            payload_length = struct.unpack_from("<H", response_header, 3)[0]
            if payload_length > 1024:
                raise AssertionError("oversized SIYI response")
            response = response_header + read_exact(sock, payload_length + 2)
            if response[:2] != b"\x55\x66":
                raise AssertionError("unexpected SIYI response header")
            received_crc = struct.unpack_from("<H", response, len(response) - 2)[0]
            if crc16(response[:-2]) != received_crc:
                raise AssertionError("bad SIYI response CRC")
            if response[7] != 0x18:
                continue
            payload = response[8:-2]
            if len(payload) != 2:
                raise AssertionError(
                    f"unexpected SIYI zoom payload length {len(payload)}"
                )
            return payload[0] + payload[1] / 10.0


def terminate(process):
    if process is None or process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def wait_for_path(path, process, description, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        if process.poll() is not None:
            raise AssertionError(f"{description} exited with status {process.returncode}")
        time.sleep(0.05)
    raise TimeoutError(f"timed out waiting for {description}")


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
            heartbeat = connection.wait_heartbeat(timeout=5)
            if heartbeat is not None:
                return connection
            connection.close()
        except (ConnectionError, OSError):
            pass
        time.sleep(0.2)
    raise TimeoutError("timed out connecting to ArduCopter MAVLink")


def command(connection, command_id, params, timeout=10):
    while connection.recv_match(type="COMMAND_ACK", blocking=False) is not None:
        pass
    values = list(params) + [0.0] * (7 - len(params))
    connection.mav.command_long_send(
        connection.target_system, connection.target_component, command_id, 0,
        *values[:7],
    )
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ack = connection.recv_match(type="COMMAND_ACK", blocking=True, timeout=1)
        if ack is None or ack.command != command_id:
            continue
        accepted = {
            mavutil.mavlink.MAV_RESULT_ACCEPTED,
            mavutil.mavlink.MAV_RESULT_IN_PROGRESS,
        }
        if ack.result not in accepted:
            raise AssertionError(
                f"MAVLink command {command_id} rejected with result {ack.result}"
            )
        return ack
    raise TimeoutError(f"no acknowledgement for MAVLink command {command_id}")


def decode_fixed_string(value):
    if isinstance(value, str):
        return value.rstrip("\0")
    return bytes(value).split(b"\0", 1)[0].decode("ascii", errors="replace")


def wait_camera_information(connection, timeout=35):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        command(
            connection, mavutil.mavlink.MAV_CMD_REQUEST_MESSAGE,
            [mavutil.mavlink.MAVLINK_MSG_ID_CAMERA_INFORMATION, 0], timeout=5,
        )
        message = connection.recv_match(
            type="CAMERA_INFORMATION", blocking=True, timeout=3
        )
        # Model discovery can complete a scheduler cycle before the firmware
        # query. Wait for both so this checks the complete SIYI handshake.
        if message is not None and message.firmware_version != 0:
            return message
    raise TimeoutError("ArduPilot did not publish CAMERA_INFORMATION")


def attitude_degrees(message):
    roll, pitch, yaw = Quaternion(message.q).euler
    return tuple(math.degrees(value) for value in (roll, pitch, yaw))


def wait_attitude(connection, predicate, timeout=20):
    deadline = time.monotonic() + timeout
    latest = None
    while time.monotonic() < deadline:
        message = connection.recv_match(
            type="GIMBAL_DEVICE_ATTITUDE_STATUS", blocking=True, timeout=1
        )
        if message is None:
            continue
        latest = attitude_degrees(message)
        if predicate(latest):
            return latest
    raise TimeoutError(f"gimbal attitude did not converge; latest={latest}")


def show_log_tails(log_paths):
    for path in log_paths:
        print(f"\n--- {path} (tail) ---", file=sys.stderr)
        try:
            lines = path.read_text(errors="replace").splitlines()
            print("\n".join(lines[-80:]), file=sys.stderr)
        except OSError as error:
            print(error, file=sys.stderr)


def main():
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: test_ardupilot_siyi.py CAMERA_APP GIMBAL_SIM "
            "ARDUCOPTER OUTPUT_DIRECTORY"
        )
    camera_binary = pathlib.Path(sys.argv[1]).resolve()
    gimbal_script = pathlib.Path(sys.argv[2]).resolve()
    arducopter = pathlib.Path(sys.argv[3]).resolve()
    output = pathlib.Path(sys.argv[4]).resolve()
    repo_root = pathlib.Path(__file__).resolve().parent.parent
    runtime = output / "runtime"
    run = runtime / "run"
    capture_root = runtime / "mnt/DCIM/capture"
    record_root = runtime / "mnt/DCIM/record"
    for directory in (run, capture_root, record_root):
        directory.mkdir(parents=True, exist_ok=True)

    log_paths = [run / "gimbal.log", run / "camera-app.log", run / "arducopter.log"]
    gimbal_ready = run / "gimbal.ready"
    camera_ready = run / "camera-app.ready"
    for path in (gimbal_ready, camera_ready):
        path.unlink(missing_ok=True)
    for path in capture_root.glob("SITL_*.jpg"):
        path.unlink()

    gimbal_port = reserve_port(socket.SOCK_DGRAM)
    camera_port = reserve_port(socket.SOCK_STREAM)
    mavlink_port = reserve_port(socket.SOCK_STREAM)
    instance = os.getpid() % 100 + 10
    processes = []
    logs = []
    failed = True
    try:
        gimbal_log = log_paths[0].open("w", encoding="utf-8")
        logs.append(gimbal_log)
        gimbal = subprocess.Popen(
            [sys.executable, str(gimbal_script), "--port", str(gimbal_port),
             "--ready-file", str(gimbal_ready)],
            stdout=gimbal_log, stderr=subprocess.STDOUT,
        )
        processes.append(gimbal)
        wait_for_path(gimbal_ready, gimbal, "gimbal simulator")

        env = os.environ.copy()
        env.update({
            "CAMERA_APP_UART": f"udp://127.0.0.1:{gimbal_port}",
            "CAMERA_APP_PORT": str(camera_port),
            "CAMERA_APP_MAVLINK_TCP_PORT": "0",
            "CAMERA_APP_MAVLINK_UDP_PORT": "0",
            "CAMERA_APP_CONFIG": str(repo_root / "camera_app/camera.ini"),
            "CAMERA_APP_READY_PATH": str(camera_ready),
            "CAMERA_APP_RECORD_STATE": str(run / "recording.state"),
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
        wait_for_path(camera_ready, camera, "camera app")

        copter_log = log_paths[2].open("w", encoding="utf-8")
        logs.append(copter_log)
        copter = subprocess.Popen(
            [str(arducopter), "--wipe", "--model", "quad", "--speedup", "1",
             "--instance", str(instance),
             "--serial0", f"tcp:{mavlink_port}",
             "--serial1", "none", "--serial2", "none",
             "--serial5", f"tcpclient:127.0.0.1:{camera_port}",
             "--defaults", str(repo_root / "tests/ardupilot_siyi.parm")],
            cwd=runtime, stdout=copter_log, stderr=subprocess.STDOUT,
        )
        processes.append(copter)
        connection = connect_mavlink(mavlink_port, copter)

        information = wait_camera_information(connection)
        vendor = decode_fixed_string(information.vendor_name)
        model = decode_fixed_string(information.model_name)
        assert vendor == "Siyi", vendor
        assert model == "ZT30", model
        assert information.firmware_version == 0x0409, hex(information.firmware_version)
        print(f"AP_Camera: identified {vendor} {model}, firmware 0x{information.firmware_version:04x}")

        command(
            connection, mavutil.mavlink.MAV_CMD_SET_MESSAGE_INTERVAL,
            [mavutil.mavlink.MAVLINK_MSG_ID_GIMBAL_DEVICE_ATTITUDE_STATUS,
             100000.0],
        )
        initial = wait_attitude(connection, lambda _angles: True)
        # AP_Mount_Siyi uses P=1.5 and 90 deg/s full scale, truncating its
        # command to an integer. The measured MT11 dead zone is +/-5 command
        # units, so it can stop up to 6 * 90 / (100 * 1.5) = 3.6 degrees
        # short. See docs/mt11-rate-calibration.md. Preserve the real plant;
        # check substantial motion in both directions within that bound.
        tolerance = 3.6 + 0.1  # feedback is quantised to 0.1 degrees
        previous = initial
        for target_pitch in (-5.0, -35.0):
            command(
                connection, mavutil.mavlink.MAV_CMD_DO_GIMBAL_MANAGER_PITCHYAW,
                [target_pitch, 0.0, math.nan, math.nan, 0, 0, 0],
            )
            final = wait_attitude(
                connection,
                lambda angles: abs(angles[1] - target_pitch) <= tolerance,
            )
            assert abs(final[1] - previous[1]) > 5.0, (previous, final)
            assert abs(final[2]) < 2.0, final
            print(f"AP_Mount: target pitch {target_pitch}, attitude moved from {previous} to {final}")
            previous = final

        command(
            connection, mavutil.mavlink.MAV_CMD_SET_CAMERA_ZOOM,
            [mavutil.mavlink.ZOOM_TYPE_RANGE, 20.0, 1.0],
        )
        # ArduPilot's ZT30 profile maps 20% to a requested 6.8x system
        # zoom. MT11's 3.44x base lens quantises that to 1.9x optical:
        # 3.44 * 1.9 = 6.536x, reported as 6.5x by SIYI's tenths field.
        # Check achieved zoom, not the unquantised request.
        expected_zoom = 6.5
        deadline = time.monotonic() + 5
        zoom = query_zoom(camera_port)
        while time.monotonic() < deadline and abs(zoom - expected_zoom) >= 0.01:
            time.sleep(0.1)
            zoom = query_zoom(camera_port)
        assert abs(zoom - expected_zoom) < 0.01, zoom
        command(
            connection, mavutil.mavlink.MAV_CMD_IMAGE_START_CAPTURE,
            [1.0, 0.0, 1.0],
        )
        expected = [capture_root / f"SITL_000001_{suffix}.jpg" for suffix in "CZI"]
        fixture = (repo_root / "build/sitl/photo.jpg").read_bytes()
        assert fixture, "SITL photo fixture is empty"
        # Creating the final file precedes writing it. On a busy CI runner
        # existence alone can observe the last capture while it is empty.
        def captures_complete():
            return all(path.is_file() and path.stat().st_size == len(fixture)
                       for path in expected)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and not captures_complete():
            time.sleep(0.1)
        assert captures_complete(), expected
        assert all(path.read_bytes() == fixture for path in expected), expected
        print(f"AP_Camera: zoom reached {zoom:.1f}x and C/Z/I shutter files captured")

        connection.close()
        failed = False
    finally:
        for process in reversed(processes):
            terminate(process)
        for log in logs:
            log.close()
        if failed:
            show_log_tails(log_paths)
    print("PASS ArduPilot AP_Mount/AP_Camera SIYI integration against MT11 SITL")


if __name__ == "__main__":
    main()
