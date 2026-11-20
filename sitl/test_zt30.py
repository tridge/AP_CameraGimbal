#!/usr/bin/env python3
"""ZT30 scene/module-zoom/FOV contracts over UniGCS, SIYI and MAVLink.

Uses the real SITL binary with isolated ports/files and a nominal simulated
MCU. This does not qualify the physical ZT30 UART or SigmaStar driver.
"""
import argparse
import math
import os
import re
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

os.environ.setdefault('MAVLINK20', '1')
from pymavlink import mavutil
from test_unigcs import Client, LongClient, camera_version
from test_sitl import reserve_port, terminate, request as public_request, verify_rtsp_video
from test_mavlink_parameters import receive, drain, wait_ready

M = mavutil.mavlink
CAMERA = M.MAV_COMP_ID_CAMERA
ROOT = Path(__file__).resolve().parents[1]


def run(build, orientation, no_version_reply=False):
    with tempfile.TemporaryDirectory(prefix='zt30-contract-') as temporary:
        root = Path(temporary)
        ports = {key: reserve_port(kind) for key,kind in
                 [('gimbal',socket.SOCK_DGRAM),('public',socket.SOCK_STREAM),
                  ('private',socket.SOCK_STREAM),('discovery',socket.SOCK_DGRAM),
                  ('mavlink',socket.SOCK_STREAM)]}
        rtsp = reserve_port(socket.SOCK_STREAM, span=3)
        config = root/'camera.ini'
        config.write_text('[mavlink]\nsystem_id=42\n[logging]\ndisarmed=false\n'
                          '[recording]\nresolution=1280x720\n[stream.main]\nresolution=1280x720\n')
        env = dict(os.environ, CAMERA_APP_BACKEND='zt30',
                   CAMERA_APP_UART=f"udp://127.0.0.1:{ports['gimbal']}",
                   CAMERA_APP_PORT=str(ports['public']),
                   CAMERA_APP_UNIGCS_PORT=str(ports['private']),
                   CAMERA_APP_DISCOVERY_PORT=str(ports['discovery']),
                   CAMERA_APP_RTSP_PORT=str(rtsp), CAMERA_APP_CONFIG=str(config),
                   CAMERA_APP_READY_PATH=str(root/'ready'),
                   CAMERA_APP_MAVLINK_TCP_PORT=str(ports['mavlink']),
                   CAMERA_APP_MAVLINK_UDP_PORT='0',
                   CAMERA_APP_LOG_ROOT=str(root/'logs'),
                   CAMERA_APP_CAPTURE_ROOT=str(root/'capture'),
                   CAMERA_APP_RECORD_ROOT=str(root/'record'),
                   CAMERA_APP_RECORD_STATE=str(root/'record.state'),
                   CAMERA_APP_SITL_VIDEO1=str(build/'rgb.h264'),
                   CAMERA_APP_SITL_VIDEO2=str(build/'thermal.h264'),
                   CAMERA_APP_SITL_RENDERER=str(ROOT/'sitl/terrain_video.py'),
                   CAMERA_GIMBAL_SITL_PYTHON=sys.executable,
                   CAMERA_APP_SITL_PHOTO=str(build/'photo.jpg'), CAMERA_GIMBAL_SITL_FPS='5')
        env.pop('CAMERA_APP_SITL_TERRAIN',None)
        processes=[]; clients=[]; link=None
        with (root/'process.log').open('w') as log:
            try:
                gimbal=subprocess.Popen([sys.executable,str(ROOT/'sitl/gimbal_sim.py'),
                    '--backend','zt30','--orientation',orientation,'--port',str(ports['gimbal']),
                    '--ready-file',str(root/'gimbal.ready')]+(['--no-version-reply'] if no_version_reply else []),stdout=log,stderr=log)
                processes.append(gimbal); wait_ready(root/'gimbal.ready',gimbal)
                camera=subprocess.Popen([str(build/'camera-app')],env=env,stdout=log,stderr=log)
                processes.append(camera); wait_ready(root/'ready',camera)
                link=mavutil.mavlink_connection(f"tcp:127.0.0.1:{ports['mavlink']}",
                    source_system=255, source_component=190)
                assert link.wait_heartbeat(timeout=5)

                def request_message(message_id):
                    drain(link)
                    link.mav.command_long_send(42,CAMERA,M.MAV_CMD_REQUEST_MESSAGE,0,
                        message_id,0,0,0,0,0,0)
                def fov(expected,aspect=16/9):
                    request_message(M.MAVLINK_MSG_ID_CAMERA_FOV_STATUS)
                    message=receive(link,'CAMERA_FOV_STATUS')
                    if expected is None:
                        assert math.isnan(message.hfov) and math.isnan(message.vfov),message
                    else:
                        assert abs(message.hfov-expected)<.02,message
                        vertical=math.degrees(2*math.atan(math.tan(math.radians(expected)/2)/aspect))
                        assert abs(message.vfov-vertical)<.02,message
                def ext_set(name,value,accepted=True):
                    drain(link)
                    wire=struct.pack('<f',value) if name in ('CAM_ZOOM','CAM_OPT_ZOOM') else struct.pack('<i',value)
                    kind=M.MAV_PARAM_EXT_TYPE_REAL32 if name in ('CAM_ZOOM','CAM_OPT_ZOOM') else M.MAV_PARAM_EXT_TYPE_INT32
                    link.mav.param_ext_set_send(42,CAMERA,name.encode(),wire.ljust(128,b'\0'),kind)
                    ack=receive(link,'PARAM_EXT_ACK',lambda m:m.param_id==name and m.param_result!=M.PARAM_ACK_IN_PROGRESS)
                    assert (ack.param_result==M.PARAM_ACK_ACCEPTED)==accepted,ack
                def ext_zoom(expected,wide=False):
                    name='CAM_ZOOM' if wide else 'CAM_OPT_ZOOM'
                    drain(link);link.mav.param_ext_request_read_send(42,CAMERA,name.encode(),-1)
                    value=receive(link,'PARAM_EXT_VALUE',lambda m:m.param_id==name)
                    assert abs(struct.unpack('<f',value._param_value_raw[:4])[0]-expected)<.01,value
                def module_zoom(client,value):
                    assert client.request(0xd2,b'\x0a'+struct.pack('<H',round(value*10)))==b'\x0a\x01'
                    assert client.request(0xd1,b'\x0a')==b'\x0a'+struct.pack('<H',round(value*10))

                xml=ET.parse(build.parent/'camera-definitions/zt30.xml') if (build.parent/'camera-definitions/zt30.xml').exists() else ET.parse(ROOT/'build/camera-definitions/zt30.xml')
                parameters={p.get('name'):p for p in xml.findall('parameters/parameter')}
                assert parameters['CAM_ZOOM'].get('max')=='1'
                assert parameters['CAM_OPT_ZOOM'].get('max')=='180'
                assert parameters['CAM_LENS'].get('default')=='0'

                verify_rtsp_video(rtsp,'video1',(1280,720),'h264')
                verify_rtsp_video(rtsp,'video2',(640,512),'h264')
                c=Client(ports['private']);clients.append(c)
                identity=c.request(0x94)
                assert identity[:4]==camera_version(0x7b),identity
                assert c.request(0x92)==b'\x00\x02'
                assert c.request(0xd0,b'\x0a')==b'\x0a'+struct.pack('<HHHH',1800,10,300,10)
                ext_zoom(1); fov(58.1)
                assert c.request(0xa3,b'\x01')==b'\x01\x00'
                for value,expected in [(1,58.1),(30,2.14),(180,math.degrees(2*math.atan(math.tan(math.radians(2.14)/2)/6)))]:
                    module_zoom(c,value);ext_zoom(value);fov(expected)
                    assert c.request(0x92)==b'\x00\x02'
                drain(link)
                link.mav.command_long_send(42,CAMERA,M.MAV_CMD_SET_CAMERA_ZOOM,0,2,100,0,0,0,0,0)
                ack=receive(link,'COMMAND_ACK',lambda m:m.command==M.MAV_CMD_SET_CAMERA_ZOOM)
                assert ack.result==M.MAV_RESULT_ACCEPTED,ack
                ext_zoom(180)
                ext_set('CAM_OPT_ZOOM',90);ext_zoom(90)
                # Wide must ignore background module zoom and refuse lens-local zoom.
                ext_set('CAM_LENS',0);ext_zoom(1,wide=True);fov(84.5)
                assert c.request(0x92)==b'\x01\x02'
                for value in (1,30,90,180):
                    module_zoom(c,value);ext_zoom(1,wide=True);ext_zoom(value);fov(84.5)
                ext_set('CAM_ZOOM',2,accepted=False)
                request_message(M.MAVLINK_MSG_ID_CAMERA_SETTINGS)
                settings=receive(link,'CAMERA_SETTINGS');assert math.isfinite(settings.zoomLevel) and settings.zoomLevel==0,settings
                # Thermal remains selected and uses the assumed MT11 FOV independently of RGB zoom.
                assert c.request(0x93,b'\x02\x00')==b'\x02\x00'
                for value in (1,90,180):
                    module_zoom(c,value);assert c.request(0x92)==b'\x02\x00';fov(24.2,640/512)
                assert c.request(0x94)==camera_version(0x7b)+bytes((0,0,1,0x87))
                assert c.request(0xaa,b'\x01'+struct.pack('<HHHH',200,200,400,400))==b'\x01'
                assert c.request(0xaa,b'\x00'+bytes(8))==b'\x01'
                ext_set('CAM_ZOOM',2,accepted=False)
                # Composition must survive same-source module changes.
                assert c.request(0x93,b'\x03\x00')==b'\x03\x02'
                module_zoom(c,30);assert c.request(0x92)==b'\x03\x02';fov(None)
                ext_set('CAM_LENS',1);ext_zoom(30);fov(2.14)
                c.sock.close();time.sleep(.15)
                old=LongClient(ports['private']);clients.append(old)
                assert old.request(0x94)[:4]==camera_version(0x7b)
                module_zoom(old,1);fov(58.1)
                old.sock.close()
                with socket.create_connection(('127.0.0.1',ports['public']),2) as public:
                    version=public_request(public,5,0x01)
                    header=(ROOT/'camera_app/build/binlog_version.h').read_text()
                    parsed=re.search(r'CA_FIRMWARE_VERSION \"v(\d+)\.(\d+)(?:\.(\d+))?',header)
                    expected=bytes((int(parsed[3] or 0),int(parsed[2]),int(parsed[1]),0x7b)) if parsed else bytes((0,0,0,0x7b))
                    assert version[:4]==expected and version[4:]==bytes.fromhex('0000007a00000079' if no_version_reply else '0102007a06010079'),version
                    assert public_request(public,1,0x02).startswith(b'7A')
                    assert public_request(public,2,0x16)==bytes((180,0))
                    assert public_request(public,3,0x0f,bytes((90,0)))==b'\x01'
                    assert public_request(public,4,0x18)==bytes((90,0))
                fov(math.degrees(2*math.atan(math.tan(math.radians(2.14)/2)/3)))
                assert camera.poll() is None
                print(f'PASS ZT30 {orientation}: short/long UniGCS, module/lens zoom, scenes, FOV, MAVLink, RTSP and assumed thermal geometry',flush=True)
            except Exception:
                log.flush();print((root/'process.log').read_text()[-10000:],file=sys.stderr)
                raise
            finally:
                for c in clients:c.sock.close()
                if link:link.close()
                for process in reversed(processes):terminate(process)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,default=ROOT/'build/zt30-sitl')
    parser.add_argument('--no-version-reply',action='store_true',help='test unavailable MCU firmware versions')
    parser.add_argument('--orientation',choices=('upright','inverted','both'),default='both')
    args=parser.parse_args()
    for orientation in ('upright','inverted') if args.orientation=='both' else (args.orientation,):
        run(args.build.resolve(),orientation,args.no_version_reply)
