"""Exercise the bundled PyQt launcher and real services with a tools-free PATH."""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import struct
import tempfile
import time


def main():
    import faulthandler
    faulthandler.dump_traceback_later(180, repeat=True)
    from sitl.target_properties import TARGETS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', choices=tuple(TARGETS) + ('all',), default='all')
    parser.add_argument('--video', choices=('simple', 'terrain'), default='simple')
    parser.add_argument('--orientation', choices=('upright', 'inverted'), default='upright')
    parser.add_argument('--show-window', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if not args.show_window:
        os.environ['QT_QPA_PLATFORM'] = 'offscreen'
    os.environ['PATH'] = str(Path(os.environ['SystemRoot']) / 'System32')
    os.environ.pop('PYTHONPATH', None)
    os.environ['MAVLINK20'] = '1'
    from PyQt6 import QtWidgets
    from sitl_launch import Launcher
    from sitl.test_sitl import request, siyi
    import http.cookiejar
    import urllib.parse
    import urllib.request
    def web_request(port, path):
        return browser.open(urllib.request.Request(f'http://127.0.0.1:{port}{path}',
                            headers={'Accept': 'text/html'}), timeout=4)
    def web_form(port, path, csrf, fields, timeout=4):
        data = urllib.parse.urlencode({'csrf': csrf, **fields}).encode('ascii')
        return browser.open(urllib.request.Request(f'http://127.0.0.1:{port}{path}',
                            data=data, headers={'Accept': 'text/html'}), timeout=timeout)
    def login(port):
        with web_request(port, '/login') as response:
            token = re.search(r'name=token value="([0-9a-f]{64})"', response.read().decode()).group(1)
        with web_form(port, '/login', '', {'token': token, 'username': 'admin',
                                          'password': 'ardupilot', 'lang': 'en'}) as response:
            assert response.geturl().endswith('/'), 'Login did not reach the status page'
            assert b'action=/logout' in response.read()
        assert any(cookie.name == 'session' for cookie in cookies), 'No session cookie issued'
    import av
    from pymavlink import mavutil
    app = QtWidgets.QApplication([])
    # Cygwin implements some pipes with loopback sockets. Do not select the
    # next Windows ephemeral port and then let service startup consume it.
    import random
    allocated = set()
    def reserve_port(socktype):
        for _ in range(100):
            port = random.randrange(20000, 40000)
            if port in allocated or port - 1 in allocated or port + 1 in allocated:
                continue
            try:
                with socket.socket(socket.AF_INET, socktype) as probe:
                    probe.bind(('127.0.0.1', port))
            except OSError:
                continue
            allocated.add(port)
            return port
        raise RuntimeError('No free test port')
    output = args.output or Path(tempfile.mkdtemp(prefix='Camera SITL package test '))
    output.mkdir(parents=True, exist_ok=True)
    print(f'Test output: {output}', flush=True)
    def pump_until(predicate, timeout):
        deadline = time.monotonic() + timeout
        while not predicate():
            app.processEvents()
            if time.monotonic() > deadline:
                raise AssertionError('Timed out waiting for launcher: ' + window.status.text())
            time.sleep(.02)
        app.processEvents()
    for backend in (tuple(TARGETS) if args.backend == 'all' else (args.backend,)):
        cookies = http.cookiejar.CookieJar()
        browser = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cookies))
        prefix = backend.upper() + '_SITL_'
        camera_port, web_port, mav_port = [reserve_port(socket.SOCK_STREAM) for _ in range(3)]
        while True:
            rtsp_port = reserve_port(socket.SOCK_STREAM)
            try:
                with socket.socket() as probe:
                    probe.bind(('127.0.0.1', rtsp_port + 1))
                break
            except OSError:
                pass
        build = output / backend
        os.environ.update(CAMERA_GIMBAL_SITL_BUILD=str(build))
        for key, value in [('CAMERA_PORT', camera_port), ('WEB_PORT', web_port),
                           ('RTSP_PORT', rtsp_port), ('MAVLINK_TCP_PORT', mav_port),
                           ('MAVLINK_UDP_PORT', 0)]:
            os.environ[prefix + key] = str(value)
        window = Launcher()
        panel = window.simulators[0]
        panel.camera.setCurrentIndex(panel.camera.findData(backend))
        panel.orientation.setCurrentIndex(panel.orientation.findData(args.orientation))
        panel.video.setCurrentIndex(panel.video.findData(args.video))
        window.show()
        link = None
        try:
            window.start_button.click()
            pump_until(lambda: window.phase in ('running', 'idle'), 60)
            assert window.phase == 'running', panel.log.toPlainText()
            print(f'{backend}: launcher running', flush=True)
            assert panel.web_button.isEnabled()
            window.grab().save(str(output / (backend + '-launcher.png')))
            login(web_port)
            print(f'{backend}: browser login and session cookie passed', flush=True)
            with web_request(web_port, '/live') as response:
                page = response.read().decode()
            csrf = re.search(r'data-csrf="([0-9a-f]{64})"', page).group(1)
            config = build / 'runtime/app/camera.ini'
            changed = re.sub(r'(?m)^autorecord\s*=.*$', 'autorecord = while_armed', config.read_text())
            with web_form(web_port, '/config', csrf, {'config': changed, 'action': 'save'}, timeout=30) as response:
                assert response.status == 200
            assert 'autorecord = while_armed' in config.read_text()
            ready = build / 'runtime/run/camera-app.ready'
            old_pid = re.search(r'pid=(\d+)', ready.read_text()).group(1)
            with web_form(web_port, '/restart', csrf, {}, timeout=30) as response:
                assert response.status == 200
                response.read()
            assert re.search(r'pid=(\d+)', ready.read_text()).group(1) != old_pid
            print(f'{backend}: web save and camera restart passed', flush=True)
            with web_request(web_port, '/live') as response:
                assert b'id=live-video' in response.read()
            with web_request(web_port, '/files') as response:
                assert b'files-table' in response.read()
            if TARGETS[backend]['vendor_protocol'] == 1:
                control = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                control.connect(('127.0.0.1', camera_port))
                assert len(request(control, 1, 0x0D)) == 12
                control.send(siyi(2, 0x07, b'\x0a\x00'))
                time.sleep(.2)
                with web_request(web_port, '/live/attitude.json') as response:
                    attitude = json.load(response)
                assert attitude['yaw_rate_dps'] > 0, attitude
                print(f'{backend}: web and SIYI responding', flush=True)
                control.send(siyi(3, 0x07, b'\x00\x00'))
            link = mavutil.mavlink_connection(f'tcp:127.0.0.1:{mav_port}', source_system=42, source_component=1)
            def telemetry():
                now = int(time.monotonic() * 1000) & 0xffffffff
                link.mav.heartbeat_send(mavutil.mavlink.MAV_TYPE_FIXED_WING,
                    mavutil.mavlink.MAV_AUTOPILOT_ARDUPILOTMEGA, 128, 0, 4)
                link.mav.global_position_int_send(now, -353632610, 1491652300, 620000, 36000,
                                                  1000, 0, 0, 0)
                link.mav.autopilot_state_for_gimbal_device_send(42, 154, time.monotonic_ns() // 1000,
                    [1, 0, 0, 0], 0, 10, 0, 0, 0, 0, 2, 0)
            telemetry()
            link.mav.param_request_list_send(42, 100)
            assert link.recv_match(type='PARAM_VALUE', blocking=True, timeout=5), 'No MAVLink parameters'
            for stream_id in (1, 2):
                link.mav.command_long_send(42, 100, mavutil.mavlink.MAV_CMD_REQUEST_MESSAGE, 0,
                    mavutil.mavlink.MAVLINK_MSG_ID_VIDEO_STREAM_INFORMATION, stream_id, 0, 0, 0, 0, 0)
                info = link.recv_match(type='VIDEO_STREAM_INFORMATION', blocking=True, timeout=5)
                expected_rate = 20 if args.video == 'terrain' else TARGETS[backend]['frame_rate']
                assert info is not None and info.framerate == expected_rate, info
            # The graphics path must render actual vehicle telemetry, not just
            # create a VTK window. Keep messages moving while decoding frames.
            import threading
            halt = threading.Event()
            def send_telemetry():
                while not halt.wait(.1):
                    telemetry()
            sender = threading.Thread(target=send_telemetry, daemon=True)
            sender.start()
            try:
                if args.video == 'terrain':
                    # Allow meshes and imagery to arrive before saving frames.
                    time.sleep(25)
                    assert re.search(r'\b[1-9][0-9]* meshes',
                                     (build / 'runtime/run/camera_app.log').read_text()), 'No terrain meshes loaded'
                for stream in ('video1', 'video2'):
                    with av.open(f'rtsp://127.0.0.1:{rtsp_port}/{stream}',
                                 options={'rtsp_transport': 'tcp'}, timeout=15) as video:
                        images = []
                        for frame in video.decode(video=0):
                            images.append(frame)
                            if len(images) == 12:
                                break
                        assert len(images) == 12
                        assert images[0].width >= 1280 and images[0].height >= 720
                        images[-1].to_image().save(output / (backend + '-' + stream + '.png'))
                        print(f'{backend} {stream}: {images[0].width}x{images[0].height}, 12 decoded frames', flush=True)
                with web_request(web_port, '/live/video1.mp4') as response:
                    assert b'ftyp' in response.read(1024)
                link.mav.command_long_send(42, 100, mavutil.mavlink.MAV_CMD_VIDEO_START_CAPTURE, 0, 0, 0, 0, 0, 0, 0, 0)
                time.sleep(3)
            finally:
                halt.set(); sender.join(timeout=2)
            if TARGETS[backend]['vendor_protocol'] == 1: control.close()
            with web_form(web_port, '/logout', csrf, {}) as response:
                response.read()
            with web_request(web_port, '/parameters') as response:
                assert response.geturl().endswith('/login'), 'Logout left the session valid'
            login(web_port)
            window.stop_button.click()
            pump_until(lambda: window.phase == 'idle', 15)
            assert not ready.exists(), 'Camera did not remove its ready file'
            assert 'stopped' in (build / 'runtime/run/camera_app.log').read_text(), 'Camera did not finish graceful shutdown'
            (output / (backend + '-video-service.log')).write_bytes(
                (build / 'runtime/run/camera_app.log').read_bytes())
            recordings = list((build / 'runtime/mnt/DCIM/record').glob('*.mp4'))
            assert recordings, 'No recording created'
            for path in recordings:
                with av.open(str(path)) as video:
                    assert next(video.decode(video=0)).width >= 1280
            saved = config.read_bytes()
            window.start_button.click()
            pump_until(lambda: window.phase in ('running', 'idle'), 60)
            assert window.phase == 'running', panel.log.toPlainText()
            assert config.read_bytes() == saved, 'Saved configuration changed on restart'
            with web_request(web_port, '/') as response:
                assert response.geturl().endswith('/login'), 'Old session survived web-server restart'
            login(web_port)
            print(f'{backend}: logout and session invalidation on restart passed', flush=True)
            window.stop_button.click()
            pump_until(lambda: window.phase == 'idle', 15)
            panel.clear_parameters.setChecked(True)
            window.start_button.click()
            pump_until(lambda: window.phase in ('running', 'idle'), 60)
            assert window.phase == 'running', panel.log.toPlainText()
            assert config.with_suffix('.ini.reset.bak').read_bytes() == saved
            assert 'autorecord = false' in config.read_text()
            assert all(path.exists() for path in recordings), 'Reset removed media'
            window.stop_button.click()
            pump_until(lambda: window.phase == 'idle', 15)
            print(f'PASS {backend} {args.orientation} {args.video}: GUI, gimbal, MAVLink parameters, '
                  'RTSP/live MP4, recording, restart and shutdown with no external tools', flush=True)
            # Windows exclusive binds produce the permission-denied failure
            # seen when a ground station already owns the camera's UDP port.
            panel.clear_parameters.setChecked(False)
            overrides = {prefix + 'MAVLINK_' + transport + '_PORT':
                         os.environ.pop(prefix + 'MAVLINK_' + transport + '_PORT')
                         for transport in ('TCP', 'UDP')}
            def set_ports(settings, ports):
                for transport, port in ports.items():
                    settings = re.sub(r'(?m)^' + transport.lower() + r'_port\s*=.*$',
                                      transport.lower() + '_port = ' + str(port), settings)
                return settings
            def check_mavlink(transport, port):
                endpoint = 'tcp' if transport == 'TCP' else 'udpout'
                connection = mavutil.mavlink_connection(f'{endpoint}:127.0.0.1:{port}',
                                                        source_system=42, source_component=1)
                try:
                    connection.mav.heartbeat_send(mavutil.mavlink.MAV_TYPE_FIXED_WING,
                        mavutil.mavlink.MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, 4)
                    connection.mav.param_request_list_send(42, 100)
                    assert connection.recv_match(type='PARAM_VALUE', blocking=True, timeout=5), transport
                finally:
                    connection.close()
            try:
                for blocked in (('UDP',), ('TCP',), ('UDP', 'TCP')):
                    ports = {'TCP': reserve_port(socket.SOCK_STREAM),
                             'UDP': reserve_port(socket.SOCK_DGRAM)}
                    blockers = []
                    try:
                        for transport in blocked:
                            socktype = socket.SOCK_STREAM if transport == 'TCP' else socket.SOCK_DGRAM
                            blocker = socket.socket(socket.AF_INET, socktype)
                            blockers.append(blocker)
                            blocker.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                            blocker.bind(('0.0.0.0', ports[transport]))
                            if transport == 'TCP':
                                blocker.listen(1)
                        config.write_text(set_ports(config.read_text(), ports))
                        window.start_button.click()
                        pump_until(lambda: window.phase in ('running', 'idle'), 60)
                        assert window.phase == 'running', panel.log.toPlainText()
                        login(web_port)
                        with web_request(web_port, '/live') as response:
                            csrf = re.search(r'data-csrf="([0-9a-f]{64})"', response.read().decode()).group(1)
                        log = (build / 'runtime/run/camera_app.log').read_text()
                        for transport, port in ports.items():
                            if transport in blocked:
                                assert f'MAVLink {transport} port {port} unavailable:' in log, log
                                assert f'continuing without {transport}' in log, log
                            else:
                                check_mavlink(transport, port)
                        replacement = {'TCP': reserve_port(socket.SOCK_STREAM),
                                       'UDP': reserve_port(socket.SOCK_DGRAM)}
                        changed = set_ports(config.read_text(), replacement)
                        with web_form(web_port, '/config', csrf, {'config': changed, 'action': 'save'}, timeout=30) as response:
                            response.read()
                            assert response.status == 200
                        with web_form(web_port, '/restart', csrf, {}, timeout=30) as response:
                            response.read()
                            assert response.status == 200
                        for transport, port in replacement.items():
                            check_mavlink(transport, port)
                        print(f'PASS {backend}: {"/".join(blocked)} blocked; web login, surviving '
                              'MAVLink transport and port reconfiguration/restart', flush=True)
                    finally:
                        if window.phase != 'idle':
                            window.stop()
                            pump_until(lambda: window.phase == 'idle', 15)
                        for blocker in blockers:
                            blocker.close()
            finally:
                os.environ.update(overrides)
        finally:
            if link:
                link.close()
            (output / (backend + '-launcher.log')).write_text(panel.log.toPlainText(), encoding='utf-8')
            if window.phase != 'idle':
                window.stop()
                pump_until(lambda: window.phase == 'idle', 15)
            window.close()
    if args.backend == 'all':
        os.environ['CAMERA_GIMBAL_SITL_BUILD'] = str(output / 'multi')
        window = Launcher()
        window.count.setValue(4)
        endpoints = []
        try:
            for index, (backend, panel) in enumerate(zip(TARGETS, window.simulators)):
                panel.camera.setCurrentIndex(panel.camera.findData(backend))
                panel.orientation.setCurrentIndex(panel.orientation.findData(args.orientation))
                panel.video.setCurrentIndex(panel.video.findData(args.video))
                prefix = backend.upper() + '_SITL_'
                web_port, camera_port, mav_port = [reserve_port(socket.SOCK_STREAM) for _ in range(3)]
                while True:
                    rtsp_port = reserve_port(socket.SOCK_STREAM)
                    try:
                        with socket.socket() as probe:
                            probe.bind(('127.0.0.1', rtsp_port + 1))
                        break
                    except OSError:
                        pass
                for name, value, stride in (('WEB_PORT', web_port, 1), ('CAMERA_PORT', camera_port, 1),
                                            ('RTSP_PORT', rtsp_port, 10), ('MAVLINK_TCP_PORT', mav_port, 10)):
                    os.environ[prefix + name] = str(value - index * stride)
                os.environ[prefix + 'MAVLINK_UDP_PORT'] = '0'
                endpoints.append((web_port, mav_port, rtsp_port))
            window.start()
            pump_until(lambda: window.phase in ('running', 'idle'), 150)
            assert window.phase == 'running', window.status.text() + '\n' + '\n'.join(p.log.toPlainText()[-3000:] for p in window.simulators)
            for index, (web_port, mav_port, rtsp_port) in enumerate(endpoints):
                with web_request(web_port, '/login') as response:
                    assert response.status == 200
                connection = mavutil.mavlink_connection(f'tcp:127.0.0.1:{mav_port}', source_system=42, source_component=1)
                try:
                    connection.mav.heartbeat_send(mavutil.mavlink.MAV_TYPE_FIXED_WING,
                                                 mavutil.mavlink.MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, 4)
                    connection.mav.command_long_send(42, 100+index, mavutil.mavlink.MAV_CMD_REQUEST_CAMERA_INFORMATION,
                                                     0, 1, 0, 0, 0, 0, 0, 0)
                    info = connection.recv_match(type='CAMERA_INFORMATION', blocking=True, timeout=5)
                    assert info and info.get_srcComponent() == 100+index, info
                    assert info.gimbal_device_id == (154, 171, 172, 173)[index], info
                finally:
                    connection.close()
                with av.open(f'rtsp://127.0.0.1:{rtsp_port}/video1', options={'rtsp_transport': 'tcp'}, timeout=15) as video:
                    assert next(video.decode(video=0)).width >= 1280
            window.stop()
            pump_until(lambda: window.phase == 'idle', 20)
            assert all(not (p.runtime / 'camera-app.ready').exists() for p in window.simulators)
            print('PASS four simultaneous packaged cameras: web, distinct MAVLink identities, video and stop-all', flush=True)
        finally:
            window.stop()
            pump_until(lambda: window.phase == 'idle', 20)
            for index, panel in enumerate(window.simulators):
                (output / f'multi-{index+1}-launcher.log').write_text(panel.log.toPlainText(), encoding='utf-8')
            window.close()
    print('PASS standalone Windows package', flush=True)
    faulthandler.cancel_dump_traceback_later()
