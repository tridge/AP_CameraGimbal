#!/usr/bin/env python3
"""Run four real camera stacks together through the tabbed Qt launcher."""
import os
from pathlib import Path
import random
import socket
import subprocess
import sys
import tempfile
from unittest import mock

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
os.environ.setdefault('MAVLINK20', '1')
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from pymavlink import mavutil
from sitl_launch import Launcher, QtWidgets, REPO, owned_processes
from test_launch import until
from test_sitl import web_request


def main():
    app = QtWidgets.QApplication([])
    root = Path(tempfile.mkdtemp(prefix='sitl-multi-real-'))
    print(f'Four-camera test output: {root}', flush=True)
    allocated = set()

    def base_port(stride, adjacent=False):
        for _ in range(100):
            base = random.randrange(20000, 45000)
            ports = {base + i * stride + n for i in range(4) for n in range(2 if adjacent else 1)}
            if ports & allocated:
                continue
            sockets = []
            try:
                for port in ports:
                    for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
                        sock = socket.socket(socket.AF_INET, kind)
                        sockets.append(sock)
                        sock.bind(('127.0.0.1', port))
                allocated.update(ports)
                return base
            except OSError:
                pass
            finally:
                for sock in sockets:
                    sock.close()
        raise RuntimeError('Could not find free test ports')

    web, vendor, rtsp, mav = base_port(1), base_port(1), base_port(10, True), base_port(10)
    env = {'CAMERA_GIMBAL_SITL_BUILD': str(root / 'camera'),
           'CAMERA_GIMBAL_SITL_PYTHON': str(REPO / 'build/terrain-venv/bin/python')}
    for backend in ('mt11', 'a8', 'zr10', 'z1mini'):
        for name, value in (('WEB_PORT', web), ('CAMERA_PORT', vendor), ('RTSP_PORT', rtsp),
                            ('MAVLINK_TCP_PORT', mav), ('MAVLINK_UDP_PORT', mav), ('GIMBAL_PORT', 0)):
            env[backend.upper() + '_SITL_' + name] = str(value)
    window = Launcher(REPO)
    window.count.setValue(4)
    tokens = []
    with mock.patch.dict(os.environ, env):
        try:
            for backends in (('mt11',) * 4, ('mt11', 'a8', 'zr10', 'z1mini')):
                for i, panel in enumerate(window.simulators):
                    panel.camera.setCurrentIndex(panel.camera.findData(backends[i]))
                    panel.orientation.setCurrentIndex(i % 2)
                window.start()
                until(app, lambda: window.phase in ('running', 'idle'), timeout=180)
                assert window.phase == 'running', window.status.text() + '\n' + '\n'.join(p.log.toPlainText()[-3000:] for p in window.simulators)
                tokens = [p.token for p in window.simulators]
                for i, panel in enumerate(window.simulators):
                    with web_request(web + i, '/') as response:
                        assert response.status == 200
                    with socket.create_connection(('127.0.0.1', vendor + i), timeout=5):
                        pass
                    for transport in ('tcp', 'udpout'):
                        link = mavutil.mavlink_connection(f'{transport}:127.0.0.1:{mav + 10*i}',
                                                          source_system=42, source_component=1)
                        try:
                            link.mav.heartbeat_send(mavutil.mavlink.MAV_TYPE_FIXED_WING,
                                                   mavutil.mavlink.MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, 4)
                            link.mav.srcSystem, link.mav.srcComponent = 255, 190
                            link.mav.command_long_send(42, 100+i, mavutil.mavlink.MAV_CMD_REQUEST_CAMERA_INFORMATION,
                                                       0, 1, 0, 0, 0, 0, 0, 0)
                            info = link.recv_match(type='CAMERA_INFORMATION', blocking=True, timeout=5)
                            assert info and info.get_srcComponent() == 100+i, info
                            assert info.gimbal_device_id == (154, 171, 172, 173)[i], info
                        finally:
                            link.close()
                    result = subprocess.run(['ffmpeg', '-v', 'error', '-rtsp_transport', 'tcp',
                        '-i', f'rtsp://127.0.0.1:{rtsp + 10*i}/video1', '-frames:v', '1', '-f', 'null', '-'],
                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=20)
                    assert result.returncode == 0, result.stderr.decode()
                    assert panel.web_button.isEnabled()
                saved = [(p.runtime.parent / 'app/camera.ini').read_bytes() for p in window.simulators]
                window.stop()
                until(app, lambda: window.phase == 'idle', timeout=20)
                assert all(not owned_processes(token) for token in tokens)
                assert saved == [(p.runtime.parent / 'app/camera.ini').read_bytes() for p in window.simulators]
                print(f'PASS concurrent {backends}: four web servers, TCP/UDP MAVLink identities, RTSP decoding and cleanup', flush=True)
        finally:
            window.stop()
            until(app, lambda: window.phase == 'idle', timeout=20)
            for i, panel in enumerate(window.simulators):
                (root / f'simulator-{i+1}.log').write_text(panel.log.toPlainText())
            window.close()


if __name__ == '__main__':
    main()
