#!/usr/bin/env python3
"""Check MISB transport/telemetry, recording and restart selection in each SITL port."""
import argparse
import json
import math
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time

from target_properties import SITL_TARGETS

ROOT = Path(__file__).resolve().parents[1]
KEY = bytes.fromhex('060e2b34020b01010e01030101000000')


def klv_items(data):
    assert data[:16] == KEY
    at, length = 17, data[16]
    if length & 128:
        count = length & 127
        length = int.from_bytes(data[at:at+count], 'big')
        at += count
    assert at + length == len(data)
    checksum = sum(v << (8 if i % 2 == 0 else 0) for i, v in enumerate(data[:-2])) & 65535
    assert checksum == int.from_bytes(data[-2:], 'big')
    assert data[at] == 2 and data[-4:-2] == b'\x01\x02'
    items = {}
    while at < len(data):
        tag, size = data[at:at+2]
        at += 2
        assert tag not in items and at + size <= len(data)
        items[tag] = data[at:at+size]
        at += size
    assert items[65] == b'\x08'
    return items


def pts(p):
    assert p[0] & 1 and p[2] & 1 and p[4] & 1
    return ((p[0] >> 1) & 7) << 30 | int.from_bytes(p[1:3], 'big') >> 1 << 15 | int.from_bytes(p[3:5], 'big') >> 1


def transport(data):
    assert len(data) % 188 == 0
    continuity, pending, packets, types, descriptors = {}, {}, {}, {}, {}
    pcrs = []
    for at in range(0, len(data), 188):
        p = data[at:at+188]
        assert p[0] == 0x47 and not p[1] & 0x80
        pid, control = (p[1] & 31) * 256 + p[2], (p[3] >> 4) & 3
        assert control
        off = 4
        if control & 2:
            if p[4] >= 7 and p[5] & 0x10:
                pcrs.append((int.from_bytes(p[6:10], 'big') << 1) | (p[10] >> 7))
            off += 1 + p[4]
        if not control & 1:
            continue
        if pid in continuity:
            assert (p[3] & 15) == (continuity[pid] + 1) % 16, (pid, at)
        continuity[pid] = p[3] & 15
        payload = p[off:]
        if pid in (0, 0x1000):
            assert p[1] & 64
            section = payload[1+payload[0]:]
            size = ((section[1] & 15) << 8 | section[2]) + 3
            section = section[:size]
            crc = 0xffffffff
            for byte in section:
                crc ^= byte << 24
                for _ in range(8):
                    crc = ((crc << 1) ^ (0x04c11db7 if crc & 0x80000000 else 0)) & 0xffffffff
            assert crc == 0
            if pid == 0x1000:
                i = 12 + ((section[10] & 15) << 8 | section[11])
                while i < size-4:
                    kind = section[i]
                    stream = (section[i+1] & 31) << 8 | section[i+2]
                    n = (section[i+3] & 15) << 8 | section[i+4]
                    types[stream] = kind
                    descriptors[stream] = section[i+5:i+5+n]
                    i += 5+n
            continue
        if p[1] & 64:
            if pid in pending:
                packets.setdefault(pid, []).append(bytes(pending[pid]))
            pending[pid] = bytearray()
        if pid in pending:
            pending[pid].extend(payload)
    for pid, p in pending.items():
        packets.setdefault(pid, []).append(bytes(p))
    assert types[0x102] == 0x15
    assert b'\x26\x09\x01\x00\xffKLVA\x00\x0f' in descriptors[0x102]
    assert b'\x27\x09' in descriptors[0x102]
    metadata, video = [], []
    for pid, pes_packets in packets.items():
        for p in pes_packets:
            assert p[:3] == b'\0\0\x01'
            assert p[7] >> 6 == 2
            timestamp = pts(p[9:14])
            if pid == 0x101:
                video.append(timestamp)
            elif pid == 0x102:
                assert p[3] == 0xfc and len(p) == 6+int.from_bytes(p[4:6], 'big')
                cell = p[9+p[8]:]
                assert cell[0] == 0 and cell[2] == 0xdf
                assert len(cell) == 5+int.from_bytes(cell[3:5], 'big')
                metadata.append((timestamp, klv_items(cell[5:])))
    assert metadata and video
    return video, metadata, pcrs


def check_file(path, codec='h264', complete=True):
    video, metadata, pcrs = transport(path.read_bytes())
    kpts = [p for p, _ in metadata]
    assert kpts == video if complete else set(video).issubset(kpts)
    assert len(video) >= 3
    assert all(0 < ((b-a) & 0x1ffffffff) <= 3600 for a, b in zip(pcrs, pcrs[1:]))
    located = [items for _, items in metadata if 13 in items]
    assert located
    for items in located:
        lat = int.from_bytes(items[13], 'big', signed=True) * 90 / 2147483647
        assert abs(lat + 35.363261) < 1e-6, lat
        assert 16 in items and 17 in items
        assert 90 in items and 91 in items
        assert int.from_bytes(items[91], 'big') == 0x80000000
        pitch = int.from_bytes(items[90], 'big', signed=True)*90/2147483647
        assert abs(pitch-math.degrees(.6)) < .00001
    info = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-show_streams', '-of', 'json', str(path)]))
    assert [s['codec_name'] for s in info['streams']] == [codec, 'klv'], info
    result = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(path), '-map', '0:v:0', '-f', 'null', '-'], capture_output=True, timeout=30)
    assert result.returncode == 0, result.stderr.decode()
    print(f'PASS {path}: {len(video)} video packets, synchronous KLV, checksum, position, FOV and decode', flush=True)


def capture(rtsp_port, stream, destination, seconds=2.5):
    client = RTSP(rtsp_port, stream)
    deadline = time.monotonic()+seconds
    data = bytearray()
    try:
        while time.monotonic() < deadline:
            head = client.file.read(4)
            assert len(head) == 4 and head[0] == 36
            packet = client.file.read(int.from_bytes(head[2:], 'big'))
            if head[1] != 0:
                continue
            assert packet[1] & 127 == 33
            assert (len(packet)-12) % 188 == 0
            data.extend(packet[12:])
    finally:
        client.close()
    destination.write_bytes(data)


def discovered_streams(link):
    link.mav.command_long_send(42, 100, M.MAV_CMD_REQUEST_VIDEO_STREAM_INFORMATION, 0, 0, 0, 0, 0, 0, 0, 0)
    result = {}
    deadline = time.monotonic()+5
    while time.monotonic() < deadline:
        message = link.recv_match(type='VIDEO_STREAM_INFORMATION', blocking=True, timeout=.2)
        if message:
            result[message.stream_id] = message
            if len(result) == message.count:
                assert sorted(result) == list(range(1, message.count+1))
                return result
    raise AssertionError('Missing VIDEO_STREAM_INFORMATION entries')


def streaming(link, stream_id, enabled):
    command_id = M.MAV_CMD_VIDEO_START_STREAMING if enabled else M.MAV_CMD_VIDEO_STOP_STREAMING
    link.mav.command_long_send(42, 100, command_id, 0, stream_id, 0, 0, 0, 0, 0, 0)
    status, ack = None, False
    deadline = time.monotonic()+5
    while time.monotonic() < deadline:
        message = link.recv_match(type=['VIDEO_STREAM_STATUS', 'COMMAND_ACK'], blocking=True, timeout=.2)
        if message is None:
            continue
        if message.get_type() == 'VIDEO_STREAM_STATUS' and message.stream_id == stream_id:
            status = message
        elif message.get_type() == 'COMMAND_ACK' and message.command == command_id:
            assert message.result == M.MAV_RESULT_ACCEPTED, message
            ack = True
        if status is not None and ack:
            return status
    raise AssertionError('Missing MISB stream status/ACK')


def check_mavproxy_live(infos, checkout):
    """Use this checkout's real camera module, then exercise both video backends."""
    sys.path.insert(0, str(checkout))
    sys.path.insert(0, str(checkout/'tests'))
    from test_mavproxy_camera import FakeMPState
    from MAVProxy.modules.mavproxy_camera import CameraModule
    from MAVProxy.modules.mavproxy_camera.video_view import video_pipeline
    from unittest import mock
    import cv2
    state = FakeMPState()
    state.settings.target_system = 42
    module = CameraModule(state)
    try:
        for info in infos.values():
            module.mavlink_packet(info)
        module.selected_camera = (42, 100)
        misb = [s for s in infos.values() if s.uri.endswith('.misb')]
        with mock.patch('MAVProxy.modules.lib.mp_util.has_wxpython', True):
            module._sync_menus()
            menu = next(item for item in module.menus[0].items if item.name == 'Video')
            for info in misb:
                assert any(info.name in item.name and item.returnkey.endswith('view %u' % info.stream_id) for item in menu.items)
        with mock.patch('MAVProxy.modules.lib.mp_util.has_wxpython', True), mock.patch(
                'MAVProxy.modules.mavproxy_camera.video_view.VideoView') as viewer:
            module.cmd_camera(['for', '42:100', 'view', 'rgb'])
            assert viewer.call_args.args[2].stream_id == 1
            assert viewer.call_args.args[3] == infos[1].uri
            assert viewer.call_args.args[3].endswith('.misb')
        module.views.clear()
        for info in misb:
            with mock.patch('MAVProxy.modules.mavproxy_camera.video_view.VideoView') as viewer:
                module.cmd_camera(['for', '42:100', 'view', str(info.stream_id)])
                assert viewer.call_args.args[3] == info.uri
            capture = cv2.VideoCapture(info.uri, cv2.CAP_FFMPEG,
                [cv2.CAP_PROP_OPEN_TIMEOUT_MSEC, 8000, cv2.CAP_PROP_READ_TIMEOUT_MSEC, 8000])
            try:
                assert capture.isOpened()
                for _ in range(3):
                    ok, image = capture.read()
                    assert ok and image.shape[:2] == (info.resolution_v, info.resolution_h)
            finally:
                capture.release()
            # The same pipeline passed to MPImage must decode the video pad
            # while leaving the KLV data pad unlinked.
            try:
                import gi
            except ImportError:
                continue
            gi.require_version('Gst', '1.0')
            from gi.repository import Gst
            Gst.init(None)
            pipeline = Gst.parse_launch(video_pipeline(info, info.uri, 75))
            sink = pipeline.get_by_name('appsink0')
            if sink is None:
                iterator = pipeline.iterate_sinks()
                _, sink = iterator.next()
            sink.set_property('sync', False)
            sink.set_property('max-buffers', 1)
            sink.set_property('drop', True)
            pipeline.set_state(Gst.State.PLAYING)
            try:
                sample = sink.emit('try-pull-sample', 15*Gst.SECOND)
                error = pipeline.get_bus().pop_filtered(Gst.MessageType.ERROR)
                assert sample is not None, error.parse_error() if error else 'No GStreamer frame'
                caps = sample.get_caps().get_structure(0)
                assert caps.get_value('width') == info.resolution_h
                assert caps.get_value('height') == info.resolution_v
            finally:
                pipeline.set_state(Gst.State.NULL)
        print('PASS MAVProxy: discovered MISB menu entries, selection, FFmpeg and GStreamer live decode', flush=True)
    finally:
        module.unload()


def check_mavvidplay(path):
    from MAVProxy.modules.lib.video_telemetry import VideoIndex, camera_pose
    from MAVProxy.modules.lib.mavvidplay import FrameReader
    import av
    index = VideoIndex(path)
    assert index.misb_count == len(index.samples)
    assert all(s.record and s.record['schema'] == 'apcg.telemetry.v1' for s in index.samples)
    assert any(abs(s.record['vehicle_attitude']['roll_rad']-math.radians(150)) < 1e-6 for s in index.samples)
    assert any(camera_pose(s.record) is not None for s in index.samples)
    with av.open(str(path)) as source:
        reference = [f.to_ndarray(format='bgr24') for f in source.decode(video=0)]
    reader = FrameReader(index)
    try:
        for generation, number in enumerate((0, len(reference)-1, 3, len(reference)//2, 1)):
            reader.request(number, generation)
            actual, actual_generation, image, error = reader.results.get(timeout=10)
            assert error is None, error
            assert (actual, actual_generation) == (number, generation)
            assert (image == reference[number]).all()
    finally:
        reader.stop()
    print('PASS mavvidplay: camera MISB recording, frame metadata, backward seek and GOP preroll', flush=True)


def run(backend, directory, codec, mavproxy=None):
    # Choose the checkout before loading helpers that may import MAVProxy.
    if mavproxy:
        sys.path.insert(0, str(mavproxy))
    global RTSP, command, port, ready, stop, mavutil, M
    from test_video_telemetry import RTSP, command, port, ready, stop, mavutil
    M = mavutil.mavlink
    build = ROOT/'build'/('sitl' if backend == 'mt11' else backend+'-sitl')
    directory.mkdir(parents=True)
    config = directory/'camera.ini'
    target = SITL_TARGETS[backend]
    sizes = ('1280x720', '1920x1080', '3840x2160', '2560x1440', '960x540', '854x480', '640x360', '426x240', '320x180')
    def smallest(role):
        mask = target[role+'_resolutions']
        return min((s for i, s in enumerate(sizes) if mask & (1 << i)), key=lambda s: int(s.split('x')[0]))
    config.write_text('[video]\ntelemetry=misb\n[mavlink]\nsystem_id=42\n[thermal]\nrecord_fps=0\n'
                      f'[stream.main]\ncodec={codec}\nresolution={smallest("main")}\n'
                      f'[stream.sub]\ncodec={codec}\nresolution={smallest("sub")}\n'
                      f'[recording]\nresolution={smallest("recording")}\n')
    gport, mport, rport = port(socket.SOCK_DGRAM), port(), port()
    cam_ready, gim_ready = directory/'camera.ready', directory/'gimbal.ready'
    env = dict(os.environ, CAMERA_APP_UART=f'udp://127.0.0.1:{gport}', CAMERA_APP_PORT=str(port()),
        CAMERA_APP_RTSP_PORT=str(rport), CAMERA_APP_MAVLINK_TCP_PORT=str(mport), CAMERA_APP_MAVLINK_UDP_PORT='0',
        CAMERA_APP_CONFIG=str(config), CAMERA_APP_READY_PATH=str(cam_ready), CAMERA_APP_RECORD_STATE=str(directory/'recording.state'),
        CAMERA_APP_RECORD_ROOT=str(directory/'record'), CAMERA_APP_CAPTURE_ROOT=str(directory/'capture'),
        CAMERA_APP_SITL_VIDEO1=str(ROOT/'build/sitl/rgb.h264'), CAMERA_APP_SITL_VIDEO2=str(ROOT/'build/sitl/thermal.h264'),
        CAMERA_GIMBAL_SITL_FPS='10', CAMERA_GIMBAL_SITL_PYTHON=sys.executable)
    # Fast deterministic rendering avoids map/network dependencies.
    camera = gimbal = link = None
    done = threading.Event()
    feeder = None
    with (directory/'camera.log').open('w') as clog, (directory/'gimbal.log').open('w') as glog:
        try:
            gimbal = subprocess.Popen([sys.executable, str(ROOT/'sitl/gimbal_sim.py'), '--backend', backend,
                '--port', str(gport), '--ready-file', str(gim_ready)], stdout=glog, stderr=subprocess.STDOUT)
            ready(gim_ready, gimbal)
            camera = subprocess.Popen([str(build/'camera-app'), '--backend', backend], env=env, stdout=clog, stderr=subprocess.STDOUT)
            ready(cam_ready, camera)
            link = mavutil.mavlink_connection(f'tcp:127.0.0.1:{mport}', source_system=42, source_component=1)
            def send():
                while not done.is_set():
                    link.mav.heartbeat_send(M.MAV_TYPE_QUADROTOR, M.MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, M.MAV_STATE_ACTIVE)
                    link.mav.global_position_int_send(0, -353632610, 1491652300, 620250, 36500, 2500, 1000, -200, 9000)
                    link.mav.attitude_send(0, math.radians(150), .6, .6, 0, 0, 0)
                    done.wait(.05)
            feeder = threading.Thread(target=send)
            feeder.start()
            assert link.recv_match(type='HEARTBEAT', blocking=True, timeout=5)
            time.sleep(.5)
            infos = discovered_streams(link)
            misb = [s for s in infos.values() if s.uri.endswith('.misb')]
            assert len(misb) == int(target['num_streams'])
            base_count = len(infos)-len(misb)
            raw = [s for s in infos.values() if s.type == 200]
            assert not raw or raw[0].stream_id == 3
            for variant in misb:
                source = infos[variant.stream_id+base_count]
                assert variant.stream_id <= int(target['num_streams'])
                assert source.name.endswith('APCG JSON')
                assert variant.name.endswith('MISB KLV')
                assert variant.uri == source.uri+'.misb'
                assert variant.type == M.VIDEO_STREAM_TYPE_RTSP
                for field in ('encoding', 'resolution_h', 'resolution_v', 'framerate', 'hfov', 'flags'):
                    assert getattr(variant, field) == getattr(source, field), (field, variant)
                status = streaming(link, variant.stream_id, False)
                assert not status.flags & M.VIDEO_STREAM_STATUS_FLAGS_RUNNING
                unchanged = discovered_streams(link)
                assert unchanged[source.stream_id].flags & M.VIDEO_STREAM_STATUS_FLAGS_RUNNING
                if raw:
                    assert unchanged[3].type == 200 and unchanged[3].flags == raw[0].flags
                assert streaming(link, variant.stream_id, True).flags & M.VIDEO_STREAM_STATUS_FLAGS_RUNNING
            print(f'PASS {backend}: MISB discovery, count, IDs, codec/geometry and independent status/control', flush=True)
            if mavproxy:
                check_mavproxy_live(infos, mavproxy)
            command(link, M.MAV_CMD_VIDEO_START_CAPTURE)
            for stream in range(int(SITL_TARGETS[backend]['num_streams'])):
                path = directory/f'live{stream+1}.ts'
                capture(rport, f'video{stream+1}.misb', path)
                check_file(path, 'hevc' if codec == 'h265' else 'h264', complete=False)
                if stream == 0:
                    for protocol in ('tcp', 'udp'):
                        probe = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-rtsp_transport', protocol,
                            '-analyzeduration', '1000000', '-probesize', '1000000',
                            '-read_intervals', '%+#20', '-show_entries', 'stream=codec_name:packet=codec_type,pts',
                            '-of', 'json', f'rtsp://127.0.0.1:{rport}/video1.misb'], timeout=30))
                        assert {s['codec_name'] for s in probe['streams']} == {'klv', 'hevc' if codec == 'h265' else 'h264'}
                        assert {p['codec_type'] for p in probe['packets']} == {'video', 'data'}
                    print(f'PASS {backend}: FFmpeg RTSP clients receive video and KLV over both TCP and UDP', flush=True)
                # Compatible elementary previews still expose APCG telemetry.
                client = RTSP(rport, f'video{stream+1}')
                try:
                    assert next(client.frames(1, 'hevc' if codec == 'h265' else 'h264'))[1]['position']
                finally:
                    client.close()
            command(link, M.MAV_CMD_VIDEO_STOP_CAPTURE)
            recordings = list((directory/'record').rglob('*.ts'))
            assert len(recordings) == int(SITL_TARGETS[backend]['num_recording_channels']), recordings
            assert not list((directory/'record').rglob('*.mp4'))
            for path in recordings:
                check_file(path)
                if mavproxy:
                    check_mavvidplay(path)
            # Saving a new format leaves the running mode and its endpoints
            # intact. The shared restart-only parameter path handles the switch.
            link.mav.param_set_send(42, 100, b'VIDEO_META_FMT', 0, M.MAV_PARAM_TYPE_REAL32)
            reply = link.recv_match(type='PARAM_VALUE', blocking=True, timeout=5)
            assert reply and reply.param_id == 'VIDEO_META_FMT' and reply.param_value == 0
            assert 'telemetry = apcg' in config.read_text()
            path = directory/'after-save.ts'
            capture(rport, 'video1.misb', path, seconds=1.5)
            check_file(path, 'hevc' if codec == 'h265' else 'h264', complete=False)
            done.set()
            feeder.join(3)
            link.close()
            link = None
            stop(camera)
            cam_ready.unlink(missing_ok=True)
            camera = subprocess.Popen([str(build/'camera-app'), '--backend', backend], env=env, stdout=clog, stderr=subprocess.STDOUT)
            ready(cam_ready, camera)
            with socket.create_connection(('127.0.0.1', rport), timeout=5) as sock:
                sock.settimeout(.5)
                sock.sendall(f'DESCRIBE rtsp://127.0.0.1:{rport}/video1.misb RTSP/1.0\r\nCSeq: 1\r\n\r\n'.encode())
                try:
                    response = sock.recv(4096)
                except socket.timeout:
                    response = b'' # xop ignores DESCRIBE for an unknown path.
                assert b'200 OK' not in response
            client = RTSP(rport, 'video1')
            try:
                assert next(client.frames(1, 'hevc' if codec == 'h265' else 'h264'))[1]['schema'] == 'apcg.telemetry.v1'
            finally:
                client.close()
            link = mavutil.mavlink_connection(f'tcp:127.0.0.1:{mport}', source_system=255, source_component=190)
            assert link.recv_match(type='HEARTBEAT', blocking=True, timeout=5)
            infos = discovered_streams(link)
            assert len(infos) == base_count and not any(s.uri.endswith('.misb') for s in infos.values())
            command(link, M.MAV_CMD_VIDEO_START_CAPTURE)
            time.sleep(2)
            command(link, M.MAV_CMD_VIDEO_STOP_CAPTURE)
            mp4s = list((directory/'record').rglob('*.mp4'))
            assert len(mp4s) == int(target['num_recording_channels'])
            for path in mp4s:
                records = subprocess.check_output([sys.executable, str(ROOT/'tools/video_telemetry.py'), str(path)], text=True)
                assert records and all(json.loads(line)['schema'] == 'apcg.telemetry.v1' for line in records.splitlines())
            print(f'PASS {backend}: parameter save preserves active MISB mode; restart restores APCG endpoints and recordings', flush=True)
        finally:
            done.set()
            if feeder:
                feeder.join(3)
            if link:
                link.close()
            stop(camera)
            stop(gimbal)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', choices=SITL_TARGETS, action='append')
    parser.add_argument('--codec', choices=('h264', 'h265'), default='h264')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--mavproxy', type=Path, help='also check this MAVProxy checkout and mavvidplay against the camera')
    args = parser.parse_args()
    directory = (args.output or Path(tempfile.mkdtemp(prefix='apcam-misb-'))).resolve()
    print(f'Evidence: {directory}', flush=True)
    for backend in args.backend or SITL_TARGETS:
        if args.codec == 'h265' and not SITL_TARGETS[backend]['stream_codecs'] & 2:
            continue
        run(backend, directory/backend, args.codec, args.mavproxy.resolve() if args.mavproxy else None)


if __name__ == '__main__':
    main()
