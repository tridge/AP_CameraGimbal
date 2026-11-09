#!/usr/bin/env python3
"""Exercise MT11's FFV1 stream through the actual MAVProxy thermal reader."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time

os.environ['MAVLINK20'] = '1'

REPO = Path(__file__).resolve().parents[1]


def check_recording(link, stream, root, config, armed, ThermalReader):
    """Exercise persistent rates and the existing recording policy end to end."""
    from pymavlink import mavutil
    import numpy as np
    import struct
    M = mavutil.mavlink

    def receive(kind, predicate):
        deadline = time.monotonic()+5
        while time.monotonic()<deadline:
            message=link.recv_match(type=kind, blocking=True, timeout=.2)
            if message and predicate(message): return message
        raise AssertionError('Missing '+kind)

    def param(name, value, extended=False):
        if extended:
            link.mav.param_ext_set_send(42,100,name.encode(),struct.pack('<i',value),M.MAV_PARAM_EXT_TYPE_INT32)
            ack=receive('PARAM_EXT_ACK',lambda m:m.param_id==name and m.param_result!=M.PARAM_ACK_IN_PROGRESS)
            assert ack.param_result==M.PARAM_ACK_ACCEPTED, ack
        else:
            link.mav.param_set_send(42,100,name.encode(),value,M.MAV_PARAM_TYPE_REAL32)
            ack=receive('PARAM_VALUE',lambda m:m.param_id==name)
            assert ack.param_value==value, ack

    def command(command, arg):
        link.mav.command_long_send(42,100,command,0,arg,0,0,0,0,0,0)
        ack=receive('COMMAND_ACK',lambda m:m.command==command)
        assert ack.result==M.MAV_RESULT_ACCEPTED, ack

    files=lambda: set((root/'record').glob('*.raw*.mkv'))
    param('RAW_RECORD_FPS',10,extended=True)
    param('RAW_STREAM_FPS',2)
    param('REC_AUTOSTART',2)
    time.sleep(.3)
    assert not files(), 'Recorded while disarmed'
    armed.set()
    deadline=time.monotonic()+5
    while not files() and time.monotonic()<deadline: time.sleep(.05)
    assert files(), 'No raw recording on arm'
    first=next(iter(files()))
    # A client that cannot drain frames must not gate the SD recording.
    from urllib.parse import urlparse
    uri=urlparse(stream.uri)
    slow=socket.socket(); slow.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,1024)
    slow.connect((uri.hostname,uri.port)); slow.sendall(b'GET /thermal.mkv HTTP/1.1\r\nHost: test\r\n\r\n')
    reader=ThermalReader(stream.uri)
    try:
        stamps=[]
        for pixels,m in reader.frames():
            stamps.append(m['capture_monotonic_us'])
            if len(stamps)==7: break
        rate=6e6/(stamps[-1]-stamps[0])
        assert 1.5<rate<2.2, rate
        param('RAW_STREAM_FPS',1)
        frames=reader.frames()
        for _ in range(2): next(frames)  # drain frames queued before the rate change
        stamps=[next(frames)[1]['capture_monotonic_us'] for _ in range(4)]
        rate=3e6/(stamps[-1]-stamps[0])
        assert .85<rate<1.15, rate
        param('RAW_STREAM_FPS',2)
        print('PASS live rate change to 1 fps on the existing connection',flush=True)
    finally:
        reader.close(); slow.close()
    before=first.stat().st_size
    command(M.MAV_CMD_VIDEO_STOP_STREAMING,3)
    time.sleep(.8)
    assert first.stat().st_size>before, 'Stream stop halted recording'
    command(M.MAV_CMD_VIDEO_START_STREAMING,3)
    armed.clear()
    time.sleep(.5)
    stopped_size=first.stat().st_size
    time.sleep(.3)
    assert first.stat().st_size==stopped_size, 'Recording continued after disarm'
    reader=ThermalReader(str(first))
    try:
        captured=[]
        for pixels,m in reader.frames():
            expected=(np.arange(640*512,dtype=np.uint32)+m['frame_id']*257).astype(np.uint16).reshape(512,640)
            np.testing.assert_array_equal(pixels,expected)
            captured.append(m['capture_monotonic_us'])
        rate=(len(captured)-1)*1e6/(captured[-1]-captured[0])
        assert 9<rate<10.5, (len(captured),rate)
    finally: reader.close()
    print('PASS independent stream/record rates, slow client, stream stop, arm/disarm and lossless recorded samples',flush=True)
    # Manual recording works with automatic recording disabled. Live rate zero
    # closes only the raw file; re-enabling starts a fresh, non-overwriting file.
    param('REC_AUTOSTART',0)
    command(M.MAV_CMD_VIDEO_START_CAPTURE,0)
    time.sleep(.5)
    second=next(iter(files()-{first}))
    size=second.stat().st_size
    param('RAW_RECORD_FPS',4)
    time.sleep(.7)
    assert files()=={first,second} and second.stat().st_size>size
    param('RAW_RECORD_FPS',0,extended=True)
    size=second.stat().st_size
    time.sleep(.4)
    assert second.stat().st_size==size
    # Exercise the same atomic INI replacement performed by the web service.
    text=config.read_text().replace('record_fps = 0','record_fps = 4')
    assert text!=config.read_text(), text
    temporary=config.with_suffix('.new'); temporary.write_text(text); temporary.replace(config)
    deadline=time.monotonic()+4
    while len(files())<3 and time.monotonic()<deadline: time.sleep(.05)
    assert len(files())==3
    time.sleep(1)
    command(M.MAV_CMD_VIDEO_STOP_CAPTURE,0)
    assert second.stat().st_size==size
    # Enabled policy starts immediately, independently of the armed state.
    param('REC_AUTOSTART',1)
    deadline=time.monotonic()+4
    while len(files())<4 and time.monotonic()<deadline: time.sleep(.05)
    assert len(files())==4
    time.sleep(.4)
    param('REC_AUTOSTART',0)
    param('RAW_STREAM_FPS',5)
    saved=config.read_text()
    assert 'stream_fps = 5' in saved and 'record_fps = 4' in saved
    print('PASS manual/automatic recording, live disable/re-enable, INI reload and persistent rate parameters',flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mavproxy', type=Path, required=True)
    parser.add_argument('--build', type=Path, default=REPO/'build/sitl')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--terrain', action='store_true', help='test terrain rendering using offline textured meshes')
    parser.add_argument('--viewer', action='store_true', help='also smoke-test the wx viewer on DISPLAY')
    args = parser.parse_args()
    sys.path.insert(0, str(args.mavproxy.resolve()))
    from pymavlink import mavutil
    from pymavlink.quaternion import Quaternion
    from test_video_telemetry import port, ready, RTSP
    import numpy as np
    from MAVProxy.modules.mavproxy_camera.thermal_stream import ThermalReader, projection_pose
    root = args.output or Path(tempfile.mkdtemp(prefix='raw-thermal-sitl-'))
    root.mkdir(parents=True, exist_ok=True)
    for name in ('camera.ready', 'gimbal.ready'):
        (root/name).unlink(missing_ok=True)
    print('Artifacts:', root, flush=True)
    gp, mp, rp, raw = port(socket.SOCK_DGRAM), port(), port(), port()
    config = root/'camera.ini'
    config.write_text('[mavlink]\nsystem_id=0\n[recording]\nautorecord=false\n')
    env = dict(os.environ, CAMERA_APP_BACKEND='mt11', CAMERA_APP_UART=f'udp://127.0.0.1:{gp}',
        CAMERA_APP_PORT=str(port()), CAMERA_APP_RTSP_PORT=str(rp),
        CAMERA_APP_RAW_THERMAL_PORT=str(raw),
        CAMERA_APP_MAVLINK_TCP_PORT=str(mp), CAMERA_APP_MAVLINK_UDP_PORT='0',
        CAMERA_APP_CONFIG=str(config), CAMERA_APP_READY_PATH=str(root/'camera.ready'),
        CAMERA_APP_RECORD_STATE=str(root/'recording.state'), CAMERA_APP_RECORD_ROOT=str(root/'record'),
        CAMERA_APP_CAPTURE_ROOT=str(root/'capture'),
        CAMERA_APP_SITL_VIDEO1=str(args.build.resolve()/'rgb.h264'),
        CAMERA_APP_SITL_VIDEO2=str(args.build.resolve()/'thermal.h264'))
    env.pop('CAMERA_APP_SITL_TERRAIN', None)
    if args.terrain:
        env['CAMERA_APP_SITL_TERRAIN'] = str(REPO/'sitl/test_raw_thermal_terrain_renderer.py')
    stop = threading.Event()
    armed = threading.Event()
    camera = gimbal = link = feeder = None
    with (root/'camera.log').open('w') as clog, (root/'gimbal.log').open('w') as glog:
        try:
            gimbal = subprocess.Popen([sys.executable, str(REPO/'sitl/gimbal_sim.py'), '--backend', 'mt11',
                '--port',str(gp),'--ready-file',str(root/'gimbal.ready')],stdout=glog,stderr=subprocess.STDOUT)
            ready(root/'gimbal.ready',gimbal,root/'gimbal.log')
            camera = subprocess.Popen([str(args.build.resolve()/'camera-app')],env=env,stdout=clog,stderr=subprocess.STDOUT)
            ready(root/'camera.ready',camera,root/'camera.log')
            link = mavutil.mavlink_connection(f'tcp:127.0.0.1:{mp}',source_system=255,source_component=190)
            M = mavutil.mavlink
            def feed():
                fc = mavutil.mavlink_connection(f'tcp:127.0.0.1:{mp}',source_system=42,source_component=1)
                started = time.monotonic()
                try:
                    while not stop.is_set():
                        t = int((time.monotonic()-started)*1000)+1000
                        fc.mav.heartbeat_send(M.MAV_TYPE_QUADROTOR,M.MAV_AUTOPILOT_ARDUPILOTMEGA,M.MAV_MODE_FLAG_SAFETY_ARMED if armed.is_set() else 0,0,M.MAV_STATE_ACTIVE)
                        fc.mav.global_position_int_send(t,-353632610,1491652300,650000,70000,0,0,0,0)
                        fc.mav.autopilot_state_for_gimbal_device_send(42,0,t*1000,
                            Quaternion([.1,-.2,.6]).q,0,0,0,0,0,0,0,0,angular_velocity_z=0)
                        stop.wait(.05)
                finally: fc.close()
            feeder = threading.Thread(target=feed); feeder.start()
            assert link.recv_match(type='HEARTBEAT',blocking=True,timeout=5)
            time.sleep(.3)
            link.mav.command_long_send(42,100,M.MAV_CMD_REQUEST_MESSAGE,0,269,0,0,0,0,0,0)
            streams = {}
            deadline = time.monotonic()+5
            while len(streams)<5 and time.monotonic()<deadline:
                m = link.recv_match(type='VIDEO_STREAM_INFORMATION',blocking=True,timeout=.5)
                if m: streams[m.stream_id]=m
            assert set(streams)=={1,2,3,4,5}, streams
            assert streams[1].type==M.VIDEO_STREAM_TYPE_RTSP and streams[2].type==M.VIDEO_STREAM_TYPE_RTSP
            stream=streams[3]
            assert stream.type==200 and stream.count==5 and stream.encoding==0
            assert (stream.resolution_h,stream.resolution_v)==(640,512)
            print('PASS third-stream discovery and unchanged display stream types',flush=True)
            if args.terrain:
                time.sleep(2)
            samples=[]
            for connection in range(2):
                reader=ThermalReader(stream.uri)
                try:
                    terrain_deadline = time.monotonic() + 15
                    for pixels,metadata in reader.frames():
                        if args.terrain:
                            import hashlib
                            assert metadata['simulation_source'] == 'terrain' and metadata['rotation_deg'] == 0
                            if pixels.std() < 1 and time.monotonic() < terrain_deadline:
                                continue  # allow asynchronous terrain/imagery uploads to settle
                            assert pixels.dtype == np.uint16 and pixels.std() > 1, metadata
                            assert 14.99 <= metadata['minimum_c'] <= metadata['maximum_c'] <= 45.01
                            assert hashlib.sha256(pixels.astype('<u2').tobytes()).hexdigest() == metadata['telemetry']['test_sha256']
                        else:
                            expected=(np.arange(640*512,dtype=np.uint32)+metadata['frame_id']*257).astype(np.uint16).reshape(512,640)
                            assert np.array_equal(pixels,expected), 'raw bits or frame/metadata association changed'
                            assert pixels.dtype==np.uint16 and pixels.min()==0 and pixels.max()==65535
                            assert metadata['simulated'] and metadata['rotation_deg']==180
                        assert projection_pose(metadata) is not None
                        assert metadata['telemetry']['position']['lat_e7']==-353632610
                        samples.append(metadata)
                        if len(samples)>=(connection+1)*6: break
                finally: reader.close()
            assert all(a['capture_monotonic_us']<b['capture_monotonic_us'] for a,b in zip(samples,samples[1:]))
            (root/'metadata.json').write_text(json.dumps(samples,indent=2))
            print('PASS lossless samples, exact per-frame metadata, native geometry, capture pose and reconnect',flush=True)
            for command, running in [(M.MAV_CMD_VIDEO_STOP_STREAMING,False),(M.MAV_CMD_VIDEO_START_STREAMING,True)]:
                link.mav.command_long_send(42,100,command,0,3,0,0,0,0,0,0)
                m=link.recv_match(type='VIDEO_STREAM_STATUS',blocking=True,timeout=3)
                assert m and m.stream_id==3 and bool(m.flags&1)==running
                ack=link.recv_match(type='COMMAND_ACK',blocking=True,timeout=3)
                assert ack and ack.command==command and ack.result==M.MAV_RESULT_ACCEPTED
            reader=ThermalReader(stream.uri)
            try: assert next(reader.frames())[0].dtype==np.uint16
            finally: reader.close()
            client=RTSP(rp,'video1')
            try: assert next(client.frames(1,'h264'))
            finally: client.close()
            print('PASS raw start/stop and concurrent ordinary RTSP video',flush=True)
            # A second receiver and a client that never completes its HTTP
            # request must not stall the first receiver or the camera.
            slow=socket.create_connection(('127.0.0.1',raw),timeout=3)
            slow.sendall(b'GET /thermal.mkv HTTP/1.1\r\n')
            readers=[]
            try:
                readers=[ThermalReader(stream.uri), ThermalReader(stream.uri)]
                for receiver in readers:
                    assert next(receiver.frames())[0].shape == (512,640)
                time.sleep(2.1)
                assert slow.recv(1)==b''
            finally:
                slow.close()
                for receiver in readers: receiver.close()
            print('PASS concurrent readers and incomplete HTTP client timeout',flush=True)
            # A streaming client that keeps sending must not make the worker
            # spin on a permanently readable socket.
            def camera_cpu_while_streaming(extra):
                client=socket.create_connection(('127.0.0.1',raw),timeout=3)
                try:
                    client.sendall(b'GET /thermal.mkv HTTP/1.1\r\nHost: test\r\n\r\n')
                    client.recv(65536)
                    # sent once streaming, so it is not part of the request
                    if extra: client.sendall(extra)
                    fields=open(f'/proc/{camera.pid}/stat').read().rsplit(')',1)[1].split()
                    start=int(fields[11])+int(fields[12])
                    deadline=time.monotonic()+3
                    while time.monotonic()<deadline: client.recv(65536)
                    fields=open(f'/proc/{camera.pid}/stat').read().rsplit(')',1)[1].split()
                    return (int(fields[11])+int(fields[12])-start)/os.sysconf('SC_CLK_TCK')
                finally: client.close()
            quiet=camera_cpu_while_streaming(b'')
            chatty=camera_cpu_while_streaming(b'unexpected client bytes')
            assert chatty<quiet+1.0, (quiet,chatty)
            print(f'PASS streaming client input does not spin the worker ({quiet:.2f}s vs {chatty:.2f}s CPU)',flush=True)
            if args.terrain:
                print('PASS terrain renderer to C++ bridge to FFV1 to MAVProxy, bit-exact native pixels', flush=True)
                return
            subprocess.run([sys.executable,str(REPO/'tools/raw_thermal_probe.py'),stream.uri,
                '--mavproxy',str(args.mavproxy.resolve()),'--frames','2','--test-pattern',
                '--output',str(root/'probe')],check=True,timeout=15)

            check_recording(link,stream,root,config,armed,ThermalReader)
            if args.viewer:
                from types import SimpleNamespace
                from MAVProxy.modules.mavproxy_camera.thermal_view import ThermalView
                viewer=ThermalView(SimpleNamespace(logdir=str(root),module=lambda _:None),
                    SimpleNamespace(system_id=42,component_id=100,label=lambda:'SITL thermal test'),
                    stream,stream.uri)
                try:
                    deadline=time.monotonic()+8
                    while time.monotonic()<deadline:
                        viewer.check_events()
                        assert viewer.alive(), 'viewer process exited'
                        time.sleep(.05)
                    assert viewer.shown is not None, viewer.error
                    viewer.paused=True
                    viewer._save()
                    saved=next((root/'raw_thermal').glob('*.bin'))
                    assert saved.stat().st_size==640*512*2
                    assert viewer._pixel(0,0) is not None
                finally: viewer.close()
                print('PASS wx thermal window, temperature readout and raw save',flush=True)

        finally:
            stop.set()
            if feeder: feeder.join(5)
            if link: link.close()
            for process in (camera,gimbal):
                if process and process.poll() is None:
                    process.terminate()
                    try: process.wait(5)
                    except subprocess.TimeoutExpired: process.kill(); process.wait()

if __name__=='__main__': main()
