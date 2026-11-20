#!/usr/bin/env python3
"""Exercise MCU-delegated A8 camera operations and isolate their replies."""
import os
from pathlib import Path
import pty
import socket
import struct
import subprocess
import sys
import tempfile

from test_a8_attitude import a8_frame, crc16, read_a8_frames, siyi, terminate, wait_path
from test_mavlink_integration import reserve_tcp_udp_port

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='a8-delegated-') as directory:
    root = Path(directory)
    master, slave = pty.openpty()
    port = reserve_tcp_udp_port()
    config = root / 'camera.ini'
    config.write_text('[mount]\norientation=upright\n[uart]\nprotocol=none\n'
                      '[mavlink]\ntcp_port=0\nudp_port=0\n')
    photo = root / 'source.jpg'
    photo.write_bytes(b'photo fixture')
    env = dict(os.environ, CAMERA_APP_READY_PATH=str(root/'ready'),
               CAMERA_APP_RECORD_ROOT=str(root/'record'), CAMERA_APP_CAPTURE_ROOT=str(root/'capture'),
               CAMERA_APP_LOG_ROOT=str(root/'logs'), CAMERA_APP_SITL_PHOTO=str(photo),
               CAMERA_APP_MAVLINK_TCP_PORT='0', CAMERA_APP_MAVLINK_UDP_PORT='0')
    client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client.connect(('127.0.0.1', port))
    client.settimeout(2)
    with (root/'app.log').open('w+') as log:
        proc = subprocess.Popen([binary, '--backend', 'a8', '--uart', os.ttyname(slave),
                                 '--port', str(port), '--config', str(config)],
                                env=env, stdout=log, stderr=log)
        def network(op, payload=b''):
            client.send(siyi(1, 33, op, payload))
            response = client.recv(2048)
            assert response[7] == op and crc16(response[:-2]) == int.from_bytes(response[-2:], 'little')
            return response[8:-2]

        def delegated(op, payload=b'', reply_op=None):
            os.write(master, a8_frame(9, 0x2324, 0x35, siyi(1, 0x6402, op, payload)))
            frames = read_a8_frames(master, 1, .8)
            assert len(frames) == 1, [f.hex() for f in frames]
            frame = frames[0]
            assert frame[1] == 0x0a and frame[7:11] == bytes([0x2c, 0x2e, 0x6b, 0x35]), frame.hex()
            response = frame[11:-2]
            assert response[:3] == b'\x55\x66\x02', response.hex()
            assert response[7] == (op if reply_op is None else reply_op)
            assert len(response) == struct.unpack_from('<H', response, 3)[0] + 10
            assert crc16(response[:-2]) == int.from_bytes(response[-2:], 'little')
            return response[8:-2]

        try:
            wait_path(root/'ready', proc)
            assert len(read_a8_frames(master, 4)) == 4
            os.write(master, a8_frame(0x0a, 1, 0x17, b'\x01') +
                     a8_frame(0x0a, 2, 0x15, b'\x01'))
            assert network(0x0a)[3:6] == b'\x00\x01\x01'
            # Exact external-UART capture: this must get a reply even with
            # uart.protocol=none (which does not own the MCU connector).
            os.write(master, bytes.fromhex('aa09020ad324232e2c6b35556601000002640a81dc7003'))
            frames = read_a8_frames(master, 1)
            assert len(frames) == 1 and frames[0][1] == 0x0a and frames[0][10] == 0x35
            assert frames[0][13] == 2 and frames[0][18] == 0x0a
            assert frames[0][22:25] == b'\x00\x01\x01'
            assert delegated(0x16) == b'\x06\x00'
            assert delegated(0x0f, b'\x02\x05') == b'\x01'
            assert delegated(0x18) == b'\x02\x05'
            assert delegated(0x05, b'\x01') == struct.pack('<H', 26)
            assert delegated(0x0c, b'\x02', 0x0b) == b'\x05'
            assert delegated(0x0a)[3] == 1
            assert delegated(0x0c, b'\x02', 0x0b) == b'\x06'
            assert delegated(0x0a)[3] == 0
            assert delegated(0x0c, b'\x00', 0x0b) == b'\x00'
            assert list((root/'capture').glob('*.jpg'))
            assert all(p.read_bytes() == b'photo fixture' for p in (root/'capture').glob('*.jpg'))
            photo.unlink()
            assert delegated(0x0c, b'\x00', 0x0b) == b'\x01'
            # None of these responses may reach the last network SDK client.
            client.settimeout(.2)
            try:
                leaked = client.recv(2048)
                raise AssertionError('delegated response leaked to UDP: ' + leaked.hex())
            except socket.timeout:
                pass
            client.settimeout(2)
            # Invalid/unsupported delegated requests must not echo back to
            # the MCU for it to delegate again, nor change camera state.
            bad_crc = bytearray(siyi(1, 1, 0x0f, b'\x03\0')); bad_crc[-1] ^= 1
            wrong_destination = bytearray(a8_frame(9, 1, 0x35, siyi(1, 1, 0x0c, b'\x02')))
            wrong_destination[8] = 0x34
            wrong_destination[-2:] = struct.pack('<H', crc16(wrong_destination[:-2]))
            for frame in [a8_frame(9, 1, 0x35, bad_crc), wrong_destination,
                          a8_frame(1, 1, 0x35, siyi(1, 1, 0x0c, b'\x02')),
                          a8_frame(0x0a, 1, 0x35, siyi(1, 1, 0x0c, b'\x02')),
                          a8_frame(9, 1, 0x35, siyi(1, 1, 0x7f)),
                          a8_frame(9, 1, 0x35, siyi(1, 1, 0x0f, b'\x03'))]:
                os.write(master, frame)
                assert not read_a8_frames(master, 1, .15), frame.hex()
            assert network(0x0a)[3] == 0
            assert network(0x18) == b'\x02\x06'
            assert not read_a8_frames(master, 1, .15), 'network reply routed to MCU'
            print('PASS A8 delegated config, zoom, recording, photo/error, routing isolation and loop prevention')
        except BaseException:
            log.flush(); log.seek(0); print(log.read()[-6000:]); raise
        finally:
            terminate(proc); client.close(); os.close(master); os.close(slave)
