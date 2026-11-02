#!/usr/bin/env python3
"""Repeated MAVLink targets must not restart the gimbal positioning controller."""
import argparse
import math
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time

from test_mavlink_parameters import port, stop, wait_ready, connect
from gimbal_sim import Gimbal, parse_mt11_private, parse_a8_private, parse_siyi, siyi_frame
from pymavlink import mavutil
from pymavlink.quaternion import Quaternion

ROOT = Path(__file__).resolve().parents[1]
M = mavutil.mavlink


class MCU:
    def __init__(self, backend, mounting_direction):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(('127.0.0.1', 0))
        self.socket.settimeout(0.02)
        self.gimbal = Gimbal(mounting_direction, backend)
        self.parse = parse_mt11_private if backend == "mt11" else parse_a8_private
        self.lock = threading.Lock()
        self.finished = threading.Event()
        self.angles = []
        self.drop_angle = False
        self.drop_feedback = False
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def run(self):
        while not self.finished.is_set():
            try:
                data, peer = self.socket.recvfrom(4096)
            except socket.timeout:
                continue
            with self.lock:
                self.gimbal.update()
                *_, command, payload = self.parse(data)
                if command == 0x16:
                    _, _, opcode, _ = parse_siyi(payload)
                    if opcode == 0x0e:
                        self.angles.append(time.monotonic())
                        if self.drop_angle:
                            self.drop_angle = False
                            continue
                    if opcode == 0x0d and self.drop_feedback:
                        continue
                reply = self.gimbal.handle_private(data)
                if reply:
                    self.socket.sendto(reply, peer)

    def count(self):
        with self.lock:
            return len(self.angles)

    def close(self):
        self.finished.set()
        self.thread.join()
        self.socket.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', choices=('mt11', 'a8', 'zr10'), default='mt11')
    parser.add_argument('--orientation', choices=('upright', 'inverted'), default='upright')
    parser.add_argument('--build', type=Path)
    args = parser.parse_args()
    build = args.build or ROOT / 'build' / ('sitl' if args.backend == 'mt11' else args.backend + '-sitl')
    with tempfile.TemporaryDirectory(prefix=args.backend + '-angle-hold-') as tmp:
        root = Path(tmp)
        config = root / 'camera.ini'
        config.write_text('[mavlink]\nsystem_id=42\n')
        mcu = MCU(args.backend, 2 if args.orientation == 'inverted' else 1)
        initial_yaw = mcu.gimbal.properties['gimbal_yaw_max'] - 5
        changed_yaw, recovery_yaw = initial_yaw - 1, initial_yaw - 15
        tcp_port, vendor_port = port(), port()
        env = dict(os.environ, CAMERA_APP_CONFIG=str(config), CAMERA_APP_BACKEND=args.backend,
                   CAMERA_APP_UART=f'udp://127.0.0.1:{mcu.socket.getsockname()[1]}',
                   CAMERA_APP_PORT=str(vendor_port), CAMERA_APP_RTSP_PORT=str(port()),
                   CAMERA_APP_MAVLINK_TCP_PORT=str(tcp_port),
                   CAMERA_APP_MAVLINK_UDP_PORT='0', CAMERA_APP_READY_PATH=str(root / 'ready'))
        for key in ('CAMERA_APP_SITL_TERRAIN', 'CAMERA_APP_SITL_VIDEO1', 'CAMERA_APP_SITL_VIDEO2'):
            env.pop(key, None)
        camera = link = None
        with (root / 'camera.log').open('w') as log:
            try:
                camera = subprocess.Popen([str(build.resolve() / 'camera-app')],
                                          env=env, stdout=log, stderr=subprocess.STDOUT)
                wait_ready(root / 'ready', camera)
                link = connect(f'tcp:127.0.0.1:{tcp_port}')
                # Let auto mounting detection finish before measuring duplicate
                # targets: a new mounting transform must resend the target.
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as vendor:
                    vendor.settimeout(0.2)
                    deadline = time.monotonic() + 4
                    while True:
                        vendor.sendto(siyi_frame(1, 1, 0x0a), ('127.0.0.1', vendor_port))
                        try:
                            _, _, opcode, payload = parse_siyi(vendor.recv(4096))
                            if opcode == 0x0a and payload[5] == mcu.gimbal.mounting_direction:
                                break
                        except socket.timeout:
                            pass
                        assert time.monotonic() < deadline, 'mounting direction not detected'
                        time.sleep(0.05)

                def target(yaw=initial_yaw, duration=0.2, rates=None):
                    deadline = time.monotonic() + duration
                    q = Quaternion([0, math.radians(-25), math.radians(yaw)]).q
                    while time.monotonic() < deadline:
                        link.mav.gimbal_device_set_attitude_send(42, M.MAV_COMP_ID_GIMBAL,
                            M.GIMBAL_DEVICE_FLAGS_YAW_IN_VEHICLE_FRAME, q,
                            math.nan, math.nan if rates is None else 0,
                            math.nan if rates is None else rates)
                        time.sleep(0.1)
                        while link.recv_match(blocking=False) is not None:
                            pass

                target(duration=4)
                assert mcu.count() == 1, ('hold/slew resent target', mcu.angles)
                target(yaw=changed_yaw)
                assert mcu.count() == 2, 'changed target was delayed'
                target(yaw=changed_yaw, duration=2)
                assert mcu.count() == 2, 'settled target resent'
                target(yaw=changed_yaw + 0.01)
                assert mcu.count() == 2, 'same wire target resent due to float noise'

                # A rate command cancels the angle even when its value is zero.
                target(yaw=changed_yaw, rates=0)
                target(yaw=changed_yaw)
                assert mcu.count() == 3, ('angle after rate control', mcu.angles)

                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as vendor:
                    vendor.sendto(siyi_frame(1, 42, 0x08, b'\x01'), ('127.0.0.1', vendor_port))
                time.sleep(0.1)
                target(yaw=changed_yaw)
                assert mcu.count() == 4, 'angle after vendor neutral was suppressed'
                target(yaw=changed_yaw, duration=2)

                with mcu.lock:
                    mcu.drop_angle = True
                before = mcu.count()
                target(yaw=recovery_yaw, duration=3)
                assert mcu.count() == before + 2, 'lost target did not recover once'

                with mcu.lock:
                    mcu.gimbal.yaw = 0
                    mcu.gimbal.target = None
                    mcu.gimbal.yaw_rate = mcu.gimbal.pitch_rate = 0
                before = mcu.count()
                target(yaw=recovery_yaw, duration=4)
                assert mcu.count() == before + 1, 'MCU reset did not recover once'

                with mcu.lock:
                    mcu.drop_feedback = True
                before = mcu.count()
                target(yaw=recovery_yaw, duration=3.2)
                assert 2 <= mcu.count() - before <= 3, 'stale feedback retry missing/unbounded'
                print(f'PASS {args.backend} {args.orientation}: angle hold, slew, changed targets, rate/vendor override, lost command, reset and feedback loss')
            except Exception:
                print((root / 'camera.log').read_text())
                raise
            finally:
                if link:
                    link.close()
                stop(camera)
                mcu.close()


if __name__ == '__main__':
    main()
