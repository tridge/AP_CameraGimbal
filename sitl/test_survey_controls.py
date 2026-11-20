#!/usr/bin/env python3
"""Survey ignores external pointing while it holds the gimbal, and follows it once paused."""
import argparse
import json
import os
from pathlib import Path
import socket
import math
import struct
import subprocess
import sys
import tempfile
import time

from test_mavlink_parameters import connect, port, receive, stop, survey_status_path, wait_ready, write
from test_sitl import siyi, request as siyi_request
from test_unigcs import Client, command as unigcs_command
from pymavlink import mavutil

M = mavutil.mavlink
ROOT = Path(__file__).resolve().parents[1]


def run(binary):
    with tempfile.TemporaryDirectory(prefix='survey-controls-') as directory:
        root = Path(directory)
        cfg = root/'camera.ini'
        cfg.write_text('[mavlink]\nsystem_id=42\nposition_targeting=true\n')
        gp, cp, sp, up = (port() for _ in range(4))
        env = dict(os.environ, CAMERA_APP_BACKEND='mt11', CAMERA_APP_CONFIG=str(cfg),
                   CAMERA_APP_UART=f'udp://127.0.0.1:{gp}', CAMERA_APP_PORT=str(sp),
                   CAMERA_APP_UNIGCS_PORT=str(up), CAMERA_APP_DISCOVERY_PORT=str(port()),
                   CAMERA_APP_MAVLINK_TCP_PORT=str(cp), CAMERA_APP_MAVLINK_UDP_PORT='0',
                   CAMERA_APP_RTSP_PORT=str(port()), CAMERA_APP_RAW_THERMAL_PORT=str(port()),
                   CAMERA_APP_CAPTURE_ROOT=str(root/'capture'), CAMERA_APP_RECORD_ROOT=str(root/'record'),
                   CAMERA_APP_LOG_ROOT=str(root/'logs'), CAMERA_APP_READY_PATH=str(root/'camera.ready'))
        for key in ('CAMERA_APP_SITL_VIDEO1', 'CAMERA_APP_SITL_VIDEO2', 'CAMERA_APP_SITL_TERRAIN'):
            env.pop(key, None)
        processes = []
        link = client = fc = manual = None
        status_path = survey_status_path(cfg)
        with (root/'test.log').open('w+') as log:
            try:
                processes.append(subprocess.Popen([sys.executable, str(ROOT/'sitl/gimbal_sim.py'),
                    '--port', str(gp), '--ready-file', str(root/'gimbal.ready')], stdout=log, stderr=log))
                wait_ready(root/'gimbal.ready', processes[-1])
                processes.append(subprocess.Popen([str(binary)], env=env, stdout=log, stderr=log))
                wait_ready(root/'camera.ready', processes[-1])
                link = connect(f'tcp:127.0.0.1:{cp}')
                receive(link, 'HEARTBEAT')
                fc = connect(f'tcp:127.0.0.1:{cp}')
                fc.mav.srcSystem, fc.mav.srcComponent = 42, 1

                def owner(ours=True):
                    fc.mav.heartbeat_send(M.MAV_TYPE_FIXED_WING, M.MAV_AUTOPILOT_ARDUPILOTMEGA,
                                          M.MAV_MODE_FLAG_CUSTOM_MODE_ENABLED, 10, M.MAV_STATE_ACTIVE)
                    fc.mav.gimbal_manager_status_send(1000, 0, 154, 42, 154 if ours else 1, 0, 0)
                    while fc.recv_match(blocking=False):
                        pass

                def command(command_id, params, expected=M.MAV_RESULT_ACCEPTED):
                    link.mav.command_long_send(42, 100, command_id, 0, *(params + [0]*(7-len(params))))
                    ack = receive(link, 'COMMAND_ACK', lambda m: m.command == command_id)
                    assert ack.result == expected, ack

                def state(predicate, owned=True):
                    deadline = time.monotonic()+3
                    last = None
                    while time.monotonic() < deadline:
                        owner(owned)
                        if status_path.exists():
                            last = json.loads(status_path.read_text())
                            if predicate(last):
                                return last
                        time.sleep(.05)
                    raise AssertionError(last)

                def start():
                    command(M.MAV_CMD_SET_CAMERA_MODE, [0, 2])
                    state(lambda s: s['mode'] == 2 and s['state'] == 'waiting for fresh terrain and telemetry')

                manual_port = int(dict(line.split('=', 1) for line in
                    (root/'camera.ready').read_text().splitlines())['manual_port'])
                manual = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                manual.settimeout(2)

                def lease(action, token=bytes(16)):
                    manual.sendto(struct.pack('<II16sfi', 0x4d434131, action, token, 0, 0),
                                  ('127.0.0.1', manual_port))
                    return struct.unpack('<II16sfi', manual.recv(32))

                acquired = lease(1)
                assert acquired[-1] == 0
                command(M.MAV_CMD_SET_CAMERA_MODE, [0, 2], M.MAV_RESULT_TEMPORARILY_REJECTED)
                state(lambda s: s['mode'] == 0)
                assert lease(3, acquired[2])[-1] == 0

                def attitude():
                    with socket.create_connection(('127.0.0.1', sp), 2) as sock:
                        return siyi_request(sock, 20, 0x0d)[:6]

                def ignored(before, expected='waiting for fresh terrain and telemetry'):
                    time.sleep(.15)
                    after = state(lambda s: s['mode'] == 2 and
                                  s['state'] == expected)
                    assert after['captured'] == 0
                    assert attitude() == before, 'external command moved the gimbal in Survey mode'

                def rates(moving):
                    deadline = time.monotonic()+2
                    while time.monotonic() < deadline:
                        with socket.create_connection(('127.0.0.1', sp), 2) as sock:
                            feedback = struct.unpack('<6h', siyi_request(sock, 25, 0x0d)[:12])
                        if any(feedback[3:]) == moving:
                            return
                        time.sleep(.02)
                    raise AssertionError(('gimbal should be moving' if moving else 'latched external rate survived ownership grant', feedback))

                def external_rate():
                    fc.mav.gimbal_device_set_attitude_send(42, 154, 0,
                        [math.nan]*4, math.nan, .2, .3)
                    rates(True)

                # Direct FC rates do not belong to the camera's ROI controller.
                # A grant must still stop them before telemetry/terrain permit
                # survey steering, even though subsequent external stops reject.
                command(M.MAV_CMD_SET_CAMERA_MODE, [0, 2])
                state(lambda s: s['state'] == 'waiting for gimbal ownership', owned=False)
                external_rate()
                state(lambda s: s['state'] == 'waiting for fresh terrain and telemetry')
                rates(False)
                before = attitude();time.sleep(.2)
                assert attitude() == before, 'gimbal drifted after the survey ownership grant'
                # Entering Survey from Photo must discard a direct rate too.
                command(M.MAV_CMD_SET_CAMERA_MODE, [0, 0])
                external_rate()
                command(M.MAV_CMD_SET_CAMERA_MODE, [0, 2])
                state(lambda s: s['state'] == 'waiting for gimbal ownership', owned=False)
                rates(False)

                # Manual pointing paths pause a waiting survey. A delayed
                # manager grant must not silently restart it afterwards.
                for source in ('SIYI','UniGCS','web PTZ','ROI LOCATION'):
                    command(M.MAV_CMD_SET_CAMERA_MODE, [0, 0])
                    command(M.MAV_CMD_SET_CAMERA_MODE, [0, 2])
                    state(lambda s: s['state'] == 'waiting for gimbal ownership', owned=False)
                    if source == 'SIYI':
                        with socket.create_connection(('127.0.0.1', sp), 2) as sock:
                            siyi_request(sock, 26, 0x0e, struct.pack('<hh', 100, 100))
                    elif source == 'UniGCS':
                        client = Client(up)
                        client.sock.sendall(unigcs_command(0x9a, b'\x0a\x00', dest=0x2e, link=0x11))
                    elif source == 'web PTZ':
                        token = lease(1)
                        assert token[-1] == 0
                        assert lease(3, token[2])[-1] == 0
                    else:
                        link.mav.command_int_send(42, 154, M.MAV_FRAME_GLOBAL,
                            M.MAV_CMD_DO_SET_ROI_LOCATION, 0, 0, 0, 0, 0, 0, -352826408, 1490052529, 600)
                        ack = receive(link, 'COMMAND_ACK', lambda m: m.command == M.MAV_CMD_DO_SET_ROI_LOCATION)
                        assert ack.result == M.MAV_RESULT_ACCEPTED, ack
                    state(lambda s: s['state'].startswith('survey paused:'), owned=False)
                    owner();time.sleep(.6)
                    state(lambda s: s['state'].startswith('survey paused:'))
                if client:
                    client.sock.close();client = None

                start()
                before = attitude()
                assert lease(1)[-1] == -16  # EBUSY: web PTZ cannot stop survey first.
                # Both manager output and GCS commands must be dropped after
                # ownership is confirmed, before they clear ROI or hit hardware.
                for sender in (fc, link):
                    for flags, q, rate in ((0, [math.cos(.15), 0, math.sin(.15), 0], math.nan),
                                           (0, [math.nan]*4, .2),
                                           (M.GIMBAL_DEVICE_FLAGS_NEUTRAL, [math.nan]*4, math.nan)):
                        owner()
                        sender.mav.gimbal_device_set_attitude_send(42, 154, flags, q, math.nan, rate, rate)
                        ignored(before)
                for sender in (fc, link):
                    for roi in (M.MAV_CMD_DO_SET_ROI_LOCATION, M.MAV_CMD_DO_SET_ROI_NONE):
                        sender.mav.command_int_send(42, 154, M.MAV_FRAME_GLOBAL, roi, 0, 0,
                                                 0, 0, 0, 0, -352826408, 1490052529, 600)
                        ack = receive(sender, 'COMMAND_ACK', lambda m: m.command == roi)
                        assert ack.result == M.MAV_RESULT_TEMPORARILY_REJECTED, ack
                        ignored(before)
                command(M.MAV_CMD_CAMERA_TRACK_RECTANGLE, [.2, .2, .4, .4],
                        M.MAV_RESULT_TEMPORARILY_REJECTED)
                ignored(before)

                # Cover every manual SIYI command recognized by the dispatcher.
                for opcode, payload in [(0x07, b'\x0a\x00'), (0x08, b'\x01'),
                                        (0x0e, struct.pack('<hh', 200, 100)), (0x40, bytes(6)),
                                        (0x0c, b'\x03'), (0x0c, b'\x04'), (0x0c, b'\x05')]:
                    with socket.create_connection(('127.0.0.1', sp), 2) as sock:
                        sock.sendall(siyi(1, opcode, payload))
                    ignored(before)
                client = Client(up)
                # Camera commands remain available; tracking cannot acquire
                # pointing control even if tracking is otherwise unavailable.
                assert client.request(0xd1, b'\x0a')[0] == 10  # zoom query
                assert len(client.request(0xa4)) == 1  # thermal palette query
                assert client.request(0xaa, struct.pack('<BHHHH', 1, 100, 100, 200, 200)) == b'\x06'
                ignored(before)
                for opcode, payload in [(0x9a, b'\x0a\x00'), (0x9b, b'\x01')]:
                    client.sock.sendall(unigcs_command(opcode, payload, dest=0x2e, link=0x11))
                    ignored(before)

                # Once paused the survey no longer holds the gimbal, so the
                # flight controller's (pilot's) targets and other pointing work.
                def follows(sender, pitch):
                    before = attitude()
                    sender.mav.gimbal_device_set_attitude_send(42, 154, 0,
                        [math.cos(pitch/2), 0, math.sin(pitch/2), 0], math.nan, math.nan, math.nan)
                    time.sleep(.5)
                    assert attitude() != before, 'paused survey ignored external pointing'

                command(M.MAV_CMD_IMAGE_STOP_CAPTURE, [])
                state(lambda s: s['state'] == 'survey paused: capture stopped')
                follows(link, .3)
                start()
                fc.mav.gimbal_manager_status_send(1000, 0, 154, 99, 190, 0, 0)
                state(lambda s: s['state'] == 'survey paused: gimbal takeover')
                follows(fc, -.3)
                # every other pointing path works again too
                # 2 (tracking disabled) rather than 6 (busy): past the survey check
                assert client.request(0xaa, struct.pack('<BHHHH', 1, 100, 100, 200, 200)) == b'\x02'
                before = attitude()
                with socket.create_connection(('127.0.0.1', sp), 2) as sock:
                    siyi_request(sock, 23, 0x0e, struct.pack('<hh', -300, -200))
                time.sleep(.5)
                assert attitude() != before, 'paused survey ignored SIYI pointing'
                link.mav.command_int_send(42, 154, M.MAV_FRAME_GLOBAL, M.MAV_CMD_DO_SET_ROI_NONE, 0, 0,
                                          0, 0, 0, 0, 0, 0, 0)
                ack = receive(link, 'COMMAND_ACK', lambda m: m.command == M.MAV_CMD_DO_SET_ROI_NONE)
                assert ack.result == M.MAV_RESULT_ACCEPTED, ack
                # ArduPilot denies a renewal after a pilot or GCS takeover
                start()
                fc.mav.command_ack_send(M.MAV_CMD_DO_GIMBAL_MANAGER_CONFIGURE,
                                        M.MAV_RESULT_DENIED, 0, 0, 42, 154)
                state(lambda s: s['state'] == 'survey paused: gimbal takeover')
                follows(fc, .3)
                # and refuses a claim while the mount is retracted
                command(M.MAV_CMD_SET_CAMERA_MODE, [0, 0])
                command(M.MAV_CMD_SET_CAMERA_MODE, [0, 2])
                fc.mav.command_ack_send(M.MAV_CMD_DO_GIMBAL_MANAGER_CONFIGURE,
                                        M.MAV_RESULT_TEMPORARILY_REJECTED, 0, 0, 42, 154)
                deadline = time.monotonic()+3
                while 'retracted' not in json.loads(status_path.read_text())['state']:
                    assert time.monotonic() < deadline, status_path.read_text()
                    time.sleep(.05)
                command(M.MAV_CMD_SET_CAMERA_MODE, [0, 0])
                state(lambda s: s['mode'] == 0)
                before = attitude()
                # Photo and Video are explicit exits: pointing works again.
                for mode in (0, 1):
                    command(M.MAV_CMD_SET_CAMERA_MODE, [0, mode])
                    state(lambda s: s['mode'] == mode)
                    with socket.create_connection(('127.0.0.1', sp), 2) as sock:
                        siyi_request(sock, 21, 0x0e, struct.pack('<hh', 200, 100))
                    time.sleep(.3)
                    assert attitude() != before
                    with socket.create_connection(('127.0.0.1', sp), 2) as sock:
                        siyi_request(sock, 22, 0x08, b'\x01')
                    time.sleep(.3)
                    start()
                    before = attitude()

                start()
                assert write(link, 'MAV_POS_TARGET', 0) == 0
                before = state(lambda s: s['state'].startswith('survey paused'))
                command(M.MAV_CMD_IMAGE_START_CAPTURE, [0, .1, 2], M.MAV_RESULT_UNSUPPORTED)
                after = state(lambda s: s['state'] == before['state'])
                assert after['captured'] == before['captured']
                assert not Path(str(cfg)+'.survey.json').exists()
                print('PASS survey stops latched FC rates on entry/grant, pauses waiting claims on manual pointing, ignores external pointing while owned, follows takeover, reports refused claims and restores Photo/Video control')
            except BaseException:
                log.flush()
                log.seek(0)
                print(log.read())
                raise
            finally:
                if client:
                    client.sock.close()
                if link:
                    link.close()
                if fc:
                    fc.close()
                if manual:
                    manual.close()
                for process in reversed(processes):
                    stop(process)
                status_path.unlink(missing_ok=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--camera', type=Path, default=ROOT/'build/sitl/camera-app')
    run(parser.parse_args().camera.resolve())
