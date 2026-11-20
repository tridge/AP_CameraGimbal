#!/usr/bin/env python3
"""Check command audit records in real BIN files from the running camera app."""
import math
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile

from test_mavlink_integration import mavutil, reserve_tcp_udp_port, terminate, wait_path

M = mavutil.mavlink


def receive(link, kind):
    msg = link.recv_match(type=kind, blocking=True, timeout=4)
    assert msg is not None, kind
    return msg


def main():
    binary, simulator = [str(Path(arg).resolve()) for arg in sys.argv[1:]]
    vendor, mav, gimbal_port = [reserve_tcp_udp_port() for _ in range(3)]
    with tempfile.TemporaryDirectory(prefix='mavlink-binlog-') as tmp:
        root = Path(tmp)
        config, ready, gimbal_ready = [root / n for n in ('camera.ini', 'ready', 'gimbal.ready')]
        config.write_text('[mavlink]\nsystem_id=1\nposition_targeting=true\n[recording]\nautorecord=while_armed\n')
        env = dict(os.environ, CAMERA_APP_UART=f'udp://127.0.0.1:{gimbal_port}',
                   CAMERA_APP_PORT=str(vendor), CAMERA_APP_CONFIG=str(config),
                   CAMERA_APP_MAVLINK_TCP_PORT=str(mav), CAMERA_APP_MAVLINK_UDP_PORT=str(mav),
                   CAMERA_APP_READY_PATH=str(ready), CAMERA_APP_LOG_ROOT=str(root/'logs'),
                   CAMERA_APP_RECORD_ROOT=str(root/'record'), CAMERA_APP_CAPTURE_ROOT=str(root/'capture'),
                   CAMERA_APP_RECORD_STATE=str(root/'recording.state'))
        processes, links = [], []
        expected = {'MAVC': [], 'GMBC': [], 'MAVP': []}
        with (root/'test.log').open('w+') as output:
            try:
                gimbal = subprocess.Popen([sys.executable, simulator, '--port', str(gimbal_port),
                    '--ready-file', str(gimbal_ready)], stdout=output, stderr=output)
                processes.append(gimbal)
                wait_path(gimbal_ready, gimbal)
                camera = subprocess.Popen([binary], env=env, stdout=output, stderr=output)
                processes.append(camera)
                wait_path(ready, camera)
                link = mavutil.mavlink_connection(f'tcp:127.0.0.1:{mav}', source_system=42, source_component=190)
                links.append(link)
                udp = mavutil.mavlink_connection(f'udpout:127.0.0.1:{mav}', source_system=43, source_component=191)
                links.append(udp)

                def identity(connection, target):
                    return dict(TS=1, TC=target, SS=connection.mav.srcSystem, SC=connection.mav.srcComponent)

                def command(cmd, result=0, params=(0,)*7, connection=link, target=100, integer=False, frame=0):
                    fields = identity(connection, target)
                    if integer:
                        connection.mav.command_int_send(1, target, frame, cmd, 0, 0, *params)
                    else:
                        connection.mav.command_long_send(1, target, cmd, 0, *params)
                    fields.update(Cmd=cmd, WL=int(not integer), Fr=frame if integer else 255, Res=result,
                                  **dict(zip(('P1','P2','P3','P4','X','Y','Z'), params)))
                    expected['MAVC'].append(fields)
                    if result != 255:
                        ack = receive(connection, 'COMMAND_ACK')
                        assert ack.command == cmd and ack.result == result, ack

                def parameter(name, value, result=0, connection=link, target=100, ext=False, raw=None, ptype=None):
                    ptype = ptype if ptype is not None else M.MAV_PARAM_TYPE_REAL32
                    if raw is None:
                        raw = struct.pack('<f', value)
                    if ext:
                        raw = raw.ljust(128, b'\0')
                        connection.mav.param_ext_set_send(1, target, name.encode(), raw, ptype)
                    else:
                        connection.mav.param_set_send(1, target, name.encode(), value, ptype)
                        raw = struct.pack('<f', value).ljust(128, b'\0')
                    expected['MAVP'].append(dict(identity(connection, target), Name=name, Val=value,
                                                 Ext=int(ext), PT=ptype, Res=result, raw=raw))
                    if result != 255 and (ext or name != 'UNKNOWN'):
                        reply = receive(connection, 'PARAM_EXT_ACK' if ext else 'PARAM_VALUE')
                        if ext:
                            assert reply.param_result == result, reply

                # Logging initially off: the enabling write itself must be retained.
                parameter('LOG_DISARMED', 1)
                command(M.MAV_CMD_VIDEO_START_CAPTURE)
                command(M.MAV_CMD_VIDEO_STOP_CAPTURE, connection=udp)
                # Make the recording-state path unwritable to exercise a backend failure.
                (root/'recording.state').mkdir()
                command(M.MAV_CMD_VIDEO_START_CAPTURE, result=M.MAV_RESULT_FAILED)
                (root/'recording.state').rmdir()
                command(M.MAV_CMD_SET_CAMERA_ZOOM, params=(2, 15, 0, 0, 0, 0, 0))
                command(M.MAV_CMD_DO_SET_ROI_LOCATION, target=154, integer=True,
                        params=(0, 0, 0, 0, -353612345, 1491654321, 543.25))
                command(M.MAV_CMD_DO_SET_ROI_LOCATION, result=M.MAV_RESULT_DENIED,
                        target=154, integer=True, frame=M.MAV_FRAME_LOCAL_NED,
                        params=(0, 0, 0, 0, -353612345, 1491654321, 543.25))
                # Unsupported requests still preserve all parameters, NaNs, fractional P5/P6 and exact X/Y.
                command(60000, result=M.MAV_RESULT_UNSUPPORTED,
                        params=(1.25, -2.5, math.nan, math.inf, 123.125, -456.875, -7.25))
                command(60001, result=M.MAV_RESULT_UNSUPPORTED, target=154, integer=True,
                        params=(1, 2, 3, 4, 2147483647, -2147483647, 5))
                command(M.MAV_CMD_VIDEO_START_CAPTURE, result=255, target=101)

                def setpoint(flags, q, rates, result=0, target=154, connection=link):
                    connection.mav.gimbal_device_set_attitude_send(1, target, flags, q, *rates)
                    expected['GMBC'].append(dict(identity(connection, target), Flg=flags, Res=result,
                        **dict(zip(('Q1','Q2','Q3','Q4','VX','VY','VZ'), (*q, *rates)))))

                nan = math.nan
                vehicle = M.GIMBAL_DEVICE_FLAGS_YAW_IN_VEHICLE_FRAME
                setpoint(vehicle, [1, 0, 0, 0], [nan]*3)
                setpoint(vehicle, [nan]*4, [.125, -.25, .5], connection=udp)
                setpoint(M.GIMBAL_DEVICE_FLAGS_NEUTRAL, [nan]*4, [nan]*3)
                setpoint(M.GIMBAL_DEVICE_FLAGS_RETRACT, [nan]*4, [nan]*3)
                setpoint(M.GIMBAL_DEVICE_FLAGS_YAW_IN_EARTH_FRAME, [1, 0, 0, 0], [nan]*3,
                         result=M.MAV_RESULT_TEMPORARILY_REJECTED)
                setpoint(vehicle, [nan]*4, [nan]*3, result=M.MAV_RESULT_DENIED)
                setpoint(vehicle, [1, 0, 0, 0], [nan]*3, target=171, result=255)
                # Synchronize both sockets before acquiring manual ownership.
                command(M.MAV_CMD_REQUEST_CAMERA_INFORMATION, connection=udp)
                command(M.MAV_CMD_REQUEST_CAMERA_INFORMATION)
                manual = int(dict(line.split('=', 1) for line in ready.read_text().splitlines())['manual_port'])
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as ipc:
                    ipc.settimeout(3)
                    ipc.sendto(struct.pack('=II16sfi', 0x4d434131, 1, bytes(16), 0, 0), ('127.0.0.1', manual))
                    _, _, token, _, result = struct.unpack('=II16sfi', ipc.recv(1024))
                    assert result == 0
                    setpoint(vehicle, [1, 0, 0, 0], [nan]*3, result=M.MAV_RESULT_TEMPORARILY_REJECTED)
                    command(M.MAV_CMD_DO_SET_ROI_NONE, target=154, integer=True,
                            result=M.MAV_RESULT_TEMPORARILY_REJECTED)
                    ipc.sendto(struct.pack('=II16sfi', 0x4d434131, 3, token, 0, 0), ('127.0.0.1', manual))
                    assert struct.unpack('=II16sfi', ipc.recv(1024))[-1] == 0
                parameter('IMG_BRIGHTNESS', 63, connection=udp)
                parameter('IMG_BRIGHTNESS', 1000, result=M.PARAM_ACK_FAILED)
                parameter('UNKNOWN', 2, result=M.PARAM_ACK_VALUE_UNSUPPORTED)
                parameter('IMG_BRIGHTNESS', 64, target=101, result=255)
                parameter('CAM_ZOOM', 2.5, ext=True)
                parameter('CAM_SOURCE', 1, ext=True, raw=struct.pack('<i', 1), ptype=M.MAV_PARAM_EXT_TYPE_INT32)
                parameter('CAM_ZOOM', 1000, ext=True, result=M.PARAM_ACK_VALUE_UNSUPPORTED)
                parameter('UNKNOWN', 255, ext=True, raw=b'\xff', ptype=M.MAV_PARAM_EXT_TYPE_UINT8,
                          result=M.PARAM_ACK_VALUE_UNSUPPORTED)
                raw = bytes(range(128))
                parameter('UNKNOWN', nan, ext=True, raw=raw, ptype=M.MAV_PARAM_EXT_TYPE_CUSTOM,
                          result=M.PARAM_ACK_VALUE_UNSUPPORTED, connection=udp)
                parameter('CAM_ZOOM', 3.5, ext=True, target=101, result=255)

                # FC telemetry/arming are separate from command envelopes, but retain sender IDs.
                link.mav.srcSystem, link.mav.srcComponent = 1, 1
                link.mav.heartbeat_send(M.MAV_TYPE_FIXED_WING, M.MAV_AUTOPILOT_ARDUPILOTMEGA,
                                        M.MAV_MODE_FLAG_SAFETY_ARMED, 10, M.MAV_STATE_ACTIVE)
                link.mav.attitude_send(1000, 0, 0, .5, 0, 0, .1)
                link.mav.global_position_int_send(1000, -353612345, 1491654321, 600000, 100000, 100, 200, -30, 100)
                link.mav.autopilot_state_for_gimbal_device_send(1, 154, 1000000, [1,0,0,0],
                                                               0, 0, 1, 2, 3, 0, 0, 0)
                link.mav.heartbeat_send(M.MAV_TYPE_FIXED_WING, M.MAV_AUTOPILOT_ARDUPILOTMEGA,
                                        0, 11, M.MAV_STATE_ACTIVE)
                link.mav.srcSystem, link.mav.srcComponent = 42, 190
                # Barrier guarantees all preceding TCP requests reached the logger.
                command(M.MAV_CMD_REQUEST_CAMERA_INFORMATION)
                command(M.MAV_CMD_REQUEST_CAMERA_INFORMATION, connection=udp)
                parameter('LOG_DISARMED', 0)
                terminate(camera)

                records = {}
                for path in (root/'logs').glob('*.BIN'):
                    reader = mavutil.mavlink_connection(str(path))
                    while (msg := reader.recv_match()) is not None:
                        assert msg.get_type() != 'BAD_DATA', msg
                        records.setdefault(msg.get_type(), []).append(msg)
                    reader.close()
                for kind, wanted in expected.items():
                    actual = records.get(kind, [])
                    assert len(actual) == len(wanted), (kind, len(actual), len(wanted))
                    # TCP and UDP ordering is independent. Match within each sender stream.
                    for fields in wanted:
                        index = next(i for i, r in enumerate(actual) if r.SS == fields['SS'] and r.SC == fields['SC'])
                        msg = actual.pop(index)
                        for key, value in fields.items():
                            if key == 'raw':
                                # DataFlash 'a' decodes into two int16 arrays, preserving every byte.
                                got = struct.pack('<64h', *msg.Raw1, *msg.Raw2)
                            else:
                                got = getattr(msg, key)
                            assert (math.isnan(got) if isinstance(value, float) and math.isnan(value) else got == value), (kind, key, got, value)
                assert all((m.SS, m.SC) == (1, 1) for k in ('MAVH','ATT','POS') for m in records[k])
                assert len(records['MAVH']) == 2
                assert [m.Mode for m in records['MAVH']] == [10, 11]
                assert len(records['ATT']) == 2 and len(records['POS']) == 1
                assert {m.Active for m in records['VID']} == {0, 1}
                assert not records.get('CMD'), 'old ambiguous CMD records are still emitted'
                print('PASS decoded MAVC/GMBC/MAVP/MAVH, sender/target IDs, exact parameters, raw values, accepted/rejected/ignored requests, recording and FC telemetry')
            except BaseException:
                output.flush(); output.seek(0)
                print(output.read()[-12000:])
                raise
            finally:
                for connection in links:
                    connection.close()
                for process in reversed(processes):
                    terminate(process)


if __name__ == '__main__':
    main()
