#!/usr/bin/env python3
"""Download an open camera-app recording via Files, then kill its writer."""
import argparse
import base64
import concurrent.futures
import fcntl
import http.client
import json
import os
from pathlib import Path
import selectors
import socket
import struct
import subprocess
import tempfile
import time
import urllib.parse

REPO = Path(__file__).resolve().parents[2]


def wait_line(process, expected):
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        assert selector.select(10), f'no writer output: {process.poll()}'
    assert process.stdout.readline().strip() == expected


def check_video(path, frames, clean=True):
    probe = subprocess.run(['ffprobe', '-v', 'error', '-count_frames', '-show_streams',
                            '-of', 'json', str(path)], capture_output=True, check=True)
    info = json.loads(probe.stdout)['streams'][0]
    assert int(info['nb_read_frames']) >= frames, (path, info)
    decode = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(path), '-map', '0:v:0',
                             '-f', 'null', '-'], capture_output=True, check=True)
    if clean:
        assert not probe.stderr and not decode.stderr, (probe.stderr, decode.stderr)
    # Check that every complete frame retains its JSON, FOV and timestamp.
    telemetry = subprocess.check_output([os.sys.executable, str(REPO / 'tools/video_telemetry.py'), str(path)])
    records = [json.loads(line) for line in telemetry.splitlines()]
    assert len(records) >= frames
    assert all(abs(r['hfov_deg'] - 51.5466) < .001 for r in records)
    assert [r['pts90k'] for r in records[:frames]] == list(range(0, frames * 9000, 9000))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, nargs='?')
    parser.add_argument('--vlc', action='store_true',
                        help='also verify duration and seeking with installed libVLC')
    args = parser.parse_args()
    output = (args.output or Path(tempfile.mkdtemp(prefix='recording-recovery-'))).resolve()
    print(f'Logs and recordings: {output}', flush=True)
    root = output / 'runtime'
    media = root / 'mnt'
    for path in (root / 'app', root / 'run', media):
        path.mkdir(parents=True, exist_ok=True)
    (root / 'app/web.pass').write_text('recording-test-password\n')
    web_binary = output / 'web'
    with (output / 'build.log').open('w') as log:
        subprocess.run(['make', '-C', str(REPO / 'camera_app'), 'tests/test_mp4'], stdout=log, stderr=log, check=True, close_fds=False)
        subprocess.run(['make', '-C', str(REPO / 'web'), 'sitl', f'SITL_ROOT={root}',
                        f'SITL_TARGET={web_binary}'], stdout=log, stderr=log, check=True, close_fds=False)
    fixture = output / 'source.h264'
    subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=320x240:rate=10',
                    '-t', '2.3', '-c:v', 'libx264', '-x264-params',
                    'aud=1:bframes=0:keyint=10:slices=4', '-y', str(fixture)], check=True)
    path = media / 'recording.mp4'
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    auth = 'Basic ' + base64.b64encode(b'admin:recording-test-password').decode()

    def download():
        conn = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
        try:
            conn.request('GET', '/file?download=1&path=' + urllib.parse.quote(str(path)),
                         headers={'Authorization': auth})
            response = conn.getresponse()
            data = response.read()
            assert response.status == 200, (response.status, data)
            assert len(data) == int(response.getheader('Content-Length'))
            return data
        finally:
            conn.close()

    writer = web = None
    with (output / 'web.log').open('w') as log:
        try:
            web = subprocess.Popen([str(web_binary), '-p', str(port)], stdout=log, stderr=log)
            deadline = time.monotonic() + 5
            while True:
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=.2):
                        break
                except OSError:
                    assert web.poll() is None and time.monotonic() < deadline
                    time.sleep(.02)
            writer = subprocess.Popen([str(REPO / 'camera_app/tests/test_mp4'), str(fixture), str(path)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=dict(os.environ, CA_TEST_MP4_PAUSE='1'))
            wait_line(writer, b'READY active recording')
            # Verify that the HTTP size snapshot waits for an in-progress frame
            # write, instead of exposing an incomplete moof/mdat to players.
            with path.open('rb') as lock, concurrent.futures.ThreadPoolExecutor() as pool:
                fcntl.flock(lock, fcntl.LOCK_EX)
                pending = pool.submit(download)
                time.sleep(.2)
                assert not pending.done(), 'download ignored frame write lock'
                fcntl.flock(lock, fcntl.LOCK_UN)
                active = output / 'download-active.mp4'
                active.write_bytes(pending.result(timeout=5))
            check_video(active, 10)
            if args.vlc:
                from check_vlc_video import check_vlc_video
                check_vlc_video(active, 1.0)
            assert writer.poll() is None
            writer.stdin.write(b'\n')
            writer.stdin.flush()
            wait_line(writer, b'READY unclosed recording')
            grown = output / 'download-grown.mp4'
            grown.write_bytes(download())
            assert grown.stat().st_size > active.stat().st_size
            assert grown.read_bytes().startswith(active.read_bytes())
            check_video(grown, 23)
            if args.vlc:
                check_vlc_video(grown, 2.3)
            writer.kill()  # SIGKILL: no close, metadata finalization or cleanup
            writer.wait(timeout=5)
            assert writer.returncode == -9
            assert path.read_bytes() == grown.read_bytes()
            check_video(path, 23)
            if args.vlc:
                check_vlc_video(path, 2.3)
            # Simulate torn final writes: partial moof header/body, mdat header
            # and payload. Earlier fragments must remain decodable.
            data = path.read_bytes()
            offset = 0
            moofs = []
            while offset + 8 <= len(data):
                size, kind = struct.unpack_from('>I4s', data, offset)
                assert size >= 8
                if kind == b'moof':
                    moofs.append((offset, size))
                offset += size
            start, moof_size = moofs[-1]
            for i, cut in enumerate((start, start + 4, start + 12,
                                     start + moof_size + 4, len(data) - 100)):
                truncated = output / f'truncated-{i}.mp4'
                truncated.write_bytes(data[:cut])
                check_video(truncated, 22, clean=False)
            print('PASS Files downloads during recording, immutable prefixes, SIGKILL and torn final fragments')
        finally:
            for process in (writer, web):
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)


if __name__ == '__main__':
    main()
