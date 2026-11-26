#!/usr/bin/env python3
"""Publish a recorded APCG raw thermal Matroska file to SupportProxy as a camera would.

The file is sent as its prefix followed by one Cluster at a time, paced by the
Cluster timestamps, using the camera's chunked HTTP PUT. For testing live viewers.
"""
import argparse
import mmap
import socket
import sys
import time
from urllib.parse import quote, urlsplit

EBML, SEGMENT, INFO, TRACKS, CLUSTER, TIMESTAMP = (
    0x1A45DFA3, 0x18538067, 0x1549A966, 0x1654AE6B, 0x1F43B675, 0xE7)
MAX_CLUSTER = 4 * 1024 * 1024


def element(data, at):
    """Return (id, payload offset, payload size or None if unknown) of the element at `at`."""
    first = data[at]
    id_len = 8 - first.bit_length() + 1
    ident = int.from_bytes(data[at:at + id_len], 'big')
    first = data[at + id_len]
    size_len = 8 - first.bit_length() + 1
    raw = int.from_bytes(data[at + id_len:at + id_len + size_len], 'big')
    size = raw & ((1 << (7 * size_len)) - 1)
    unknown = size == (1 << (7 * size_len)) - 1
    return ident, at + id_len + size_len, None if unknown else size


def split(data):
    """Return (prefix bytes, [(timestamp ticks, start, end)]) with Cluster offsets in data."""
    at, clusters = 0, []
    ident, payload, size = element(data, at)
    if ident != EBML or size is None:
        raise ValueError('not a Matroska file')
    at = payload + size
    ident, payload, size = element(data, at)
    if ident != SEGMENT:
        raise ValueError('no Matroska Segment')
    # the relay needs an unknown-size Segment, as a live camera sends
    head = [bytes(data[:at]), bytes.fromhex('1853806701ffffffffffffff')]
    at = payload
    seen = []
    while at < len(data):
        try:
            ident, payload, size = element(data, at)
        except IndexError:
            break
        if size is None or payload + size > len(data):
            break  # truncated tail of an interrupted recording
        if ident == CLUSTER:
            if seen != [INFO, TRACKS]:
                raise ValueError('expected Info and Tracks before the first Cluster')
            # the relay limits both the element and the HTTP chunk carrying it
            if payload + size - at > MAX_CLUSTER:
                raise ValueError('Cluster larger than the relay accepts')
            tid, tpayload, tsize = element(data, payload)
            if tid != TIMESTAMP:
                raise ValueError('Cluster does not start with its timestamp')
            clusters.append((int.from_bytes(data[tpayload:tpayload + tsize], 'big'), at, payload + size))
        elif ident in (INFO, TRACKS) and not clusters:
            seen.append(ident)
            head.append(bytes(data[at:payload + size]))
        at = payload + size
    if not clusters:
        raise ValueError('no Clusters found')
    return b''.join(head), clusters


def scale_ms(prefix):
    """Milliseconds per timestamp tick (TimestampScale, default 1 ms)."""
    at = prefix.find(bytes.fromhex('2ad7b1'))
    if at < 0:
        return 1.0
    size = prefix[at + 3] & 0x7F
    return int.from_bytes(prefix[at + 4:at + 4 + size], 'big') / 1e6


def publish(url, password, data, prefix, clusters, speed, tick_ms):
    host, port = url.hostname, url.port or 80
    target = url.path or '/v1.mkv'
    if password:
        target += '?pw=' + quote(password, safe='')
    with socket.create_connection((host, port), timeout=10) as sock:
        sock.sendall((
            f'PUT {target} HTTP/1.1\r\nHost: {host}:{port}\r\n'
            'Content-Type: video/x-matroska\r\nTransfer-Encoding: chunked\r\n'
            'Expect: 100-continue\r\n\r\n').encode())
        # the reply can arrive in more than one segment
        reply = b''
        while b'\r\n\r\n' not in reply and len(reply) < 4096:
            more = sock.recv(1024)
            if not more:
                break
            reply += more
        if not reply.startswith(b'HTTP/1.1 100'):
            raise ConnectionError('publish refused: ' + reply.split(b'\r\n')[0].decode(errors='replace'))
        sock.settimeout(30)

        def chunk(body):
            sock.sendall(b'%x\r\n' % len(body) + body + b'\r\n')

        chunk(prefix)
        start, first = time.monotonic(), clusters[0][0]
        for ticks, begin, end in clusters:
            wait = start + (ticks - first) * tick_ms / 1000 / speed - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            chunk(data[begin:end])
        # hold the last frame for one frame time before ending the publication
        if len(clusters) > 1:
            time.sleep(min(1.0, (clusters[-1][0] - clusters[-2][0]) * tick_ms / 1000 / speed))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('file', help='APCG raw thermal .mkv')
    parser.add_argument('url', help='publish URL, e.g. http://proxy.example.org:40003/v1.mkv')
    parser.add_argument('--pw', help='publish password')
    parser.add_argument('--pw-file', help='file holding the publish password')
    parser.add_argument('--loop', action='store_true',
                        help='publish again from the start, as a new publisher, for ever')
    parser.add_argument('--speed', type=float, default=1.0, help='playback rate (default 1)')
    parser.add_argument('--retry', type=float, default=5.0,
                        help='seconds between attempts with --loop (default 5)')
    args = parser.parse_args()
    url = urlsplit(args.url)
    if url.scheme != 'http' or not url.hostname:
        parser.error('the publish URL must be http://HOST:PORT/vN.mkv')
    password = args.pw
    if args.pw_file:
        with open(args.pw_file) as stream:
            password = stream.read().strip()
    # mapped, not read: only the Cluster being sent is copied
    with open(args.file, 'rb') as stream:
        data = mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ)
    prefix, clusters = split(data)
    tick_ms = scale_ms(prefix)
    seconds = (clusters[-1][0] - clusters[0][0]) * tick_ms / 1000
    print(f'{len(clusters)} frames over {seconds:.1f} s', flush=True)
    while True:
        try:
            publish(url, password, data, prefix, clusters, args.speed, tick_ms)
            print('published', flush=True)
        except (OSError, ConnectionError) as error:
            print(f'publish failed: {error}', file=sys.stderr, flush=True)
            if not args.loop:
                return 1
        if not args.loop:
            return 0
        time.sleep(args.retry if args.retry > 0 else 0)


if __name__ == '__main__':
    sys.exit(main())
