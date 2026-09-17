#!/usr/bin/env python3
"""Exercise camera XML download and PARAM_EXT controls with real MAVProxy/SITL.

Build the four SITL targets first. Uses isolated ports and runtime directories.
"""
import argparse
import binascii
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

os.environ.setdefault('MAVLINK20', '1')
import pexpect
from pymavlink import mavutil
from MAVProxy.modules.mavproxy_camera.definition import CameraDefinition, decode_value
from test_mavlink_parameters import port, stop, wait_ready, connect, receive, drain

M = mavutil.mavlink
CAMERA = M.MAV_COMP_ID_CAMERA
ROOT = Path(__file__).resolve().parents[1]


def info(link):
    link.mav.command_long_send(42, CAMERA, M.MAV_CMD_REQUEST_CAMERA_INFORMATION, 0,
                               1, 0, 0, 0, 0, 0, 0)
    return receive(link, 'CAMERA_INFORMATION')


def download(link):
    sequence = 0
    def ftp(opcode, session=0, data=b'', size=None, offset=0):
        nonlocal sequence
        sequence = (sequence + 2) & 65535
        payload = struct.pack('<HBBBBBBI', sequence, session, opcode,
                              len(data) if size is None else size, 0, 0, 0, offset) + data
        link.mav.file_transfer_protocol_send(0, 42, CAMERA, payload.ljust(251, b'\0'))
        response = receive(link, 'FILE_TRANSFER_PROTOCOL', lambda m:
                           m.target_system == 255 and m.target_component == 190 and
                           struct.unpack('<H', bytes(m.payload[:2]))[0] == (sequence + 1) & 65535)
        assert response.get_srcComponent() == CAMERA
        raw = bytes(response.payload)
        header = struct.unpack('<HBBBBBBI', raw[:12])
        assert header[2] == 128, (header, raw[12])
        return header, raw[12:12 + header[3]]
    header, data = ftp(4, data=b'/camera.xml')
    size = struct.unpack('<I', data)[0]
    session = header[1]
    xml = b''
    while len(xml) < size:
        _, data = ftp(15, session, size=239, offset=len(xml))
        xml += data
    ftp(1, session)
    return xml


def read(link, name, index=-1):
    link.mav.param_ext_request_read_send(42, CAMERA, name.encode(), index)
    response = receive(link, 'PARAM_EXT_VALUE', lambda m: index == m.param_index if index >= 0 else m.param_id == name)
    return decode_value(response)


def write(link, definition, name, value, result=M.PARAM_ACK_ACCEPTED, wire_type=None):
    parameter = definition.parameters[name]
    link.mav.param_ext_set_send(42, CAMERA, name.encode(), struct.pack("<" + parameter.fmt, value).ljust(128, b"\0"),
                                parameter.wire_type if wire_type is None else wire_type)
    ack = receive(link, 'PARAM_EXT_ACK', lambda m: m.param_id == name and m.param_result != M.PARAM_ACK_IN_PROGRESS, timeout=30)
    assert ack.param_result == result, ack
    return decode_value(ack)


def protocol_checks(link, definition, target):
    link.mav.param_ext_request_list_send(42, CAMERA)
    values = {}
    while len(values) < len(definition.parameters):
        message = receive(link, 'PARAM_EXT_VALUE')
        assert message.param_count == len(definition.parameters)
        assert message.get_srcComponent() == CAMERA and message.get_srcSystem() == 42
        values[message.param_id] = decode_value(message)
    assert set(values) == set(definition.parameters)
    names = list(definition.parameters)
    assert read(link, '', len(names) - 1) == values[names[-1]]
    assert read(link, 'CAM_MODE') == (1 if target == 'z1mini' else 0)
    if target != 'z1mini':
        write(link, definition, 'CAM_MODE', 1)
        assert read(link, 'CAM_MODE') == 1
        link.mav.command_long_send(42, CAMERA, M.MAV_CMD_SET_CAMERA_MODE, 0, 0, 0, 0, 0, 0, 0, 0)
        receive(link, 'COMMAND_ACK', lambda m: m.command == M.MAV_CMD_SET_CAMERA_MODE)
        assert read(link, 'CAM_MODE') == 0
    write(link, definition, 'REC_AUTOSTART', 2)
    assert read(link, 'REC_AUTOSTART') == 2
    link.mav.param_request_read_send(42, CAMERA, b'REC_AUTOSTART', -1)
    assert receive(link, 'PARAM_VALUE', lambda m: m.param_id == 'REC_AUTOSTART').param_value == 2
    write(link, definition, 'REC_AUTOSTART', 3, M.PARAM_ACK_VALUE_UNSUPPORTED)
    write(link, definition, 'REC_AUTOSTART', 1, M.PARAM_ACK_VALUE_UNSUPPORTED, M.MAV_PARAM_EXT_TYPE_REAL32)
    assert read(link, 'REC_AUTOSTART') == 2
    write(link, definition, 'REC_AUTOSTART', 0)
    if 'IMG_EXPOSURE' in definition.parameters:
        write(link, definition, 'IMG_EXPOSURE', -4)
        assert read(link, 'IMG_EXPOSURE') == -4
        write(link, definition, 'IMG_BRIGHTNESS', 101, M.PARAM_ACK_VALUE_UNSUPPORTED)
    if 'CAM_ZOOM' in definition.parameters:
        write(link, definition, 'CAM_ZOOM', 25)
        assert abs(read(link, 'CAM_ZOOM') - 25) < 0.001
        write(link, definition, 'CAM_ZOOM', float('nan'), M.PARAM_ACK_VALUE_UNSUPPORTED)
    if target == 'mt11':
        write(link, definition, 'CAM_PALETTE', 10)
        assert read(link, 'CAM_PALETTE') == 10
        write(link, definition, 'CAM_PALETTE', 1, M.PARAM_ACK_VALUE_UNSUPPORTED)
        write(link, definition, 'CAM_SOURCE', 1)
        assert read(link, 'CAM_SOURCE') == 1
        write(link, definition, 'CAM_THERM_GAIN', 0)
        assert read(link, 'CAM_THERM_GAIN') == 0
        write(link, definition, 'CAM_AUTOFOCUS', 1)
        assert read(link, 'CAM_AUTOFOCUS') == 0
    drain(link)
    link.mav.param_ext_request_read_send(43, CAMERA, b'CAM_MODE', -1)
    assert link.recv_match(type='PARAM_EXT_VALUE', blocking=True, timeout=0.25) is None
    link.mav.param_ext_request_read_send(42, 154, b'CAM_MODE', -1)
    assert link.recv_match(type='PARAM_EXT_VALUE', blocking=True, timeout=0.25) is None
    link.mav.param_ext_set_send(42, CAMERA, b'UNKNOWN_PARAM', bytes(128), M.MAV_PARAM_EXT_TYPE_INT32)
    assert receive(link, 'PARAM_EXT_ACK').param_result == M.PARAM_ACK_VALUE_UNSUPPORTED


def live_config_checks(link, definition, target, directory, camera, rtsp_port):
    def heartbeat(armed, system=42, component=1):
        old = link.mav.srcSystem, link.mav.srcComponent
        link.mav.srcSystem, link.mav.srcComponent = system, component
        link.mav.heartbeat_send(M.MAV_TYPE_FIXED_WING, M.MAV_AUTOPILOT_ARDUPILOTMEGA,
                                M.MAV_MODE_FLAG_SAFETY_ARMED if armed else 0, 0, M.MAV_STATE_ACTIVE)
        link.mav.srcSystem, link.mav.srcComponent = old

    def recording(expected):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            link.mav.command_long_send(42, CAMERA, M.MAV_CMD_REQUEST_CAMERA_CAPTURE_STATUS,
                                       0, 1, 0, 0, 0, 0, 0, 0)
            status = receive(link, 'CAMERA_CAPTURE_STATUS')
            if bool(status.video_status) == expected:
                return
        raise AssertionError(('recording', expected, status))

    heartbeat(True)  # cache armed state while automatic recording is disabled
    write(link, definition, 'REC_AUTOSTART', 2)
    recording(True)
    old_profile = read(link, 'REC_RESOLUTION')
    new_profile = 1 if old_profile != 1 else (2 if target == 'z1mini' else 0)
    before = (directory / 'camera.ini').read_bytes()
    write(link, definition, 'REC_RESOLUTION', new_profile, M.PARAM_ACK_FAILED)
    assert read(link, 'REC_RESOLUTION') == old_profile
    assert (directory / 'camera.ini').read_bytes() == before
    recording(True)
    heartbeat(False, component=190)
    heartbeat(False, system=43)
    recording(True)
    heartbeat(False)
    recording(False)
    write(link, definition, 'REC_AUTOSTART', 0)
    write(link, definition, 'REC_RESOLUTION', new_profile)
    assert read(link, 'REC_RESOLUTION') == new_profile
    write(link, definition, 'REC_AUTOSTART', 1)
    recording(True)
    time.sleep(1.5)
    write(link, definition, 'REC_AUTOSTART', 0)
    recording(False)
    videos = sorted((directory / 'record').glob('*.mp4'), key=lambda p: p.stat().st_mtime)
    rgb = [p for p in videos if '_T' not in p.name][-1]
    probe = subprocess.check_output(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                                    '-show_entries', 'stream=width,height', '-of', 'csv=p=0', str(rgb)], text=True).strip()
    assert probe == {0: '1280,720', 1: '1920,1080', 2: '3840,2160'}[new_profile], (rgb, probe)
    write(link, definition, 'REC_RESOLUTION', old_profile)
    if target != 'z1mini':
        if target == 'mt11':
            write(link, definition, 'CAM_SOURCE', 0)
        old_resolution = read(link, 'VIDEO_MAIN_RES')
        new_resolution = 1 if old_resolution != 1 else 0
        write(link, definition, 'VIDEO_MAIN_RES', new_resolution)
        write(link, definition, 'VIDEO_MAIN_CODEC', 1)
        probe = subprocess.check_output([
            'ffprobe', '-v', 'error', '-rtsp_transport', 'tcp', '-analyzeduration', '1000000',
            '-probesize', '1000000', '-select_streams', 'v:0', '-show_entries',
            'stream=codec_name,width,height', '-of', 'csv=p=0',
            f'rtsp://127.0.0.1:{rtsp_port}/video1'], text=True, timeout=20).strip()
        assert probe == ('hevc,1920,1080' if new_resolution == 1 else 'hevc,1280,720'), probe
        write(link, definition, 'VIDEO_MAIN_CODEC', 0)
        write(link, definition, 'VIDEO_MAIN_RES', old_resolution)
        if 'CAM_ZOOM' in definition.parameters:
            assert abs(read(link, 'CAM_ZOOM') - 25) < 0.001
    # A persistence failure must roll back an already applied live policy.
    config = directory / 'camera.ini'
    backup = directory / 'camera.saved'
    config.rename(backup)
    config.mkdir()
    try:
        write(link, definition, 'REC_AUTOSTART', 1, M.PARAM_ACK_FAILED)
        assert read(link, 'REC_AUTOSTART') == 0
        recording(False)
        write(link, definition, 'REC_RESOLUTION', new_profile, M.PARAM_ACK_FAILED)
        assert read(link, 'REC_RESOLUTION') == old_profile
    finally:
        config.rmdir()
        backup.rename(config)
    assert camera.poll() is None


def mavproxy_checks(endpoint, directory, link, definition):
    env = dict(os.environ, MPLCONFIGDIR=str(directory / 'matplotlib'))
    with (directory / 'mavproxy-console.log').open('w') as transcript:
        cli = pexpect.spawn(shutil.which('mavproxy.py'), [
            '--master', endpoint, '--target-system', '42', '--target-component', str(CAMERA),
            '--source-system', '255', '--mav20', '--nowait', '--default-modules', 'ftp,camera',
            '--cmd', f'camera select 42:{CAMERA}', '--logfile', str(directory / 'mavproxy.tlog'),
        ], cwd=str(directory), env=env, encoding='utf-8', timeout=30)
        cli.logfile_read = transcript
        try:
            cli.expect(r'loaded %u settings' % len(definition.parameters))
            time.sleep(2)
            for with_time in (1, 0):
                cli.sendline('ftp set list_time %u' % with_time)
                cli.sendline('ftp list')
                cli.expect(r'camera\.xml\s+\d+')
                cli.expect(r'Total size')
            cli.sendline('camera params')
            cli.expect(r'REC_AUTOSTART\s+0\s+')
            last = list(definition.parameters)[-1]
            cli.expect(re.escape(last) + r'\s+\d+\s+')
            cli.sendline('camera param REC_AUTOSTART 2')
            deadline = time.monotonic() + 5
            while read(link, 'REC_AUTOSTART') != 2:
                assert time.monotonic() < deadline
                time.sleep(0.1)
            cli.sendline('camera params')
            cli.expect(r'REC_AUTOSTART\s+2\s+')
            if 'CAM_PALETTE' in definition.parameters:
                cli.sendline('camera param CAM_PALETTE 3')
                deadline = time.monotonic() + 5
                while read(link, 'CAM_PALETTE') != 3:
                    assert time.monotonic() < deadline
                    time.sleep(0.1)
                cli.sendline('camera param CAM_AUTOFOCUS 1')
                time.sleep(0.5)
                cli.sendline('camera params')
                cli.expect(r'CAM_AUTOFOCUS\s+0\s+')
            # Re-download exercises session close/reopen and refresh-all.
            cli.sendline('camera definition')
            cli.expect(r'loaded %u settings' % len(definition.parameters))
        finally:
            cli.close(force=True)


def test_target(target, output):
    build = ROOT / 'build' / ('sitl' if target == 'mt11' else target + '-sitl')
    directory = Path(tempfile.mkdtemp(prefix=target + '-', dir=output))
    config = directory / 'camera.ini'
    template = ROOT / ('camera_app/camera.ini' if target == 'mt11' else
                        'sitl/zr10.ini' if target == 'zr10' else 'packaging/' + target + '/camera.ini')
    config.write_text(re.sub(r'(?m)^system_id\s*=.*$', f'system_id = 42\ncamera_component_id = {CAMERA}', re.sub(r'(?m)^camera_component_id\s*=.*\n?', '', template.read_text())))
    tcp_port, gimbal_port = port(), port()
    ready, gimbal_ready = directory / 'camera.ready', directory / 'gimbal.ready'
    env = dict(os.environ, CAMERA_APP_BACKEND=target, CAMERA_APP_CONFIG=str(config),
               CAMERA_APP_UART=f'udp://127.0.0.1:{gimbal_port}',
               CAMERA_APP_PORT=str(port()), CAMERA_APP_RTSP_PORT=str(port()),
               CAMERA_APP_MAVLINK_TCP_PORT=str(tcp_port), CAMERA_APP_MAVLINK_UDP_PORT=str(port()),
               CAMERA_APP_READY_PATH=str(ready), CAMERA_APP_RECORD_ROOT=str(directory / 'record'),
               CAMERA_APP_RECORD_STATE=str(directory / 'record.state'),
               CAMERA_APP_CAPTURE_ROOT=str(directory / 'capture'),
               CAMERA_APP_SITL_VIDEO1=str(build / ('rgb.h264' if target == 'mt11' else 'main.h264')),
               CAMERA_APP_SITL_VIDEO2=str(build / ('thermal.h264' if target == 'mt11' else 'sub.h264')),
               CAMERA_APP_SITL_PHOTO=str(build / 'photo.jpg'))
    camera = gimbal = link = None
    try:
        with (directory / 'camera.log').open('w') as log, (directory / 'gimbal.log').open('w') as glog:
            gimbal = subprocess.Popen([sys.executable, str(ROOT / 'sitl/gimbal_sim.py'), '--backend', target,
                                       '--port', str(gimbal_port), '--ready-file', str(gimbal_ready)],
                                      stdout=glog, stderr=subprocess.STDOUT)
            wait_ready(gimbal_ready, gimbal)
            camera = subprocess.Popen([str(build / 'camera-app'), '--backend', target], env=env,
                                      stdout=log, stderr=subprocess.STDOUT)
            wait_ready(ready, camera)
            endpoint = f'tcp:127.0.0.1:{tcp_port}'
            link = connect(endpoint)
            if CAMERA != 100:
                for send in (
                    lambda: link.mav.command_long_send(42, 100, M.MAV_CMD_REQUEST_CAMERA_INFORMATION, 0, 1, 0, 0, 0, 0, 0, 0),
                    lambda: link.mav.param_request_read_send(42, 100, b'MAV_CAM_COMP_ID', -1),
                    lambda: link.mav.param_ext_request_list_send(42, 100),
                    lambda: link.mav.file_transfer_protocol_send(0, 42, 100, bytes(251)),
                ):
                    drain(link)
                    send()
                    assert link.recv_match(type=['CAMERA_INFORMATION', 'COMMAND_ACK', 'PARAM_VALUE', 'PARAM_EXT_VALUE', 'FILE_TRANSFER_PROTOCOL'], blocking=True, timeout=.3) is None
            information = info(link)
            assert information.get_srcComponent() == CAMERA
            assert information.cam_definition_uri == f'mftp://[;comp={CAMERA}]/camera.xml'
            xml = download(link)
            assert information.cam_definition_version == (binascii.crc_hqx(xml, 0xffff) or 0xffff)
            assert xml == (ROOT / 'build/camera-definitions' / (target + '.xml')).read_bytes()
            (directory / 'camera.xml').write_bytes(xml)
            definition = CameraDefinition(xml)
            protocol_checks(link, definition, target)
            live_config_checks(link, definition, target, directory, camera, int(env['CAMERA_APP_RTSP_PORT']))
            mavproxy_checks(endpoint, directory, link, definition)
            print(f'PASS {target}: XML download, fetch-all/read/set, live controls and MAVProxy ({directory})', flush=True)
    finally:
        if link: link.close()
        stop(camera)
        stop(gimbal)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--targets', nargs='+', choices=['mt11', 'a8', 'zr10', 'z1mini'],
                        default=['mt11', 'a8', 'zr10', 'z1mini'])
    parser.add_argument('--component', type=int, choices=range(100, 106), default=100)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/camera-definition-test')
    args = parser.parse_args()
    CAMERA = args.component
    args.output.mkdir(parents=True, exist_ok=True)
    for target in args.targets:
        test_target(target, args.output.resolve())
