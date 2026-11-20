#!/usr/bin/env python3
"""A first geographic ROI bootstraps command-only CADDX rate tracking."""
import math
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time

from test_rpi_video_uri import reserve_port, wait_path
from pymavlink import mavutil

M = mavutil.mavlink


def main():
    binary = str(Path(sys.argv[1]).resolve())
    port = reserve_port(socket.SOCK_STREAM)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as wire, tempfile.TemporaryDirectory() as temp:
        wire.bind(('127.0.0.1', 0)); wire.settimeout(.05)
        root = Path(temp)
        config, ready = root/'camera.ini', root/'ready'
        # Omit tracking_method to exercise the factory Rate default.
        config.write_text('[mavlink]\nsystem_id=42\nposition_targeting=true\n')
        env = dict(os.environ, CAMERA_APP_UART=f'udp://127.0.0.1:{wire.getsockname()[1]}',
            CAMERA_APP_MAVLINK_TCP_PORT=str(port), CAMERA_APP_MAVLINK_UDP_PORT='0',
            CAMERA_APP_READY_PATH=str(ready), CAMERA_APP_RECORD_ROOT=str(root),
            CAMERA_APP_CAPTURE_ROOT=str(root), CAMERA_APP_LOG_ROOT=str(root))
        done = threading.Event(); link = thread = None
        with (root/'camera.log').open('w') as log:
            app = subprocess.Popen([binary, '--backend', 'rpi_libcam_caddx', '--config', str(config)],
                env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                wait_path(ready, app)
                link = mavutil.mavlink_connection(f'tcp:127.0.0.1:{port}', source_system=42, source_component=1)
                def feed():
                    start = time.monotonic()
                    while not done.is_set():
                        elapsed = time.monotonic() - start
                        link.mav.heartbeat_send(M.MAV_TYPE_FIXED_WING, M.MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, M.MAV_STATE_ACTIVE)
                        link.mav.global_position_int_send(round(elapsed*1000),
                            round((-35 + math.degrees(elapsed/6378137))*1e7), 1490000000,
                            600000, 100000, 100, 0, 0, 0)
                        link.mav.attitude_send(round(elapsed*1000), 0, 0, 0, 0, 0, 0)
                        done.wait(.05)
                thread = threading.Thread(target=feed); thread.start()
                time.sleep(.3)
                try: wire.recv(100)
                except socket.timeout: pass
                else: raise AssertionError('camera startup moved the gimbal')
                target_lon = 149 + math.degrees(100 / (6378137 * math.cos(math.radians(-35))))
                link.mav.command_int_send(42, 154, M.MAV_FRAME_GLOBAL, M.MAV_CMD_DO_SET_ROI_LOCATION,
                    0, 0, 0, 0, 0, 0, -350000000, round(target_lon*1e7), 500)
                ack = link.recv_match(type='COMMAND_ACK', blocking=True, timeout=3)
                assert ack and ack.command == M.MAV_CMD_DO_SET_ROI_LOCATION and ack.result == M.MAV_RESULT_ACCEPTED, ack
                samples = []
                end = time.monotonic() + 2
                while time.monotonic() < end:
                    try: packet = wire.recv(100)
                    except socket.timeout: continue
                    assert len(packet) == 10 and packet[:2] == b'\xa5\x5a'
                    pitch = (packet[5] | ((packet[6] & 15) << 8)) * 360 / 4096
                    yaw = ((packet[6] >> 4) | (packet[7] << 4)) * 360 / 4096
                    samples.append((pitch if pitch <= 180 else pitch-360, yaw if yaw <= 180 else yaw-360))
                assert len(samples) > 10, samples
                assert -50 < samples[0][0] < -40 and 85 < samples[0][1] < 95, samples[0]
                assert samples[-1][1] > samples[0][1] + .4, (samples[0], samples[-1])
                done.set(); thread.join(2)
                # Vehicle telemetry expiry must stop rate integration.
                time.sleep(3)
                wire.setblocking(False)
                while True:
                    try: wire.recv(100)
                    except BlockingIOError: break
                wire.settimeout(.5)
                stable = [wire.recv(100) for _ in range(5)]
                assert all(p == stable[0] for p in stable), 'stale vehicle telemetry left a rate active'
                print('PASS CADDX no startup motion, first-ROI acquisition, moving Rate tracking and stale-data stop')
            except Exception:
                log.flush(); print((root/'camera.log').read_text()); raise
            finally:
                done.set()
                if thread: thread.join(2)
                if link: link.close()
                app.terminate(); app.wait(timeout=5)


if __name__ == '__main__': main()
