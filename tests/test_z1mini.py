#!/usr/bin/env python3
"""Offline Z1-Mini: PTY MCU, real media receiver, MAVLink, web and overlay ZIP."""
import base64
import ast
import hashlib
import http.client
import json
import os
from pathlib import Path
import pty
import re
import select
import socket
import struct
import subprocess
import tempfile
import threading
import time
import zipfile
from urllib.parse import urlencode

from pymavlink.dialects.v20 import ardupilotmega as mav

ROOT = Path(__file__).resolve().parents[1]


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def crc(data):
    c = 0
    for byte in data:
        c ^= byte << 8
        for _ in range(8):
            c = ((c << 1) ^ (0x1021 if c & 0x8000 else 0)) & 65535
    return c


class MCU:
    def __init__(self):
        self.master, self.slave = pty.openpty()
        self.path = os.ttyname(self.slave)
        self.stop = threading.Event()
        self.frames = []
        self.thread = threading.Thread(target=self.work, daemon=True)
        self.thread.start()

    def work(self):
        buffer = b''
        feedback = bytearray(24)
        feedback[:2] = b'\xb5\x9a'
        struct.pack_into('<h', feedback, 14, -1000)
        struct.pack_into('<h', feedback, 16, -11114)
        struct.pack_into('<h', feedback, 22, 250)
        while not self.stop.wait(.02):
            if self.frames:  # Real MCU requires a command before its first reply.
                os.write(self.master, feedback + struct.pack('>H', crc(feedback)))
            if select.select([self.master], [], [], 0)[0]:
                buffer += os.read(self.master, 8192)
            while len(buffer) >= 40:
                if buffer[:2] != b'\xa9\x5b':
                    buffer = buffer[1:]
                    continue
                frame, buffer = buffer[:40], buffer[40:]
                if crc(frame) == 0:
                    self.frames.append(frame)
                    if frame[7] == 0x10 and frame[10] == 0:
                        feedback[14:16] = frame[8:10]
                        feedback[22:24] = frame[11:13]

    def close(self):
        self.stop.set()
        self.thread.join(2)
        os.close(self.master)
        os.close(self.slave)


class RTSP:
    def __init__(self, units):
        self.units = units
        self.sock = socket.socket()
        self.sock.bind(('127.0.0.1', 0))
        self.port = self.sock.getsockname()[1]
        self.sock.listen(2)
        self.sock.settimeout(.2)
        self.stop = threading.Event()
        self.options = 0
        self.connections = 0
        self.thread = threading.Thread(target=self.work, daemon=True)
        self.thread.start()

    def work(self):
        while not self.stop.is_set():
            try:
                c, _ = self.sock.accept()
            except socket.timeout:
                continue
            self.connections += 1
            try:
                with c:
                    buffer = b''
                    playing = False
                    sequence, timestamp, index = 65500, 0xffff0000, 0
                    next_frame = time.monotonic()
                    while not self.stop.is_set():
                        if select.select([c], [], [], .01)[0]:
                            data = c.recv(4096)
                            if not data:
                                break
                            buffer += data
                        while b'\r\n\r\n' in buffer:
                            header, buffer = buffer.split(b'\r\n\r\n', 1)
                            method = header.split(b' ', 1)[0]
                            cseq = re.search(rb'CSeq: (\d+)', header)[1]
                            body = b''
                            if method == b'DESCRIBE':
                                body = b'v=0\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=control:streamid=0\r\n'
                            if method == b'PLAY':
                                playing = True
                            if method == b'OPTIONS':
                                self.options += 1
                            c.sendall(b'RTSP/1.0 200 OK\r\nCSeq: ' + cseq +
                                      b'\r\nSession: z1fixture;timeout=60\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nContent-Length: ' +
                                      str(len(body)).encode() + b'\r\n\r\n' + body)
                        if not playing or time.monotonic() < next_frame:
                            continue
                        # Exercise 30 fps -> 5 fps with RTP clock wrap.
                        interval = 1 / 30 if index < 60 else .2
                        next_frame = time.monotonic() + interval
                        timestamp = (timestamp + round(90000 * interval)) & 0xffffffff
                        unit = self.units[index % len(self.units)]
                        packets = []
                        for nal in unit:
                            if len(nal) <= 1000:
                                packets.append(nal)
                            else:
                                chunks = [nal[i:i+998] for i in range(1, len(nal), 998)]
                                for j, chunk in enumerate(chunks):
                                    packets.append(bytes([(nal[0] & 0xe0) | 28,
                                        (nal[0] & 31) | (128 if j == 0 else 0) | (64 if j == len(chunks)-1 else 0)]) + chunk)
                        for j, payload in enumerate(packets):
                            rtp = struct.pack('>BBHII', 0x80, 96 | (128 if j == len(packets)-1 else 0), sequence, timestamp, 42) + payload
                            c.sendall(b'$\x00' + struct.pack('>H', len(rtp)) + rtp)
                            sequence = (sequence + 1) & 65535
                        index += 1
            except (ConnectionError, OSError):
                pass

    def close(self):
        self.stop.set()
        self.thread.join(3)
        self.sock.close()


def main():
    web_source = (ROOT/'web/mt11-web.c').read_text()
    allowlist = re.search(r'static const char \*const allowed\[\] = \{(.*?)\};',
                          web_source, re.S)
    assert allowlist
    accepted = set(re.findall(r'"(gcu/[^" ]+)"', allowlist.group(1)))
    builder = ast.parse((ROOT/'tools/build_z1mini_package.py').read_text())
    app_files, ipc_files = set(), set()
    for node in ast.walk(builder):
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name) and target.id == 'payload' and isinstance(node.value, ast.Dict):
                    app_files.update(key.value for key in node.value.keys
                                     if isinstance(key, ast.Constant) and isinstance(key.value, str))
                if isinstance(target, ast.Subscript) and isinstance(target.value, ast.Name):
                    key = target.slice
                    if isinstance(target.value, ast.Name) and target.value.id == 'payload' and isinstance(key, ast.Constant) and isinstance(key.value, str):
                        app_files.add(key.value)
                    if isinstance(target.value, ast.Name) and target.value.id == 'members' and isinstance(key, ast.Constant) and isinstance(key.value, str):
                        ipc_files.add(key.value)
    builder_members = {'gcu/ap/' + name for name in app_files} | ipc_files
    assert accepted == builder_members, (accepted, builder_members)
    with tempfile.TemporaryDirectory(prefix='z1mini-test-') as temp:
        root = Path(temp)
        log = (root / 'build.log').open('w')
        run('cc', '-O2', '-Wall', '-Wextra', '-Werror', '-DAPCAM_TARGET=APCAM_TARGET_Z1_MINI', '-I'+str(ROOT/'include'), '-I'+str(ROOT/'camera_app/include'),
            '-o', str(root/'unit'), str(ROOT/'camera_app/tests/test_z1mini.c'),
            str(ROOT/'camera_app/src/backends/z1mini/pipeline.c'), str(ROOT/'camera_app/src/log.c'), '-lm', '-lutil')
        run(str(root/'unit'))
        run('cc', '-O2', '-Wall', '-Wextra', '-Werror', '-DAPCAM_TARGET=APCAM_TARGET_Z1_MINI', '-I'+str(ROOT/'include'), '-I'+str(ROOT/'camera_app/include'),
            '-o', str(root/'native-unit'), str(ROOT/'camera_app/tests/test_z1mini_native.c'),
            str(ROOT/'camera_app/src/backends/z1mini/native.c'), str(ROOT/'camera_app/src/log.c'), '-pthread')
        run(str(root/'native-unit'))
        run('make', '-C', str(ROOT/'camera_app'), 'CAMERA_BACKEND=z1mini', 'z1mini-host', '-j8', stdout=log, stderr=log)
        source = root/'source.h264'
        run('ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i',
            'testsrc2=size=1920x1080:rate=10', '-frames:v', '10', '-c:v', 'libx264',
            '-preset', 'ultrafast', '-x264-params', 'aud=1:bframes=0:keyint=10', '-f', 'h264', str(source))
        nals = [n for n in re.split(b'\x00\x00\x00?\x01', source.read_bytes()) if n]
        units = []
        for nal in nals:
            if nal[0] & 31 == 9:
                units.append([])
            units[-1].append(nal)
        rtsp = RTSP(units)
        mcu = MCU()
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp.bind(('127.0.0.1', 0)); udp.settimeout(.5)
        mavport, tcpport, outputport = port(), port(), port()
        encoder = mav.MAVLink(None, srcSystem=255, srcComponent=191)
        parser = mav.MAVLink(None)
        ready = root/'ready'
        records = root/'records'; records.mkdir()
        captures = root/'capture'; captures.mkdir()
        config = root/'camera.ini'
        config.write_text((ROOT/'packaging/z1mini/camera.ini').read_text().replace('udp_port = 14550', f'udp_port = {mavport}').replace('tcp_port = 14550', f'tcp_port = {tcpport}'))
        binary = ROOT/'camera_app/build/z1mini-host/camera-app'
        env = dict(os.environ, CA_Z1_SOURCE_PORT=str(rtsp.port), CAMERA_APP_READY_PATH=str(ready),
                   CAMERA_APP_RECORD_ROOT=str(records), CAMERA_APP_CAPTURE_ROOT=str(captures),
                   CAMERA_APP_RTSP_PORT=str(outputport))
        app_log = (root/'app.log').open('w')
        app = subprocess.Popen([str(binary), '--uart', mcu.path, '--config', str(config)], env=env, stdout=app_log, stderr=app_log)
        web = None
        def send(message):
            udp.sendto(message.pack(encoder), ('127.0.0.1', mavport))
        def receive(kind, timeout=3):
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                try:
                    data = udp.recv(4096)
                except socket.timeout:
                    continue
                for msg in parser.parse_buffer(data) or []:
                    if msg.get_type() == kind:
                        return msg
            raise AssertionError(f'no {kind}; app log:\n'+(root/'app.log').read_text())
        def command(cmd, component=100, p1=0):
            send(mav.MAVLink_command_long_message(0, component, cmd, 0, p1, 0, 0, 0, 0, 0, 0))
        try:
            for _ in range(180):
                if ready.exists():
                    break
                if app.poll() is not None:
                    raise AssertionError((root/'app.log').read_text())
                time.sleep(.1)
            assert ready.exists(), (root/'app.log').read_text()
            assert 'udp_port=2337\n' in ready.read_text() # XFRobot, not SIYI
            fc = mav.MAVLink(None, srcSystem=1, srcComponent=1)
            udp.sendto(mav.MAVLink_heartbeat_message(mav.MAV_TYPE_QUADROTOR,
                mav.MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, mav.MAV_STATE_STANDBY, 3).pack(fc),
                ('127.0.0.1', mavport))
            command(mav.MAV_CMD_REQUEST_CAMERA_INFORMATION)
            info = receive('CAMERA_INFORMATION')
            assert bytes(info.model_name).rstrip(b'\0') == b'XFRobot Z1-Mini', info
            assert info.flags == mav.CAMERA_CAP_FLAGS_CAPTURE_VIDEO | mav.CAMERA_CAP_FLAGS_HAS_VIDEO_STREAM
            receive('COMMAND_ACK')
            command(mav.MAV_CMD_REQUEST_MESSAGE, 154, mav.MAVLINK_MSG_ID_GIMBAL_DEVICE_ATTITUDE_STATUS)
            a = receive('GIMBAL_DEVICE_ATTITUDE_STATUS')
            assert a.q[0] > .9
            receive('COMMAND_ACK')
            send(mav.MAVLink_gimbal_device_set_attitude_message(0, 154, mav.GIMBAL_DEVICE_FLAGS_YAW_IN_VEHICLE_FRAME,
                [float('nan')]*4, float('nan'), .1, -.1))
            time.sleep(.15)
            assert mcu.frames
            assert struct.unpack_from('<h', mcu.frames[-1], 8)[0] > -1000
            assert struct.unpack_from('<h', mcu.frames[-1], 11)[0] < 250
            time.sleep(.5)
            held = mcu.frames[-1][8:13]
            time.sleep(.2)
            assert mcu.frames[-1][8:13] == held, 'rate timeout did not hold'
            # Offline flight-controller telemetry must survive in the video
            # itself, so a downstream mapper needs no separate MAVLink log.
            fc = mav.MAVLink(None, srcSystem=1, srcComponent=1)
            for message in [
                mav.MAVLink_heartbeat_message(mav.MAV_TYPE_QUADROTOR, mav.MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, mav.MAV_STATE_STANDBY, 3),
                mav.MAVLink_global_position_int_message(1000, -353632610, 1491652300, 650000, 120000, 120, -230, 40, 9000),
                mav.MAVLink_attitude_message(1000, .1, -.2, 1.5, 0., 0., 0.),
            ]:
                udp.sendto(message.pack(fc), ('127.0.0.1', mavport))
            time.sleep(.1)
            command(mav.MAV_CMD_VIDEO_START_CAPTURE)
            assert receive('COMMAND_ACK').result == mav.MAV_RESULT_ACCEPTED
            time.sleep(3)
            command(mav.MAV_CMD_VIDEO_STOP_CAPTURE)
            assert receive('COMMAND_ACK').result == mav.MAV_RESULT_ACCEPTED
            files = list(records.glob('*.mp4')); assert len(files) == 1
            probe = subprocess.check_output(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-count_frames',
                '-show_entries', 'stream=codec_name,width,height,nb_read_frames', '-of', 'json', str(files[0])])
            stream = json.loads(probe)['streams'][0]
            assert stream['codec_name'] == 'h264' and stream['width'] == 1920 and int(stream['nb_read_frames']) >= 5, stream
            run('ffmpeg', '-hide_banner', '-loglevel', 'error', '-i', str(files[0]), '-f', 'null', '-')
            telemetry_file = root/'video-telemetry.jsonl'
            run('python3', str(ROOT/'tools/video_telemetry.py'), str(files[0]), '--output', str(telemetry_file))
            telemetry = [json.loads(line) for line in telemetry_file.read_text().splitlines()]
            assert len(telemetry) == int(stream['nb_read_frames']), 'missing per-frame SEI telemetry'
            sample = telemetry[len(telemetry)//2]
            assert sample['position']['lat_e7'] == -353632610, sample
            assert sample['position']['lon_e7'] == 1491652300, sample
            assert sample['position']['alt_amsl_m'] == 650, sample
            assert sample['velocity']['ve_m_s'] == -2.3, sample
            assert abs(sample['vehicle_attitude']['pitch_rad'] + .2) < .00001, sample
            assert sample['gimbal_attitude'] is not None, sample
            assert abs(sample['hfov_deg'] - 54.7) < .001, 'nominal Z1 lens HFOV missing'
            assert all(b['pts90k'] > a['pts90k'] for a,b in zip(telemetry, telemetry[1:]))
            (root/'web.pass').write_text('test-password\n')
            temp_file = root/'soc_temp'; temp_file.write_text('80125\n')
            webbin = root/'z1mini-web'
            paths = {
                'APP_DIR':root, 'APP_SELECTION_DIR':root, 'MEDIA_ROOT':root,
                'PASSWORD_PATH':root/'web.pass', 'REPLACEMENT_CONFIG_PATH':config,
                'REPLACEMENT_CONFIG_BACKUP_PATH':root/'config.bak', 'SESSION_PATH':root/'sessions',
                'USER_LOCK_PATH':root/'users.lock', 'UPGRADE_LOCK_PATH':root/'upgrade.lock',
                'APP_SELECTION_PATH':root/'selection', 'RUNTIME_DIR':root, 'CAMERA_READY_PATH':ready,
                'VENDOR_CAMERA_PATH':root/'vendor', 'REPLACEMENT_CAMERA_PATH':binary,
                'SOC_TEMPERATURE_PATH':temp_file,
            }
            run('cc', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-address-of-packed-member',
                '-DAPCAM_TARGET=APCAM_TARGET_Z1_MINI', '-DMT11_WEB_TEST', '-I'+str(ROOT/'camera_app/build/mavlink/all/include'),
                *[f'-D{k}="{v}"' for k,v in paths.items()], str(ROOT/'web/mt11-web.c'), '-o', str(webbin), '-lm', stdout=log, stderr=log)
            webport = port()
            web = subprocess.Popen([str(webbin), '-p', str(webport)], stdout=log, stderr=log)
            time.sleep(.3)
            auth = 'Basic '+base64.b64encode(b'admin:test-password').decode()
            def request(path):
                conn = http.client.HTTPConnection('127.0.0.1', webport, timeout=5)
                conn.request('GET', path, headers={'Authorization':auth})
                response = conn.getresponse(); data = response.read(); conn.close()
                assert response.status == 200, (path, response.status, data)
                return data.decode()
            page = request('/')
            assert '80.1' in page and 'SoC temperature' in page, page
            assert 'id=firmware-upload' in page and 'Z1Mini_AP_*.gcu' in page
            temp_file.write_text('not-a-temperature\n')
            assert '80.1' not in request('/')
            temp_file.unlink()
            assert '80.1' not in request('/')
            params = request('/parameters')
            assert 'value="siyi"' not in params
            main_options = re.search(r'<select[^>]*name="main_resolution"[^>]*>(.*?)</select>', params, re.S)[1]
            assert '1920x1080' in main_options and '3840x2160' not in main_options
            assert 'name="recording_resolution"' in params
            attitude = request('/live/attitude.json')
            assert 'pitch_deg' in json.loads(attitude), attitude
            assert json.loads(attitude)['yaw_rate_dps'] is None
            live = request('/live')
            token = re.search(r'data-csrf="([a-f0-9]+)"', live)[1]
            connection = http.client.HTTPConnection('127.0.0.1', webport, timeout=5)
            connection.request('POST', '/live/control', urlencode({'csrf':token, 'action':'acquire'}),
                {'Authorization':auth, 'Content-Type':'application/x-www-form-urlencoded'})
            response = connection.getresponse(); lease = response.read().decode().strip(); connection.close()
            assert response.status == 200, lease
            before = struct.unpack_from('<h', mcu.frames[-1], 11)[0]
            connection = http.client.HTTPConnection('127.0.0.1', webport, timeout=5)
            connection.request('POST', '/live/control', urlencode({'csrf':token, 'action':'right', 'value':'10', 'lease':lease}),
                {'Authorization':auth, 'Content-Type':'application/x-www-form-urlencoded'})
            response = connection.getresponse(); body = response.read(); connection.close()
            assert response.status == 200, (response.status, body)
            time.sleep(.1)
            assert struct.unpack_from('<h', mcu.frames[-1], 11)[0] > before
            # Web TCP queries must not take over the autopilot UDP route.
            receive('GIMBAL_DEVICE_ATTITUDE_STATUS')
            # Real output RTSP must be independently consumable.
            run('ffmpeg', '-hide_banner', '-loglevel', 'error', '-rtsp_transport', 'tcp',
                '-i', f'rtsp://127.0.0.1:{outputport}/video1', '-frames:v', '3', '-f', 'null', '-', timeout=12)
            # Verify the republished clock follows thermal 5 fps, not the nominal
            # 30 fps encoded in the original SPS. Capture packets without transcoding.
            timed = root/'relayed.mkv'
            run('ffmpeg', '-hide_banner', '-loglevel', 'error', '-rtsp_transport', 'tcp',
                '-i', f'rtsp://127.0.0.1:{outputport}/video1', '-c', 'copy', '-frames:v', '6', str(timed), timeout=12)
            data = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-show_entries',
                'packet=pts_time', '-select_streams', 'v:0', '-of', 'json', str(timed)]))
            pts = [float(p['pts_time']) for p in data['packets']]
            # FFmpeg normalizes the initial packets to its output time origin;
            # verify the established cadence, including every later packet.
            assert len(pts) == 6 and all(.19 < b-a < .21 for a,b in zip(pts[1:], pts[2:])), pts
            # Let the same source connection run past its keepalive interval.
            deadline = time.monotonic() + 22
            while not rtsp.options and time.monotonic() < deadline:
                time.sleep(.1)
            assert rtsp.options and rtsp.connections == 1, (rtsp.options, rtsp.connections)
            # Inspect a complete package after the cross build, if present.
            packages = list((ROOT/'build').glob('Z1Mini_AP_*.gcu'))
            packages += list((ROOT/'release').rglob('Z1Mini_AP_*.gcu'))
            for path in packages:
                with zipfile.ZipFile(path) as z:
                    assert z.testzip() is None
                    hooks = {'gcu/ipc/run.sh', 'gcu/ipc/camera_gcu.sh'}
                    assert all(n.startswith('gcu/ap/') or n in hooks for n in z.namelist())
                    actual = set(z.namelist())
                    assert actual <= accepted, (actual, accepted)
                    assert {'gcu/ap/manifest.json', 'gcu/ap/SHA256SUMS',
                            'gcu/ap/camera-app', 'gcu/ap/z1mini-web'} <= actual
                    # The vendor updater replaces /opt/bin/gcu; rcS needs ipc/run.sh.
                    assert hooks <= set(z.namelist())
                    assert 'gcu/gb_control' not in z.namelist() and 'gcu/ipc/main' not in z.namelist()
                    for line in z.read('gcu/ap/SHA256SUMS').decode().splitlines():
                        digest, name = line.split('  ')
                        assert hashlib.sha256(z.read('gcu/ap/'+name)).hexdigest() == digest
            print('Z1-Mini offline integration passed: MCU, MAVLink, H264/MP4/RTSP, web temperature and package scope')
        finally:
            if web:
                web.terminate(); web.wait(timeout=5)
            app.terminate()
            try:
                app.wait(timeout=10)
            except subprocess.TimeoutExpired:
                app.kill(); app.wait()
            udp.close(); mcu.close(); rtsp.close()
            if app.returncode:
                print((root/'app.log').read_text())
            log.close()
            if app.returncode or not web:
                print((root/"build.log").read_text()[-5000:])


if __name__ == '__main__':
    main()
