"""Streaming terrain upload, activation and rejection/rollback checks."""
import base64
import socket
import time
import binascii
import io
import os
from pathlib import Path
import struct
import zipfile


def tile():
    data = bytearray(2048)
    struct.pack_into('<QiiHHH', data, 0, (1 << 56)-1, 0, 0, 0, 1, 30)
    struct.pack_into('<896h', data, 22, *([500]*896))
    data[1821] = 1
    struct.pack_into('<H', data, 16, binascii.crc_hqx(data[:1821], 0))
    return bytes(data)


def archive(name, data, mode=None):
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        info = zipfile.ZipInfo(name)
        info.compress_type = zipfile.ZIP_DEFLATED
        if mode is not None:
            info.create_system = 3
            info.external_attr = mode << 16
        z.writestr(info, data)
    return out.getvalue()


def check_terrain_upload(request, root, csrf, port):
    headers = {'Content-Type': 'application/octet-stream', 'X-CSRF-Token': csrf}
    def upload(body, token=csrf):
        return request('POST', '/survey/terrain', 'initial-password', body,
                       {**headers, 'X-CSRF-Token': token})
    good = archive('N00E000.DAT', tile() + bytes(1024*1024))
    status, body, _ = upload(good, '0'*64)
    assert status == 403, (status, body)
    status, body, _ = upload(good)
    assert status == 200, (status, body)
    current = root/'mnt/TERRAIN/current'
    first = Path(current.read_text().strip())
    assert (current.parent/first/'N00E000.DAT').stat().st_size == 2048+1024*1024
    corrupt = bytearray(tile()); corrupt[25] ^= 1
    for bad in (archive('../N00E000.DAT', tile()), archive('N00E000.DAT', corrupt),
                archive('N00E000.DAT', tile(), 0o120777), good[:-16],
                archive('N00E000.DAT', b'garbage'), archive('S01E000.DAT', tile())):
        status, body, _ = upload(bad)
        assert status == 400, (status, body)
        assert Path(current.read_text().strip()) == first, 'bad upload replaced active terrain'
        assert len(list(current.parent.glob('dataset-*'))) == 1, 'staging files leaked'
    # Disconnect halfway through a streamed body: the installed version survives.
    auth = base64.b64encode(b'admin:initial-password').decode()
    with socket.create_connection(('127.0.0.1', port), timeout=5) as connection:
        connection.sendall((f'POST /survey/terrain HTTP/1.1\r\nHost: localhost\r\n'
                            f'Authorization: Basic {auth}\r\nX-CSRF-Token: {csrf}\r\n'
                            'Content-Length: 1000000\r\n\r\n').encode() + good[:64])
        connection.shutdown(socket.SHUT_WR)
        while connection.recv(4096):
            pass
    assert Path(current.read_text().strip()) == first
    assert len(list(current.parent.glob('dataset-*'))) == 1
    # Full-sized local archive: http.client streams the open file. This also
    # tests receive() bypassing its ordinary 256 KiB in-memory body limit.
    sample = os.environ.get('SURVEY_TERRAIN_ZIP')
    if sample:
        with open(sample, 'rb') as data:
            status, body, _ = request('POST', '/survey/terrain', 'initial-password', data,
                {**headers, 'Content-Length': str(Path(sample).stat().st_size)})
        assert status == 200, (status, body)
        assert b'30 m spacing' in body, body
        assert Path(current.read_text().strip()) != first
        assert not (current.parent/first).exists()
    print('PASS terrain ZIP streaming, block validation, atomic replacement and rejected-upload rollback')
