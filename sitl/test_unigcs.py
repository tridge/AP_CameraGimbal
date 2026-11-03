#!/usr/bin/env python3
"""Isolated SIYI UniGCS wire-protocol integration test (no physical camera)."""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
from unittest.mock import patch
from target_properties import TARGETS, transform

from gimbal_sim import Gimbal, mt11_private_frame, parse_mt11_private, a8_private_frame, parse_a8_private
from test_sitl import reserve_port, terminate, read_rtsp_sdp, verify_rtsp_sdp, verify_rtsp_video, verify_rtsp_rtcp, request as public_request


CAMERA_ADDRESS = 0x34


def camera_version(product_id, version=None):
    """Expected build identity; override for binaries from another build tree."""
    if version is None:
        header = Path(__file__).resolve().parents[1]/'camera_app/build/binlog_version.h'
        value = re.search(r'^#define CA_FIRMWARE_VERSION (.+)$', header.read_text(), re.M)
        assert value, f'No firmware identity in {header}'
        version = json.loads(value[1])
    match = re.fullmatch(r'v?([0-9]+)\.([0-9]+)(?:\.([0-9]+))?', version)
    numbers = [int(part or 0) for part in match.groups()] if match else [0, 0, 0]
    if any(part > 255 for part in numbers):
        numbers = [0, 0, 0]
    return bytes((*reversed(numbers), product_id))


def long_crc(data):
    crc=0
    for byte in data:
        crc ^= byte<<24
        for _ in range(8):
            crc=((crc<<1) ^ (0x04c11db7 if crc&0x80000000 else 0))&0xffffffff
    return crc


def long_command(cmd, payload=b'', control=1):
    header=b'\x55\x66\xaa\xbb'+struct.pack('<BIHB',control,len(payload),123,cmd)
    frame=header+struct.pack('<I',long_crc(header))+payload
    return frame+struct.pack('<I',long_crc(frame))


class LongClient:
    def __init__(self, port):
        self.sock=socket.create_connection(('127.0.0.1',port),2)
        self.buf=b''
        self.received=[]

    def reply(self, cmd):
        deadline=time.monotonic()+2
        while time.monotonic()<deadline:
            if len(self.buf)>=16:
                assert self.buf[:4]==b'\x55\x66\xaa\xbb'
                assert long_crc(self.buf[:12])==struct.unpack_from('<I',self.buf,12)[0]
                size=struct.unpack_from('<I',self.buf,5)[0]+20
                assert size<=4116
                if len(self.buf)>=size:
                    raw,self.buf=self.buf[:size],self.buf[size:]
                    assert raw[4]==2 and long_crc(raw[:-4])==struct.unpack_from('<I',raw,size-4)[0]
                    self.received.append(raw[11])
                    if raw[11]==cmd:return raw[16:-4]
                    continue
            self.sock.settimeout(max(.001,deadline-time.monotonic()))
            data=self.sock.recv(8192)
            assert data,'old-format session closed'
            self.buf+=data
        raise AssertionError(f'no old-format reply to {cmd:02x}')

    def request(self, cmd, payload=b''):
        self.sock.sendall(long_command(cmd,payload))
        return self.reply(cmd)

def command(cmd, payload=b'', dest=None, link=0x16, source=0xd0):
    return mt11_private_frame(9, 123, cmd, payload, source=source, destination=CAMERA_ADDRESS if dest is None else dest, link=link)


class Client:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), 2)
        self.buf = b''

    def reply(self, cmd, source=None, timeout=2):
        source = CAMERA_ADDRESS if source is None else source
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if len(self.buf) >= 6:
                n = struct.unpack_from('<H', self.buf, 3)[0] + 14
                if len(self.buf) >= n:
                    raw, self.buf = self.buf[:n], self.buf[n:]
                    control, _, src, dst, _, op, payload = parse_mt11_private(raw)
                    assert control in (8, 10) and dst == 0xd0
                    if op == cmd and src == source:
                        return payload
                    continue
            self.sock.settimeout(max(.001, end-time.monotonic()))
            data = self.sock.recv(8192)
            assert data, 'private connection unexpectedly closed'
            self.buf += data
        raise TimeoutError(f'no reply {source:02x}/{cmd:02x}')

    def request(self, cmd, payload=b'', dest=None, link=0x16, response=None):
        self.sock.sendall(command(cmd, payload, dest, link))
        return self.reply(cmd if response is None else response, dest)


def verify_a8_native_gimbal():
    for orientation in (1, 2):
        with patch('gimbal_sim.time.monotonic', return_value=100) as clock:
            gimbal = Gimbal(orientation, 'a8')
            def send(cmd, payload, control=9, source=0x2c, link=0x6b):
                return gimbal.handle_private(a8_private_frame(
                    control, 123, cmd, payload, source=source, destination=0x2e, link=link))
            for preset, expected in ((1, (0, 0)), (2, (0, -90)),
                                     (3, (0, -20)), (4, (40, -90))):
                gimbal.yaw, gimbal.pitch = 40, -20
                gimbal.target = None
                assert send(0x9b, bytes((preset,)), source=0xd0, link=0x11) is None
                assert gimbal.target is None, 'A8 must ignore untranslated MT11 presets'
                assert send(0x07, bytes((preset,))) is None
                if orientation == 2 and preset != 3:
                    assert gimbal.target is None, 'inverted A8 ignores native pitch presets'
                    continue
                for _ in range(100):
                    clock.return_value += .05
                    gimbal.update()
                assert abs(gimbal.yaw-expected[0]) < .1 and abs(gimbal.pitch-expected[1]) < .1
            send(0x06, b'\x28\x14')
            assert gimbal.target is None and gimbal.commanded_rates != (0, 0)
            send(0x06, b'\x00\x00', control=8)
            assert gimbal.commanded_rates == (0, 0)
            for cmd, payload, control in ((0x07,b'\x00',9), (0x07,b'\x05',9),
                                           (0x06,b'\x7f\x00',9), (0x06,b'\x01',9),
                                           (0x07,b'\x01',10)):
                assert send(cmd,payload,control) is None
                assert gimbal.target is None and gimbal.commanded_rates == (0, 0)


def verify_native_gimbal(backend):
    if backend == 'a8':
        verify_a8_native_gimbal()
        return
    build = mt11_private_frame if backend == "mt11" else a8_private_frame
    parse = parse_mt11_private if backend == "mt11" else parse_a8_private
    for orientation in (1, 2):
        with patch('gimbal_sim.time.monotonic', return_value=100) as clock:
            gimbal = Gimbal(orientation, backend)
            def send(cmd, payload, control=9):
                return gimbal.handle_private(build(
                    control, 123, cmd, payload, source=0xd0, destination=0x2e, link=0x11))
            for preset, expected in ((1, (0, 0)), (2, (0, -90)),
                                     (3, (0, -20)), (4, (40, -90))):
                gimbal.yaw, gimbal.pitch = 40, -20
                gimbal.target = None
                response = parse(send(0x9b, bytes((preset,))))
                assert response[0] == 10 and response[2:6] == (0x2e, 0xd0, 0x11, 0x9b)
                assert response[-1] == b'\x01'
                for _ in range(100):
                    clock.return_value += .05
                    gimbal.update()
                assert abs(gimbal.yaw-expected[0]) < .1 and abs(gimbal.pitch-expected[1]) < .1
            # Rate commands cancel a preset, and zero rates stop the motion.
            send(0x9b, b'\x01')
            assert parse(send(0x9a, b'\x28\x14'))[-1] == b'\x01'
            assert gimbal.target is None
            before = (gimbal.yaw, gimbal.pitch)
            clock.return_value += .2
            gimbal.update()
            assert abs(gimbal.yaw-before[0]) > .1 and abs(gimbal.pitch-before[1]) > .1
            assert send(0x9a, b'\x00\x00', control=8) is None
            # Allow the simulated motor response to settle.
            for _ in range(100):
                clock.return_value += .05
                gimbal.update()
            stopped = (gimbal.yaw, gimbal.pitch)
            clock.return_value += .2
            gimbal.update()
            assert abs(gimbal.yaw-stopped[0]) < .01 and abs(gimbal.pitch-stopped[1]) < .01
            assert send(0x9a, b'\x7f\x00') is None
            assert send(0x9a, b'\x01') is None
            assert send(0x9b, b'\x01\x00') is None
            assert send(0x9a, b'\x28\x14', control=10) is None
            assert parse(send(0x9b, b'\x00'))[-1] == b'\x00'
            assert gimbal.target is None and gimbal.commanded_rates == (0, 0)


def verify_late_interface(port, product_id):
    """Check the socket joins a NIC created after startup, and after replacement."""
    address = '192.0.2.1'
    for _ in range(2):
        subprocess.run(['ip', 'link', 'add', 'ugcs-test', 'type', 'dummy'], check=True)
        try:
            subprocess.run(['ip', 'address', 'add', address+'/24', 'dev', 'ugcs-test'], check=True)
            subprocess.run(['ip', 'link', 'set', 'ugcs-test', 'multicast', 'on', 'up'], check=True)
            deadline = time.monotonic()+8
            while True:
                # Linux automatically joins all-hosts once. Require a second
                # membership from our socket, not just the kernel's default.
                interface = None
                joined = False
                for line in Path('/proc/net/igmp').read_text().splitlines()[1:]:
                    fields = line.split()
                    if not line[0].isspace():
                        interface = fields[1]
                    elif interface == 'ugcs-test' and fields[0] == '010000E0':
                        joined = int(fields[1]) >= 2
                if joined:
                    break
                assert time.monotonic() < deadline, 'no multicast membership on late interface: '+Path('/proc/net/igmp').read_text()
                time.sleep(.05)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
                udp.bind((address, 0))
                udp.settimeout(2)
                udp.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(address))
                udp.sendto(bytes.fromhex('aa09030000f90000d034f00199a4'), ('224.0.0.1', port))
                raw, peer = udp.recvfrom(256)
                payload = parse_mt11_private(raw)[-1]
                assert peer[0] == address
                assert payload[:5] == bytes((product_id,))+socket.inet_aton(address)
                assert payload[13:19] != bytes(6)
        finally:
            subprocess.run(['ip', 'link', 'delete', 'ugcs-test'], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('camera', type=Path)
    parser.add_argument('--backend', choices=('mt11','a8','zr10'), default='mt11')
    parser.add_argument('--orientation', choices=('upright','inverted'), default='upright')
    parser.add_argument('--video-codec', choices=('h264', 'h265'))
    parser.add_argument('--firmware-version', help='expected MT11_VERSION; defaults to the generated build header')
    parser.add_argument('--late-interface', action='store_true',
                        help='test delayed NIC discovery; run with sudo unshare -n')
    args = parser.parse_args()
    if args.late_interface:
        assert os.geteuid() == 0 and {n for _, n in socket.if_nameindex()} == {'lo'}, 'Use sudo unshare -n'
        subprocess.run(['ip', 'link', 'set', 'lo', 'up'], check=True)
    product_id = {'mt11':0x89, 'a8':0x72, 'zr10':0x6b}[args.backend]
    version_reply = camera_version(product_id, args.firmware_version)
    thermal = args.backend == 'mt11'
    verify_native_gimbal(args.backend)
    def read_pose(public):
        yaw,pitch = struct.unpack_from('<hh',public_request(public,22,0x0d))
        _,pitch,yaw = transform(TARGETS[args.backend], 'feedback', args.orientation == 'inverted',
                                (0,pitch/10,yaw/10))
        return yaw*10,pitch*10
    camera = args.camera.resolve()
    script = Path(__file__).with_name('gimbal_sim.py')
    with tempfile.TemporaryDirectory(prefix='unigcs-test-') as directory:
        root = Path(directory)
        ready = root/'ready.json'
        config = root/'camera.ini'
        photo = root/'photo.jpg'
        subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','color=size=64x48',
                        '-frames:v','1','-threads','1',str(photo)], check=True)
        config.write_text('[recording]\nautorecord=false\nresolution=1280x720\n'
                          '[logging]\ndisarmed=false\n'
                          f'[stream.main]\ncodec={args.video_codec or "h264"}\n')
        ports = {name: reserve_port(kind) for name, kind in [
            ('gimbal', socket.SOCK_DGRAM), ('private', socket.SOCK_STREAM),
            ('discovery', socket.SOCK_DGRAM), ('public', socket.SOCK_STREAM)]}
        env = dict(os.environ, CAMERA_APP_BACKEND=args.backend, CAMERA_APP_UART=f"udp://127.0.0.1:{ports['gimbal']}",
                   CAMERA_APP_PORT=str(ports['public']),
                   CAMERA_APP_RTSP_PORT=str(reserve_port(socket.SOCK_STREAM, span=3)), CAMERA_APP_UNIGCS_PORT=str(ports['private']),
                   CAMERA_APP_DISCOVERY_PORT=str(ports['discovery']), CAMERA_APP_CONFIG=str(config),
                   CAMERA_APP_READY_PATH=str(ready), CAMERA_APP_RECORD_ROOT=str(root/'record'),
                   CAMERA_APP_SITL_PHOTO=str(photo),
                   CAMERA_APP_CAPTURE_ROOT=str(root/'capture'), CAMERA_APP_LOG_ROOT=str(root/'logs'),
                   CAMERA_APP_RECORD_STATE=str(root/'record.state'), CAMERA_APP_MAVLINK_TCP_PORT='0',
                   CAMERA_APP_MAVLINK_UDP_PORT='0')
        for key in ('CAMERA_APP_SITL_VIDEO1','CAMERA_APP_SITL_VIDEO2','CAMERA_APP_SITL_TERRAIN','CAMERA_APP_SITL_RENDERER'):
            env.pop(key, None)
        if args.video_codec:
            video = root/'fixture.h264'
            subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i',
                'testsrc2=size=320x180:rate=5','-t','1','-c:v','libx264',
                '-threads','1','-preset','ultrafast','-f','h264',str(video)], check=True)
            env['CAMERA_APP_SITL_VIDEO1'] = str(video)
            # Hold the first encoded frame until a client has completed PLAY.
            # This makes the early-DESCRIBE regression deterministic.
            release_encoder = root/'release-encoder'
            renderer = root/'gated-renderer.py'
            renderer.write_text(
                "import sys, time\nfrom pathlib import Path\n"
                f"sys.path.insert(0, {str(script.parent)!r})\n"
                "import terrain_video\n"
                "original = terrain_video.Encoder.encode\n"
                "def gated(self, *args, **kwargs):\n"
                "    deadline = time.monotonic() + 10\n"
                f"    while not Path({str(release_encoder)!r}).exists():\n"
                "        assert time.monotonic() < deadline, 'encoder not released'\n"
                "        time.sleep(.01)\n"
                "    return original(self, *args, **kwargs)\n"
                "terrain_video.Encoder.encode = gated\nterrain_video.main()\n")
            env['CAMERA_APP_SITL_RENDERER'] = str(renderer)
        processes = []
        with (root/'run.log').open('w') as log:
            try:
                gimbal_ready = root/'gimbal-ready'
                processes.append(subprocess.Popen([sys.executable, str(script), '--backend',args.backend,
                    '--orientation',args.orientation,'--port',str(ports['gimbal']),'--ready-file',str(gimbal_ready)], stdout=log, stderr=log))
                deadline = time.monotonic()+5
                while not gimbal_ready.exists() and time.monotonic()<deadline:
                    time.sleep(.02)
                assert gimbal_ready.exists()
                processes.append(subprocess.Popen([str(camera)], env=env, stdout=log, stderr=log))
                deadline = time.monotonic()+8
                while not ready.exists() and time.monotonic()<deadline:
                    assert processes[-1].poll() is None
                    time.sleep(.02)
                assert ready.exists(), 'camera not ready'
                if args.video_codec:
                    from test_video_telemetry import RTSP
                    rtsp = int(env['CAMERA_APP_RTSP_PORT'])
                    sdp = read_rtsp_sdp(rtsp, 'video1')
                    assert f'a=rtpmap:96 {args.video_codec.upper()}/90000' in sdp, sdp
                    assert 'sprop-' not in sdp, sdp
                    early = RTSP(rtsp, 'video1')
                    try:
                        release_encoder.touch()
                        frame, _ = next(early.frames(1, args.video_codec))
                        kinds = {(nal[0] & 31) if args.video_codec == 'h264' else ((nal[0] >> 1) & 63)
                                 for nal in frame}
                        assert ({7, 8} if args.video_codec == 'h264' else {32, 33, 34}) <= kinds, kinds
                    finally:
                        early.close()
                    verify_rtsp_video(rtsp, 'video1', (1280, 720) if args.backend == 'zr10' else (1920, 1080), args.video_codec)
                    verify_rtsp_sdp(rtsp, 'main.264' if thermal else 'video1', args.video_codec)
                    # A second interface must not inherit the first DESCRIBE's IP.
                    verify_rtsp_sdp(rtsp, 'video1', args.video_codec, '127.0.0.2')
                    verify_rtsp_rtcp(rtsp, 'video1')
                if args.late_interface:
                    verify_late_interface(ports['discovery'], product_id)
                with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as udp:
                    udp.settimeout(2)
                    # Exact discovery request from the stock capture.
                    udp.sendto(bytes.fromhex('aa09030000f90000d034f00199a4'),('127.0.0.1',ports['discovery']))
                    raw,_ = udp.recvfrom(256)
                    _,_,src,dst,link,cmd,p = parse_mt11_private(raw)
                    # UniGCS probes network ID 34 for every model; 2C is the
                    # A8/ZR10 UART identity, not the discovery/TCP endpoint.
                    assert (src,dst,link,cmd,len(p)) == (0x34,0xd0,0xf0,1,19)
                    assert p[:5] == bytes((product_id,127,0,0,1))
                    assert p[13:19] != bytes(6), 'missing discovery MAC address'
                    udp.setsockopt(socket.IPPROTO_IP,socket.IP_MULTICAST_IF,socket.inet_aton('127.0.0.1'))
                    udp.sendto(bytes.fromhex('aa09030000f90000d034f00199a4'),('224.0.0.1',ports['discovery']))
                    raw,peer = udp.recvfrom(256)
                    assert peer[0] == '127.0.0.1'
                    assert parse_mt11_private(raw)[-1] == p
                c = Client(ports['private'])
                # Bad short header followed by split/coalesced valid frames.
                raw = b'\xaa\x09\x03\x00\x10\x00'+command(0x94)+command(0x80)
                for part in (raw[:3],raw[3:9],raw[9:]):
                    c.sock.sendall(part)
                assert c.reply(0x94) == version_reply + (bytes((0,0,1,0x87)) if args.video_codec else b'')
                assert c.reply(0x80) == bytes(5)
                # MT11 starts on its wide RGB sensor, but its visible scene is
                # still Zoom. Wide (01) makes Android hide the target button.
                assert c.request(0x92) == (b'\x00\x02' if thermal else b'\x00\x00')
                # A second client cannot claim the same MCU return route.
                extra = Client(ports['private'])
                assert extra.sock.recv(1) == b''
                extra.sock.close()
                # A no-ACK request is applied without replying. TCP ordering
                # means the next response must be the status query.
                c.sock.sendall(mt11_private_frame(8,124,0x94,source=0xd0,destination=CAMERA_ADDRESS,link=0x16))
                c.sock.sendall(command(0x80))
                try:
                    c.reply(0x94,timeout=.15)
                    raise AssertionError('no-ACK request produced a reply')
                except TimeoutError:
                    pass
                for cmd in (0xa0,0xb4,0xc2):
                    assert c.request(cmd,dest=0x2e,link=0x11)
                assert c.request(0x83,b'\x01')[0:2] == bytes((1, 2 if args.video_codec == 'h265' else 1))
                assert c.request(0xd5,b'\x03') == b'\x03\x00\x00\x00'
                assert c.request(0xc6,b'\x01') == b'\x01'
                photos = set((root/'capture').glob('*.jpg'))
                assert photos, 'C6 photo ACK did not produce an image'
                # 9F/00 is a legacy command, not a v3 capture alias.
                c.sock.sendall(command(0x9f,b'\x00'))
                c.request(0x80)
                assert set((root/'capture').glob('*.jpg')) == photos
                assert c.request(0xe1,b'\x0a')[:4] == b'\x0a\x32\x32\x32'
                if args.backend != 'zr10':
                    assert c.request(0xe3,b'\x00\x0a\x3c') == b'\x00\x0a\x01'
                    image_state = c.request(0xe1,b'\x0a')
                    assert image_state[1] == 60, (image_state.hex(), config.read_text())
                    assert 'brightness=60' in config.read_text().replace(' ','')
                    # Invalid input must not alter the state; a following valid
                    # query proves the connection remains usable without fake ACKs.
                    c.sock.sendall(command(0xe3,b'\x00\x0a\xff'))
                    image_state = c.request(0xe1,b'\x0a')
                    assert image_state[1] == 60, (image_state.hex(), config.read_text())
                else:
                    assert c.request(0xe3,b'\x00\x0a\x3c') == b'\x00\x0a\x00'
                    assert c.request(0xe1,b'\x0a')[1] == 50
                if thermal:
                    assert c.request(0xa5,b'\x03') == b'\x03'
                    assert c.request(0xa4) == b'\x03'
                public = socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
                public.connect(('127.0.0.1',ports['public']))
                # Native UniGCS commands reach the same simulated gimbal
                # that reports attitude through the public SDK.
                before = public_request(public, 20, 0x0d)
                assert c.request(0x9a, b'\x28\x14', dest=0x2e, link=0x11) == b'\x01'
                time.sleep(.25)
                after = public_request(public, 21, 0x0d)
                assert before[:4] != after[:4], 'private rates did not move the gimbal'
                assert c.request(0x9a, b'\x00\x00', dest=0x2e, link=0x11) == b'\x01'
                assert c.request(0x9b, b'\x01', dest=0x2e, link=0x11) == b'\x01'
                deadline = time.monotonic()+3
                while time.monotonic() < deadline:
                    yaw, pitch = read_pose(public)
                    if abs(yaw) <= 1 and abs(pitch) <= 1:
                        break
                    time.sleep(.05)
                assert abs(yaw) <= 1 and abs(pitch) <= 1, 'private centre did not reach target'
                if thermal:
                    assert public_request(public,1,0x1a) == b'\x03'
                assert c.request(0xd2,b'\x0a\x14\x00') == b'\x0a\x01'
                deadline = time.monotonic()+2
                while c.request(0xd1,b'\x0a') != b'\x0a\x14\x00' and time.monotonic()<deadline:
                    time.sleep(.05)
                assert c.request(0xd1,b'\x0a') == b'\x0a\x14\x00'
                assert public_request(public,2,0x18) == b'\x02\x00'
                if thermal:
                    assert c.request(0x92) == b'\x00\x02'
                c.request(0x98,b'\x01')
                time.sleep(1.1 if args.backend == "zr10" else .25)
                assert struct.unpack_from('<H',c.request(0xd1,b'\x0a'),1)[0] > 20
                c.request(0x98,b'\x00')
                if args.backend == 'zr10': time.sleep(1.1)
                stopped = c.request(0xd1,b'\x0a')
                time.sleep(1.1 if args.backend == 'zr10' else .15)
                assert c.request(0xd1,b'\x0a') == stopped
                if thermal:
                    assert c.request(0x93,b'\x02\x00') == b'\x02\x00'
                    assert c.request(0x92) == b'\x02\x00'
                    assert public_request(public,3,0x10) == b'\x02\x00'
                assert c.request(0x81,b'\x01') == b'\x01\x01'
                assert c.request(0x80)[0] == 1
                assert public_request(public,4,0x0a)[3] == 1
                assert c.request(0x81,b'\x00') == b'\x00\x01'
                assert c.request(0x86,b'\x00',response=0x85) == b'\0'
                assert c.reply(0xf0,timeout=2) == b'\x01'
                c.request(0x98,b'\x01')
                c.sock.close(); public.close()
                time.sleep(.15)
                c = Client(ports['private'])
                if args.backend == 'zr10': time.sleep(1.1)
                assert c.request(0x92) == (b'\x02\x00' if thermal else b'\x00\x00')
                stopped = c.request(0xd1,b'\x0a')
                time.sleep(1.1 if args.backend == 'zr10' else .15)
                assert c.request(0xd1,b'\x0a') == stopped
                # Restart just the MCU simulator, as during development.
                # ICMP port-unreachable must not kill the camera/video app.
                gimbal = processes[0]
                terminate(gimbal)
                for _ in range(8):
                    c.sock.sendall(command(0xa0, dest=0x2e, link=0x11))
                    time.sleep(.05)
                    assert processes[1].poll() is None, 'camera exited while gimbal UDP peer was down'
                processes[0] = subprocess.Popen(gimbal.args, stdout=log, stderr=log)
                deadline = time.monotonic()+5
                while not gimbal_ready.exists() and time.monotonic()<deadline:
                    time.sleep(.02)
                assert gimbal_ready.exists(), 'gimbal restart failed'
                assert c.request(0x9b, b'\x01', dest=0x2e, link=0x11) == b'\x01'
                c.sock.close()
                time.sleep(.1)
                old=LongClient(ports['private'])
                # Replay a captured UniGCS A8 query, split across TCP reads.
                captured=bytes.fromhex('5566aabb01000000000000802d977a34b7ad40eb')
                old.sock.sendall(captured[:2]); old.sock.sendall(captured[2:])
                assert len(old.reply(0x80))==5
                assert old.request(0x94)==version_reply + (bytes((0,0,1,0x87)) if args.video_codec else b'')
                assert len(old.request(0x83,b'\x00'))==9
                old.received.clear()
                old.sock.sendall(long_command(0x94,control=0)+long_command(0x80))
                assert len(old.reply(0x80))==5
                assert 0x94 not in old.received, 'unexpected no-ACK response'
                # Android UniGCS 3.2.1 uses 9F/00 on the A8, unlike MT11 C6/01.
                photos = set((root/'capture').glob('*.jpg'))
                assert old.request(0x9f,b'\x00') == b'\x00'
                captured_photos = set((root/'capture').glob('*.jpg')) - photos
                assert captured_photos, 'legacy photo ACK did not capture an image'
                assert all(p.read_bytes().startswith(b'\xff\xd8') for p in captured_photos)
                photos |= captured_photos
                old.received.clear()
                for payload in (b'',b'\x01',b'\x00\x00'):
                    old.sock.sendall(long_command(0x9f,payload))
                old.request(0x80)
                assert 0x9f not in old.received, 'invalid photo request acknowledged'
                assert set((root/'capture').glob('*.jpg')) == photos
                old.received.clear()
                old.sock.sendall(long_command(0x9f,b'\x00',control=0))
                old.request(0x80)
                assert 0x9f not in old.received, 'no-ACK photo request acknowledged'
                assert set((root/'capture').glob('*.jpg')) > photos
                # A storage failure must not be reported as a successful capture.
                (root/'capture').rename(root/'saved-capture')
                (root/'capture').write_text('not a directory')
                try:
                    old.received.clear()
                    old.sock.sendall(long_command(0x9f,b'\x00'))
                    old.request(0x80)
                    assert 0x9f not in old.received, 'failed photo capture acknowledged'
                    assert old.request(0xc6,b'\x01') == b'\x00'
                finally:
                    (root/'capture').unlink()
                    (root/'saved-capture').rename(root/'capture')
                assert old.request(0x81,b'\x01') == b'\x01\x01'
                assert old.request(0x80)[0] == 1
                assert old.request(0x81,b'\x00') == b'\x00\x01'
                assert old.request(0x80)[0] == 0
                if args.backend == 'a8':
                    # An ACK alone cannot prove dispatch: the A8 MCU has a
                    # different command namespace from the network client.
                    public = socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
                    public.connect(('127.0.0.1',ports['public']))
                    before = read_pose(public)
                    assert old.request(0x9a,b'\x0c\x00') == b'\x01'
                    time.sleep(.3)
                    assert read_pose(public) != before, 'Android drag did not move A8'
                    assert old.request(0x9a,b'\x00\x00') == b'\x01'
                    # Allow the calibrated motor response and 10 Hz feedback
                    # to settle before checking that the stop holds position.
                    time.sleep(1)
                    stopped_pose = read_pose(public)
                    time.sleep(.2)
                    assert all(abs(a-b)<=1 for a,b in zip(read_pose(public),stopped_pose)), \
                        'Android release did not stop A8'
                    for preset, expected_pitch in ((2,-900),(1,0),(4,-900),(3,-900),(1,0)):
                        expected_yaw = 0
                        if preset == 4:
                            # Start away from centre to prove pitch-only
                            # look-down preserves the measured yaw.
                            _,wire_pitch,wire_yaw = transform(TARGETS[args.backend], 'angle_command',
                                args.orientation == 'inverted', (0,-20,20), inverse=True)
                            public_request(public,23,0x0e,struct.pack('<hh',round(wire_yaw*10),round(wire_pitch*10)))
                            deadline = time.monotonic()+3
                            while time.monotonic() < deadline:
                                yaw,pitch = read_pose(public)
                                if abs(yaw-200) <= 1 and abs(pitch+200) <= 1:
                                    break
                                time.sleep(.05)
                            assert abs(yaw-200) <= 1 and abs(pitch+200) <= 1
                            expected_yaw = 200
                        assert old.request(0x9b,bytes((preset,))) == b'\x01'
                        deadline = time.monotonic()+3
                        while time.monotonic() < deadline:
                            yaw,pitch = read_pose(public)
                            if abs(yaw-expected_yaw) <= 1 and abs(pitch-expected_pitch) <= 1:
                                break
                            time.sleep(.05)
                        assert abs(yaw-expected_yaw) <= 1 and abs(pitch-expected_pitch) <= 1, (preset,yaw,pitch)
                    assert old.request(0x9b,b'\x00') == b'\x00'
                    assert old.request(0x9b,b'\x05') == b'\x00'
                    public.close()
                assert old.request(0x9b,b'\x01')==b'\x01'
                assert old.request(0x9a,b'\x00\x00')==b'\x01'
                old.sock.close()
                print('PASS: discovery, CRC recovery, fragmentation, startup, gimbal routing, camera controls, persistence, public SDK coexistence, heartbeat, reconnect')
            except BaseException:
                log.flush()
                print((root/'run.log').read_text(),file=sys.stderr)
                raise
            finally:
                for process in reversed(processes):
                    terminate(process)


if __name__ == '__main__':
    main()
