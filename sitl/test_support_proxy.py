#!/usr/bin/env python3
"""Exercise the camera SITL against a real local ArduPilot SupportProxy."""
import argparse
import concurrent.futures
import hashlib
import json
import os
import re
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

os.environ.setdefault('MAVLINK20', '1')
from pymavlink import mavutil
from test_video_telemetry import port, ready, stop

REPO = Path(__file__).resolve().parents[1]
M = mavutil.mavlink
KEY = hashlib.sha256(b'camera-sitl-signing').digest()
PASSWORD = 'publish&test?=secret'


def wait_for(check, message, timeout=20):
    end = time.monotonic() + timeout
    while not check():
        assert time.monotonic() < end, message
        time.sleep(.05)


def stop_group(process):
    if process is None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=5)


def run_case(args, directory, fixture, signing=True, password=True, codec='h264'):
    directory.mkdir()
    sys.path.insert(0, str(args.proxy))
    import keydb_lib
    user, engineer, v1, v2, v3 = [port() for _ in range(5)]
    db = keydb_lib.init_db(str(directory / 'keys.tdb'))
    db.transaction_start()
    keydb_lib.add_entry(db, user, engineer, 'camera SITL', 'camera-sitl-signing')
    keydb_lib.set_flag(db, engineer, 'video')
    if signing:
        keydb_lib.set_flag(db, engineer, 'bidi_sign')
    keydb_lib.set_video_ports(db, engineer, [v1, v2, v3] if args.raw_thermal else [v1, v2])
    for i in range(3 if args.raw_thermal else 2):
        keydb_lib.set_video_slot_flag(db, engineer, i, 'record')
        keydb_lib.set_video_slot_flag(db, engineer, i, 'session_ok')
    if password:
        keydb_lib.set_video_publish_pass(db, engineer, PASSWORD)
    db.transaction_prepare_commit(); db.transaction_commit(); db.close()
    config = directory / 'camera.ini'
    extension = '.ts' if args.telemetry_format == 'misb' else '.mp4'
    mavlink_enabled = args.case not in ('disabled', 'video-only')
    streams = 1 if args.case == 'single-video' else 2
    config.write_text(f'''[video]
telemetry = {args.telemetry_format}
[mavlink]
system_id = 0
[recording]
autorecord = true
[stream.main]
codec = {'h265' if codec == 'hevc' else 'h264'}
[stream.sub]
codec = {'h265' if codec == 'hevc' else 'h264'}
[support_proxy]
enabled = {'false' if args.case == 'disabled' else 'true'}
host = localhost
mavlink_port = {user if mavlink_enabled else 0}
signing = {'true' if signing else 'false'}
signing_passphrase = camera-sitl-signing
video1_port = {v1}
video2_port = {v2 if streams == 2 else 0}
video1_name = Front Camera
video2_name = Thermal Camera
video3_port = {v3 if args.raw_thermal else 0}
video3_name = Raw Thermal
publish_password = "{PASSWORD if password else ''}"
network_address = 192.0.2.25/24
network_gateway = 192.0.2.1
''')
    gimbal_port, mav_port, rtsp_port = port(socket.SOCK_DGRAM), port(), port()
    mav_udp = port(socket.SOCK_DGRAM) if args.case == 'session' else 0
    camera_ready, gimbal_ready = directory / 'camera.ready', directory / 'gimbal.ready'
    recordings = directory / 'recordings'; recordings.mkdir()
    env = dict(os.environ, CAMERA_APP_BACKEND=args.backend,
        CAMERA_APP_UART=f'udp://127.0.0.1:{gimbal_port}', CAMERA_APP_PORT=str(port()),
        CAMERA_APP_RTSP_PORT=str(rtsp_port), CAMERA_APP_MAVLINK_TCP_PORT=str(mav_port),
        CAMERA_APP_MAVLINK_UDP_PORT=str(mav_udp), CAMERA_APP_CONFIG=str(config),
        CAMERA_APP_READY_PATH=str(camera_ready), CAMERA_APP_RECORD_STATE=str(directory / 'recording.state'),
        CAMERA_APP_RECORD_ROOT=str(recordings), CAMERA_APP_CAPTURE_ROOT=str(directory / 'capture'),
        CAMERA_APP_SITL_VIDEO1=str(fixture), CAMERA_APP_SITL_VIDEO2=str(fixture))
    if args.raw_thermal:
        env['CAMERA_APP_RAW_THERMAL_PORT'] = '0'  # publish through the proxy alone
    camera = gimbal = proxy = fc = eng = feeder = None
    done = threading.Event(); received_commands = []; feed_errors = []
    logs = []
    def log(name):
        f = (directory / name).open('w'); logs.append(f); return f
    try:
        proxy_log = log('proxy.log')
        proxy = subprocess.Popen([str(args.proxy / 'supportproxy')], cwd=directory,
                                 stdout=proxy_log, stderr=subprocess.STDOUT, start_new_session=True)
        wait_for(lambda: 'video slot 1 listening' in (directory / 'proxy.log').read_text(), 'proxy startup')
        gimbal = subprocess.Popen([sys.executable, str(REPO / 'sitl/gimbal_sim.py'),
            '--backend', args.backend, '--port', str(gimbal_port), '--ready-file', str(gimbal_ready)],
            stdout=log('gimbal.log'), stderr=subprocess.STDOUT)
        ready(gimbal_ready, gimbal, directory / 'gimbal.log')
        camera = subprocess.Popen([str(args.build / 'camera-app'), '--backend', args.backend], env=env,
                                   stdout=log('camera.log'), stderr=subprocess.STDOUT)
        ready(camera_ready, camera, directory / 'camera.log')
        local_endpoint = f'udpout:127.0.0.1:{mav_udp}' if mav_udp else f'tcp:127.0.0.1:{mav_port}'
        fc = mavutil.mavlink_connection(local_endpoint, source_system=42, source_component=1)
        def feed():
            try:
                epoch = time.monotonic()
                while not done.is_set():
                    boot_ms = 1000+int((time.monotonic()-epoch)*1000)
                    fc.mav.heartbeat_send(M.MAV_TYPE_QUADROTOR, M.MAV_AUTOPILOT_ARDUPILOTMEGA,
                                          M.MAV_MODE_FLAG_SAFETY_ARMED, 0, M.MAV_STATE_ACTIVE)
                    fc.mav.global_position_int_send(boot_ms, -353632610, 1491652300, 620250, 36500, 100, 200, 0, 1234)
                    fc.mav.attitude_send(boot_ms, .1, -.2, .3, 0, 0, 0)
                    while (message := fc.recv_match(blocking=False)) is not None:
                        if message.get_type() == 'COMMAND_LONG' and message.confirmation == 77:
                            received_commands.append(message)
                    done.wait(.05)
            except Exception as e:
                feed_errors.append(e)
        feeder = threading.Thread(target=feed); feeder.start()
        if args.case == 'disabled':
            time.sleep(3)
            assert camera.poll() is None and any(recordings.rglob('*'+extension))
            assert 'SupportProxy MAVLink UDP connected' not in (directory / 'camera.log').read_text()
            assert 'RTSP publisher' not in (directory / 'proxy.log').read_text()
            assert 'have UDP conn1' not in (directory / 'proxy.log').read_text()
            print(f'PASS {args.backend} disabled: no proxy traffic, local recording continues', flush=True)
            return
        if mavlink_enabled:
            eng = mavutil.mavlink_connection(f'udpout:127.0.0.1:{engineer}', source_system=255, source_component=190)
            eng.setup_signing(KEY, sign_outgoing=True, link_id=7)
            end = time.monotonic() + 15
            seen = set(); signed_vehicle = False
            while time.monotonic() < end and len(seen) < 3:
                eng.mav.heartbeat_send(M.MAV_TYPE_GCS, M.MAV_AUTOPILOT_INVALID, 0, 0, M.MAV_STATE_ACTIVE)
                message = eng.recv_match(blocking=True, timeout=.2)
                if message and message.get_srcSystem() == 42:
                    seen.add(message.get_srcComponent())
                    if message.get_srcComponent() == 1 and message.get_signed(): signed_vehicle = True
            assert {1, M.MAV_COMP_ID_CAMERA, M.MAV_COMP_ID_GIMBAL} <= seen, seen
            if signing: assert signed_vehicle, 'vehicle telemetry not signed'
            eng.mav.command_long_send(42, 1, M.MAV_CMD_REQUEST_MESSAGE, 77, M.MAVLINK_MSG_ID_AUTOPILOT_VERSION, 0, 0, 0, 0, 0, 0)
            wait_for(lambda: received_commands, 'engineer command not forwarded to flight controller')
            assert not received_commands[0].get_signed(), 'proxy signature leaked onto local link'
            # Camera parameter responses must also return through the proxy.
            eng.mav.param_request_read_send(42, M.MAV_COMP_ID_CAMERA, b'PROXY_MAV_PORT', -1)
            param = eng.recv_match(type='PARAM_VALUE', blocking=True, timeout=5)
            assert param and param.param_id == 'PROXY_MAV_PORT' and int(param.param_value) == user, param
            eng.mav.command_long_send(42, M.MAV_COMP_ID_CAMERA, M.MAV_CMD_REQUEST_VIDEO_STREAM_INFORMATION,
                                      0, 1, 0, 0, 0, 0, 0, 0)
            info = eng.recv_match(type='VIDEO_STREAM_INFORMATION', blocking=True, timeout=5)
            assert info and info.name == 'Front Camera' and info.uri == f'http://localhost:{v1}/v1.ts', info
            assert info.type == M.VIDEO_STREAM_TYPE_MPEG_TS

        expected_publishers = streams + int(args.raw_thermal)
        wait_for(lambda: (directory / 'camera.log').read_text().count('publishing') >= expected_publishers,
                 'video publishers did not connect', 25)
        for index in range(streams):
            wait_for(lambda: re.search(rf'video slot {index} stats:.*join=ready',
                                      (directory / 'proxy.log').read_text()),
                     f'proxy stream {index} not ready for viewers', 40)
        # Read each real proxy output concurrently, decode it and recover SEI.
        def video(portnum, index):
            target = directory / f'proxy-video{index}.ts'
            cmd = ['ffmpeg', '-v', 'error', '-rw_timeout', '15000000', '-i',
                   f'http://127.0.0.1:{portnum}/v{index}.ts', '-t', '2', '-map', '0:v:0', '-c:v', 'copy', '-y', str(target)]
            result = subprocess.run(cmd, capture_output=True, timeout=35)
            assert result.returncode == 0, result.stderr.decode()
            decode = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(target), '-f', 'null', '-'], capture_output=True, timeout=20)
            assert decode.returncode == 0 and not decode.stderr, decode.stderr.decode()
            records = subprocess.check_output([sys.executable, str(REPO / 'tools/video_telemetry.py'), str(target)], timeout=20)
            values = [json.loads(line) for line in records.splitlines()]
            assert len(values) >= 20, len(values)
            steps = [b['pts90k'] - a['pts90k'] for a, b in zip(values, values[1:])]
            assert all(step > 0 for step in steps), steps
            assert any(v['position'] and v['position']['lat_e7'] == -353632610 and v['hfov_deg'] for v in values)
            return len(values)
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            counts = list(pool.map(lambda x: video(*x), [(v1, 1), (v2, 2)][:streams]))
        def check_raw():
            if not args.raw_thermal: return
            import importlib.util
            import numpy as np
            # The shared helpers already imported the installed MAVProxy, so
            # load the checkout's reader by path rather than through sys.path.
            spec = importlib.util.spec_from_file_location(
                'thermal_stream', args.mavproxy / 'MAVProxy/modules/mavproxy_camera/thermal_stream.py')
            thermal_stream = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(thermal_stream)
            ThermalReader = thermal_stream.ThermalReader
            wait_for(lambda: re.search(r'video slot 2 stats:.*join=ready',
                                      (directory/'proxy.log').read_text()), 'raw stream not ready')
            uri = f'http://127.0.0.1:{v3}/v3.mkv'
            if eng:
                eng.mav.command_long_send(42, 100, M.MAV_CMD_REQUEST_VIDEO_STREAM_INFORMATION,
                                           0, 3, 0, 0, 0, 0, 0, 0)
                info = eng.recv_match(type='VIDEO_STREAM_INFORMATION', blocking=True, timeout=5)
                assert info and info.stream_id == 3 and info.count == 3 and info.type == 200, info
                assert info.uri == f'http://localhost:{v3}/v3.mkv', info
                assert info.name == 'Raw Thermal' and info.flags & M.VIDEO_STREAM_STATUS_FLAGS_THERMAL, info
                # Without a local listener the stream must still start and stop.
                for command, running in ((M.MAV_CMD_VIDEO_STOP_STREAMING, False), (M.MAV_CMD_VIDEO_START_STREAMING, True)):
                    eng.mav.command_long_send(42, 100, command, 0, 3, 0, 0, 0, 0, 0, 0)
                    status = eng.recv_match(type='VIDEO_STREAM_STATUS', blocking=True, timeout=5)
                    assert status and status.stream_id == 3 and bool(status.flags & M.VIDEO_STREAM_STATUS_FLAGS_RUNNING) == running, status
                    ack = eng.recv_match(type='COMMAND_ACK', blocking=True, timeout=5)
                    assert ack and ack.command == command and ack.result == M.MAV_RESULT_ACCEPTED, ack
            reader = ThermalReader(uri)
            try:
                stamps = []
                for pixels, meta in reader.frames():
                    expected = (np.arange(640*512, dtype=np.uint32)+meta['frame_id']*257).astype(np.uint16).reshape(512,640)
                    assert np.array_equal(pixels, expected), 'proxy changed native samples'
                    assert meta['telemetry']['position']['lat_e7'] == -353632610, meta
                    stamps.append(meta['capture_monotonic_us'])
                    if len(stamps) == 10: break
                assert len(stamps) == 10 and all(b>a for a,b in zip(stamps, stamps[1:]))
            finally:
                reader.close()
            print('PASS raw thermal: MAVLink discovery, native 16-bit samples and capture telemetry through proxy', flush=True)
        check_raw()
        if args.reconnect:
            before = sum(p.stat().st_size for p in recordings.rglob('*'+extension))
            connected = (directory / 'camera.log').read_text().count('publishing')
            stop_group(proxy)
            time.sleep(3)
            after = sum(p.stat().st_size for p in recordings.rglob('*'+extension))
            assert after > before and camera.poll() is None, 'proxy outage interrupted local recording'
            proxy = subprocess.Popen([str(args.proxy / 'supportproxy')], cwd=directory,
                                     stdout=proxy_log, stderr=subprocess.STDOUT, start_new_session=True)
            wait_for(lambda: (directory / 'camera.log').read_text().count('publishing') >= connected + expected_publishers,
                     'publishers did not reconnect', 25)
            time.sleep(15)  # allow the proxy's codec probe and join cache to refill
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                counts = list(pool.map(lambda x: video(*x), [(v1, 1), (v2, 2)][:streams]))
            check_raw()
            print('PASS proxy restart: local recording kept growing and both publishers recovered', flush=True)
        assert not feed_errors, feed_errors
        print(f'PASS {args.backend} {codec} signing={signing} password={password}: MAVLink relay={mavlink_enabled}, {streams} decoded video streams with telemetry {counts}', flush=True)
    finally:
        done.set()
        if feeder: feeder.join(timeout=3)
        if fc: fc.close()
        if eng: eng.close()
        stop(camera); stop(gimbal); stop_group(proxy)
        for f in logs: f.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', choices=['mt11', 'a8'], default='mt11')
    parser.add_argument('--build', type=Path, default=REPO / 'build/sitl')
    parser.add_argument('--proxy', type=Path, default=REPO.parent / 'SupportProxy')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--telemetry-format', choices=['apcg', 'misb'], default='apcg')
    parser.add_argument('--case', choices=['signed', 'session', 'hevc', 'disabled', 'video-only', 'single-video'], default='signed')
    parser.add_argument('--reconnect', action='store_true')
    parser.add_argument('--raw-thermal', action='store_true')
    parser.add_argument('--mavproxy', type=Path, default=REPO.parent / 'MAVProxy',
                        help='MAVProxy checkout providing the raw thermal reader (--raw-thermal)')
    args = parser.parse_args()
    if args.raw_thermal and args.backend != 'mt11': parser.error('--raw-thermal requires mt11')
    args.build = args.build.resolve(); args.proxy = args.proxy.resolve()
    directory = (args.output or Path(tempfile.mkdtemp(prefix='supportproxy-sitl-'))).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    codec = 'hevc' if args.case == 'hevc' else 'h264'
    fixture = directory / ('source.' + codec)
    encoder = ['-c:v', 'libx265', '-x265-params', 'aud=1:repeat-headers=1:keyint=25:bframes=0:pools=1:log-level=error'] if codec == 'hevc' else ['-c:v', 'libx264', '-x264-params', 'aud=1:repeat-headers=1:keyint=25:bframes=0']
    subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=320x240:rate=25',
                    '-t', '2', *encoder, '-y', str(fixture)], check=True)
    print('Logs:', directory, flush=True)
    run_case(args, directory / 'case', fixture, signing=args.case != 'session', password=args.case != 'session', codec=codec)


if __name__ == '__main__':
    main()
