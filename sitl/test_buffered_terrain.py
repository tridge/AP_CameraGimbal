#!/usr/bin/env python3
"""Verify video cadence and shutdown while the terrain renderer stalls briefly."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

from test_video_telemetry import REPO, RTSP, port, ready, stop

RENDERER = '''import argparse, json, os, socket, struct, time
p=argparse.ArgumentParser();p.add_argument('--fd',type=int);a,_=p.parse_known_args()
data=open(os.environ['TEST_TERRAIN_FRAME'],'rb').read()
with socket.socket(fileno=a.fd) as s, s.makefile('rb') as requests:
    s.sendall(b'R')
    for i,line in enumerate(requests):
        request=json.loads(line)
        print(json.dumps({'frame':i,'lead':request['prediction_ms']}),flush=True)
        time.sleep(.14 if i in (25,50) else .005)
        # Vary the last RTP packet size, including small tails susceptible to
        # Nagle/delayed-ACK stalls. This is a valid H.264 filler-data NAL.
        frame=data+bytes([0,0,0,1,12])+bytes([255])*(i*97%1200)+bytes([128])
        packet=struct.pack('!II',len(frame),1)+frame
        s.sendall(packet+packet+struct.pack('!IIII',0,0,0,0)+struct.pack('<QBBHBBi7f',0,0,1,0,255,0,0,*([float('nan')]*7)))
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=REPO / 'build/sitl')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='sitl-buffering-') as temp:
        root = Path(temp)
        fixture = root / 'frame.h264'
        subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                        'testsrc2=size=640x360:rate=20', '-frames:v', '1',
                        '-c:v', 'libx264', '-preset', 'ultrafast',
                        '-x264-params', 'aud=1:repeat-headers=1', str(fixture)], check=True)
        renderer = root / 'renderer.py'
        renderer.write_text(RENDERER)
        config = root / 'camera.ini'
        config.write_text('[mavlink]\nsystem_id=42\n')
        camera_ready = root / 'camera.ready'
        rtsp_port = port()
        gimbal_port = port(socket.SOCK_DGRAM)
        gimbal_ready = root / 'gimbal.ready'
        env = dict(os.environ, CAMERA_APP_CONFIG=str(config), CAMERA_APP_BACKEND='mt11',
                   CAMERA_APP_PORT=str(port()), CAMERA_APP_RTSP_PORT=str(rtsp_port),
                   CAMERA_APP_UART=f'udp://127.0.0.1:{gimbal_port}',
                   CAMERA_APP_MAVLINK_TCP_PORT='0', CAMERA_APP_MAVLINK_UDP_PORT='0',
                   CAMERA_APP_READY_PATH=str(camera_ready), CAMERA_APP_SITL_TERRAIN=str(renderer),
                   CAMERA_GIMBAL_SITL_PYTHON=sys.executable, CAMERA_GIMBAL_SITL_FPS='20',
                   TEST_TERRAIN_FRAME=str(fixture))
        camera = client = gimbal = None
        with (root / 'camera.log').open('w') as log:
            try:
                gimbal = subprocess.Popen([sys.executable, str(REPO / 'sitl/gimbal_sim.py'),
                                           '--port', str(gimbal_port), '--ready-file', str(gimbal_ready)],
                                          stdout=log, stderr=subprocess.STDOUT)
                ready(gimbal_ready, gimbal)
                camera = subprocess.Popen([str(args.build.resolve() / 'camera-app')], env=env,
                                          stdout=log, stderr=subprocess.STDOUT)
                ready(camera_ready, camera, root / 'camera.log')
                client = RTSP(rtsp_port, 'video1')
                times = [time.monotonic() for _ in client.frames(80, 'h264')]
                intervals = [b-a for a,b in zip(times[5:], times[6:])]
                assert max(intervals) < .09, intervals
                assert min(intervals) > .025, intervals
                assert abs((times[-1]-times[5]) / (len(times)-6) - .05) < .003
                client.close(); client = None
                begin = time.monotonic()
                camera.terminate()
                camera.wait(timeout=3)
                assert camera.returncode == 0
                print(f'PASS two 140 ms render stalls: max frame interval {max(intervals)*1000:.1f} ms; '
                      f'bounded-queue shutdown {time.monotonic()-begin:.2f} s')
            except Exception:
                print((root / 'camera.log').read_text())
                raise
            finally:
                if client: client.close()
                stop(camera)
                stop(gimbal)
        leads = [json.loads(line)['lead'] for line in (root / 'camera.log').read_text().splitlines()
                 if line.startswith('{')]
        assert max(leads) <= 250 and max(leads) >= 140, leads


if __name__ == '__main__':
    main()
