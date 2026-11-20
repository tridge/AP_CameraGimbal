#!/usr/bin/env python3
"""ZR10 MCU routing regression with a PTY and real observed replies."""
import os
import pathlib
import pty
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
from test_a8_attitude import mavutil, a8_frame, crc16, read_a8_frames, reserve_port, siyi, terminate, wait_path

binary=str(pathlib.Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory() as d:
    root=pathlib.Path(d)
    master,slave=pty.openpty()
    port=reserve_port(socket.SOCK_DGRAM)
    mavport=reserve_port(socket.SOCK_STREAM)
    config=root/'camera.ini'
    config.write_text('[mount]\norientation=upright\n[mavlink]\nsystem_id=1\nposition_targeting=false\ntcp_port=0\nudp_port=0\n[stream.main]\nresolution=2560x1440\n')
    env=dict(os.environ,CAMERA_APP_READY_PATH=str(root/'ready'),CAMERA_APP_RECORD_ROOT=str(root/'record'),CAMERA_APP_CAPTURE_ROOT=str(root/'capture'))
    env['CAMERA_APP_MAVLINK_TCP_PORT']=str(mavport)
    proc=subprocess.Popen([binary,'--backend','zr10','--uart',os.ttyname(slave),'--port',str(port),'--config',str(config)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    client=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);client.settimeout(2);client.connect(('127.0.0.1',port))
    def find_query(opcode, command=0x16):
        end=time.monotonic()+3
        while time.monotonic()<end:
            for f in read_a8_frames(master,1,.3):
                assert f[7:10]==bytes([0x2c,0x2e,0x6b])
                if f[10]==command and f[18]==opcode:return f
        raise AssertionError('missing UART query '+hex(opcode))
    def find_native(sub, payload, flags=9):
        end=time.monotonic()+3
        while time.monotonic()<end:
            for f in read_a8_frames(master,1,.3):
                if f[10]==sub:
                    assert f[1]==flags and f[11:-2]==payload, f.hex()
                    return f
        raise AssertionError("missing native command " + hex(sub))
    def reply(opcode):
        end=time.monotonic()+3
        while time.monotonic()<end:
            data=client.recv(2048)
            if data[7]==opcode:
                assert crc16(data[:-2])==int.from_bytes(data[-2:],'little')
                return data
        raise AssertionError('missing public reply')
    try:
        wait_path(root/'ready',proc)
        assert 'backend=zr10' in (root/'ready').read_text()
        startup=read_a8_frames(master,2,1.2)
        assert startup and all(f[10]==0x16 and f[18] in (1,0x18,0x0d) for f in startup)
        client.send(siyi(1,10,0x0d));find_query(0x0d)
        payload=struct.pack('<6h',-13,0,0,0,-3,3)
        os.write(master,a8_frame(0x0a,379,0x16,siyi(2,55821,0x0d,payload)))
        assert reply(0x0d)[8:-2]==payload, 'ZR10 must preserve uncalibrated raw attitude'
        client.send(siyi(1,11,0x16));find_query(0x16)
        os.write(master,a8_frame(0x0a,386,0x16,siyi(2,55827,0x16,b'\x1e\0')))
        assert reply(0x16)[8:-2]==b'\x1e\0', 'must not advertise the A8 digital zoom limit'
        client.send(siyi(1,12,0x0a));data=reply(0x0a)
        assert len(data[8:-2])>=6 and data[11]==0
        # Configuration request from MCU is answered back to MCU, not looped.
        os.write(master,a8_frame(9,384,0x35,siyi(1,123,0x0a)))
        frame=find_query(0x0a, 0x35)
        assert frame[1]==0x0a
        assert frame[13]&2 and len(frame[11:-2])>=16
        client.send(siyi(1,13,0x0f,b'\x02\0'));find_native(0x37,b'\x02\0',8)
        for opcode,sub,payload in [(7,6,b'\x08\xf8'),(7,6,b'\0\0'),(8,7,b'\x01'),(5,4,b'\0')]:
            client.send(siyi(1,15,opcode,payload));find_native(sub,payload)
        os.write(master,a8_frame(9,399,0x35,siyi(1,98,0x0f,b'\x03\0')))
        find_native(0x37,b'\x03\0',8)
        # Unknown camera-side lens handling must not loop the request back.
        os.write(master,a8_frame(9,400,0x35,siyi(1,99,4,b'\x01')))
        for f in read_a8_frames(master,1,.3):assert f[10]!=0x16 or f[18]!=4
        client.send(siyi(1,14,0x18));find_query(0x18)
        os.write(master,a8_frame(0x0a,401,0x16,siyi(2,55828,0x18,b'\x02\0')))
        assert reply(0x18)[8:-2]==b'\x02\0'
        mav=mavutil.mavlink_connection(f'tcp:127.0.0.1:{mavport}',source_system=250)
        try:
            mav.mav.gimbal_device_set_attitude_send(1,mavutil.mavlink.MAV_COMP_ID_GIMBAL,
                0,[float('nan')]*4,float('nan'),-0.1,0.1)
            find_native(6,b'\x0a\xf6')
            mav.mav.gimbal_device_set_attitude_send(1,mavutil.mavlink.MAV_COMP_ID_GIMBAL,
                mavutil.mavlink.GIMBAL_DEVICE_FLAGS_NEUTRAL,[float('nan')]*4,
                float('nan'),float('nan'),float('nan'))
            find_native(7,b'\x01')
            # A single MAVLink zoom step must be bounded, never continuous zoom.
            for kind,value,opcode,payload in [(0,1,0x0f,b'\x02\x01'),
                                            (2,100,0x0f,b'\x0a\0'),
                                            (1,0,0x05,b'\0')]:
                mav.mav.command_long_send(1,100,mavutil.mavlink.MAV_CMD_SET_CAMERA_ZOOM,
                                          0,kind,value,0,0,0,0,0)
                find_native(0x37 if opcode==0x0f else 4,payload,8 if opcode==0x0f else 9)
                ack=mav.recv_match(type='COMMAND_ACK',blocking=True,timeout=3)
                assert ack and ack.result==mavutil.mavlink.MAV_RESULT_ACCEPTED
            mav.mav.command_long_send(1,100,mavutil.mavlink.MAV_CMD_SET_CAMERA_FOCUS,
                                      0,4,0,0,0,0,0,0)
            ack=mav.recv_match(type='COMMAND_ACK',blocking=True,timeout=3)
            assert ack and ack.result==mavutil.mavlink.MAV_RESULT_UNSUPPORTED
            # Real MCU zoom replies drive both video stream telemetry messages.
            # Both streams use the same optical lens; beyond 10x is uncalibrated.
            for zoom,hfov in [(1,72),(10,7),(30,0)]:
                client.send(siyi(1,20+zoom,0x18));find_query(0x18)
                value=bytes([zoom,0])
                os.write(master,a8_frame(0x0a,500+zoom,0x16,siyi(2,600+zoom,0x18,value)))
                assert reply(0x18)[8:-2]==value
                for stream in (1,2):
                    for name in ('VIDEO_STREAM_INFORMATION','VIDEO_STREAM_STATUS'):
                        msgid=getattr(mavutil.mavlink,'MAVLINK_MSG_ID_'+name)
                        mav.mav.command_long_send(1,100,mavutil.mavlink.MAV_CMD_REQUEST_MESSAGE,
                                                  0,msgid,stream,0,0,0,0,0)
                        message=mav.recv_match(type=name,blocking=True,timeout=3)
                        assert message and message.stream_id==stream and message.hfov==hfov, message
        finally:
            mav.close()
        print('PASS ZR10: readiness/model, independent MCU replies, raw attitude, real zoom limit, local configuration, delegated request routing and bounded MAVLink zoom')
    finally:
        terminate(proc);client.close();os.close(master);os.close(slave)
        err=proc.stderr.read().decode()
        if proc.returncode not in (0,-15):raise AssertionError(err)
