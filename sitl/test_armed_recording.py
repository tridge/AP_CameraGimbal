#!/usr/bin/env python3
"""Check heartbeat-controlled recording against the real camera application in SITL."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

from test_video_telemetry import M, REPO, command, mavutil, port, ready, stop


def heartbeat(link, system=42, component=M.MAV_COMP_ID_AUTOPILOT1, armed=False,
              kind=M.MAV_TYPE_QUADROTOR, autopilot=M.MAV_AUTOPILOT_ARDUPILOTMEGA):
    link.mav.srcSystem, link.mav.srcComponent = system, component
    # Other base_mode flags must not be mistaken for the armed bit.
    mode = M.MAV_MODE_FLAG_CUSTOM_MODE_ENABLED | M.MAV_MODE_FLAG_STABILIZE_ENABLED
    if armed:
        mode |= M.MAV_MODE_FLAG_SAFETY_ARMED
    link.mav.heartbeat_send(kind, autopilot, mode, 0, M.MAV_STATE_ACTIVE)
    link.mav.srcSystem, link.mav.srcComponent = 255, 190


def recording(link, expected):
    while link.recv_match(blocking=False) is not None:
        pass
    link.mav.command_long_send(42, M.MAV_COMP_ID_CAMERA,
                              M.MAV_CMD_REQUEST_CAMERA_CAPTURE_STATUS, 0,
                              0, 0, 0, 0, 0, 0, 0)
    status = link.recv_match(type='CAMERA_CAPTURE_STATUS', blocking=True, timeout=3)
    assert status is not None, 'no capture status'
    assert status.get_srcSystem() == 42
    assert status.video_status == int(expected), status
    if not expected:
        assert status.recording_time_ms == 0, status
    return status


def wait_for_frames(paths):
    # Recording starts at a keyframe; rendering/encoding may take longer than
    # a fixed sleep on a busy host. Require actual packets before disarming.
    deadline = time.monotonic()+8
    pending = set(paths)
    while pending and time.monotonic()<deadline:
        for path in list(pending):
            result = subprocess.run([
                'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-count_packets',
                '-show_entries', 'stream=nb_read_packets', '-of', 'json', str(path)],
                capture_output=True, text=True, timeout=3)
            if result.returncode == 0:
                streams=json.loads(result.stdout).get('streams', [])
                if streams and int(streams[0].get('nb_read_packets', 0))>0:
                    pending.remove(path)
        if pending: time.sleep(.05)
    assert not pending, f'Recordings have no frames: {pending}'


def run_case(args, directory, fixture, mode, system_id):
    directory.mkdir()
    config = directory / 'camera.ini'
    config.write_text(f'[video]\ntelemetry=apcg\n[recording]\nautorecord = {mode}\n'
                      f'[mavlink]\nsystem_id = {system_id}\n')
    camera_ready, gimbal_ready = directory / 'camera.ready', directory / 'gimbal.ready'
    gimbal_port, tcp_port = port(socket.SOCK_DGRAM), port()
    recordings = directory / 'recordings'
    recordings.mkdir()
    env = dict(os.environ, CAMERA_APP_BACKEND=args.backend,
               CAMERA_APP_UART=f'udp://127.0.0.1:{gimbal_port}',
               CAMERA_APP_PORT=str(port()), CAMERA_APP_RTSP_PORT=str(port()),
               CAMERA_APP_MAVLINK_TCP_PORT=str(tcp_port), CAMERA_APP_MAVLINK_UDP_PORT='0',
               CAMERA_APP_CONFIG=str(config), CAMERA_APP_READY_PATH=str(camera_ready),
               CAMERA_APP_RECORD_STATE=str(directory / 'recording.state'),
               CAMERA_APP_RECORD_ROOT=str(recordings),
               CAMERA_APP_CAPTURE_ROOT=str(directory / 'capture'),
               CAMERA_APP_SITL_VIDEO1=str(fixture), CAMERA_APP_SITL_VIDEO2=str(fixture))
    camera = gimbal = link = None
    with (directory / 'camera.log').open('w') as clog, (directory / 'gimbal.log').open('w') as glog:
        try:
            gimbal = subprocess.Popen([sys.executable, str(REPO / 'sitl/gimbal_sim.py'),
                '--backend', args.backend, '--port', str(gimbal_port),
                '--ready-file', str(gimbal_ready)], stdout=glog, stderr=subprocess.STDOUT)
            ready(gimbal_ready, gimbal)
            camera = subprocess.Popen([str(args.build / 'camera-app'), '--backend', args.backend],
                                       env=env, stdout=clog, stderr=subprocess.STDOUT)
            ready(camera_ready, camera)
            link = mavutil.mavlink_connection(f'tcp:127.0.0.1:{tcp_port}',
                                              source_system=255, source_component=190)
            if mode == 'while_armed':
                assert not (directory / 'recording.state').exists()
                if system_id == 0:
                    heartbeat(link, system=255, kind=M.MAV_TYPE_GCS, armed=True)
                    heartbeat(link, component=M.MAV_COMP_ID_CAMERA, kind=M.MAV_TYPE_CAMERA,
                              autopilot=M.MAV_AUTOPILOT_INVALID, armed=True)
                    assert link.recv_match(blocking=True, timeout=.3) is None
                    assert not (directory / 'recording.state').exists()
                    # Already armed at startup: the first FC heartbeat both
                    # selects the system ID and starts recording.
                    heartbeat(link, armed=True)
                else:
                    # A different FC arriving first must not drive recording
                    # when the camera's system ID is explicitly configured.
                    heartbeat(link, system=43, armed=True)
                    heartbeat(link, component=2, armed=True)
                    recording(link, False)
                    heartbeat(link, armed=True)
                event = link.recv_match(type='CAMERA_CAPTURE_STATUS', blocking=True, timeout=3)
                assert event is not None and event.video_status == 1, event
                recording(link, True)
                files = set(recordings.glob('*.mp4'))
                assert len(files) == (2 if args.backend == 'mt11' else 1)
                for system, component in ((43, 1), (255, 190), (42, 2), (42, 100)):
                    heartbeat(link, system=system, component=component)
                    recording(link, True)
                heartbeat(link, armed=True)
                recording(link, True)
                assert set(recordings.glob('*.mp4')) == files  # repeated arm is idempotent
                time.sleep(.4)  # no heartbeat is not a disarm; allow frames to arrive
                wait_for_frames(files)
                assert recording(link, True).recording_time_ms >= 300
                heartbeat(link)
                recording(link, False)
                for system, component in ((43, 1), (255, 190), (42, 2), (42, 100)):
                    heartbeat(link, system=system, component=component, armed=True)
                    recording(link, False)
                # A second arm/disarm cycle opens new recordings and finalizes them.
                heartbeat(link, armed=True)
                recording(link, True)
                time.sleep(.4)
                wait_for_frames(set(recordings.glob('*.mp4'))-files)
                heartbeat(link)
                recording(link, False)
                assert len(list(recordings.glob('*.mp4'))) == 2 * len(files)
            else:
                recording(link, mode == 'true')
                heartbeat(link, armed=True)
                recording(link, mode == 'true')
                heartbeat(link)
                recording(link, mode == 'true')
                # Manual recording remains available with automatic recording disabled.
                if mode == 'false':
                    command(link, M.MAV_CMD_VIDEO_START_CAPTURE)
                    heartbeat(link)
                    recording(link, True)
                time.sleep(.4)
                wait_for_frames(recordings.glob('*.mp4'))
                command(link, M.MAV_CMD_VIDEO_STOP_CAPTURE)
                recording(link, False)
            paths=list(recordings.glob('*.mp4'))
            if args.backend == 'mt11':
                raw=list(recordings.glob('*.raw.mkv'))
                assert len(raw)*2==len(paths), (raw,paths)
                paths+=raw
            for path in paths:
                info = json.loads(subprocess.check_output([
                    'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-count_packets',
                    '-show_entries', 'stream=nb_read_packets', '-of', 'json', str(path)]))
                assert int(info['streams'][0]['nb_read_packets']) > 0, path
            print(f'PASS {args.backend} autorecord={mode} system_id={system_id}: '
                  'capture status and finalized video/raw recordings', flush=True)
        finally:
            if link is not None:
                link.close()
            stop(camera)
            stop(gimbal)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', choices=('mt11', 'a8'), default='mt11')
    parser.add_argument('--build', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    args.build = (args.build or REPO / 'build' / ('sitl' if args.backend == 'mt11' else 'a8-sitl')).resolve()
    directory = (args.output or Path(tempfile.mkdtemp(prefix='armed-recording-'))).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    print(f'Logs and recordings: {directory}', flush=True)
    fixture = directory / 'source.h264'
    width, height, fps = (640, 360, 10) if args.backend == 'mt11' else (1280, 720, 25)
    subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                    f'testsrc2=size={width}x{height}:rate={fps}', '-t', '1',
                    '-c:v', 'libx264', '-x264-params',
                    'aud=1:bframes=0:keyint=1:repeat-headers=1', '-y', str(fixture)], check=True)
    for mode, system_id in (('false', 42), ('true', 42), ('while_armed', 0), ('while_armed', 42)):
        run_case(args, directory / f'{mode}-{system_id}', fixture, mode, system_id)


if __name__ == '__main__':
    main()
