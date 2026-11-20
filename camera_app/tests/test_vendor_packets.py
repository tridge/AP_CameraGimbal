#!/usr/bin/env python3
"""Reconstruct vendor packets from BIN logs and compare with TCP/UDP/UART traffic."""
import argparse
import errno
import os
from pathlib import Path
import pty
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time

from test_integration import siyi, private, crc16
from test_mavlink_integration import mavutil, reserve_tcp_udp_port, terminate, wait_path


def decode(root):
    result = []
    for path in sorted(root.glob('*.BIN')):
        reader = mavutil.mavlink_connection(str(path))
        groups = {}
        while (m := reader.recv_match()) is not None:
            assert m.get_type() != 'BAD_DATA', m
            if m.get_type() not in ('SIIN','SIOU','XFIN','XFOU'):
                continue
            if m.Id not in groups:
                assert m.Ofs == 0
                groups[m.Id] = (m, bytearray())
            first, data = groups[m.Id]
            assert (m.TimeUS, m.Len, m.Res, m.get_type()) == (first.TimeUS, first.Len, first.Res, first.get_type())
            assert m.Ofs == len(data), m
            chunk = struct.pack('<96h', *m.D1, *m.D2, *m.D3)
            n = min(192, m.Len-m.Ofs)
            assert not any(chunk[n:]), 'chunk padding was not zeroed'
            data.extend(chunk[:n])
        reader.close()
        for first, data in groups.values():
            assert len(data) == first.Len, 'partial packet logged'
            result.append((first, bytes(data)))
    assert len({m.Id for m, _ in result}) == len(result), 'packet ID reused across sessions'
    return result


def probe(binary):
    with tempfile.TemporaryDirectory(prefix='vendor-binlog-probe-') as tmp:
        root = Path(tmp)
        subprocess.run([binary, tmp], check=True)
        packets = decode(root)
        payload = bytes(range(256))*16
        large = siyi(1, 0x1234, 0x7f, payload[:2038])
        native = private(8, 65535, 0x2e, 0x34, 0xee, payload)
        old_query=bytes.fromhex('5566aabb01000000000000802d977a34b7ad40eb')
        assert [p for _, p in packets] == [large, large, native, old_query, b'\xb5\x9a\x01\x02']
        assert [m.Res for m, _ in packets] == [0, -errno.EPIPE, 0, 0, 0]
        assert [(m.Src,m.Dst) for m, _ in packets] == [(0,0),(0,0),(0x2e,0x34),(0,0),(0,0)]
        assert [(m.Cmd,m.Seq) for m,_ in packets] == [(0x7f,0x1234),(0x7f,0x1234),(0xee,65535),(0x80,0),(65535,65535)]
        assert packets[3][0].get_type()=='SIIN' and packets[3][0].Proto==6
        print('PASS maximum SIYI/private frame chunking, raw bytes, errno, failure metadata and log sessions')


def xf_packet(order, extra=b''):
    p = bytearray(70)
    p[:2] = b'\xa8\xe5'; p[4]=2; p[69]=order
    p += extra
    struct.pack_into('<H',p,2,len(p)+2)
    return p + struct.pack('>H',crc16(p))


def read_frame(sock, xf=False):
    data=bytearray()
    header=4 if xf else 8
    while len(data)<header:
        part=sock.recv(header-len(data)); assert part
        data+=part
    length=struct.unpack_from('<H', data, 2 if xf else 3)[0]+(0 if xf else 10)
    while len(data)<length:
        part=sock.recv(length-len(data)); assert part
        data+=part
    return bytes(data)


def run(binary, backend, simulator):
    vendor, mav, gimbal_port = [reserve_tcp_udp_port() for _ in range(3)]
    with tempfile.TemporaryDirectory(prefix='vendor-binlog-'+backend+'-') as tmp:
        root=Path(tmp)
        config, ready, gimbal_ready=[root/n for n in ('camera.ini','ready','gimbal.ready')]
        config.write_text('[mavlink]\nsystem_id=1\n[logging]\ndisarmed=true\n' +
                          ('[uart]\nprotocol=siyi\n' if backend != 'z1mini' else ''))
        master, slave=pty.openpty()
        env=dict(os.environ, CAMERA_APP_UART=f'udp://127.0.0.1:{gimbal_port}',
            CAMERA_APP_EXTERNAL_UART=os.ttyname(slave), CAMERA_APP_PORT=str(vendor), CAMERA_APP_CONFIG=str(config),
            CAMERA_APP_MAVLINK_TCP_PORT=str(mav), CAMERA_APP_MAVLINK_UDP_PORT='0',
            CAMERA_APP_READY_PATH=str(ready), CAMERA_APP_LOG_ROOT=str(root/'logs'),
            CAMERA_APP_RECORD_ROOT=str(root/'record'), CAMERA_APP_CAPTURE_ROOT=str(root/'capture'),
            CAMERA_APP_RECORD_STATE=str(root/'recording.state'))
        processes, sockets, expected=[], [], []
        xf = backend=='z1mini'
        serial_rx=bytearray()
        def serial_frame():
            deadline=time.monotonic()+3
            while time.monotonic()<deadline:
                if len(serial_rx)>=8:
                    n=struct.unpack_from('<H',serial_rx,3)[0]+10
                    if len(serial_rx)>=n:
                        frame=bytes(serial_rx[:n]);del serial_rx[:n];return frame
                if select.select([master],[],[],.05)[0]: serial_rx.extend(os.read(master,4096))
            raise AssertionError('no SIYI UART response')
        def expect(out, link, port, data):
            expected.append(('XF' if xf else 'SI', out, link, port, data))
        with (root/'test.log').open('w+') as log:
            try:
                gimbal=subprocess.Popen([sys.executable, simulator, '--backend', backend, '--port', str(gimbal_port),
                    '--ready-file', str(gimbal_ready)], stdout=log,stderr=log)
                processes.append(gimbal); wait_path(gimbal_ready,gimbal)
                camera=subprocess.Popen([binary,'--backend',backend],env=env,stdout=log,stderr=log)
                processes.append(camera); wait_path(ready,camera)
                tcp=socket.create_connection(('127.0.0.1',vendor));tcp.settimeout(3);sockets.append(tcp)
                local=tcp.getsockname()[1]
                if not xf:
                    keepalive=siyi(1, 450, 0, b'\0')
                    tcp.sendall(keepalive);expect(False,2,local,keepalive)
                # Deliberately split one TCP frame and concatenate two more.
                query=xf_packet(0) if xf else siyi(1,451,1)
                tcp.sendall(query[:5]);time.sleep(.02);tcp.sendall(query[5:])
                expect(False,2,local,query);expect(True,2,local,read_frame(tcp,xf))
                tcp.sendall(query*2)
                for _ in range(2):
                    expect(False,2,local,query);expect(True,2,local,read_frame(tcp,xf))
                if xf:
                    # Unknown order with the largest supported packet, including payload beyond one log chunk.
                    long=xf_packet(0x7f,bytes(range(184)))
                    assert len(long)==256
                    tcp.sendall(long);expect(False,2,local,long)
                    reply=read_frame(tcp,True);assert reply[70]==1;expect(True,2,local,reply)
                else:
                    # Unknown SIYI command is still captured in full (and may be tunneled to the MCU).
                    long=siyi(1,452,0x7f,bytes(range(256))*5)
                    tcp.sendall(long);expect(False,2,local,long)
                    os.write(master,query[:3]);os.write(master,query[3:])
                    expect(False,3,0,query);expect(True,3,0,serial_frame())
                udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);udp.settimeout(3);sockets.append(udp)
                udp.bind(('127.0.0.1',0))
                receiver=udp
                if xf:
                    receiver=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);receiver.settimeout(3);sockets.append(receiver)
                    receiver.bind(('127.0.0.1',2338))
                udp.sendto(query,('127.0.0.1',vendor));expect(False,1,udp.getsockname()[1],query)
                expect(True,1,receiver.getsockname()[1],receiver.recv(4096))
                # CRC-invalid frames are rejected, then a valid packet verifies stream resynchronization.
                corrupt=bytearray(query);corrupt[-1]^=1
                tcp.sendall(corrupt+query)
                expect(False,2,local,query);expect(True,2,local,read_frame(tcp,xf))
                time.sleep(.25) # Collect periodic MCU feedback and drain pending sends.
                terminate(camera)
                # Native MCU responses can also be forwarded asynchronously.
                # Retain every remaining wire frame rather than assuming only request/reply traffic.
                def remainder(data, link, port):
                    while data:
                        assert len(data)>=(4 if xf else 8)
                        n=struct.unpack_from('<H',data,2 if xf else 3)[0]+(0 if xf else 10)
                        assert 0<n<=len(data)
                        expect(True,link,port,bytes(data[:n]));del data[:n]
                tail=bytearray()
                while part:=tcp.recv(4096): tail.extend(part)
                remainder(tail,2,local)
                receiver.setblocking(False)
                while True:
                    try: tail=bytearray(receiver.recv(4096))
                    except BlockingIOError: break
                    remainder(tail,1,receiver.getsockname()[1])
                if not xf:
                    while select.select([master],[],[],0)[0]:serial_rx.extend(os.read(master,4096))
                    remainder(serial_rx,3,0)
                packets=decode(root/'logs')
                actual=[]
                for m,p in packets:
                    if m.Proto == (4 if xf else 1):
                        assert (m.Src,m.Dst)==(0,0)
                        assert m.IP==(0 if m.Lnk==3 else 0x7f000001),m
                        actual.append((m.get_type()[:2],m.get_type().endswith('OU'),m.Lnk,m.Port,p))
                for item in expected:
                    assert item in actual, ('missing',backend,item)
                    actual.remove(item)
                assert not actual, ('unexpected public packets',backend,actual)
                native=[(m,p) for m,p in packets if m.Proto==(5 if xf else 2 if backend=='mt11' else 3)]
                assert any(m.get_type().endswith('IN') for m,p in native)
                assert any(m.get_type().endswith('OU') for m,p in native)
                for m,p in native:
                    assert m.Lnk==5
                    if xf:
                        assert p[:2] in (b'\xa9\x5b',b'\xb5\x9a') and crc16(p)==0
                        assert m.Cmd==m.Seq==65535
                    else:
                        assert p[0]==0xaa and struct.unpack_from('<H',p,len(p)-2)[0]==crc16(p[:-2])
                        source_offset=8 if backend=='mt11' else 7
                        assert (m.Src,m.Dst)==tuple(p[source_offset:source_offset+2])
                if backend=='mt11':
                    assert any(m.Cmd==0x13 and m.get_type()=='SIOU' for m,p in native), 'startup packet missing'
                    assert any(long in p for m,p in native), 'unknown public packet not retained in tunnel'
                print('PASS',backend,'public TCP/UDP'+('/UART' if not xf else ''), 'and MCU packets, endpoints, startup, fragmentation, keepalive/unknown commands')
            except BaseException:
                log.flush();log.seek(0);print(log.read()[-10000:]);raise
            finally:
                for sock in sockets:sock.close()
                for proc in reversed(processes):terminate(proc)
                os.close(master);os.close(slave)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe')
    parser.add_argument('--simulator', default='../sitl/gimbal_sim.py')
    for backend in ('mt11','a8','zr10','z1mini'):parser.add_argument('--'+backend)
    args=parser.parse_args()
    if args.probe:probe(str(Path(args.probe).resolve()))
    for backend in ('mt11','a8','zr10','z1mini'):
        binary=getattr(args,backend)
        if binary:run(str(Path(binary).resolve()),backend,str(Path(args.simulator).resolve()))
