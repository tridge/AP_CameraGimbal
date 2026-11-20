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
from sitl.launcher_config import MAX_SIMULATORS, vendor_stride, vendor_ports
from test_z1mini_sitl import packet, receive
from test_sitl import crc16
from sitl.target_properties import TARGETS
from test_sitl import web_request


def main():
    app = QtWidgets.QApplication([])
    root = Path(tempfile.mkdtemp(prefix='sitl-multi-real-'))
    print(f'Four-camera test output: {root}', flush=True)
    allocated = set()

    def base_port(stride, adjacent=False, span=1):
        for _ in range(100):
            base = random.randrange(20000, 32000)
            ports = {base + i * stride + n for i in range(MAX_SIMULATORS) for n in range(max(span, 2 if adjacent else 1))}
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

    web, vendor, rtsp, mav = base_port(1), base_port(10, span=4), base_port(10, True), base_port(10)
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
            for backends in (('mt11',) * 4, ('mt11', 'a8', 'zr10', 'z1mini'), ('z1mini',) * 4):
                z1_defaults = backends == ('z1mini',) * 4
                os.environ['CAMERA_GIMBAL_SITL_BUILD'] = str(root / ('z1-defaults' if z1_defaults else 'camera'))
                os.environ['Z1MINI_SITL_CAMERA_PORT'] = str(int(TARGETS['z1mini']['vendor_port']) if z1_defaults else vendor)
                for i, panel in enumerate(window.simulators):
                    panel.camera.setCurrentIndex(panel.camera.findData(backends[i]))
                    panel.orientation.setCurrentIndex(i % 2)
                    # Exercise concurrent services without requiring terrain's VTK renderer.
                    panel.video.setCurrentIndex(panel.video.findData('simple'))
                window.start()
                until(app, lambda: window.phase in ('running', 'idle'), timeout=180)
                assert window.phase == 'running', window.status.text() + '\n' + '\n'.join(p.log.toPlainText()[-3000:] for p in window.simulators)
                tokens = [p.token for p in window.simulators]
                for i, panel in enumerate(window.simulators):
                    with web_request(web + i, '/') as response:
                        assert response.status == 200
                    target = TARGETS[backends[i]]
                    command_port = (int(target['vendor_port']) if z1_defaults else vendor) + i * vendor_stride(target)
                    vendor_tcp, vendor_udp = vendor_ports(target,command_port)
                    with socket.create_connection(('127.0.0.1', vendor_tcp), timeout=5) as connection:
                        if z1_defaults:
                            connection.sendall(packet(0))
                            assert receive(connection)[70] == 0
                    if z1_defaults:
                        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as receiver:
                            receiver.bind(('127.0.0.1',int(target['vendor_reply_port'])))
                            receiver.settimeout(3)
                            receiver.sendto(packet(0),('127.0.0.1',vendor_udp))
                            reply=receiver.recv(4096)
                            assert len(reply)==73 and crc16(reply)==0 and reply[70]==0

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
