#!/usr/bin/env python3
"""RPi video stream 1 URI: configured URI, live reload, rejection and automatic fallback.
Also checks that changing the gimbal UART device override asks for a restart."""
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

os.environ.setdefault("MAVLINK20", "1")

from pymavlink import mavutil  # noqa: E402

CAMERA_COMPONENT = 100


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


def write_config(path, video1_uri, telemetry="misb", uart_device_override='""'):
    # Replace the file so the camera app's change detection always sees it.
    temporary = path.with_suffix(".tmp")
    temporary.write_text(
        "[general]\ntimezone = UTC\n"
        "[mavlink]\nsystem_id = 1\n"
        f"[network]\nvideo1_uri = {video1_uri}\n"
        f"uart_device_override = {uart_device_override}\n"
        f"[video]\ntelemetry = {telemetry}\n",
        encoding="ascii",
    )
    os.replace(temporary, path)


def stream_uri(connection):
    connection.mav.command_long_send(
        1, CAMERA_COMPONENT,
        mavutil.mavlink.MAV_CMD_REQUEST_VIDEO_STREAM_INFORMATION, 0,
        1, 0, 0, 0, 0, 0, 0)
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        message = connection.recv_match(
            type="VIDEO_STREAM_INFORMATION", blocking=True, timeout=0.5)
        if message is not None and message.stream_id == 1:
            return message.uri
    raise AssertionError("no VIDEO_STREAM_INFORMATION")


def wait_uri(connection, predicate, timeout=5.0):
    deadline = time.monotonic() + timeout
    uri = None
    while time.monotonic() < deadline:
        uri = stream_uri(connection)
        if predicate(uri):
            return uri
        time.sleep(0.2)
    raise AssertionError(f"unexpected stream URI {uri!r}")


def wait_config_state(status, process, state, timeout=5.0):
    deadline = time.monotonic() + timeout
    seen = None
    while time.monotonic() < deadline:
        if status.exists():
            seen = status.read_text(encoding="ascii").split("\n")[0].split()[2]
            if seen == state:
                return
        assert process.poll() is None, "camera app exited"
        time.sleep(0.1)
    raise AssertionError(f"config state {seen!r}, expected {state!r}")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_rpi_video_uri.py CAMERA_APP_RPI_HOST")
    binary = str(pathlib.Path(sys.argv[1]).resolve())
    mavlink_port = reserve_port(socket.SOCK_STREAM)
    first = "rtsp://192.0.2.10:8554/video1"
    second = "rtsps://pi.example.com:9000/camera/main"
    with tempfile.TemporaryDirectory() as directory:
        temporary = pathlib.Path(directory)
        ready = temporary / "ready"
        config = temporary / "camera.ini"
        write_config(config, first)
        env = os.environ.copy()
        env.update({
            "CAMERA_APP_READY_PATH": str(ready),
            "CAMERA_APP_MAVLINK_TCP_PORT": str(mavlink_port),
            "CAMERA_APP_MAVLINK_UDP_PORT": "0",
            "CAMERA_APP_RECORD_ROOT": str(temporary),
            "CAMERA_APP_CAPTURE_ROOT": str(temporary),
            "CAMERA_APP_LOG_ROOT": str(temporary),
        })
        process = subprocess.Popen(
            [binary, "--backend", "rpi_libcam_caddx", "--config", str(config)],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        try:
            wait_path(ready, process)
            # A loopback MAVLink route stands in for a local mavlink-router.
            connection = mavutil.mavlink_connection(
                f"tcp:127.0.0.1:{mavlink_port}", source_system=250)
            assert stream_uri(connection) == first

            write_config(config, second)
            wait_uri(connection, lambda uri: uri == second)

            # Not an RTSP URI; keep the last good value.
            write_config(config, "http://pi.example.com/video1")
            time.sleep(1.5)
            assert stream_uri(connection) == second

            write_config(config, '""')
            automatic = wait_uri(connection, lambda uri: uri != second)
            assert automatic.startswith("rtsp://") and automatic.endswith("/video1.misb")
            write_config(config, first, telemetry="apcg")
            wait_uri(connection, lambda uri: uri == first)
            write_config(config, '""', telemetry="apcg")
            wait_uri(connection, lambda uri: uri.endswith("/video1"))
            assert process.poll() is None, "camera app restarted"

            # The gimbal link is only read at startup, so saving it needs a restart.
            status = pathlib.Path(str(ready) + ".config")
            write_config(config, '""')
            wait_config_state(status, process, "applied")
            write_config(config, '""', uart_device_override="/dev/ttyAMA0")
            wait_config_state(status, process, "restart")
            connection.close()
        finally:
            process.terminate()
            try:
                output, _ = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                output, _ = process.communicate()
        assert "Video 1 URI must be blank or an rtsp:// or rtsps:// URI" in output, output
    print("PASS RPi video 1 URI: configured, live reload, invalid kept previous, automatic;\n"
          "     a changed UART device override needs a restart")


if __name__ == "__main__":
    main()
