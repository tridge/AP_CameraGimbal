#!/usr/bin/env python3
"""Exercise real packet capture in an isolated network namespace.

Run: sudo unshare -n python3 camera_app/tests/test_network_capture.py
The caller must isolate the network namespace; no external traffic is needed.
"""
import argparse
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


def wait_for(predicate):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.02)
    raise AssertionError('timed out waiting for capture state')


def packets(path):
    raw = path.read_bytes()
    magic, major, minor, zone, precision, snaplen, linktype = struct.unpack_from('<IHHIIII', raw)
    assert (magic, major, minor, linktype) == (0xa1b2c3d4, 2, 4, 113)
    offset = 24
    frames = []
    while offset < len(raw):
        sec, usec, captured, original = struct.unpack_from('<IIII', raw, offset)
        assert abs(sec - time.time()) < 30 and 0 <= usec < 1000000
        assert 16 <= captured <= original and captured <= snaplen
        packet = raw[offset+16:offset+16+captured]
        assert len(packet) == captured
        kind, hardware, length = struct.unpack_from('!HHH', packet)
        assert kind in (0, 4) and hardware == 772 and length == 6  # loopback
        assert packet[14:16] == b'\x08\x00'  # IPv4 payload
        frames.append(packet[16:])
        offset += 16 + captured
    assert offset == len(raw)
    return frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--no-permission', action='store_true', help='check failure reporting without CAP_NET_RAW')
    args = parser.parse_args()
    if not args.no_permission:
        assert os.geteuid() == 0, 'Use sudo unshare -n to isolate test traffic'
        interfaces = {name for _, name in socket.if_nameindex()}
        assert interfaces == {'lo'}, f'Not an isolated namespace: {interfaces}'
        subprocess.run(['ip', 'link', 'set', 'lo', 'up'], check=True)
    with tempfile.TemporaryDirectory(prefix='network-capture-test-') as temporary:
        root = Path(temporary)
        binary = root/'capture'
        subprocess.run(['c++', '-std=gnu++17', '-O2', '-Wall', '-Wextra', '-Werror',
            '-DCA_NETWORK_CAPTURE_FILE_BYTES=4096', '-I'+str(ROOT/'include'),
            '-I'+str(ROOT/'camera_app/include'), str(ROOT/'camera_app/tests/test_network_capture.cpp'),
            str(ROOT/'camera_app/src/recording/network_capture.cpp'), '-pthread', '-o', str(binary)], check=True)
        (root/'DCIM').mkdir()
        ready = root/'ready'
        status = root/'ready.capture'
        directory = root/'DCIM/network'
        process = subprocess.Popen([str(binary), str(root/'DCIM/record'), str(ready)],
                                   stdin=subprocess.PIPE, text=True)
        def state():
            return status.read_text() if status.exists() else ''
        def command(text):
            process.stdin.write(text+'\n'); process.stdin.flush()
        try:
            wait_for(lambda: 'Off' in state())
            assert not directory.exists(), 'capture created files while disabled'
            command('on')
            if args.no_permission:
                wait_for(lambda: 'Capture failed:' in state())
                assert state().splitlines()[0].endswith(' 1')
                assert not list(directory.glob('*.pcap'))
                command('off'); wait_for(lambda: 'Off;' in state())
                print('PASS capture defaults off and reports missing packet-socket permission')
                return
            wait_for(lambda: 'Capturing' in state())
            receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            receiver.bind(('127.0.0.1', 0))
            sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            def send(index):
                payload = f'APCAM-CAPTURE-{index:04d}:'.encode() + b'x'*250
                sender.sendto(payload, receiver.getsockname())
                assert receiver.recv(2048) == payload
                time.sleep(.005)
            send(0)
            first = wait_for(lambda: next(directory.glob('*.pcap'), None))
            old = first.open('rb')
            old_inode = os.fstat(old.fileno()).st_ino
            command('on')  # repeated configure(true) must not restart capture
            time.sleep(.05)
            assert first.stat().st_ino == old_inode
            for index in range(1, 100):
                send(index)
            wait_for(lambda: first.stat().st_ino != old_inode)
            old_bytes = old.read()
            assert old_bytes.startswith(b'\xd4\xc3\xb2\xa1')
            old.seek(0); assert old.read() == old_bytes  # replaced slot keeps old download intact
            old.close()
            # TCP as well as UDP must be captured with complete payloads.
            listener = socket.socket()
            listener.bind(('127.0.0.1', 0)); listener.listen()
            client = socket.create_connection(listener.getsockname())
            server, _ = listener.accept()
            client.sendall(b'APCAM-TCP-CAPTURE')
            assert server.recv(1024) == b'APCAM-TCP-CAPTURE'
            client.close(); server.close(); listener.close()
            time.sleep(.2)
            command('off'); wait_for(lambda: 'Off;' in state())
            files = sorted(directory.glob('*.pcap'))
            assert len(files) == 4
            assert all(24 < path.stat().st_size <= 4096 for path in files)
            snapshots = {path: path.read_bytes() for path in files}
            for index in range(100, 103):
                send(index)
            assert snapshots == {path: path.read_bytes() for path in files}, 'capture continued after disable'
            frames = [frame for path in files for frame in packets(path)]
            assert any(b'APCAM-CAPTURE-0099:' in frame for frame in frames)
            assert any(b'APCAM-TCP-CAPTURE' in frame for frame in frames)
            reader = shutil.which('tcpdump')
            assert reader, 'Install tcpdump for independent PCAP decoding'
            decoded = ''.join(subprocess.check_output([reader, '-nn', '-r', str(path)],
                               stderr=subprocess.DEVNULL, text=True) for path in files)
            assert 'UDP, length' in decoded and 'Flags [' in decoded
            if shutil.which('tshark'):
                decoded = ''.join(subprocess.check_output(['tshark', '-r', str(path), '-T', 'fields',
                                 '-e', 'udp.payload', '-e', 'tcp.payload'],
                                 stderr=subprocess.DEVNULL, text=True) for path in files)
                assert b'APCAM-TCP-CAPTURE'.hex() in decoded
                assert b'APCAM-CAPTURE-0099:'.hex() in decoded
            # Start a fresh session without deleting the other retained slots.
            command('on'); wait_for(lambda: 'Capturing' in state()); send(200)
            time.sleep(.1)
            command('off'); wait_for(lambda: 'Off;' in state())
            assert len(list(directory.glob('*.pcap'))) == 4
            assert any(b'APCAM-CAPTURE-0200:' in p.read_bytes() for p in files)
            assert sum(p.read_bytes() != snapshots[p] for p in files) == 1
            # Missing storage fails visibly rather than falling back to RAM/rootfs.
            shutil.rmtree(directory); (root/'DCIM').rmdir()
            command('on'); wait_for(lambda: 'Capture failed:' in state())
            assert state().splitlines()[0].endswith(' 1')
            command('off'); wait_for(lambda: 'Off;' in state())
            receiver.close(); sender.close()
            print('PASS UDP/TCP PCAP payloads/timestamps, independent decoding, bounded rotation, retained downloads, live stop/restart and storage errors')
        finally:
            command('quit')
            process.communicate(timeout=5)
            assert process.returncode == 0


if __name__ == '__main__':
    main()
