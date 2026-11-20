#!/usr/bin/env python3
"""Exercise fragmented MCU UART MAVLink 1/2, reply routing and SIYI coexistence."""
import os
import ipaddress
import json
from pathlib import Path
import pty
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlsplit
from pymavlink.dialects.v10 import ardupilotmega as v1
from pymavlink.dialects.v20 import ardupilotmega as v2
from test_a8_attitude import a8_frame, crc16, crc8, reserve_port, siyi, terminate, wait_path
from test_integration import private
from test_mavlink_integration import reserve_tcp_udp_port

binary = str(Path(sys.argv[1]).resolve())
backend = sys.argv[2] if len(sys.argv) > 2 else 'a8'
v3 = backend == 'mt11'
# Serial URLs require an up, non-loopback IPv4 interface. Keep the rest of
# the UART tests usable inside a network namespace with only loopback.
interfaces = json.loads(subprocess.check_output(['ip', '-j', '-4', 'address', 'show', 'up'], text=True))
have_camera_ipv4 = any('LOOPBACK' not in interface['flags'] and
                      any(address['family']=='inet' for address in interface.get('addr_info', []))
                      for interface in interfaces)
if not have_camera_ipv4:
    print('SKIP non-loopback stream URL assertion: no up non-loopback IPv4 interface')
with tempfile.TemporaryDirectory(prefix='mcu-uart-') as directory:
    root = Path(directory)
    master, slave = pty.openpty()
    sdk_port = reserve_tcp_udp_port()
    mav_port = reserve_port(socket.SOCK_STREAM)
    config = root/'camera.ini'
    config.write_text('[uart]\nprotocol=none\n[mavlink]\nsystem_id=0\ntcp_port=0\nudp_port=0\n')
    photo = root/'fixture.jpg'; photo.write_bytes(b'photo fixture')
    env = dict(os.environ, CAMERA_APP_READY_PATH=str(root/'ready'),
               CAMERA_APP_RECORD_ROOT=str(root/'record'), CAMERA_APP_CAPTURE_ROOT=str(root/'capture'),
               CAMERA_APP_LOG_ROOT=str(root/'logs'), CAMERA_APP_SITL_PHOTO=str(photo),
               CAMERA_APP_MAVLINK_TCP_PORT=str(mav_port), CAMERA_APP_MAVLINK_UDP_PORT='0')
    incoming = bytearray(); frames = []; messages = []
    parser = v2.MAVLink(None)
    sequence = 0
    check_atomic = True
    def outer(payload, command=0x3f, flags=9, destination=None):
        packet = bytearray(private(flags, 123, 0x2e, 0x34, command, payload) if v3 else
                           a8_frame(flags, 123, command, payload))
        if destination is not None:
            packet[9 if v3 else 8] = destination
            packet[-2:] = struct.pack('<H', crc16(packet[:-2]))
        return packet
    def send(payload, **kwargs):
        os.write(master, outer(payload, **kwargs))
    def wire(mod, name, *args, system=247, component=190):
        global sequence
        m = mod.MAVLink(None, srcSystem=system, srcComponent=component)
        m.seq = sequence; sequence = (sequence + 1) % 256
        return getattr(m, name+'_encode')(*args).pack(m)
    def command(mod, cmd, *params, target=100):
        return wire(mod, 'command_long', 1, target, cmd, 0, *(list(params)+[0]*(7-len(params))))
    def pump(seconds=.25):
        end = time.monotonic()+seconds
        while time.monotonic() < end:
            if not select.select([master], [], [], max(0,end-time.monotonic()))[0]: break
            incoming.extend(os.read(master, 8192))
            while len(incoming) >= (14 if v3 else 13):
                assert incoming[0] == 0xaa
                size = (14+int.from_bytes(incoming[3:5], 'little')) if v3 else 13+incoming[3]
                if len(incoming) < size: break
                f = bytes(incoming[:size]); del incoming[:size]
                assert crc16(f[:-2]) == int.from_bytes(f[-2:], 'little')
                assert crc8(f[:5 if v3 else 4]) == f[5 if v3 else 4]
                frames.append(f)
                off = 12 if v3 else 11
                assert f[off-1] != 0x3f, 'MCU 3f return path loses packets on A8'
                if f[off-1] != 0x35: continue
                assert f[1] == 0x0a and f[off-4:off] == bytes([0x34 if v3 else 0x2c, 0x2e, 0x6b, 0x35])
                if f[off:off+2] == b'\x55\x66': continue  # delegated SDK reply
                assert len(f[off:-2]) <= 255
                if check_atomic:
                    # The MCU can insert its own packets between private frames.
                    # Camera replies which fit must therefore be complete here.
                    local = v2.MAVLink(None)
                    complete = local.parse_buffer(f[off:-2]) or []
                    assert b''.join(m.get_msgbuf() for m in complete) == f[off:-2]
                for msg in parser.parse_buffer(f[off:-2]) or []:
                    assert msg.get_srcComponent() == 100, msg
                    messages.append(msg)
    def take(kind, timeout=2):
        end = time.monotonic()+timeout
        while time.monotonic() < end:
            for i,m in enumerate(messages):
                if m.get_type() == kind: return messages.pop(i)
            pump(.05)
        raise AssertionError('missing '+kind)
    def drain():
        pump(.15); messages.clear(); frames.clear()
    with (root/'app.log').open('w+') as log:
        proc = subprocess.Popen([binary, '--backend', backend, '--uart', os.ttyname(slave),
                                 '--port', str(sdk_port), '--config', str(config)],
                                env=env, stdout=log, stderr=log)
        network = None
        try:
            wait_path(root/'ready', proc); drain()
            network = socket.create_connection(('127.0.0.1',mav_port)); network.settimeout(.15)
            # SIYI input on the raw stream must not start MAVLink or execute actions.
            send(siyi(1,1,0x0c,b'\x02')); pump()
            assert not messages
            # Invalid envelopes must not establish identity or act on commands.
            hb = wire(v1,'heartbeat',2,3,0,0,3,system=1,component=1)
            for flags,dest in [(0x0a,None),(1,None),(9,0x77)]: send(hb, flags=flags,destination=dest)
            pump(); assert not messages
            # Identity heartbeat and requests split at every part of their header/body.
            for part in [hb[:1],hb[1:3],hb[3:8],hb[8:-1],hb[-1:]]:send(part)
            assert take('HEARTBEAT').get_srcSystem() == 1
            drain()
            # Drain network broadcasts so response isolation can be checked below.
            try:
                while network.recv(8192):pass
            except socket.timeout:pass
            for mod in [v1,v2]:
                data = command(mod,512,259)
                send(data[:7]); send(data[7:19]); send(data[19:])
                assert take('CAMERA_INFORMATION').model_name
                ack=take('COMMAND_ACK'); assert ack.command==512 and ack.result==0
                # MCU UART has no IP peer: advertise a camera interface, not
                # the wildcard fallback from a failed UDP route lookup.
                send(command(mod,512,269,1))
                stream=take('VIDEO_STREAM_INFORMATION')
                uri=urlsplit(stream.uri)
                assert uri.scheme=='rtsp' and uri.path=='/video1.misb', stream
                address=ipaddress.IPv4Address(uri.hostname)
                if have_camera_ipv4:
                    assert not address.is_unspecified and not address.is_loopback, stream
                ack=take('COMMAND_ACK'); assert ack.command==512 and ack.result==0
            # Host media has no tracking source, so it must not add periodic
            # idle tracking status traffic to the paced external UART.
            pump(.3)
            assert not any(m.get_type()=='CAMERA_TRACKING_IMAGE_STATUS' for m in messages)
            # No camera-info response may leak onto the TCP client.
            np=v2.MAVLink(None); seen=[]
            try:
                while True:seen.extend(np.parse_buffer(network.recv(8192)) or [])
            except socket.timeout:pass
            assert not any(m.get_type()=='CAMERA_INFORMATION' for m in seen)
            # Broadcast targets may be zero-trimmed by MAVLink 2.
            send(wire(v2,'command_long',0,0,512,0,259,0,0,0,0,0,0))
            take('CAMERA_INFORMATION'); take('COMMAND_ACK'); drain()
            # Gimbal control/requests are already handled by the MCU: no second action.
            send(command(v2,512,283,target=154))
            send(wire(v2,'command_long',0,0,512,0,283,0,0,0,0,0,0))
            send(wire(v2,'gimbal_device_set_attitude',1,154,0,[1,0,0,0],0,0,0))
            send(wire(v2,'command_int',1,154,6,195,0,0,0,0,0,0,-350000000,1490000000,100))
            pump(); assert not any(m.get_type()=='COMMAND_ACK' for m in messages)
            assert not any(f[11 if v3 else 10] in (0x06,0x0e) for f in frames)
            # Bad CRC is rejected; the following good request recovers.
            drain(); bad=bytearray(command(v2,512,259)); bad[-1]^=1; send(bad); pump()
            assert not any(m.get_type()=='CAMERA_INFORMATION' for m in messages)
            send(command(v1,512,259)); take('CAMERA_INFORMATION'); take('COMMAND_ACK')
            # MAVFTP read returns >255 bytes on the wire: exercise TX splitting.
            check_atomic = False
            def ftp(op, payload=b'', session=0, size=None):
                data=struct.pack('<HBBBBBBI',sequence,session,op,len(payload) if size is None else size,0,0,0,0)+payload
                send(wire(v2,'file_transfer_protocol',0,1,100,list(data.ljust(251,b'\0'))))
                return bytes(take('FILE_TRANSFER_PROTOCOL').payload)
            opened=ftp(4,b'/camera.xml\0'); assert opened[3]==128
            drain(); block=ftp(5,session=opened[2],size=239)
            assert block[3]==128 and block[4]==239 and block[12:17]==b'<?xml'
            assert sum(f[11 if v3 else 10]==0x35 for f in frames)>=2
            # MAVLink starts recording; raw SIYI cannot toggle it a second time.
            send(command(v1,2500)); assert take('COMMAND_ACK').result==0
            raw=siyi(1,9,0x0c,b'\x02'); send(raw); send(raw,command=0x35); pump()
            send(siyi(1,10,0x0a),command=0x35); pump()
            config_replies=[f[12 if v3 else 11:-2] for f in frames
                            if f[11 if v3 else 10]==0x35 and f[12 if v3 else 11:-2].startswith(b'\x55\x66')]
            assert config_replies[-1][7]==0x0a and config_replies[-1][11]==0
            # Photo commands act once via MAVLink, and once via delegated SIYI.
            send(command(v2,2000,0,0,1,0)); assert take('COMMAND_ACK').result==0; pump()
            assert len(list((root/'capture').glob('SITL_000001*.jpg')))==3
            raw=siyi(1,11,0x0c,b'\0'); send(raw); send(raw,command=0x35); pump()
            assert len(list((root/'capture').glob('*.jpg')))==6
            # Ordinary network MAVLink still uses its own reply route.
            drain(); network.sendall(command(v2,512,259)); found=False; end=time.monotonic()+2
            while time.monotonic()<end and not found:
                try: found=any(m.get_type()=='CAMERA_INFORMATION' for m in (np.parse_buffer(network.recv(8192)) or []))
                except socket.timeout:pass
            assert found; pump(); assert not any(m.get_type()=='CAMERA_INFORMATION' for m in messages)
            print('PASS',backend,'MCU UART: MAVLink 1/2, fragments, FTP chunks, SIYI coexistence, routing and duplicate protection')
        except BaseException:
            log.flush(); log.seek(0); print(log.read()[-5000:]); raise
        finally:
            if network:network.close()
            terminate(proc); os.close(master); os.close(slave)
