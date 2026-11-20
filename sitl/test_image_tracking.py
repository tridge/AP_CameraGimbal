#!/usr/bin/env python3
"""Exercise dlib, gimbal feedback, SIYI and MAVLink on real SITL video frames."""
import argparse
import json
import math
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
from test_mavlink_parameters import M, connect, receive, wait_ready, stop, port
from test_unigcs import Client, LongClient
from gimbal_sim import siyi_frame
from target_properties import TARGETS

ROOT = Path(__file__).resolve().parents[1]


def run(args, directory):
    narrow = args.zoom_scene or args.thermal_scene
    root = Path(directory)
    root.mkdir(parents=True, exist_ok=True)
    for name in ('ready','gimbal-ready'): (root/name).unlink(missing_ok=True)
    config = root/'camera.ini'
    config.write_text('[mavlink]\nsystem_id=42\n[logging]\ndisarmed=true\n'
                      '[recording]\nautorecord=false\n[overlay]\ncross=false\n'
                      '[stream.main]\ncodec=h265\n[stream.sub]\ncodec=h265\n')
    ports = dict(gimbal=port(socket.SOCK_DGRAM), private=args.private_port or (37256 if args.tablet else port()),
                 discovery=37258 if args.tablet else port(socket.SOCK_DGRAM),
                 public=port(), rtsp=8554 if args.tablet else port(),
                 mavlink=14550 if args.tablet else port())
    # Keep the public SDK port isolated even in tablet mode: an existing
    # aircraft SITL may otherwise stream mount rates to the standard 37260.
    # Android uses the private TCP service for these UI operations.
    env = dict(os.environ, CAMERA_APP_BACKEND=args.backend,
               CAMERA_APP_UART=f'udp://127.0.0.1:{ports["gimbal"]}',
               CAMERA_APP_PORT=str(ports['public']), CAMERA_APP_RTSP_PORT=str(ports['rtsp']),
               CAMERA_APP_UNIGCS_PORT=str(ports['private']), CAMERA_APP_DISCOVERY_PORT=str(ports['discovery']),
               CAMERA_APP_MAVLINK_TCP_PORT=str(ports['mavlink']), CAMERA_APP_MAVLINK_UDP_PORT='0',
               CAMERA_APP_CONFIG=str(config), CAMERA_APP_READY_PATH=str(root/'ready'),
               CAMERA_APP_RECORD_ROOT=str(root/'record'), CAMERA_APP_CAPTURE_ROOT=str(root/'capture'),
               CAMERA_APP_LOG_ROOT=str(root/'logs'), CAMERA_APP_RECORD_STATE=str(root/'record.state'),
               CAMERA_APP_SITL_VIDEO1='tracking-fixture', CAMERA_APP_SITL_RENDERER=str(ROOT/'sitl/tracking_video.py'),
               CAMERA_GIMBAL_SITL_PYTHON=sys.executable, CAMERA_GIMBAL_SITL_FPS='20',
               TRACKING_TEST_YAW=('3' if args.tablet or narrow else '10') if args.backend=='mt11' else '-28',
               TRACKING_TEST_PITCH='-18' if (args.tablet or narrow) and args.backend=='mt11' else '-15')
    env.pop('CAMERA_APP_SITL_TERRAIN', None)
    processes = []
    client = link = None
    with (root/'run.log').open('w') as log:
        try:
            processes.append(subprocess.Popen([sys.executable, str(ROOT/'sitl/gimbal_sim.py'), '--backend', args.backend,
                '--port', str(ports['gimbal']), '--ready-file', str(root/'gimbal-ready')], stdout=log, stderr=log))
            wait_ready(root/'gimbal-ready', processes[0])
            processes.append(subprocess.Popen([str(args.camera.resolve())], env=env, stdout=log, stderr=log))
            wait_ready(root/'ready', processes[1])
            (root/'session.json').write_text(json.dumps(dict(ports=ports, pids=[p.pid for p in processes]), indent=2))
            link = connect(f'tcp:127.0.0.1:{ports["mavlink"]}')
            if args.tablet:
                print(f'Tablet SITL ready: {root}', flush=True)
                while all(p.poll() is None for p in processes): time.sleep(1)
                return
            def command(cmd, *params):
                link.mav.command_long_send(42, 100, cmd, 0, *(list(params)+[0]*(7-len(params))))
                return receive(link, 'COMMAND_ACK', lambda m: m.command==cmd).result
            assert command(M.MAV_CMD_REQUEST_CAMERA_INFORMATION)==M.MAV_RESULT_ACCEPTED
            # Repeat request since receive(COMMAND_ACK) can consume the preceding information.
            link.mav.command_long_send(42, 100, M.MAV_CMD_REQUEST_CAMERA_INFORMATION, 0, *([0]*7))
            info = receive(link, 'CAMERA_INFORMATION')
            assert info.flags & M.CAMERA_CAP_FLAGS_HAS_TRACKING_RECTANGLE
            assert info.flags & M.CAMERA_CAP_FLAGS_HAS_TRACKING_POINT
            client = Client(ports['private']) if args.backend=='mt11' else LongClient(ports['private'])
            version = client.request(0x94)
            assert len(version)==8 and version[7]==0x87, version.hex()
            assert client.request(0xa3, b'\1')==b'\1\0'
            assert client.request(0xa2)==b'\1'
            if narrow:
                assert args.backend=='mt11'
                assert client.request(0x93,b'\2\0' if args.thermal_scene else b'\0\0')==(b'\2\0' if args.thermal_scene else b'\0\2')
            time.sleep(.5)
            hfov = TARGETS[args.backend]['lens3_fov_h' if args.thermal_scene else 'lens2_fov_h' if args.zoom_scene else 'lens1_fov_h']
            width,height = (1280,720) if args.thermal_scene else (1920,1080)
            aspect = 640/512 if args.thermal_scene else width/height
            cx = .5 + math.tan(math.radians(3 if narrow else 10))/(2*math.tan(math.radians(hfov)/2))
            cy = .5 - math.tan(math.radians(2 if narrow else 5))/(2*math.tan(math.radians(hfov)/2))*aspect
            rect = (cx-.065, cy-.13*aspect/2, cx+.065, cy+.13*aspect/2)
            assert command(M.MAV_CMD_CAMERA_TRACK_RECTANGLE, float('nan'), 0, 1, 1)==M.MAV_RESULT_DENIED
            assert command(M.MAV_CMD_SET_MESSAGE_INTERVAL,M.MAVLINK_MSG_ID_CAMERA_TRACKING_IMAGE_STATUS,-.5)==M.MAV_RESULT_DENIED
            # Reverse drag proves the vendor coordinates are sorted and use encoder pixels.
            selection = struct.pack('<BHHHH', 1, round(rect[2]*width), round(rect[3]*height),
                                    round(rect[0]*width), round(rect[1]*height))
            assert client.request(0xaa, selection)==b'\1'
            first = receive(link, 'CAMERA_TRACKING_IMAGE_STATUS', lambda m: m.tracking_status & 1, timeout=5)
            initial = math.hypot((first.rec_top_x+first.rec_bottom_x)/2-.5,
                                 (first.rec_top_y+first.rec_bottom_y)/2-.5)
            last = first
            started = time.monotonic()
            while time.monotonic()-started < 5:
                client.request(0xa2)  # private session heartbeat
                last = receive(link, 'CAMERA_TRACKING_IMAGE_STATUS', lambda m: m.tracking_status & 1)
            error = math.hypot((last.rec_top_x+last.rec_bottom_x)/2-.5,
                               (last.rec_top_y+last.rec_bottom_y)/2-.5)
            assert error < .04 and error < initial*.6, (initial, error, last)
            # Passing through the center during an oscillation is insufficient:
            # a stationary selection must also remain centered afterwards.
            settled_until=time.monotonic()+2
            while time.monotonic()<settled_until:
                client.request(0xa2)
                last=receive(link, 'CAMERA_TRACKING_IMAGE_STATUS', lambda m: m.tracking_status & 1)
                settled_error=math.hypot((last.rec_top_x+last.rec_bottom_x)/2-.5,
                                         (last.rec_top_y+last.rec_bottom_y)/2-.5)
                assert settled_error<.04, ('settled',settled_error,last)
            assert command(M.MAV_CMD_CAMERA_STOP_TRACKING)==M.MAV_RESULT_ACCEPTED
            receive(link, 'CAMERA_TRACKING_IMAGE_STATUS', lambda m: m.tracking_status==0)
            # MAVLink can acquire the same target and SIYI cancel controls the shared state.
            rect = (last.rec_top_x,last.rec_top_y,last.rec_bottom_x,last.rec_bottom_y)
            assert command(M.MAV_CMD_CAMERA_TRACK_RECTANGLE,*rect)==M.MAV_RESULT_ACCEPTED
            receive(link, 'CAMERA_TRACKING_IMAGE_STATUS', lambda m: m.tracking_status & 1)
            assert client.request(0xaa,bytes(9))==b'\1'
            receive(link, 'CAMERA_TRACKING_IMAGE_STATUS', lambda m: m.tracking_status==0)
            # A point acquires the target as a box of +-radius around it
            px, py = (rect[0]+rect[2])/2, (rect[1]+rect[3])/2
            for bad in ((float('nan'), py), (1.5, py), (px, -.1)):
                assert command(M.MAV_CMD_CAMERA_TRACK_POINT, *bad, .065)==M.MAV_RESULT_DENIED
            assert command(M.MAV_CMD_CAMERA_TRACK_POINT, px, py, .065)==M.MAV_RESULT_ACCEPTED
            point = receive(link, 'CAMERA_TRACKING_IMAGE_STATUS', lambda m: m.tracking_status & 1)
            assert abs(point.rec_bottom_x-point.rec_top_x-.13) < .03, point
            # the radius is a fraction of width, so the box is square in pixels
            assert abs(point.rec_bottom_y-point.rec_top_y-.13*width/height) < .04, point
            assert command(M.MAV_CMD_CAMERA_STOP_TRACKING)==M.MAV_RESULT_ACCEPTED
            receive(link, 'CAMERA_TRACKING_IMAGE_STATUS', lambda m: m.tracking_status==0)
            # Explicit public SDK movement takes ownership back from tracking.
            assert command(M.MAV_CMD_CAMERA_TRACK_RECTANGLE,*rect)==M.MAV_RESULT_ACCEPTED
            receive(link, 'CAMERA_TRACKING_IMAGE_STATUS', lambda m: m.tracking_status & 1)
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as public:
                public.sendto(siyi_frame(1,17,0x07,bytes(2)),('127.0.0.1',ports['public']))
            receive(link, 'CAMERA_TRACKING_IMAGE_STATUS', lambda m: m.tracking_status==0)
            if args.backend=='mt11':
                for scene, response in ((b'\2\0',b'\2\0'), (b'\3\0',b'\3\2'), (b'\0\0',b'\0\2')):
                    assert client.request(0x93,scene)==response
                    assert client.request(0x92)==response
                assert client.request(0xba, dest=0x2e, link=0x11)==b'\0\2'
                assert client.request(0xbb,b'\1',dest=0x2e,link=0x11)==b'\1'
                assert len(client.reply(0x89,0x2e))==4
                assert client.request(0xbb,b'\0',dest=0x2e,link=0x11)==b'\0'
            print(f'PASS {args.backend}: SIYI acquisition centers target {initial:.3f} -> {error:.3f}; '
                  'shared MAVLink rectangle/point acquire, cancel, status, capability, scenes/range', flush=True)
            client.sock.close(); client=None
        except BaseException:
            print((root/'run.log').read_text()[-6000:], file=sys.stderr)
            raise
        finally:
            if client: client.sock.close()
            if link: link.close()
            for p in reversed(processes): stop(p)


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('camera', type=Path)
    parser.add_argument('--backend', choices=('a8','mt11'), required=True)
    parser.add_argument('--tablet', action='store_true', help='Serve on standard ports for interactive Android testing')
    scene=parser.add_mutually_exclusive_group()
    scene.add_argument('--zoom-scene', action='store_true', help='Exercise MT11 narrow-field visible tracking')
    scene.add_argument('--thermal-scene', action='store_true', help='Exercise MT11 thermal tracking and sensor aspect')
    parser.add_argument('--runtime', type=Path)
    parser.add_argument('--private-port', type=int, help='Override the private control port while preparing a tablet connection')
    args=parser.parse_args()
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    if args.runtime: run(args,args.runtime)
    else:
        with tempfile.TemporaryDirectory(prefix='image-tracking-') as directory: run(args,directory)
