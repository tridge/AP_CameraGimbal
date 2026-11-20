#!/usr/bin/env python3
"""Test the A8 web Network capture setting through real SITL and PCAP download.

Run in an isolated namespace: sudo unshare -n python3 sitl/test_network_capture.py
"""
import argparse
import base64
import json
from html.parser import HTMLParser
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]


class Parameters(HTMLParser):
    def __init__(self, page):
        super().__init__()
        self.active = False
        self.select = None
        self.values = {}
        self.feed(page)

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == 'form':
            self.active = attrs.get('action') == '/parameters'
        if not self.active:
            return
        if tag == 'input' and 'name' in attrs:
            self.values[attrs['name']] = attrs.get('value', '')
        elif tag == 'select':
            self.select = attrs.get('name')
        elif tag == 'option' and self.select and 'disabled' not in attrs:
            if 'selected' in attrs or self.select not in self.values:
                self.values[self.select] = attrs.get('value', '')

    def handle_endtag(self, tag):
        if tag == 'form':
            self.active = False
        elif tag == 'select':
            self.select = None


def port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def wait_for(predicate):
    deadline = time.monotonic()+10
    while time.monotonic() < deadline:
        try:
            value = predicate()
            if value:
                return value
        except (OSError, URLError):
            pass
        time.sleep(.05)
    raise AssertionError('timed out')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT/'build/a8-sitl')
    args = parser.parse_args()
    assert os.geteuid() == 0 and {n for _, n in socket.if_nameindex()} == {'lo'}, 'Use sudo unshare -n'
    subprocess.run(['ip', 'link', 'set', 'lo', 'up'], check=True)
    with tempfile.TemporaryDirectory(prefix='a8-network-capture-') as temporary:
        root = Path(temporary)
        subprocess.run([sys.executable, str(ROOT/'sitl/prepare_runtime.py'), str(root),
                        str(ROOT/'packaging/a8/camera.ini')], check=True)
        web_port, gimbal_port, vendor_port = port(), port(socket.SOCK_DGRAM), port()
        processes, logs = [], []
        def start(command, name, env=None):
            log = (root/(name+'.log')).open('w')
            logs.append(log)
            process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
            processes.append(process)
            return process
        env = dict(os.environ, CAMERA_APP_BACKEND='a8', CAMERA_APP_UART=f'udp://127.0.0.1:{gimbal_port}',
                   CAMERA_APP_CONFIG=str(root/'app/camera.ini'), CAMERA_APP_PORT=str(vendor_port),
                   CAMERA_APP_READY_PATH=str(root/'run/camera-app.ready'), CAMERA_APP_MAVLINK_TCP_PORT=str(port()),
                   CAMERA_APP_MAVLINK_UDP_PORT='0', CAMERA_APP_RTSP_PORT=str(port()),
                   CAMERA_APP_RECORD_ROOT=str(root/'mnt/DCIM/record'), CAMERA_APP_CAPTURE_ROOT=str(root/'mnt/DCIM/capture'),
                   CAMERA_APP_RECORD_STATE=str(root/'run/recording.state'), CAMERA_GIMBAL_SITL_RUNTIME=str(root),
                   CAMERA_GIMBAL_SITL_CAMERA_EXE=str(args.build.resolve()/'camera-app'),
                   CAMERA_GIMBAL_SITL_WEB_EXE=str(args.build.resolve()/'mt11-web-portable'),
                   CAMERA_GIMBAL_SITL_WEBROOT=str(ROOT/'web/webroot'))
        for key in ('CAMERA_APP_SITL_VIDEO1', 'CAMERA_APP_SITL_VIDEO2', 'CAMERA_APP_SITL_TERRAIN'):
            env.pop(key, None)
        def request(path='/parameters', values=None, auth=True):
            headers = {'Authorization': 'Basic '+base64.b64encode(b'admin:ardupilot').decode()} if auth else {}
            data = None if values is None else urlencode(values).encode()
            if data is not None:
                headers['Content-Type'] = 'application/x-www-form-urlencoded'
            with urlopen(Request(f'http://127.0.0.1:{web_port}'+path, data, headers), timeout=4) as response:
                return response.read(), response.headers
        def page():
            return request()[0].decode()
        try:
            start([sys.executable, str(ROOT/'sitl/gimbal_sim.py'), '--backend', 'a8', '--port', str(gimbal_port),
                   '--ready-file', str(root/'gimbal.ready')], 'gimbal')
            wait_for(lambda: (root/'gimbal.ready').exists())
            camera = start([str(args.build.resolve()/'camera-app')], 'camera', env)
            wait_for(lambda: (root/'run/camera-app.ready').exists())
            start([str(args.build.resolve()/'mt11-web-portable'), '-p', str(web_port)], 'web', env)
            text = wait_for(page)
            values = Parameters(text).values
            assert values['network_capture'] == 'false'
            assert 'id=network-capture-files' in text
            captures = root/'mnt/DCIM/network'
            assert not captures.exists()
            bad = dict(values, network_capture='true', csrf='wrong', action='save')
            try:
                request(values=bad)
                raise AssertionError('capture enabled without CSRF token')
            except HTTPError as error:
                assert error.code == 400
            assert not captures.exists()
            values.update(network_capture='true', action='save')
            request(values=values)
            wait_for(lambda: 'Capturing all interfaces' in page())
            assert Parameters(page()).values['network_capture'] == 'true'
            status = json.loads(request('/network-capture-status')[0])
            assert status['available'] and not status['error'] and 'Capturing' in status['message']
            assert camera.poll() is None
            # Generate a protocol-like UDP payload and verify it after web download.
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                receiver.bind(('127.0.0.1', 0))
                sender.sendto(b'A8-network-capture-integration', receiver.getsockname())
                assert receiver.recv(1024) == b'A8-network-capture-integration'
            time.sleep(.2)
            values = Parameters(page()).values
            values.update(network_capture='false', action='save')
            request(values=values)
            wait_for(lambda: 'Off; saved capture files' in page())
            assert camera.poll() is None
            saved = (root/'app/camera.ini').read_text()
            assert 'capture = false' in saved
            files = list(captures.glob('*.pcap'))
            assert files
            snapshot = {file: file.read_bytes() for file in files}
            listing, _ = request('/files?'+urlencode({'path': str(captures)}))
            assert files[0].name.encode() in listing
            matched = False
            for file in files:
                downloaded, headers = request('/file?'+urlencode({'path': str(file), 'download': 1}))
                assert headers['Content-Type'] == 'application/vnd.tcpdump.pcap'
                assert downloaded == snapshot[file]
                matched |= b'A8-network-capture-integration' in downloaded
                subprocess.run(['tcpdump', '-nn', '-r', str(file)], check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            assert matched
            assert snapshot == {file: file.read_bytes() for file in files}, 'download re-enabled capture'
            # Error text must remain valid JSON and must not inject HTML.
            failure = 'Capture failed: "test" <failure> & retry'
            (root/'run/camera-app.ready.capture').write_text(f'{camera.pid} 1\n{failure}\n')
            status = json.loads(request('/network-capture-status')[0])
            assert status['available'] and status['error'] and status['message'] == failure
            assert '&lt;failure&gt;' in page() and '<failure>' not in page()
            # A status file from a previous camera process must not claim capture is active.
            (root/'run/camera-app.ready.capture').write_text('1 0\nCapturing stale session\n')
            assert 'Capturing stale session' not in page()
            assert not json.loads(request('/network-capture-status')[0])['available']
            try:
                request('/network-capture-status', auth=False)
                raise AssertionError('status disclosed without authentication')
            except HTTPError as error:
                assert error.code == 401
            print('PASS A8 Network setting defaults off, CSRF, live start/stop, status, persistence and authenticated Wireshark PCAP download')
        except BaseException:
            for log in root.glob('*.log'):
                print(log.name, log.read_text()[-6000:], file=sys.stderr)
            raise
        finally:
            for process in reversed(processes):
                process.terminate()
                try:
                    process.wait(5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()
            for log in logs:
                log.close()


if __name__ == '__main__':
    main()
