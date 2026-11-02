#!/usr/bin/env python3
"""Route native 1080p/4K frames into separate live and recorded outputs."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from pymavlink.dialects.v20 import ardupilotmega as mav
from test_z1mini import MCU, ROOT, port, run

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--orientation', choices=('upright', 'inverted'), default='upright')
args = parser.parse_args()

with tempfile.TemporaryDirectory(prefix='z1mini-4k-') as directory:
    root = Path(directory)
    for size, color in [('1920x1080', 'blue'), ('3840x2160', 'green')]:
        run('ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', f'color=c={color}:s={size}:r=30',
            '-frames:v', '1', '-c:v', 'libx264', '-preset', 'ultrafast', '-f', 'h264', str(root/(size+'.h264')))
    helper = root/'capture'
    helper.write_text('''#!/usr/bin/env python3
import os,struct,sys,time
assert sys.argv[1:] == ["fd:3", "0"] + (["--inverted"] if os.environ["TEST_Z1_INVERTED"] == "1" else [])
from pathlib import Path
root=Path(__file__).parent
frames=[(root/(size+'.h264')).read_bytes() for size in ['1920x1080','3840x2160']]
count=0
while True:
    # Interleave recording first, preserving each stream's independent clock.
    for stream in (1,0):
        data=frames[stream]
        packet=struct.pack('<IIQII',0x34363248,len(data),1000000+count*100000//3,1,stream)+data
        while packet:
            packet=packet[os.write(3,packet):]
    count+=1
    time.sleep(1/30)
''')
    helper.chmod(0o755)
    records=root/'records';records.mkdir()
    config=root/'camera.ini'
    text=(ROOT/'packaging/z1mini/camera.ini').read_text()
    text=text.replace('[recording]\nautorecord = false\nresolution = 1920x1080',
                      '[recording]\nautorecord = false\nresolution = 3840x2160')
    assert 'orientation = upright' in text
    text=text.replace('orientation = upright', 'orientation = ' + args.orientation)
    mavport=port()
    text=text.replace('tcp_port = 16001','tcp_port = 0').replace('udp_port = 16001',f'udp_port = {mavport}')
    config.write_text(text)
    rtsp_port=port(adjacent=True)
    mcu=MCU();ready=root/'ready'
    env=dict(os.environ,TEST_Z1_INVERTED=str(int(args.orientation == 'inverted')),CAMERA_APP_Z1_NATIVE_HELPER=str(helper),CAMERA_APP_RECORD_ROOT=str(records),
             CAMERA_APP_CAPTURE_ROOT=str(root),CAMERA_APP_READY_PATH=str(ready),CAMERA_APP_RTSP_PORT=str(rtsp_port))
    log=open(root/'app.log','w')
    app=subprocess.Popen([str(ROOT/'camera_app/build/z1mini-host/camera-app'),'--uart',mcu.path,'--config',str(config)],
                         env=env,stdout=log,stderr=log)
    udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);udp.bind(('127.0.0.1',0));udp.settimeout(.5)
    encoder=mav.MAVLink(None,srcSystem=1,srcComponent=1);parser=mav.MAVLink(None)
    def send(message): udp.sendto(message.pack(encoder),('127.0.0.1',mavport))
    def command(cmd):
        send(mav.MAVLink_command_long_message(1,100,cmd,0,0,0,0,0,0,0,0))
        end=time.monotonic()+3
        while time.monotonic()<end:
            try: data=udp.recv(4096)
            except socket.timeout: continue
            for msg in parser.parse_buffer(data) or []:
                if msg.get_type()=='COMMAND_ACK' and msg.command==cmd:
                    assert msg.result==mav.MAV_RESULT_ACCEPTED,msg
                    return
        raise AssertionError('missing command acknowledgement')
    try:
        end=time.monotonic()+18
        while not ready.exists() and app.poll() is None and time.monotonic()<end: time.sleep(.05)
        assert ready.exists(),(root/'app.log').read_text()
        send(mav.MAVLink_heartbeat_message(mav.MAV_TYPE_QUADROTOR,mav.MAV_AUTOPILOT_ARDUPILOTMEGA,0,0,mav.MAV_STATE_STANDBY,3))
        send(mav.MAVLink_global_position_int_message(1000,-353632610,1491652300,650000,120000,120,-230,40,9000))
        send(mav.MAVLink_attitude_message(1000,.1,-.2,1.5,0.,0.,0.))
        time.sleep(.1)
        # Start after the helper has already emitted headers: repeated IDRs must
        # still produce a self-contained recording. Verify a second recording too.
        for _ in range(2):
            command(mav.MAV_CMD_VIDEO_START_CAPTURE)
            live=root/f'live-{_}.mp4'
            with socket.create_connection(('127.0.0.1',rtsp_port+1),timeout=3) as connection, live.open('wb') as output:
                connection.sendall(b'\0')
                assert connection.recv(1)==b'\0'
                end=time.monotonic()+2
                while time.monotonic()<end:
                    data=connection.recv(65536)
                    assert data
                    output.write(data)
            command(mav.MAV_CMD_VIDEO_STOP_CAPTURE)
            info=json.loads(subprocess.check_output(['ffprobe','-v','error','-select_streams','v:0',
                '-show_entries','stream=width,height','-of','json',str(live)]))
            assert (info['streams'][0]['width'],info['streams'][0]['height'])==(1920,1080),info
        files=list(records.glob('*.ts'));assert len(files)==2
        for file in files:
            streams=json.loads(subprocess.check_output(['ffprobe','-v','error',
                '-show_entries','stream=codec_name','-of','json',str(file)]))['streams']
            assert [stream['codec_name'] for stream in streams] == ['h264','klv'], streams
            info=json.loads(subprocess.check_output(['ffprobe','-v','error','-count_frames','-select_streams','v:0',
                '-show_entries','stream=width,height,nb_read_frames','-of','json',str(file)]))['streams'][0]
            assert (info['width'],info['height'])==(3840,2160),info
            run('ffmpeg','-v','error','-i',str(file),'-f','null','-')
            metadata=file.with_suffix('.jsonl')
            run('python3',str(ROOT/'tools/video_telemetry.py'),str(file),'--output',str(metadata))
            rows=[json.loads(line) for line in metadata.read_text().splitlines()]
            assert len(rows)==int(info['nb_read_frames']) and len(rows)>10
            assert all(row['gimbal_attitude'] is not None for row in rows)
            assert rows[0]['position']['lat_e7']==-353632610
            assert rows[0]['vehicle_attitude'] is not None
            assert abs(rows[0]['hfov_deg']-54.7)<.001
        print('PASS native Z1', args.orientation, ': simultaneous 1080p live/4K MISB TS, repeated record start/stop, per-frame FC/gimbal telemetry')
    finally:
        app.terminate()
        try: app.wait(timeout=10)
        except subprocess.TimeoutExpired: app.kill();app.wait()
        udp.close();mcu.close();log.close()
        if app.returncode: print((root/'app.log').read_text())
