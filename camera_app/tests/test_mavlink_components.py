#!/usr/bin/env python3
"""Check all six camera/gimbal identities against the app and simulated MCU."""
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from test_mavlink_integration import (
    Quaternion, mavutil, query_siyi_attitude, request_message, reserve_tcp_udp_port, terminate,
    wait_path, wait_siyi_attitude,
)

M = mavutil.mavlink
# Explicit wire values catch the non-contiguous jump from GIMBAL to GIMBAL2.
PAIRS = ((100, 154), (101, 171), (102, 172), (103, 173), (104, 174), (105, 175))


def receive(link, kind):
    message = link.recv_match(type=kind, blocking=True, timeout=4)
    assert message is not None, kind
    return message


def drain(link):
    while link.recv_match(blocking=False) is not None:
        pass


def request(link, component, kind):
    return request_message(link, component, getattr(M, 'MAVLINK_MSG_ID_' + kind), kind)


def vehicle_state(link, component):
    link.mav.srcSystem, link.mav.srcComponent = 1, 1
    try:
        link.mav.autopilot_state_for_gimbal_device_send(
            1, component, time.monotonic_ns() // 1000, [1, 0, 0, 0],
            0, 0, 0, 0, 0, 0, 0, 0)
    finally:
        link.mav.srcSystem, link.mav.srcComponent = 42, 190


def main():
    binary, simulator = [str(Path(arg).resolve()) for arg in sys.argv[1:]]
    vendor_port, mav_port, gimbal_port = [reserve_tcp_udp_port() for _ in range(3)]
    with tempfile.TemporaryDirectory(prefix='mavlink-components-') as directory:
        root = Path(directory)
        config, ready, gimbal_ready = [root / name for name in ('camera.ini', 'ready', 'gimbal.ready')]
        config.write_text('[mavlink]\nsystem_id=1\ncamera_component_id=100\n')
        env = dict(os.environ, CAMERA_APP_UART=f'udp://127.0.0.1:{gimbal_port}',
                   CAMERA_APP_PORT=str(vendor_port), CAMERA_APP_CONFIG=str(config),
                   CAMERA_APP_MAVLINK_TCP_PORT=str(mav_port), CAMERA_APP_MAVLINK_UDP_PORT=str(mav_port),
                   CAMERA_APP_READY_PATH=str(ready), CAMERA_APP_LOG_ROOT=str(root / 'logs'))
        camera = gimbal = link = udp = None
        with (root / 'test.log').open('w+') as log:
            try:
                gimbal = subprocess.Popen([sys.executable, simulator, '--port', str(gimbal_port),
                    '--ready-file', str(gimbal_ready)], stdout=log, stderr=log)
                wait_path(gimbal_ready, gimbal)
                for index, (camera_id, gimbal_id) in enumerate(PAIRS):
                    ready.unlink(missing_ok=True)
                    camera = subprocess.Popen([binary], env=env, stdout=log, stderr=log)
                    wait_path(ready, camera)
                    link = mavutil.mavlink_connection(f'tcp:127.0.0.1:{mav_port}',
                                                      source_system=42, source_component=190)
                    link.mav.srcSystem, link.mav.srcComponent = 1, 1
                    link.mav.heartbeat_send(M.MAV_TYPE_FIXED_WING, M.MAV_AUTOPILOT_ARDUPILOTMEGA,
                                            0, 0, M.MAV_STATE_ACTIVE)
                    link.mav.srcSystem, link.mav.srcComponent = 42, 190
                    for _ in range(3):
                        interval = receive(link, 'COMMAND_LONG')
                        assert interval.get_srcComponent() == gimbal_id, interval
                        assert interval.command == M.MAV_CMD_SET_MESSAGE_INTERVAL
                    heartbeats = {}
                    for _ in range(2):
                        hb = receive(link, 'HEARTBEAT')
                        heartbeats[hb.get_srcComponent()] = hb.type
                    assert heartbeats == {camera_id: M.MAV_TYPE_CAMERA, gimbal_id: M.MAV_TYPE_GIMBAL}, heartbeats
                    assert request(link, camera_id, 'CAMERA_INFORMATION').gimbal_device_id == gimbal_id
                    assert request(link, gimbal_id, 'GIMBAL_DEVICE_INFORMATION').gimbal_device_id == 0

                    # Commands and FC state for a different gimbal must be ignored.
                    wrong_id = 154 if gimbal_id != 154 else 171
                    vehicle_state(link, wrong_id)
                    status = request(link, gimbal_id, 'GIMBAL_DEVICE_ATTITUDE_STATUS')
                    accepts_earth = M.GIMBAL_DEVICE_FLAGS_ACCEPTS_YAW_IN_EARTH_FRAME
                    assert not status.flags & accepts_earth
                    vehicle_state(link, gimbal_id)
                    status = request(link, gimbal_id, 'GIMBAL_DEVICE_ATTITUDE_STATUS')
                    assert status.flags & accepts_earth
                    assert status.gimbal_device_id == 0
                    for target in (wrong_id, gimbal_id):
                        drain(link)
                        link.mav.command_int_send(1, target, M.MAV_FRAME_GLOBAL,
                            M.MAV_CMD_DO_SET_ROI_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0)
                        ack = link.recv_match(type='COMMAND_ACK', blocking=True, timeout=.3 if target == wrong_id else 4)
                        if target == wrong_id:
                            assert ack is None, ack
                        else:
                            assert ack is not None and ack.get_srcComponent() == gimbal_id, ack
                            assert ack.result == M.MAV_RESULT_ACCEPTED
                    drain(link)
                    link.mav.command_long_send(1, wrong_id, M.MAV_CMD_REQUEST_MESSAGE, 0,
                        M.MAVLINK_MSG_ID_GIMBAL_DEVICE_INFORMATION, 0, 0, 0, 0, 0, 0)
                    assert link.recv_match(type='COMMAND_ACK', blocking=True, timeout=.3) is None

                    yaw = 12 if index % 2 else -12
                    attitude = Quaternion([0, math.radians(-10), math.radians(yaw)])
                    link.mav.gimbal_device_set_attitude_send(1, gimbal_id,
                        M.GIMBAL_DEVICE_FLAGS_YAW_IN_VEHICLE_FRAME, attitude.q,
                        math.nan, math.nan, math.nan)
                    wait_siyi_attitude(vendor_port, -10, yaw)
                    wrong_attitude = Quaternion([0, 0, math.radians(-3 * yaw)])
                    link.mav.gimbal_device_set_attitude_send(1, wrong_id,
                        M.GIMBAL_DEVICE_FLAGS_YAW_IN_VEHICLE_FRAME, wrong_attitude.q,
                        math.nan, math.nan, math.nan)
                    time.sleep(.3)
                    _, held_pitch, held_yaw = query_siyi_attitude(vendor_port)
                    assert abs(held_pitch + 10) < 2 and abs(held_yaw - yaw) < 3, (held_pitch, held_yaw)

                    udp = mavutil.mavlink_connection(f'udpout:127.0.0.1:{mav_port}',
                                                     source_system=42, source_component=190)
                    assert request(udp, camera_id, 'CAMERA_INFORMATION').gimbal_device_id == gimbal_id
                    assert request(udp, gimbal_id, 'GIMBAL_DEVICE_INFORMATION').gimbal_device_id == 0
                    udp.close()
                    udp = None
                    if index + 1 < len(PAIRS):
                        # Saving the next pair must leave both current identities in use
                        # until restart; the next iteration verifies persistence.
                        next_id = PAIRS[index + 1][0]
                        drain(link)
                        link.mav.param_set_send(1, camera_id, b'MAV_CAM_COMP_ID', next_id, M.MAV_PARAM_TYPE_REAL32)
                        saved = receive(link, 'PARAM_VALUE')
                        assert saved.get_srcComponent() == camera_id and saved.param_value == next_id, saved
                        assert request(link, camera_id, 'CAMERA_INFORMATION').gimbal_device_id == gimbal_id
                        request(link, gimbal_id, 'GIMBAL_DEVICE_INFORMATION')
                    link.close()
                    link = None
                    terminate(camera)
                    camera = None
                    print(f'PASS camera {camera_id}/gimbal {gimbal_id}: discovery, TCP/UDP, FC state, commands, restart')
            except BaseException:
                log.flush()
                log.seek(0)
                print(log.read())
                raise
            finally:
                for connection in (link, udp):
                    if connection is not None:
                        connection.close()
                terminate(camera)
                terminate(gimbal)


if __name__ == '__main__':
    main()
