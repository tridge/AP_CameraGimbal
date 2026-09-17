#!/usr/bin/env python3
"""Exercise system-ID selection and persistent parameters in camera/gimbal SITL.

Requires pymavlink, MAVProxy and pexpect. All processes use isolated ports and
files; the normal SITL runtime and any running simulator remain untouched.
"""
import argparse
import os
import pathlib
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

os.environ.setdefault("MAVLINK20", "1")
import pexpect
from pymavlink import mavutil

M = mavutil.mavlink
CAMERA = M.MAV_COMP_ID_CAMERA
GIMBAL = M.MAV_COMP_ID_GIMBAL
EXPECTED = {
    "LOG_DISARMED", "OSD_CROSS", "OSD_THERMAL_FOV", "OSD_RECORD",
    "PHOTO_SCOPE", "MOUNT_ORIENT", "UART_PROTOCOL", "THERMAL_PALETTE",
    "REC_AUTOSTART", "REC_RESOLUTION", "VIDEO_MAIN_RES", "VIDEO_MAIN_CODEC",
    "VIDEO_SUB_RES", "VIDEO_SUB_CODEC", "IMG_BRIGHTNESS", "IMG_SATURATION",
    "IMG_CONTRAST", "IMG_EXPOSURE", "IMG_ISO", "IMG_SHUTTER", "IMG_METERING",
    "IMG_WHITE_BAL", "MAV_POS_TARGET", "TRACK_METHOD", "MAV_TCP_PORT", "MAV_UDP_PORT", "MAV_SYSID", "MAV_CAM_COMP_ID",
    "PROXY_ENABLE", "PROXY_MAV_PORT", "PROXY_SIGN", "PROXY_SIGN_ID", "PROXY_VID1_PORT", "PROXY_VID2_PORT",
}


def port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def wait_ready(path, process):
    deadline = time.monotonic() + 10
    while not path.exists():
        assert process.poll() is None, f"process exited: {process.returncode}"
        assert time.monotonic() < deadline, f"no ready file: {path}"
        time.sleep(0.02)


def connect(endpoint):
    return mavutil.mavlink_connection(endpoint, source_system=255,
                                      source_component=190)


def heartbeat(link, system, kind=M.MAV_TYPE_QUADROTOR,
              autopilot=M.MAV_AUTOPILOT_ARDUPILOTMEGA, component=1):
    link.mav.srcSystem, link.mav.srcComponent = system, component
    link.mav.heartbeat_send(kind, autopilot, 0, 0, M.MAV_STATE_ACTIVE)
    link.mav.srcSystem, link.mav.srcComponent = 255, 190


def drain(link):
    while link.recv_match(blocking=False) is not None:
        pass


def receive(link, kind, predicate=lambda m: True, timeout=4):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        message = link.recv_match(type=kind, blocking=True, timeout=0.1)
        if message is not None and predicate(message):
            return message
    raise AssertionError(f"no matching {kind}")


def read(link, name, system=42, component=CAMERA, index=-1):
    drain(link)
    link.mav.param_request_read_send(system, component, name.encode(), index)
    return receive(link, "PARAM_VALUE")


def write(link, name, value, system=42, param_type=M.MAV_PARAM_TYPE_REAL32):
    drain(link)
    link.mav.param_set_send(system, CAMERA, name.encode(), value, param_type)
    response = receive(link, "PARAM_VALUE", lambda m: m.param_id == name)
    assert response.get_srcSystem() == system
    assert response.get_srcComponent() == CAMERA
    return response.param_value


def check_identity(link, system, component=CAMERA):
    seen = set()
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        message = link.recv_match(blocking=True, timeout=0.2)
        if message is None:
            continue
        assert message.get_srcSystem() == system, message
        if message.get_type() == "HEARTBEAT":
            assert message.autopilot == M.MAV_AUTOPILOT_INVALID
            seen.add(message.get_srcComponent())
    gimbal = (154, 171, 172, 173, 174, 175)[component - CAMERA]
    assert seen == {component, gimbal}, seen


def check_telemetry_requests(link, system):
    expected = {
        M.MAVLINK_MSG_ID_GLOBAL_POSITION_INT: 100000,
        M.MAVLINK_MSG_ID_AUTOPILOT_STATE_FOR_GIMBAL_DEVICE: 100000,
        M.MAVLINK_MSG_ID_SYSTEM_TIME: 1000000,
    }
    while expected:
        message = receive(link, "COMMAND_LONG",
                          lambda m: m.command == M.MAV_CMD_SET_MESSAGE_INTERVAL)
        assert message.get_srcSystem() == system
        assert message.get_srcComponent() == GIMBAL
        assert (message.target_system, message.target_component) == (system, 1)
        assert message.param2 == expected.pop(int(message.param1)), message


def mavproxy_checks(endpoint, directory, observer):
    """Use the actual interactive MAVProxy CLI, including its parameter module."""
    transcript = directory / "mavproxy-console.log"
    with transcript.open("w") as log:
        env = dict(os.environ, MPLCONFIGDIR=str(directory / "matplotlib"))
        cli = pexpect.spawn(shutil.which("mavproxy.py"), [
            "--master", endpoint, "--target-system", "42", "--target-component", "100",
            "--source-system", "255", "--mav20", "--nowait", "--default-modules", "param",
            "--cmd", "set param_ftp 0", "--logfile", str(directory / "mavproxy.tlog"),
        ], cwd=str(directory), env=env, encoding="utf-8", timeout=15)
        cli.logfile_read = log
        try:
            cli.expect("MAV>")
            cli.sendline("param fetch")
            cli.expect(f"Received {len(EXPECTED)} parameters")
            cli.sendline("param save fetched.parm")
            cli.expect(rf"Saved {len(EXPECTED)} parameters to fetched\.parm")
            values = (directory / "fetched.parm").read_text()
            assert {line.split()[0] for line in values.splitlines() if line.strip()} == EXPECTED
            cli.sendline("param fetch IMG_BRIGHTNESS")
            cli.expect(r"IMG_BRIGHTNESS\s+=\s+50(?:\.0+)?")
            cli.sendline("param set IMG_BRIGHTNESS 63")
            cli.expect("Parameter applied and saved")
            cli.sendline("param fetch IMG_BRIGHTNESS")
            cli.expect(r"IMG_BRIGHTNESS\s+=\s+63(?:\.0+)?")
            assert read(observer, "IMG_BRIGHTNESS").param_value == 63
            drain(observer)
            cli.sendline("param set IMG_EXPOSURE -4")
            # MAVProxy suppresses repeated identical STATUSTEXT messages.
            receive(observer, "PARAM_VALUE", lambda m:
                    m.param_id == "IMG_EXPOSURE" and m.param_value == -4)
            cli.sendline("param fetch IMG_EXPOSURE")
            cli.expect(r"IMG_EXPOSURE\s+=\s+-4(?:\.0+)?")
            assert read(observer, "IMG_EXPOSURE").param_value == -4
            cli.sendline("param set REC_AUTOSTART 2")
            receive(observer, "PARAM_VALUE", lambda m:
                    m.param_id == "REC_AUTOSTART" and m.param_value == 2)
            cli.sendline("param fetch REC_AUTOSTART")
            cli.expect(r"REC_AUTOSTART\s+=\s+2(?:\.0+)?")
            assert write(observer, "REC_AUTOSTART", 0) == 0
        finally:
            cli.close(force=True)
    print(f"MAVProxy fetch-all/get/set passed ({transcript})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=("mt11", "a8"), default="mt11")
    parser.add_argument("--build", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    if args.backend != "mt11":
        EXPECTED.discard("OSD_THERMAL_FOV")
    repo = pathlib.Path(__file__).resolve().parents[1]
    build = (args.build or repo / "build" / ("sitl" if args.backend == "mt11" else "a8-sitl")).resolve()
    directory = (args.output or pathlib.Path(tempfile.mkdtemp(prefix="camera-mavlink-test-"))).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    assert shutil.which("mavproxy.py"), "install MAVProxy to run this test"
    config = directory / "camera.ini"
    template = repo / ("camera_app/camera.ini" if args.backend == "mt11" else "packaging/a8/camera.ini")
    config.write_text(template.read_text() + "\n[future]\n# Preserve this comment\nunknown = 99\n")
    (directory / "gimbal.ready").unlink(missing_ok=True)
    gimbal_port, tcp_port, udp_port = port(socket.SOCK_DGRAM), port(), port(socket.SOCK_DGRAM)
    ready, gimbal_ready = directory / "camera.ready", directory / "gimbal.ready"
    env = dict(os.environ,
        CAMERA_APP_BACKEND=args.backend,
        CAMERA_APP_UART=f"udp://127.0.0.1:{gimbal_port}",
        CAMERA_APP_PORT=str(port()), CAMERA_APP_RTSP_PORT=str(port()),
        CAMERA_APP_MAVLINK_TCP_PORT=str(tcp_port), CAMERA_APP_MAVLINK_UDP_PORT=str(udp_port),
        CAMERA_APP_CONFIG=str(config), CAMERA_APP_READY_PATH=str(ready),
        CAMERA_APP_RECORD_STATE=str(directory / "recording.state"),
        CAMERA_APP_RECORD_ROOT=str(directory / "record"),
        CAMERA_APP_CAPTURE_ROOT=str(directory / "capture"),
        CAMERA_APP_SITL_VIDEO1=str(build / ("rgb.h264" if args.backend == "mt11" else "main.h264")),
        CAMERA_APP_SITL_VIDEO2=str(build / ("thermal.h264" if args.backend == "mt11" else "sub.h264")),
        CAMERA_APP_SITL_PHOTO=str(build / "photo.jpg"))
    camera = gimbal = link = udp = None
    with (directory / "camera.log").open("w") as camera_log, (directory / "gimbal.log").open("w") as gimbal_log:
        def start_camera():
            ready.unlink(missing_ok=True)
            process = subprocess.Popen([str(build / "camera-app"), "--backend", args.backend],
                                       env=env, stdout=camera_log, stderr=subprocess.STDOUT)
            wait_ready(ready, process)
            return process
        try:
            gimbal = subprocess.Popen([sys.executable, str(repo / "sitl/gimbal_sim.py"),
                "--backend", args.backend, "--port", str(gimbal_port), "--ready-file", str(gimbal_ready)],
                stdout=gimbal_log, stderr=subprocess.STDOUT)
            wait_ready(gimbal_ready, gimbal)
            camera = start_camera()
            endpoint = f"tcp:127.0.0.1:{tcp_port}"
            link = connect(endpoint)
            for kind, autopilot in ((M.MAV_TYPE_GCS, M.MAV_AUTOPILOT_INVALID),
                                    (M.MAV_TYPE_GCS, M.MAV_AUTOPILOT_ARDUPILOTMEGA),
                                    (M.MAV_TYPE_CAMERA, M.MAV_AUTOPILOT_INVALID)):
                heartbeat(link, 255, kind, autopilot)
            assert link.recv_match(blocking=True, timeout=1.2) is None, "auto mode transmitted before FC heartbeat"
            heartbeat(link, 42)
            check_telemetry_requests(link, 42)
            check_identity(link, 42)
            heartbeat(link, 43)
            heartbeat(link, 255, M.MAV_TYPE_GCS)
            drain(link)
            check_identity(link, 42)
            print("Automatic ID waits for FC, ignores GCS, and retains first FC passed")
            heartbeat(link, 42)
            check_telemetry_requests(link, 42)
            print("Selected FC receives 10 Hz motion and 1 Hz SYSTEM_TIME requests, renewed after five seconds")

            drain(link)
            link.mav.param_request_list_send(42, 0)
            values = {}
            while len(values) < len(EXPECTED):
                message = receive(link, "PARAM_VALUE")
                assert message.get_srcComponent() == CAMERA
                assert message.param_count == len(EXPECTED) and message.param_type == M.MAV_PARAM_TYPE_INT32
                assert re.fullmatch(r"[A-Z][A-Z0-9_]{0,15}", message.param_id)
                values[message.param_id] = message
            assert set(values) == EXPECTED
            assert {m.param_index for m in values.values()} == set(range(len(EXPECTED)))
            for name, value in values.items():
                assert read(link, "", index=value.param_index).param_id == name
            assert read(link, "VIDEO_MAIN_CODEC").param_value == 0  # exactly 16 bytes
            for system, component in ((43, CAMERA), (42, 1), (42, GIMBAL)):
                drain(link)
                link.mav.param_request_list_send(system, component)
                link.mav.param_request_read_send(system, component, b"IMG_BRIGHTNESS", -1)
                link.mav.param_set_send(system, component, b"IMG_BRIGHTNESS", 98, M.MAV_PARAM_TYPE_REAL32)
                assert link.recv_match(type="PARAM_VALUE", blocking=True, timeout=0.3) is None
            assert read(link, "IMG_BRIGHTNESS").param_value == 50
            for name, index in (("UNKNOWN_PARAM_16X", -1), ("", -2), ("", len(EXPECTED))):
                drain(link)
                link.mav.param_request_read_send(42, CAMERA, name.encode(), index)
                assert link.recv_match(type="PARAM_VALUE", blocking=True, timeout=0.2) is None
            for name, invalid in (("IMG_BRIGHTNESS", 101), ("IMG_BRIGHTNESS", 1.5),
                                  ("IMG_BRIGHTNESS", float("nan")), ("IMG_BRIGHTNESS", float("inf")),
                                  ("REC_AUTOSTART", 3), ("THERMAL_PALETTE", 1),
                                  ("MAV_SYSID", -1), ("MAV_SYSID", 256),
                                  ("MAV_CAM_COMP_ID", 99), ("MAV_CAM_COMP_ID", 106),
                                  ("MAV_CAM_COMP_ID", 100.5)):
                original = read(link, name).param_value
                before = config.read_bytes()
                assert write(link, name, invalid) == original
                assert config.read_bytes() == before
            assert write(link, "IMG_BRIGHTNESS", 70, param_type=M.MAV_PARAM_TYPE_UINT8) == 50
            assert write(link, "REC_AUTOSTART", 2) == 2
            assert read(link, "REC_AUTOSTART").param_value == 2
            assert "autorecord = while_armed" in config.read_text()
            assert write(link, "REC_AUTOSTART", 0) == 0
            # Protocol capability discovery and command ACK must share the adopted ID.
            link.mav.command_long_send(42, CAMERA, M.MAV_CMD_REQUEST_MESSAGE, 0,
                                      M.MAVLINK_MSG_ID_AUTOPILOT_VERSION, 0, 0, 0, 0, 0, 0)
            version = receive(link, "AUTOPILOT_VERSION")
            assert version.capabilities & M.MAV_PROTOCOL_CAPABILITY_PARAM_ENCODE_C_CAST
            ack = receive(link, "COMMAND_ACK")
            assert ack.get_srcSystem() == 42 and ack.result == M.MAV_RESULT_ACCEPTED
            mavproxy_checks(endpoint, directory, link)
            # UDP uses the same identity, database and handlers as TCP.
            udp = connect(f"udpout:127.0.0.1:{udp_port}")
            assert read(udp, "IMG_BRIGHTNESS").param_value == 63
            assert write(udp, "MAV_POS_TARGET", 0) == 0
            assert write(udp, "VIDEO_MAIN_CODEC", 1) == 1
            assert read(link, "VIDEO_MAIN_CODEC").param_value == 1
            # A web/raw-config edit made since startup must be visible and retained.
            config.write_text(config.read_text().replace("contrast = 50", "contrast = 67"))
            assert read(link, "IMG_CONTRAST").param_value == 67
            assert write(link, "MAV_SYSID", 77) == 77
            assert "unknown = 99" in config.read_text() and "# Preserve this comment" in config.read_text()
            assert "codec = h265" in config.read_text()
            drain(link)
            check_identity(link, 42)  # configured ID applies only after restart
            link.close()
            stop(camera)
            camera = start_camera()
            link = connect(endpoint)
            check_identity(link, 77)  # fixed ID is usable without a flight controller
            assert read(link, "MAV_SYSID", system=77).param_value == 77
            assert read(link, "IMG_BRIGHTNESS", system=77).param_value == 63
            assert read(link, "IMG_EXPOSURE", system=77).param_value == -4
            assert read(link, "IMG_CONTRAST", system=77).param_value == 67
            assert read(link, "MAV_POS_TARGET", system=77).param_value == 0
            heartbeat(link, 42)
            drain(link)
            check_identity(link, 77)
            assert write(link, "MAV_SYSID", 0, system=77) == 0
            link.close()
            stop(camera)
            camera = start_camera()
            link = connect(endpoint)
            assert link.recv_match(blocking=True, timeout=1.2) is None
            heartbeat(link, 81)
            check_identity(link, 81)
            assert read(link, "MAV_SYSID", system=81).param_value == 0
            # Saving identity must keep the old address until restart, and
            # afterwards all camera traffic must use the configured component.
            assert write(link, "MAV_CAM_COMP_ID", 105, system=81) == 105
            drain(link)
            check_identity(link, 81)
            link.close()
            stop(camera)
            camera = start_camera()
            link = connect(endpoint)
            heartbeat(link, 81)
            check_identity(link, 81, component=105)
            assert read(link, "MAV_CAM_COMP_ID", system=81, component=105).param_value == 105
            assert read(udp, "MAV_CAM_COMP_ID", system=81, component=105).param_value == 105
            drain(link)
            link.mav.param_request_list_send(81, 105)
            configured_values = {}
            while len(configured_values) < len(EXPECTED):
                message = receive(link, "PARAM_VALUE")
                assert message.get_srcComponent() == 105
                configured_values[message.param_id] = message.param_value
            assert set(configured_values) == EXPECTED
            drain(link)
            link.mav.param_set_send(81, 100, b"IMG_BRIGHTNESS", 90, M.MAV_PARAM_TYPE_REAL32)
            assert link.recv_match(type="PARAM_VALUE", blocking=True, timeout=.3) is None
            assert read(link, "IMG_BRIGHTNESS", system=81, component=105).param_value == 63
            print("Camera component 105: persistence, heartbeat, TCP/UDP parameters and old-address rejection passed")
            print(f"{args.backend} SITL MAVLink parameters passed: TCP/UDP, addressing, validation, persistence, fixed/auto ID")
        finally:
            if link is not None:
                link.close()
            if udp is not None:
                udp.close()
            stop(camera)
            stop(gimbal)


if __name__ == "__main__":
    main()
