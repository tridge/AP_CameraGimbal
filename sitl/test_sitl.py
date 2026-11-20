#!/usr/bin/env python3
"""End-to-end smoke test for an AP_CameraGimbal SITL camera and web UI."""

import argparse
import base64
import json
import os
import pathlib
import re
import signal
import socket
import struct
import subprocess
import sys
import time
import urllib.request
import urllib.parse

if __package__:
    from .gimbal_sim import Gimbal
else:
    from gimbal_sim import Gimbal


def crc16(data):
    value = 0
    for byte in data:
        value ^= byte << 8
        for _ in range(8):
            value = ((value << 1) ^ 0x1021) & 0xFFFF if value & 0x8000 else (value << 1) & 0xFFFF
    return value


def siyi(sequence, opcode, payload=b""):
    result = struct.pack("<BBBHHB", 0x55, 0x66, 1, len(payload), sequence, opcode) + payload
    return result + struct.pack("<H", crc16(result))


def request(sock, sequence, opcode, payload=b"", timeout=2.0):
    sock.send(siyi(sequence, opcode, payload))
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        sock.settimeout(deadline - time.monotonic())
        packet = sock.recv(4096)
        length = struct.unpack_from("<H", packet, 3)[0]
        if len(packet) == length + 10 and packet[7] == opcode and crc16(packet[:-2]) == struct.unpack_from("<H", packet, len(packet) - 2)[0]:
            return packet[8:-2]
    raise TimeoutError(f"no SIYI opcode 0x{opcode:02x} reply")


_reserved_ports = set()


def reserve_port(socktype, span=1):
    """Choose unused test ports, including adjacent live-video listeners.

    BSD commonly allocates consecutive ephemeral ports. Remember a whole range
    so a later web-port allocation cannot collide with RTSP's live port (+1).
    """
    for _ in range(1000):
        sockets = []
        try:
            sock = socket.socket(socket.AF_INET, socktype)
            sockets.append(sock)
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
            ports = {(socktype, port + offset) for offset in range(span)}
            if port + span > 65536 or ports & _reserved_ports:
                continue
            for offset in range(1, span):
                adjacent = socket.socket(socket.AF_INET, socktype)
                sockets.append(adjacent)
                adjacent.bind(("127.0.0.1", port + offset))
            _reserved_ports.update(ports)
            return port
        except OSError:
            pass
        finally:
            for sock in sockets:
                sock.close()
    raise RuntimeError("Could not allocate isolated test ports")


def web_request(port, path):
    request = urllib.request.Request(f"http://127.0.0.1:{port}{path}")
    credentials = base64.b64encode(b"admin:ardupilot").decode("ascii")
    request.add_header("Authorization", f"Basic {credentials}")
    return urllib.request.urlopen(request, timeout=4)


def web_form(port, path, csrf, fields, timeout=4):
    data = urllib.parse.urlencode({"csrf": csrf, **fields}).encode("ascii")
    request = urllib.request.Request(
        f"http://127.0.0.1:{port}{path}", data=data, method="POST")
    credentials = base64.b64encode(b"admin:ardupilot").decode("ascii")
    request.add_header("Authorization", f"Basic {credentials}")
    request.add_header("Content-Type", "application/x-www-form-urlencoded")
    return urllib.request.urlopen(request, timeout=timeout)


def terminate(process):
    if process is None or process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def read_rtsp_sdp(port, path, host='127.0.0.1'):
    """A single DESCRIBE must succeed, including before the first video frame."""
    uri = f'rtsp://{host}:{port}/{path}'
    with socket.create_connection((host, port), timeout=2) as sock:
        sock.sendall(f'DESCRIBE {uri} RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n'.encode())
        response = b''
        while b'\r\n\r\n' not in response:
            part = sock.recv(4096)
            assert part, 'closed during DESCRIBE'
            response += part
        header, body = response.split(b'\r\n\r\n', 1)
        assert header.startswith(b'RTSP/1.0 200 '), response
        length = int(re.search(rb'Content-Length: (\d+)', header, re.I)[1])
        while len(body) < length:
            part = sock.recv(4096)
            assert part, 'closed during SDP'
            body += part
    return body[:length].decode()


def verify_rtsp_sdp(port, path, codec='h264', host='127.0.0.1'):
    """Parameter sets appear in fresh SDP once the encoder has produced them."""
    deadline = time.monotonic() + 5
    while True:
        sdp = read_rtsp_sdp(port, path, host)
        if 'sprop-' in sdp:
            break
        assert time.monotonic() < deadline, sdp
        time.sleep(.05)
    assert f'IN IP4 {host}\r\n' in sdp, sdp
    assert '\r\ns=AP_CameraGimbal\r\n' in sdp, sdp
    assert '\r\nc=IN IP4 0.0.0.0\r\n' in sdp, sdp
    assert f'a=rtpmap:96 {codec.upper()}/90000\r\n' in sdp, sdp
    fmtp = re.search(r'a=fmtp:96 ([^\r\n]+)', sdp)[1]
    params = dict(item.split('=', 1) for item in fmtp.split(';'))
    if codec == 'h264':
        assert params['packetization-mode'] == '1'
        sps, pps = [base64.b64decode(value, validate=True) for value in params['sprop-parameter-sets'].split(',')]
        assert sps[0] & 31 == 7 and pps[0] & 31 == 8
        assert params['profile-level-id'] == sps[1:4].hex()
    else:
        for name, nal in [('vps', 32), ('sps', 33), ('pps', 34)]:
            parameter = base64.b64decode(params[f'sprop-{name}'], validate=True)
            assert (parameter[0] >> 1) & 63 == nal
    return sdp


def verify_rtsp_video(port, path, expected_size, codec='h264'):
    verify_rtsp_sdp(port, path, codec)
    uri = f"rtsp://127.0.0.1:{port}/{path}"
    result = subprocess.run(
        ["ffprobe", "-v", "error", "-rtsp_transport", "tcp",
         "-select_streams", "v:0", "-show_entries", "stream=width,height",
         "-of", "json", uri],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10,
        check=False, text=True)
    assert result.returncode == 0, f"{uri}: {result.stderr.strip()}"
    streams = json.loads(result.stdout).get("streams", [])
    assert len(streams) == 1, (uri, streams)
    actual_size = (streams[0].get("width"), streams[0].get("height"))
    assert actual_size == expected_size, (uri, actual_size, expected_size)


def verify_rtsp_rtcp(port, path):
    """Check initial and periodic SRs against the RTP actually received."""
    uri = f'rtsp://127.0.0.1:{port}/{path}'
    with socket.create_connection(('127.0.0.1', port), timeout=10) as sock:
        stream = sock.makefile('rb')
        def request(method, url, cseq, headers=''):
            sock.sendall(f'{method} {url} RTSP/1.0\r\nCSeq: {cseq}\r\n{headers}\r\n'.encode())
            status = stream.readline()
            assert status.startswith(b'RTSP/1.0 200 '), status
            response = {}
            while True:
                line = stream.readline()
                if line == b'\r\n': break
                assert line
                name, value = line.decode().split(':', 1)
                response[name.lower()] = value.strip()
            stream.read(int(response.get('content-length', 0)))
            return response
        request('DESCRIBE', uri, 1, 'Accept: application/sdp\r\n')
        setup = request('SETUP', uri+'/track0', 2,
                        'Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n')
        session = setup['session'].split(';')[0]
        request('PLAY', uri, 3, f'Session: {session}\r\nRange: 0-\r\n')
        packets = octets = reports = 0
        previous_sequence = None
        deadline = time.monotonic() + 10
        while reports < 2 and time.monotonic() < deadline:
            header = stream.read(4)
            assert len(header) == 4 and header[0] == 36, header
            payload = stream.read(struct.unpack_from('!H', header, 2)[0])
            if header[1] == 0:
                assert payload[0] == 0x80 and payload[1] & 127 == 96
                seq, timestamp, ssrc = struct.unpack_from('!HII', payload, 2)
                if previous_sequence is not None:
                    assert seq == (previous_sequence+1) & 65535
                previous_sequence = seq
                packets += 1
                octets += len(payload)-12
            else:
                assert header[1] == 1 and payload[:4] == b'\x80\xc8\x00\x06'
                sender, seconds, fraction, rtp_time, count, size = struct.unpack_from('!6I', payload, 4)
                assert (sender, rtp_time, count, size) == (ssrc, timestamp, packets, octets)
                assert abs(seconds-2208988800+fraction/2**32-time.time()) < 5
                assert payload[28:30] == b'\x81\xca'  # SDES, one CNAME chunk
                assert struct.unpack_from('!I', payload, 32)[0] == ssrc
                assert payload[36] == 1 and payload[37] > 0
                name_end = 38 + payload[37]
                assert payload[name_end:] == bytes(len(payload)-name_end)
                reports += 1
        assert reports == 2, 'missing initial or periodic RTCP sender report'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("camera_app")
    parser.add_argument("web")
    parser.add_argument("gimbal_sim")
    parser.add_argument("runtime")
    parser.add_argument("--backend", choices=("mt11", "a8", "zr10"), default="mt11")
    parser.add_argument("--orientation", choices=("upright", "inverted"),
                        default="upright")
    args = parser.parse_args()
    camera_binary = pathlib.Path(args.camera_app).resolve()
    web_binary = pathlib.Path(args.web).resolve()
    gimbal_script = pathlib.Path(args.gimbal_sim).resolve()
    runtime = pathlib.Path(args.runtime).resolve()
    build = camera_binary.parent
    expected_vendor_rate = Gimbal(1, args.backend).vendor_rate_response("yaw", 10)
    gimbal_port = reserve_port(socket.SOCK_DGRAM)
    camera_port = reserve_port(socket.SOCK_DGRAM)
    rtsp_port = reserve_port(socket.SOCK_STREAM, span=2)
    web_port = reserve_port(socket.SOCK_STREAM)
    ready = runtime / "run/camera-app.ready"
    gimbal_ready = runtime / "run/test-gimbal.ready"
    ready.unlink(missing_ok=True)
    gimbal_ready.unlink(missing_ok=True)
    captures_before = set((runtime / "mnt/DCIM/capture").rglob("*.jpg"))
    processes = []
    logs = []
    try:
        gimbal_log = open(runtime / "run/test-gimbal.log", "w", encoding="utf-8")
        logs.append(gimbal_log)
        gimbal = subprocess.Popen(
            [sys.executable, str(gimbal_script), "--port", str(gimbal_port),
             "--backend", args.backend, "--orientation", args.orientation,
             "--ready-file", str(gimbal_ready)],
            stdout=gimbal_log, stderr=subprocess.STDOUT)
        processes.append(gimbal)
        deadline = time.monotonic() + 5
        while not gimbal_ready.exists() and time.monotonic() < deadline:
            if gimbal.poll() is not None:
                raise AssertionError(f"gimbal simulator exited {gimbal.returncode}")
            time.sleep(0.02)
        assert gimbal_ready.exists(), "gimbal simulator did not become ready"

        env = os.environ.copy()
        if args.backend != "mt11":
            video1 = build / "main.h264"
            video2 = build / "sub.h264"
            expected_sizes = ((1920, 1080), (1280, 720))
        else:
            video1 = build / "rgb.h264"
            video2 = build / "thermal.h264"
            expected_sizes = ((1920, 1080), (1280, 720))
        if args.backend == "zr10":
            expected_sizes = ((1280, 720), (1280, 720))
        env.update({
            "CAMERA_APP_BACKEND": args.backend,
            "CAMERA_APP_UART": f"udp://127.0.0.1:{gimbal_port}",
            "CAMERA_APP_PORT": str(camera_port),
            "CAMERA_APP_RTSP_PORT": str(rtsp_port),
            "CAMERA_APP_MAVLINK_TCP_PORT": "0",
            "CAMERA_APP_MAVLINK_UDP_PORT": "0",
            "CAMERA_APP_CONFIG": str(runtime / "app/camera.ini"),
            "CAMERA_APP_READY_PATH": str(ready),
            "CAMERA_APP_RECORD_STATE": str(runtime / "run/test-recording.state"),
            "CAMERA_APP_RECORD_ROOT": str(runtime / "mnt/DCIM/record"),
            "CAMERA_APP_LOG_ROOT": str(runtime / "mnt/logs"),
            "CAMERA_APP_CAPTURE_ROOT": str(runtime / "mnt/DCIM/capture"),
            "CAMERA_APP_SITL_VIDEO1": str(video1),
            "CAMERA_APP_SITL_VIDEO2": str(video2),
            "CAMERA_APP_SITL_PHOTO": str(build / "photo.jpg"),
            "MT11_WEB_CAMERA_PORT": str(camera_port),
            "MT11_WEB_LIVE_PORT": str(rtsp_port + 1),
        })
        camera_log = open(runtime / "run/camera_app.log", "w", encoding="utf-8")
        logs.append(camera_log)
        camera_command = [str(camera_binary)]
        if args.backend != "mt11":
            camera_command.extend(("--backend", args.backend))
        camera = subprocess.Popen(camera_command, env=env,
                                  stdout=camera_log, stderr=subprocess.STDOUT)
        processes.append(camera)
        deadline = time.monotonic() + 5
        while not ready.exists() and time.monotonic() < deadline:
            if camera.poll() is not None:
                raise AssertionError(f"camera-app exited {camera.returncode}")
            time.sleep(0.02)
        assert ready.exists(), "camera-app did not become ready"
        verify_rtsp_video(rtsp_port, "video1", expected_sizes[0])
        verify_rtsp_video(rtsp_port, "video2", expected_sizes[1])
        # the default [stream.main] alias serves the vendor path too
        verify_rtsp_video(rtsp_port, "main.264", expected_sizes[0])

        client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        client.connect(("127.0.0.1", camera_port))
        attitude = request(client, 1, 0x0D)
        assert len(attitude) == 12
        initial_pitch = struct.unpack_from("<h", attitude, 2)[0] / 10.0
        initial_yaw = struct.unpack_from("<h", attitude, 0)[0] / 10.0
        if args.backend != "mt11":
            # Check vendor wire coordinates independently of the canonical web display.
            yaw_sign = -1 if args.backend == "zr10" or (args.backend == "a8" and args.orientation == "upright") else 1
            expected_pitch = -160 if args.orientation == "inverted" else -20
            assert abs(initial_yaw - yaw_sign * -38.0) < 0.3, initial_yaw
            assert abs(initial_pitch - expected_pitch) < 0.3, initial_pitch
            # A positive web/SIYI rate command moves right. Both the reported
            # yaw and yaw rate must therefore increase in either mounting.
            client.send(siyi(2, 0x07, bytes((10, 0))))
            # The calibrated A8 model accelerates to its commanded rate.
            # Wait for feedback convergence instead of assuming 250 ms is
            # enough on every host and at every telemetry scheduling phase.
            deadline = time.monotonic() + 2.0
            while True:
                time.sleep(0.05)
                moved = request(client, 3, 0x0D)
                moved_yaw, _, _, moved_yaw_rate = struct.unpack_from("<hhhh", moved)
                moved_yaw /= 10.0
                moved_yaw_rate /= 10.0
                if (yaw_sign * (moved_yaw - initial_yaw) > 1.0 and
                        abs(moved_yaw_rate - yaw_sign * expected_vendor_rate) < 0.2):
                    break
                if time.monotonic() >= deadline:
                    break
            assert yaw_sign * (moved_yaw - initial_yaw) > 1.0, (initial_yaw, moved_yaw)
            assert abs(moved_yaw_rate - yaw_sign * expected_vendor_rate) < 0.2, moved_yaw_rate
            client.send(siyi(4, 0x07, b"\x00\x00"))
        else:
            request(client, 2, 0x0E, struct.pack("<hh", 100, -50))
            deadline = time.monotonic() + 2
            final_pitch = initial_pitch
            while time.monotonic() < deadline and final_pitch < -6.0:
                final_pitch = struct.unpack_from(
                    "<h", request(client, 3, 0x0D), 2)[0] / 10.0
                time.sleep(0.05)
            assert final_pitch > initial_pitch + 5.0, (initial_pitch, final_pitch)
        status = request(client, 4, 0x0A)
        expected_direction = 2 if args.orientation == "inverted" else 1
        assert len(status) == 8 and status[5] == expected_direction
        if args.backend == "mt11":
            client.send(siyi(5, 0x0C, b"\x00"))
            feedback = request(client, 6, 0x0B)
            assert feedback == b"\x00"
            captured = set((runtime / "mnt/DCIM/capture").rglob("*.jpg")) - captures_before
            assert {p.stem[-1] for p in captured} == {"C", "Z", "I"}, captured
            assert all(p.stat().st_size > 0 for p in captured)
        client.close()

        web_log = open(runtime / "run/test-web.log", "w", encoding="utf-8")
        logs.append(web_log)
        web = subprocess.Popen([str(web_binary), "-p", str(web_port)], env=env,
                               stdout=web_log, stderr=subprocess.STDOUT)
        processes.append(web)
        deadline = time.monotonic() + 5
        while True:
            try:
                with web_request(web_port, "/") as response:
                    page = response.read().decode("utf-8")
                break
            except OSError:
                if web.poll() is not None or time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)
        assert "ArduPilot camera app" in page
        # Exercise installed assets, not just the generated HTML links.
        for asset, mime in (('/style.css', 'text/css'), ('/live.js', 'application/javascript'),
                            ('/parameters.js', 'application/javascript')):
            with web_request(web_port, asset) as response:
                assert response.headers.get_content_type() == mime
                assert len(response.read()) > 100

        csrf_match = re.search(r'data-csrf="([0-9a-f]{64})"', page)
        assert csrf_match is not None
        csrf = csrf_match.group(1)
        if args.backend == "mt11":
            with web_request(web_port, "/sensors.json") as response:
                sensors = json.load(response)
            assert sensors["lidar_m"] is not None
            assert sensors["minimum_c"] == 12.0 and sensors["maximum_c"] == 42.0
            assert sensors["cpu_c"] is not None
        with web_request(web_port, "/live/attitude.json") as response:
            live = json.load(response)
        if args.backend != "mt11":
            assert live["yaw_deg"] < 0.0, live
            assert abs(live["pitch_deg"] + 20.0) < 0.3, live

        # Both gimbals accept right-positive rate commands, but the MT11's
        # raw 0x0D attitude is left-positive. Verify the target-specific web
        # build displays right motion and right rate as positive in either
        # mounting orientation.
        control = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        control.connect(("127.0.0.1", camera_port))
        control.send(siyi(80, 0x07, bytes((10, 0))))
        deadline = time.monotonic() + 2.0
        while True:
            time.sleep(0.05)
            with web_request(web_port, "/live/attitude.json") as response:
                moving = json.load(response)
            if abs(moving["yaw_rate_dps"] - expected_vendor_rate) < 0.2 or time.monotonic() >= deadline:
                break
        assert abs(moving["yaw_rate_dps"] - expected_vendor_rate) < 0.2, moving
        control.send(siyi(81, 0x07, b"\x00\x00"))
        control.close()
        time.sleep(0.15)
        with web_request(web_port, "/live/attitude.json") as response:
            pulse_start = json.load(response)
        with web_form(web_port, "/live/control", csrf, {"action": "acquire"}) as response:
            lease = response.read().decode().strip()
        assert re.fullmatch("[0-9a-f]{32}", lease), lease
        with web_form(web_port, "/live/control", csrf,
                      {"action": "right", "value": "30", "lease": lease}) as response:
            assert response.status == 200
        time.sleep(0.25)
        with web_request(web_port, "/live/attitude.json") as response:
            moved = json.load(response)
        assert moved["yaw_deg"] > pulse_start["yaw_deg"] + 3.0, \
            (pulse_start, moved)
        deadline = time.monotonic() + 2.0
        while abs(moved["yaw_rate_dps"]) >= 0.2 and time.monotonic() < deadline:
            time.sleep(0.05)
            with web_request(web_port, "/live/attitude.json") as response:
                moved = json.load(response)
        assert abs(moved["yaw_rate_dps"]) < 0.2, moved
        time.sleep(0.3)
        with web_request(web_port, "/live/attitude.json") as response:
            settled = json.load(response)
        assert abs(settled["yaw_deg"] - moved["yaw_deg"]) < 0.3, \
            (moved, settled)
        # Incoming vendor movement cannot override the manual lease; attitude
        # queries still work, and control resumes immediately after release.
        control = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        control.connect(("127.0.0.1", camera_port))
        control.send(siyi(90, 0x07, bytes((50, 0))))
        time.sleep(0.25)
        with web_request(web_port, "/live/attitude.json") as response:
            blocked = json.load(response)
        assert abs(blocked["yaw_deg"] - settled["yaw_deg"]) < 0.3, (blocked, settled)
        with web_form(web_port, "/live/control", csrf, {"action": "release", "lease": lease}):
            pass
        control.send(siyi(91, 0x07, bytes((50, 0))))
        time.sleep(0.25)
        control.send(siyi(92, 0x07, bytes((0, 0))))
        control.close()
        with web_request(web_port, "/live/attitude.json") as response:
            unlocked = json.load(response)
        assert unlocked["yaw_deg"] > blocked["yaw_deg"] + 2, (unlocked, blocked)
        with web_request(web_port, "/live/video1.mp4") as response:
            video = response.read(1024)
        assert b"ftyp" in video
    finally:
        for process in reversed(processes):
            terminate(process)
        for log in logs:
            log.close()
    print(f"PASS {args.backend.upper()} SITL {args.orientation} camera, "
          "UDP gimbal, RTSP, web UI and live video")


if __name__ == "__main__":
    main()
