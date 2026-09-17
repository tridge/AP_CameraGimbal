#!/usr/bin/env python3
"""Z1-Mini web installation of .gcu overlays, using an isolated host server."""
import base64
import hashlib
import http.client
import json
from pathlib import Path
import io
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
import zipfile

from test_upgrade_browser import check_upgrade_browser

WEB = Path(__file__).resolve().parents[1]
ROOT = WEB.parent
MAVLINK = ROOT / 'camera_app/build/mavlink/all/include'


def package(ap, ipc, sums=None, manifest=None):
    """Build an overlay ZIP like tools/build_z1mini_package.py, with overrides."""
    ap = dict(ap)
    ap.setdefault('manifest.json', manifest or json.dumps({'target': 'xfrobot-z1mini',
                                                            'vendor_isp_required': False}, indent=2).encode())
    if sums is None:
        sums = ''.join(f'{hashlib.sha256(data).hexdigest()}  {name}\n' for name, data in sorted(ap.items())).encode()
    ap['SHA256SUMS'] = sums
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, data in ap.items():
            archive.writestr('gcu/ap/' + name, data)
        for name, data in ipc.items():
            archive.writestr('gcu/ipc/' + name, data)
    return output.getvalue()


def raw_zip(entries):
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w') as archive:
        for name, data in entries.items():
            archive.writestr(name, data)
    return output.getvalue()


def lie_about_uncompressed_size(data, names, declared_size):
    """Tamper the local and central ZIP sizes while retaining deflate data."""
    data = bytearray(data)
    for signature, name_offset, length_offset, size_offset in (
            (b'PK\x03\x04', 30, 26, 22), (b'PK\x01\x02', 46, 28, 24)):
        wanted = set(names)
        position = 0
        while True:
            position = data.find(signature, position)
            if position < 0:
                break
            name_length = int.from_bytes(data[position + length_offset:
                                                position + length_offset + 2], 'little')
            name = bytes(data[position + name_offset:
                               position + name_offset + name_length]).decode()
            if name in wanted:
                data[position + size_offset:position + size_offset + 4] = declared_size.to_bytes(4, 'little')
                wanted.remove(name)
            position += name_offset + name_length
    assert not wanted, wanted
    return bytes(data)


AP = {'camera-app': b'#!/bin/sh\necho new camera\n', 'z1mini-web': b'#!/bin/sh\necho new web\n',
      'service.sh': b'#!/bin/sh\n', 'ax-capture': b'#!/bin/sh\n', 'camera.ini.default': b'[general]\n',
      'web.pass.default': b'ardupilot\n', 'README.md': b'readme\n'}
IPC = {'run.sh': b'#!/bin/sh\n./camera_gcu.sh &\n', 'camera_gcu.sh': b'#!/bin/sh\n'}

if not (MAVLINK / 'all/mavlink.h').exists():
    subprocess.run(['make', '-C', str(ROOT / 'camera_app'), 'CAMERA_BACKEND=z1mini',
                    'build/mavlink/all/include/all/mavlink.h'], check=True, stdout=subprocess.DEVNULL)

with tempfile.TemporaryDirectory(prefix='z1mini-upgrade-test-') as directory:
    root = Path(directory)
    gcu, settings, run, media = root / 'gcu', root / 'settings', root / 'run', root / 'mnt'
    for path in (gcu / 'ap', gcu / 'ipc', settings, run, media):
        path.mkdir(parents=True)
    (gcu / 'ap/camera-app').write_bytes(b'old camera\n')
    (gcu / 'ipc/lib-old').write_bytes(b'stale vendor file\n')
    (settings / 'web.pass').write_text('test-password\n')
    (settings / 'web.pass').chmod(0o600)
    config = (ROOT / 'packaging/z1mini/camera.ini').read_bytes()
    (settings / 'camera.ini').write_bytes(config)
    binary = root / 'z1mini-web'
    paths = dict(GCU_ROOT=gcu, APP_DIR=gcu / 'ap', APP_SELECTION_DIR=settings, MEDIA_ROOT=media,
                 PASSWORD_PATH=settings / 'web.pass', REPLACEMENT_CONFIG_PATH=settings / 'camera.ini',
                 REPLACEMENT_CONFIG_BACKUP_PATH=settings / 'camera.ini.bak', SESSION_PATH=run / 'sessions',
                 UPGRADE_LOCK_PATH=run / 'upgrade.lock', USER_LOCK_PATH=run / 'users.lock',
                 RUNTIME_DIR=run, CAMERA_READY_PATH=run / 'ready', REPLACEMENT_CAMERA_PATH=gcu / 'ap/camera-app',
                 SOC_TEMPERATURE_PATH=run / 'soc_temp')
    exchange_failure = root / 'disable-atomic-exchange'
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-Wno-address-of-packed-member', '-DAPCAM_TARGET=APCAM_TARGET_Z1_MINI',
                    '-DGCU_PACKAGE_MAX_EXTRACTED=4096',
                    '-DMT11_WEB_TEST', '-DMT11_WEB_SITL', f'-I{MAVLINK}',
                    *[f'-D{name}="{path}"' for name, path in paths.items()],
                    str(WEB / 'mt11-web.c'), '-o', str(binary), '-lm'], check=True)
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    log = (root / 'web.log').open('w')
    test_env = os.environ.copy()
    test_env['CAMERA_GIMBAL_TEST_EXCHANGE_FAIL'] = str(exchange_failure)
    old_umask = os.umask(0o077)
    try:
        process = subprocess.Popen([str(binary), '-p', str(port)], stdout=log,
                                   stderr=log, env=test_env)
    finally:
        os.umask(old_umask)

    def request(path, body=None, headers=None):
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
        auth = 'Basic ' + base64.b64encode(b'admin:test-password').decode()
        connection.request('GET' if body is None else 'POST', path, body=body,
                           headers={'Authorization': auth, **(headers or {})})
        response = connection.getresponse()
        result = response.status, response.read()
        connection.close()
        return result

    try:
        for attempt in range(100):
            try:
                status, page = request('/')
                break
            except ConnectionRefusedError:
                assert process.poll() is None, 'web server exited'
                time.sleep(.05)
        else:
            raise AssertionError('web server did not start')
        assert status == 200 and b'id=firmware-upload' in page and b'Z1Mini_AP_*.gcu' in page, page
        assert b'accept=.gcu' in page and b'installed into the application partition' in page
        csrf = re.search(rb'name=csrf value="([a-f0-9]{64})"', page).group(1).decode()
        name = 'Z1Mini_AP_native_v1.0_abcdef.gcu'
        rejected = ['Z1Mini_FW_x.bin', 'Z1Mini_AP_.gcu', 'MT11_FW_x.bin', '../' + name, name + '.tmp',
                    'Z1Mini.gcu', 'Z1Mini_AP_x y.gcu']
        status, script = request('/upgrade.js')
        assert status == 200
        check_upgrade_browser(script, [name, 'Z1Mini_AP_v1.1_test-2.gcu'], rejected)
        headers = {'Content-Type': 'application/octet-stream', 'X-CSRF-Token': csrf, 'X-Firmware-Name': name}
        for bad in rejected:
            assert request('/upgrade', b'x', headers | {'X-Firmware-Name': bad})[0] == 400, bad
        assert request('/upgrade', b'x', headers | {'X-CSRF-Token': 'no'})[0] == 403
        assert request('/upgrade', b'x', headers | {'Content-Type': 'text/plain'})[0] == 415
        good = package(AP, IPC)
        rejected_packages = {
            'not a zip': b'firmware upload fixture' * 4096,
            'entry outside gcu': raw_zip({'gcu/ap/x': b'1', 'etc/passwd': b'x'}),
            'traversal': raw_zip({'gcu/ap/../../x': b'1'}),
            'nested directory': raw_zip({'gcu/ap/sub/x': b'1'}),
            'missing run.sh': package(AP, {'camera_gcu.sh': b'#!/bin/sh\n'}),
            'bad checksum': package(AP, IPC, sums=b'0' * 64 + b'  camera-app\n'),
            'other target': package(AP, IPC, manifest=b'{"target": "xfrobot-other"}\n'),
            'needs vendor isp': package(AP, IPC, manifest=b'{"target": "xfrobot-z1mini", "vendor_isp_required": true}\n'),
            'unlisted extra file': package(AP | {'unlisted.bin': b'payload'}, IPC),
            'dot path': raw_zip({'gcu/ap/..': b'x'}),
            'underdeclared extracted size exceeds remaining budget': lie_about_uncompressed_size(
                package(AP | {'camera-app': b'a' * 2500, 'z1mini-web': b'b' * 2500}, IPC),
                ('gcu/ap/camera-app', 'gcu/ap/z1mini-web'), 1500),
        }
        for label, data in rejected_packages.items():
            status, message = request('/upgrade', data, headers)
            assert status == 400, (label, status, message)
            if label == 'underdeclared extracted size exceeds remaining budget':
                assert b'extraction failed' in message, (label, message)
            assert (gcu / 'ap/camera-app').read_bytes() == b'old camera\n', label
            assert not (root / 'gcu.new').exists() and not list(run.glob('firmware-upload.*')), label
        exchange_failure.touch()
        status, message = request('/upgrade', good, headers)
        assert status == 500 and b'atomically exchange' in message, (status, message)
        assert (gcu / 'ap/camera-app').read_bytes() == b'old camera\n'
        assert not (root / 'gcu.new').exists()
        exchange_failure.unlink()
        status, message = request('/upgrade', good, headers)
        assert status == 201, (status, message)
        assert b'rebooting' in message
        assert (gcu / 'ap/camera-app').read_bytes() == AP['camera-app']
        assert (gcu / 'ipc/run.sh').read_bytes() == IPC['run.sh']
        assert not (gcu / 'ipc/lib-old').exists(), 'previous installation was not replaced'
        for executable in ('ap/service.sh', 'ap/camera-app', 'ap/z1mini-web', 'ap/ax-capture', 'ipc/run.sh', 'ipc/camera_gcu.sh'):
            assert (gcu / executable).stat().st_mode & 0o111 == 0o111, executable
        assert not (root / 'gcu.new').exists() and not (root / 'gcu.old').exists()
        assert (gcu.stat().st_mode & 0o777) == 0o755
        assert ((gcu / 'ap').stat().st_mode & 0o777) == 0o755
        assert ((gcu / 'ipc').stat().st_mode & 0o777) == 0o755
        assert ((gcu / 'ap/README.md').stat().st_mode & 0o777) == 0o644
        assert not list(run.glob('firmware-upload.*'))
        assert (settings / 'camera.ini').read_bytes() == config
        # A retained-ISP package is never accepted by the destructive web
        # updater, even when the old vendor ISP is still present.
        (gcu / 'ipc/main').write_bytes(b'#!/bin/sh\n')
        (gcu / 'ipc/main').chmod(0o755)
        second = package(AP | {'camera-app': b'second\n'}, IPC,
                         manifest=b'{"target": "xfrobot-z1mini", "vendor_isp_required": true}\n')
        assert request('/upgrade', second, headers)[0] == 400
        assert (gcu / 'ap/camera-app').read_bytes() == AP['camera-app']
        assert (gcu / 'ipc/main').exists()
        log.flush()
        text = (root / 'web.log').read_text()
        assert text.count('SITL reboot request ignored') == 1, text
        print('PASS Z1-Mini: browser and server accept overlay packages, reject bad names and archives, install atomically and reboot')
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
