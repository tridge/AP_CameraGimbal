#!/usr/bin/env python3
"""Check manual web ownership against MAVLink commands and retained ROI tracking."""
import errno
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

from test_mavlink_integration import (
    GIMBAL_COMPONENT, Quaternion, discover, location_command, mavutil,
    query_siyi_attitude, reserve_tcp_udp_port, send_vehicle_attitude,
    send_vehicle_position, terminate, wait_path, wait_siyi_attitude,
)


def main():
    binary = str(Path(sys.argv[1]).resolve())
    simulator = str(Path(sys.argv[2]).resolve())
    vendor_port, mav_port, gimbal_port = [reserve_tcp_udp_port() for _ in range(3)]
    with tempfile.TemporaryDirectory(prefix="manual-mavlink-") as directory:
        root = Path(directory)
        ready = root / "ready"
        gimbal_ready = root / "gimbal.ready"
        config = root / "camera.ini"
        config.write_text("[mavlink]\nsystem_id=1\nposition_targeting=true\n")
        env = dict(os.environ, CAMERA_APP_UART=f"udp://127.0.0.1:{gimbal_port}",
                   CAMERA_APP_PORT=str(vendor_port), CAMERA_APP_CONFIG=str(config),
                   CAMERA_APP_MAVLINK_TCP_PORT=str(mav_port), CAMERA_APP_MAVLINK_UDP_PORT="0",
                   CAMERA_APP_READY_PATH=str(ready), CAMERA_APP_LOG_ROOT=str(root / "logs"))
        processes = []
        with (root / "test.log").open("w+") as log:
            def start_camera():
                ready.unlink(missing_ok=True)
                process = subprocess.Popen([binary], env=env, stdout=log, stderr=log)
                processes.append(process)
                wait_path(ready, process)
                return process

            try:
                gimbal = subprocess.Popen([sys.executable, simulator, "--port", str(gimbal_port),
                    "--ready-file", str(gimbal_ready)], stdout=log, stderr=log)
                processes.append(gimbal)
                wait_path(gimbal_ready, gimbal)
                camera = start_camera()
                link = mavutil.mavlink_connection(f"tcp:127.0.0.1:{mav_port}", source_system=42)
                discover(link)
                M = mavutil.mavlink
                link.mav.srcSystem, link.mav.srcComponent = 1, 1
                link.mav.heartbeat_send(M.MAV_TYPE_FIXED_WING, M.MAV_AUTOPILOT_ARDUPILOTMEGA,
                                        0, 0, M.MAV_STATE_ACTIVE)
                link.mav.srcSystem, link.mav.srcComponent = 42, 190
                time.sleep(.2)
                vehicle_yaw = 0

                def refresh_roi_vehicle():
                    send_vehicle_position(link, -35.36, 149.16, 600)
                    send_vehicle_attitude(link, vehicle_yaw)

                refresh_roi_vehicle()
                ack = location_command(link, M.MAV_CMD_DO_SET_ROI_LOCATION, M.MAV_FRAME_GLOBAL,
                                       -35.36 + 100 / 111319.5, 149.16, 550)
                assert ack.result == M.MAV_RESULT_ACCEPTED
                wait_siyi_attitude(vendor_port, -26.565, 0, refresh=refresh_roi_vehicle)
                ipc = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                ipc.settimeout(2)
                token = bytes(16)

                def control(action, expected=0, value=0):
                    nonlocal token
                    port = int(dict(line.split("=", 1) for line in ready.read_text().splitlines())["manual_port"])
                    ipc.sendto(struct.pack("=II16sfi", 0x4d434131, action, token, value, 0), ("127.0.0.1", port))
                    magic, received, new_token, _, result = struct.unpack("=II16sfi", ipc.recv(1024))
                    assert magic == 0x4d434131 and received == action and result == expected, result
                    if action == 1 and not result:
                        token = new_token

                control(1)
                time.sleep(.5)  # Allow the physical rate response to settle after stop.
                held = query_siyi_attitude(vendor_port)
                vehicle_yaw = 45
                refresh_roi_vehicle()
                target = Quaternion([0, 0, 0])
                link.mav.gimbal_device_set_attitude_send(1, GIMBAL_COMPONENT, 0, target.q,
                                                        float('nan'), float('nan'), float('nan'))
                link.mav.gimbal_device_set_attitude_send(1, GIMBAL_COMPONENT, 0, [float('nan')]*4, 0, .5, .5)
                ack = location_command(link, M.MAV_CMD_DO_SET_ROI_NONE, M.MAV_FRAME_GLOBAL)
                assert ack.result == M.MAV_RESULT_TEMPORARILY_REJECTED
                time.sleep(.4)
                blocked = query_siyi_attitude(vendor_port)
                assert abs(blocked[1]-held[1]) < .3 and abs(blocked[2]-held[2]) < .3, (held, blocked)
                control(3)
                wait_siyi_attitude(vendor_port, -26.565, -45, refresh=refresh_roi_vehicle)
                control(1)
                terminate(camera)
                link.close()
                start_camera()
                control(2, -errno.EACCES)  # A still-open browser cannot re-lock after restart.
                control(1)
                control(3)
                ipc.close()
                print("PASS MAVLink angle/rate lockout, ROI pause/resume, camera restart clears lease")
            except BaseException:
                log.flush()
                log.seek(0)
                print(log.read())
                raise
            finally:
                for process in reversed(processes):
                    terminate(process)


if __name__ == "__main__":
    main()
