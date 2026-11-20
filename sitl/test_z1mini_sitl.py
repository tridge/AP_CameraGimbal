#!/usr/bin/env python3
"""Z1-Mini MCU + MAVLink + public XFRobot + independent 4K recording test."""
import argparse
import configparser
import json
import math
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time
os.environ['MAVLINK20'] = '1'
from pymavlink import mavutil
from test_sitl import reserve_port, terminate, verify_rtsp_video, web_request, crc16


def packet(order, values=None, param=None):
    data = bytearray(70)
    data[:2] = b'\xa8\xe5'; data[4] = 2; data[69] = order
    if values:
        struct.pack_into('<hhh', data, 5, *[round(v * 100) for v in values])
        data[11] = 4
    if param is not None:
        data.append(param)
    struct.pack_into('<H', data, 2, len(data) + 2)
    return data + struct.pack('>H', crc16(data))


def receive(sock):
    data = bytearray()
    while len(data) < 73:
        part = sock.recv(73 - len(data))
        assert part, 'public protocol connection closed'
        data.extend(part)
    assert data[:2] == b'\x8a\x5e' and crc16(data) == 0
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True, help='isolated test build (its configuration is replaced)')
    parser.add_argument('--orientation', choices=('upright', 'inverted'), default='upright')
    parser.add_argument('--terrain', action='store_true')
    args = parser.parse_args()
    build = args.build.resolve(); runtime = build/'runtime'
    config = configparser.ConfigParser()
    config.read(runtime/'app/camera.ini')
    if not config.has_section('video'): config.add_section('video')
    config['video']['telemetry'] = 'apcg'  # This test checks MP4 recording compatibility.
    config['recording']['resolution'] = '3840x2160'
    config['recording']['autorecord'] = 'false'
    config['mount']['orientation'] = args.orientation
    with (runtime/'app/camera.ini').open('w') as f: config.write(f)
    camera_port, mav_port, web_port = [reserve_port(socket.SOCK_STREAM) for _ in range(3)]
    rtsp_port = reserve_port(socket.SOCK_STREAM, span=2)
    ready = runtime/'run/z1-gimbal.ready'
    ready.unlink(missing_ok=True)
    camera_ready = runtime/'run/camera-app.ready'
    camera_ready.unlink(missing_ok=True)
    processes = []; logs = []
    def start(argv, name, env=None):
        log = (runtime/'run'/name).open('w'); logs.append(log)
        p = subprocess.Popen(argv, env=env, stdout=log, stderr=subprocess.STDOUT)
        processes.append(p); return p
    try:
        start([sys.executable, str(Path(__file__).with_name('gimbal_sim.py')), '--backend', 'z1mini',
               '--orientation', args.orientation, '--port', '0', '--ready-file', str(ready)], 'gimbal-test.log')
        deadline = time.monotonic()+10
        while not ready.exists() and time.monotonic()<deadline: time.sleep(.05)
        assert ready.exists()
        env = dict(os.environ, CAMERA_APP_BACKEND='z1mini', CAMERA_APP_UART='udp://'+ready.read_text().strip(),
                   CAMERA_APP_PORT=str(camera_port), CAMERA_APP_RTSP_PORT=str(rtsp_port),
                   CAMERA_APP_MAVLINK_TCP_PORT=str(mav_port), CAMERA_APP_MAVLINK_UDP_PORT='0',
                   CAMERA_APP_CONFIG=str(runtime/'app/camera.ini'), CAMERA_APP_READY_PATH=str(camera_ready),
                   CAMERA_APP_RECORD_ROOT=str(runtime/'mnt/DCIM/record'), CAMERA_APP_CAPTURE_ROOT=str(runtime/'mnt/DCIM/capture'),
                   CAMERA_APP_SITL_VIDEO1=str(build/'main.h264'), CAMERA_APP_SITL_VIDEO2=str(build/'sub.h264'),
                   CAMERA_APP_SITL_PHOTO=str(build/'photo.jpg'), MT11_WEB_LIVE_PORT=str(rtsp_port+1))
        if args.terrain:
            env['CAMERA_APP_SITL_TERRAIN'] = str(Path(__file__).with_name('terrain_video.py').resolve())
            env['CAMERA_GIMBAL_SITL_PYTHON'] = sys.executable
        else:
            env.pop('CAMERA_APP_SITL_TERRAIN', None)
        camera = start([str(build/'camera-app')], 'camera-test.log', env)
        deadline = time.monotonic()+60
        while not camera_ready.exists() and time.monotonic()<deadline:
            assert camera.poll() is None, (runtime/'run/camera-test.log').read_text()
            time.sleep(.1)
        assert camera_ready.exists()
        link = mavutil.mavlink_connection(f'tcp:127.0.0.1:{mav_port}', source_system=42, source_component=1)
        # A fresh camera waits for a flight controller before selecting its
        # automatic system ID. Do not depend on a previously saved fixed ID.
        link.mav.heartbeat_send(mavutil.mavlink.MAV_TYPE_FIXED_WING,
                                mavutil.mavlink.MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, 4)
        link.mav.srcSystem, link.mav.srcComponent = 255, 190
        link.mav.command_long_send(0, 100, mavutil.mavlink.MAV_CMD_REQUEST_MESSAGE, 0,
                                   mavutil.mavlink.MAVLINK_MSG_ID_CAMERA_INFORMATION, 0,0,0,0,0,0)
        info = link.recv_match(type='CAMERA_INFORMATION', blocking=True, timeout=5)
        assert info is not None and b'Z1-Mini' in bytes(info.model_name), info
        with socket.create_connection(('127.0.0.1', camera_port), timeout=3) as vendor:
            # Fragmented and coalesced TCP frames both have to work.
            command = packet(0x10, (0, -35, 15))
            for part in (command[:3], command[3:19], command[19:]): vendor.sendall(part)
            assert receive(vendor)[70] == 0
            time.sleep(1.5)
            vendor.sendall(packet(0))
            reply = receive(vendor)
            assert abs(struct.unpack_from('<h', reply, 16)[0]/100-15) < .5
            assert abs(struct.unpack_from('<h', reply, 20)[0]/100+35) < .5
            vendor.sendall(packet(0x7f))
            assert receive(vendor)[70] == 1, 'unsupported order must fail'
            vendor.sendall(packet(0x21, param=1))
            assert receive(vendor)[70] == 0
            verify_rtsp_video(rtsp_port, 'video1', (1920,1080))
            verify_rtsp_video(rtsp_port, 'video2', (1920,1080))
            vendor.sendall(packet(0) * 4)
            for _ in range(4): assert receive(vendor)[70] == 0
            time.sleep(1)
            vendor.sendall(packet(0x21, param=0))
            assert receive(vendor)[70] == 0
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as vendor_udp:
            vendor_udp.bind(('127.0.0.1', 2338))
            vendor_udp.settimeout(.3)
            corrupt = packet(0); corrupt[-1] ^= 1
            vendor_udp.sendto(corrupt, ('127.0.0.1', camera_port))
            try:
                vendor_udp.recv(4096)
                raise AssertionError('bad-CRC vendor request accepted')
            except socket.timeout:
                pass
            vendor_udp.sendto(packet(0), ('127.0.0.1', camera_port))
            reply = vendor_udp.recv(4096)
            assert len(reply) == 73 and reply[:2] == b'\x8a\x5e' and crc16(reply) == 0
            assert abs(struct.unpack_from('<h', reply, 16)[0]/100-15) < .5
        paths = list((runtime/'mnt/DCIM/record').glob('SITL_0_*.mp4'))
        assert paths
        newest = max(paths, key=lambda p:p.stat().st_mtime)
        info = json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-of','json',str(newest)]))['streams'][0]
        assert (info['width'],info['height']) == (3840,2160), info
        web = start([str(build/'z1mini-web'), '-p', str(web_port)], 'web-test.log', env)
        deadline = time.monotonic() + 10
        while True:
            try:
                with web_request(web_port,'/live/attitude.json') as response:
                    attitude = json.load(response)
                break
            except OSError:
                if web.poll() is not None or time.monotonic() >= deadline:
                    raise
                time.sleep(.05)
        assert abs(attitude['yaw_deg'] - 15) < .5 and abs(attitude['pitch_deg'] + 35) < .5, attitude
        with web_request(web_port,'/parameters') as response:
            page = response.read()
        assert b'3840x2160' in page
        link.close()
    finally:
        for p in reversed(processes): terminate(p)
        for log in logs: log.close()
    print('PASS Z1-Mini', args.orientation, 'terrain' if args.terrain else 'simple', 'MCU, MAVLink identity, XFRobot angles/recording, TCP framing, 1080p live + 4K recording, web attitude')


if __name__ == '__main__': main()
