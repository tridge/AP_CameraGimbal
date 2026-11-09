#!/usr/bin/env python3
"""Test MAVLink telemetry in RTP access units and camera MP4 recordings in SITL."""
import argparse
import json
import math
import os
import pathlib
import random
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

os.environ.setdefault("MAVLINK20", "1")
from pymavlink import mavutil
from pymavlink.quaternion import Quaternion

REPO = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
from video_telemetry import nals, telemetry
from target_properties import TARGETS

M = mavutil.mavlink
LAT = -353632610


_issued = set()


def _ephemeral_start():
    try:
        return int(pathlib.Path("/proc/sys/net/ipv4/ip_local_port_range").read_text().split()[0])
    except (OSError, ValueError, IndexError):
        return 32768


def _candidate():
    end = _ephemeral_start()
    if end - 20000 >= 1000:
        return random.randrange(20000, end)
    # no room below the ephemeral range: let the kernel pick
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def port(kind=socket.SOCK_STREAM):
    """A free localhost port below the kernel's ephemeral range, so outgoing connections cannot
    take it before the simulator binds it; never the same port twice in one run."""
    for _ in range(1000):
        candidate = _candidate()
        if candidate in _issued:
            continue
        try:
            for k in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
                with socket.socket(socket.AF_INET, k) as sock:
                    sock.bind(("127.0.0.1", candidate))
        except OSError:
            continue
        _issued.add(candidate)
        return candidate
    raise RuntimeError("no free localhost port")


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def _log_tail(log):
    if not log:
        return ""
    tail = "".join(pathlib.Path(log).read_text(errors="replace").splitlines(True)[-40:])
    return f"; {log}:\n{tail}" if tail else ""


def ready(path, process, log=None):
    until = time.monotonic() + 10
    while not path.exists():
        if process.poll() is not None:
            raise AssertionError(f"exited with {process.returncode} before {path.name}" + _log_tail(log))
        if time.monotonic() >= until:
            raise AssertionError(f"timed out waiting for {path}" + _log_tail(log))
        time.sleep(0.02)


def command(link, value, *params):
    link.mav.command_long_send(42, M.MAV_COMP_ID_CAMERA, value, 0, *(list(params) + [0] * (7 - len(params))))
    until = time.monotonic() + 3
    while time.monotonic() < until:
        ack = link.recv_match(type="COMMAND_ACK", blocking=True, timeout=0.2)
        if ack is not None and ack.command == value:
            assert ack.result == M.MAV_RESULT_ACCEPTED, ack
            return
    raise AssertionError("missing recording ACK")


class RTSP:
    """Minimal interleaved client checking RTP marker/SEI association on wire."""
    def __init__(self, port, stream):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.file = self.sock.makefile("rb")
        self.url = f"rtsp://127.0.0.1:{port}/{stream}"
        self.sequence = 0
        self.session = None
        _, sdp = self.request("DESCRIBE", self.url, {"Accept": "application/sdp"})
        controls = re.findall(rb"a=control:([^\r\n]+)", sdp)
        control = next(c.decode() for c in controls if c != b"*")
        url = control if control.startswith("rtsp://") else self.url + "/" + control
        headers, _ = self.request("SETUP", url, {"Transport": "RTP/AVP/TCP;unicast;interleaved=0-1"})
        self.session = headers["session"].split(";")[0]
        self.request("PLAY", self.url)

    def request(self, method, url, headers=None):
        self.sequence += 1
        headers = dict(headers or {}, CSeq=str(self.sequence))
        if self.session:
            headers["Session"] = self.session
        request = f"{method} {url} RTSP/1.0\r\n" + "".join(f"{k}: {v}\r\n" for k, v in headers.items()) + "\r\n"
        self.sock.sendall(request.encode())
        status = self.file.readline()
        assert b" 200 " in status, status
        response = {}
        while True:
            line = self.file.readline()
            if line == b"\r\n":
                break
            key, value = line.decode().split(":", 1)
            response[key.lower()] = value.strip()
        return response, self.file.read(int(response.get("content-length", 0)))

    def frames(self, count, codec):
        current = []
        fragment = bytearray()
        timestamp = None
        for_frame = 0
        while for_frame < count:
            header = self.file.read(4)
            assert len(header) == 4 and header[0] == 36, header
            packet = self.file.read(struct.unpack("!H", header[2:])[0])
            if header[1] != 0:
                continue
            assert len(packet) >= 12 and packet[0] == 0x80
            pts = struct.unpack_from("!I", packet, 4)[0]
            if timestamp is None:
                timestamp = pts
            assert timestamp == pts, "timestamp advanced before end-of-frame marker"
            payload = packet[12:]
            kind = payload[0] & 31 if codec == "h264" else (payload[0] >> 1) & 63
            fragmented = kind == (28 if codec == "h264" else 49)
            if fragmented:
                fu = payload[1 if codec == "h264" else 2]
                if fu & 0x80:
                    assert not fragment
                    fragment.extend(bytes([(payload[0] & 0xe0) | (fu & 31)]) if codec == "h264" else
                                    bytes([(payload[0] & 0x81) | ((fu & 63) << 1), payload[1]]))
                fragment.extend(payload[2 if codec == "h264" else 3:])
                if fu & 0x40:
                    current.append(bytes(fragment))
                    fragment.clear()
            else:
                assert not fragment
                current.append(payload)
            if packet[1] & 0x80:
                assert not fragment
                assert any((1 <= (n[0] & 31) <= 5) if codec == "h264" else ((n[0] >> 1) & 63) <= 31
                           for n in current), "RTP marker on an SEI/parameter-set-only access unit"
                records = [r for n in current for r in telemetry(n, codec)]
                assert len(records) == 1, (len(records), pts)
                assert records[0]["pts90k"] == pts
                yield current, records[0]
                for_frame += 1
                current = []
                timestamp = None

    def close(self):
        self.file.close()
        self.sock.close()


def video_size(path):
    info = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
        '-show_entries', 'stream=width,height', '-of', 'json', str(path)]))['streams'][0]
    return info['width'], info['height']


def decode(path):
    result = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-map", "0:v:0", "-f", "null", "-"],
                            capture_output=True, timeout=30)
    assert result.returncode == 0 and not result.stderr, result.stderr.decode()


def check_mp4(path, thermal=False, visible_fov=88):
    probe = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-count_packets", "-show_streams", "-of", "json", str(path)]))
    assert len(probe["streams"]) == 1
    video = probe["streams"][0]
    assert video["codec_name"] == "h264"
    assert (video['width'], video['height']) == ((1280, 720) if thermal else (1920, 1080))
    data = subprocess.check_output(["ffmpeg", "-v", "error", "-i", str(path), "-map", "0:v:0", "-c", "copy",
                                    "-bsf:v", "h264_mp4toannexb", "-f", "h264", "pipe:1"])
    import io
    records = [r for n in nals(io.BytesIO(data)) for r in telemetry(n)]
    assert len(records) == int(video["nb_read_packets"])
    assert all(r["position"] is not None for r in records)
    assert all(0 < r["hfov_deg"] < 180 for r in records)
    if thermal:
        assert all(abs(r["hfov_deg"] - 24.2) < .001 for r in records)
    else:
        assert abs(max(r["hfov_deg"] for r in records) - visible_fov) < .001
        assert min(r["hfov_deg"] for r in records) < 30
    packets = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-select_streams", "v:0",
        "-show_packets", "-show_entries", "packet=pts,duration", "-of", "json", str(path)]))["packets"]
    assert [r["pts90k"] for r in records] == [p["pts"] for p in packets]
    assert packets[0]["pts"] == 0
    assert sum(p["duration"] for p in packets) == int(video["duration_ts"])
    decode(path)
    print(f"PASS {path.name}: {len(records)} frame SEIs with matching video timestamps", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=("mt11", "a8"), default="mt11")
    parser.add_argument("--codec", choices=("h264", "hevc"), default="h264")
    parser.add_argument("--build", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--check-stale", action="store_true")
    parser.add_argument("--terrain", action="store_true", help="test optional real 3D terrain video (requires network/cache)")
    args = parser.parse_args()
    target = TARGETS[args.backend]
    wide_fov = target["lens1_fov_h"]
    if args.terrain and args.codec != "h264":
        parser.error("terrain rendering uses H.264")
    build = (args.build or REPO / "build" / ("sitl" if args.backend == "mt11" else "a8-sitl")).resolve()
    directory = (args.output or pathlib.Path(tempfile.mkdtemp(prefix="video-telemetry-"))).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    print(f"Logs and media: {directory}", flush=True)
    width, height, fps = (640, 360, 10) if args.backend == "mt11" else (1280, 720, 25)
    if args.terrain:
        fps = 20
    fixture = directory / ("source.h264" if args.codec == "h264" else "source.h265")
    params = ["-c:v", "libx264", "-x264-params", "aud=1:bframes=0:keyint=10:repeat-headers=1:slices=4"] if args.codec == "h264" else [
        "-c:v", "libx265", "-x265-params", "aud=1:bframes=0:keyint=10:repeat-headers=1:pools=1:frame-threads=1:log-level=error"]
    subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", f"testsrc2=size={width}x{height}:rate={fps}",
                    "-t", "1", *params, "-y", str(fixture)], check=True)
    config = directory / "camera.ini"
    codec = "h265" if args.codec == "hevc" else "h264"
    config.write_text(f"[video]\ntelemetry=apcg\n[stream.main]\nresolution = 1920x1080\ncodec = {codec}\n[stream.sub]\nresolution = 1280x720\ncodec = {codec}\n[recording]\nresolution = 1920x1080\n[mavlink]\nsystem_id = 0\n")
    gimbal_port, mav_port, rtsp_port = port(socket.SOCK_DGRAM), port(), port()
    camera_ready, gimbal_ready = directory / "camera.ready", directory / "gimbal.ready"
    camera_ready.unlink(missing_ok=True)
    gimbal_ready.unlink(missing_ok=True)
    record_root = pathlib.Path(tempfile.mkdtemp(prefix="record-", dir=directory))
    env = dict(os.environ, CAMERA_APP_BACKEND=args.backend,
        CAMERA_APP_UART=f"udp://127.0.0.1:{gimbal_port}", CAMERA_APP_PORT=str(port()),
        CAMERA_APP_RTSP_PORT=str(rtsp_port), CAMERA_APP_MAVLINK_TCP_PORT=str(mav_port),
        CAMERA_APP_MAVLINK_UDP_PORT="0", CAMERA_APP_CONFIG=str(config),
        CAMERA_APP_READY_PATH=str(camera_ready), CAMERA_APP_RECORD_STATE=str(directory / "recording.state"),
        CAMERA_APP_RECORD_ROOT=str(record_root), CAMERA_APP_CAPTURE_ROOT=str(directory / "capture"),
        CAMERA_APP_SITL_VIDEO1=str(fixture), CAMERA_APP_SITL_VIDEO2=str(fixture))
    if args.terrain:
        env['CAMERA_APP_SITL_TERRAIN'] = str(REPO / 'sitl/terrain_video.py')
        env.setdefault('CAMERA_GIMBAL_SITL_PYTHON', sys.executable)
    camera = gimbal = link = None
    feeder_stop = threading.Event()
    feeder = None
    latitude = [LAT]
    interval_requests = []
    state_yaw = [0.6]
    with (directory / "camera.log").open("w") as clog, (directory / "gimbal.log").open("w") as glog:
        try:
            gimbal = subprocess.Popen([sys.executable, str(REPO / "sitl/gimbal_sim.py"), "--backend", args.backend,
                                       "--port", str(gimbal_port), "--ready-file", str(gimbal_ready)], stdout=glog, stderr=subprocess.STDOUT)
            ready(gimbal_ready, gimbal, directory / "gimbal.log")
            camera = subprocess.Popen([str(build / "camera-app"), "--backend", args.backend], env=env, stdout=clog, stderr=subprocess.STDOUT)
            ready(camera_ready, camera, directory / "camera.log")
            client = RTSP(rtsp_port, "video1")
            try:
                for _, record in client.frames(3, args.codec):
                    assert record["position"] is None and record["vehicle_attitude"] is None
            finally:
                client.close()
            link = mavutil.mavlink_connection(f"tcp:127.0.0.1:{mav_port}", source_system=255, source_component=190)
            # Auto system-ID discovery needs an FC heartbeat, but no position
            # or attitude yet: those fields must remain unknown in this reply.
            bootstrap = mavutil.mavlink_connection(f"tcp:127.0.0.1:{mav_port}", source_system=42, source_component=1)
            try:
                bootstrap.mav.heartbeat_send(M.MAV_TYPE_QUADROTOR, M.MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, M.MAV_STATE_ACTIVE)
                assert link.recv_match(type="HEARTBEAT", blocking=True, timeout=3) is not None
            finally:
                bootstrap.close()
            link.mav.command_long_send(42, M.MAV_COMP_ID_CAMERA, M.MAV_CMD_REQUEST_MESSAGE,
                                       0, M.MAVLINK_MSG_ID_CAMERA_FOV_STATUS, 0, 0, 0, 0, 0, 0)
            fov = link.recv_match(type="CAMERA_FOV_STATUS", blocking=True, timeout=3)
            assert fov is not None and abs(fov.hfov - wide_fov) < .001, fov
            assert abs(fov.vfov - math.degrees(2 * math.atan(math.tan(math.radians(wide_fov) / 2) * 9 / 16))) < .001
            assert (fov.lat_camera, fov.lon_camera, fov.alt_camera) == (2147483647,) * 3
            assert all(math.isnan(v) for v in fov.q), fov
            def send_telemetry():
                # A separate connection avoids concurrent writes on the GCS socket.
                fc = mavutil.mavlink_connection(f"tcp:127.0.0.1:{mav_port}", source_system=42, source_component=1)
                try:
                    while not feeder_stop.is_set():
                        fc.mav.heartbeat_send(M.MAV_TYPE_QUADROTOR, M.MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, M.MAV_STATE_ACTIVE)
                        fc.mav.global_position_int_send(0, latitude[0], 1491652300, 620250, 36500, 0, 0, 0, 1234)
                        yaw = state_yaw[0]
                        if yaw is not None:
                            fc.mav.autopilot_state_for_gimbal_device_send(
                                42, 0, 0, Quaternion([0.1, -0.2, yaw]).q,
                                0, 25, 10, -2, 0, 0, 0, 0,
                                angular_velocity_z=0.2 if yaw else 0)
                        # A separate ATTITUDE stream and another system's gimbal
                        # state must not overwrite the selected FC quaternion.
                        fc.mav.attitude_send(0, 0.1, -0.2, 0.3, 0, 0, 0)
                        fc.mav.srcSystem = 43
                        fc.mav.autopilot_state_for_gimbal_device_send(
                            42, M.MAV_COMP_ID_GIMBAL, 0, Quaternion([0.1, -0.2, 1.2]).q,
                            0, 0, 0, 0, 0, 0, 0, 0)
                        fc.mav.srcSystem = 42
                        while True:
                            msg = fc.recv_match(blocking=False)
                            if msg is None:
                                break
                            if msg.get_type() == 'COMMAND_LONG' and msg.command == M.MAV_CMD_SET_MESSAGE_INTERVAL:
                                interval_requests.append((time.monotonic(), msg))
                        feeder_stop.wait(0.05)
                finally:
                    fc.close()
            feeder = threading.Thread(target=send_telemetry)
            feeder.start()
            assert link.recv_match(type="HEARTBEAT", blocking=True, timeout=3) is not None
            time.sleep(0.2)
            if args.codec == "h264":
                command(link, M.MAV_CMD_VIDEO_START_CAPTURE)
            for stream in ("video1", "video2"):
                client = RTSP(rtsp_port, stream)
                raw_path = directory / f"{stream}.{args.codec}"
                records = []
                try:
                    with raw_path.open("wb") as output:
                        for i, (units, record) in enumerate(client.frames(fps * 2, args.codec)):
                            for unit in units:
                                output.write(b"\0\0\0\1" + unit)
                            records.append(record)
                            if i == fps // 2:
                                latitude[0] = LAT + 100
                finally:
                    client.close()
                assert all(r["position"] is not None and r["vehicle_attitude"] is not None for r in records)
                assert all(r["position"]["lat_e7"] in (LAT, LAT + 100) for r in records)
                assert any(r["gimbal_attitude"] is not None for r in records)
                assert all(abs(r["vehicle_attitude"]["pitch_rad"] + 0.2) < 1e-5 for r in records)
                assert all(abs(r['vehicle_attitude']['yaw_rad'] - 0.6) < 1e-5 for r in records)
                assert all(abs(r['vehicle_attitude']['yaw_rate_rad_s'] - 0.2) < 1e-5 for r in records)
                # Position and gimbal-state both supply valid NED velocity.
                assert all(r['velocity'] is not None for r in records)
                if stream == "video1":
                    assert {r["position"]["lat_e7"] for r in records} == {LAT, LAT + 100}
                expected_fov = 24.2 if args.backend == "mt11" and stream == "video2" else wide_fov
                assert all(abs(r["hfov_deg"] - expected_fov) < .001 for r in records)
                expected_size = (1920, 1080) if stream == 'video1' else (1280, 720)
                assert video_size(raw_path) == expected_size
                decode(raw_path)
                print(f"PASS {args.backend} {args.codec} {stream}: RTP marker, timestamps, one SEI/frame, changing telemetry and decode", flush=True)
            def check_stream_fov(stream_id, expected_fov, thermal):
                client = RTSP(rtsp_port, f"video{stream_id}")
                try:
                    frames = client.frames(fps * 3 if args.terrain else max(3, fps // 2), args.codec)
                    matched = 0
                    captured = bytearray()
                    for units, record in frames:
                        # A rendered frame already in flight when the zoom ACK
                        # arrives retains its original FOV. Wait for that frame.
                        if args.terrain and not matched and abs(record['hfov_deg'] - expected_fov) >= .001:
                            continue
                        # Filtering queued pre-zoom frames can discard the initial
                        # IDR/SPS. Start the decode sample at the next matching IDR.
                        if args.terrain and not matched and not any((unit[0] & 31) == 5 for unit in units):
                            continue
                        assert abs(record["hfov_deg"] - expected_fov) < .001, (record, expected_fov)
                        for unit in units:
                            captured.extend(b"\0\0\0\1" + unit)
                        matched += 1
                        if args.terrain and matched >= 3:
                            break
                    assert matched >= 3
                    captured_path = directory / f'check-video{stream_id}.{args.codec}'
                    captured_path.write_bytes(captured)
                    expected_size = (1280, 720) if thermal or stream_id == 2 else (1920, 1080)
                    assert video_size(captured_path) == expected_size
                finally:
                    client.close()
                for message_id, message_type in ((M.MAVLINK_MSG_ID_VIDEO_STREAM_INFORMATION, "VIDEO_STREAM_INFORMATION"),
                                                 (M.MAVLINK_MSG_ID_VIDEO_STREAM_STATUS, "VIDEO_STREAM_STATUS")):
                    link.mav.command_long_send(42, M.MAV_COMP_ID_CAMERA, M.MAV_CMD_REQUEST_MESSAGE,
                                               0, message_id, stream_id, 0, 0, 0, 0, 0)
                    message = link.recv_match(type=message_type, blocking=True, timeout=3)
                    assert message is not None and message.stream_id == stream_id, message
                    assert (message.resolution_h, message.resolution_v) == expected_size
                    assert message.framerate == (20 if args.terrain else 25 if args.backend == 'a8' else 30), message
                    assert message.hfov == int(expected_fov + .5), (message.to_dict(), expected_fov)
                    assert bool(message.flags & M.VIDEO_STREAM_STATUS_FLAGS_THERMAL) == thermal
                if stream_id == 1:
                    link.mav.command_long_send(42, M.MAV_COMP_ID_CAMERA, M.MAV_CMD_REQUEST_MESSAGE,
                                               0, M.MAVLINK_MSG_ID_CAMERA_FOV_STATUS, 0, 0, 0, 0, 0, 0)
                    message = link.recv_match(type="CAMERA_FOV_STATUS", blocking=True, timeout=3)
                    assert message is not None
                    aspect = 640 / 512 if thermal else expected_size[0] / expected_size[1]
                    expected_vfov = math.degrees(2 * math.atan(math.tan(math.radians(expected_fov) / 2) / aspect))
                    assert abs(message.hfov - expected_fov) < .001, message
                    assert abs(message.vfov - expected_vfov) < .001, message
                    assert message.lat_camera == latitude[0] and message.lon_camera == 1491652300, message
                    assert message.alt_camera == 620250, message
                    assert (message.lat_image, message.lon_image, message.alt_image) == (2147483647,) * 3
                    assert abs(sum(v*v for v in message.q) - 1) < 1e-5, message

            for zoom in (2.0, 4.0, 5.0, 1.0):
                command(link, M.MAV_CMD_SET_CAMERA_ZOOM, 2, (zoom - 1) / (.09 if args.backend == "mt11" else .05))
                # MAVLink zoom operates on the selected RGB lens. The initial
                # wide lens keeps its digital crop across the tele crossover.
                magnification = zoom
                visible_fov = math.degrees(2 * math.atan(math.tan(math.radians(wide_fov) / 2) / magnification))
                for stream_id in (1, 2):
                    expected_fov = 24.2 if args.backend == "mt11" and stream_id == 2 else visible_fov
                    check_stream_fov(stream_id, expected_fov, args.backend == "mt11" and stream_id == 2)
                print(f"PASS {args.backend} {args.codec} zoom {zoom:g}x: video/MAVLink FOV, visible={visible_fov:.4f} degrees", flush=True)
            if args.backend == "mt11":
                # Selecting thermal as main also selects the visible tele lens.
                # It has an independent FOV calibration, not a scaled wide-lens FOV.
                tele_fov = target["lens2_fov_h"]
                command(link, M.MAV_CMD_SET_CAMERA_SOURCE, 0, 2)
                check_stream_fov(1, 24.2, True)
                check_stream_fov(2, tele_fov, False)
                command(link, M.MAV_CMD_SET_CAMERA_SOURCE, 0, 1)
                check_stream_fov(1, tele_fov, False)
                check_stream_fov(2, 24.2, True)
                print("PASS thermal/visible source swap and tele lens FOV", flush=True)
            if args.codec == "h264":
                command(link, M.MAV_CMD_VIDEO_STOP_CAPTURE)
                recordings = list(record_root.glob("*.mp4"))
                assert len(recordings) == (2 if args.backend == "mt11" else 1)
                for path in recordings:
                    check_mp4(path, thermal=args.backend == "mt11" and path.name.startswith("SITL_1_"),
                              visible_fov=wide_fov)
            for message_id in (M.MAVLINK_MSG_ID_GLOBAL_POSITION_INT, M.MAVLINK_MSG_ID_AUTOPILOT_STATE_FOR_GIMBAL_DEVICE):
                requests = [(t, m) for t, m in interval_requests if int(m.param1) == message_id]
                assert len(requests) >= 2, (message_id, interval_requests)
                assert all(m.target_system == 42 and m.target_component == 1 and 0 < m.param2 <= 100000 for _, m in requests)
                assert requests[-1][0] - requests[0][0] >= 4.5
            print('PASS 10 Hz position/gimbal-state requests refreshed while telemetry is present; gimbal-state preferred over ATTITUDE', flush=True)
            for yaw, expected in ((0.0, 0.0), (None, 0.3)):
                state_yaw[0] = yaw
                time.sleep(1.2)  # allow the preferred source to expire for fallback
                client = RTSP(rtsp_port, "video1")
                try:
                    recent = [r for _, r in client.frames(fps, args.codec)]
                    assert all(abs(r['vehicle_attitude']['yaw_rad'] - expected) < 1e-5 for r in recent[-3:])
                finally:
                    client.close()
            print('PASS zero-trimmed gimbal-state payload and ATTITUDE fallback after source loss', flush=True)
            if args.check_stale:
                feeder_stop.set()
                feeder.join(timeout=3)
                time.sleep(10.2)
                client = RTSP(rtsp_port, "video1")
                try:
                    for _, record in client.frames(3, args.codec):
                        assert record["position"] is None and record["vehicle_attitude"] is None
                finally:
                    client.close()
                print("PASS stale telemetry omitted", flush=True)
        finally:
            feeder_stop.set()
            if feeder is not None:
                feeder.join(timeout=3)
            if link is not None:
                link.close()
            stop(camera)
            stop(gimbal)
    print("PASS video telemetry SITL", flush=True)


if __name__ == "__main__":
    main()
