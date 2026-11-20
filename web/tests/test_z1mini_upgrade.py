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
import shutil
import urllib.parse
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


def package(ap, ipc, sums=None, manifest=None, force_zip64=False):
    """Build an overlay ZIP like tools/build_z1mini_package.py, with overrides."""
    ap = dict(ap)
    ap.setdefault('manifest.json', manifest or json.dumps({'target': 'xfrobot-z1mini',
                                                            'vendor_isp_required': False}, indent=2).encode())
    if sums is None:
        sums = ''.join(f'{hashlib.sha256(data).hexdigest()}  {name}\n' for name, data in sorted(ap.items())).encode()
    ap['SHA256SUMS'] = sums
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        entries = {'gcu/ap/' + name: data for name, data in ap.items()}
        entries.update({'gcu/ipc/' + name: data for name, data in ipc.items()})
        for name, data in entries.items():
            with archive.open(name, 'w', force_zip64=force_zip64) as output_file:
                output_file.write(data)
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
      'web.pass.default': b'ardupilot\n', 'README.md': b'readme\n',
      'webroot/head.html': b'<title>new template</title>', 'webroot/style.css': b'body{color:black}'}
# Tiny stand-ins keep extraction-budget fault tests bounded while covering
# installation of every asset required to leave recovery mode.
for asset in (WEB / 'webroot').iterdir():
    if asset.is_file():
        AP.setdefault('webroot/' + asset.name, b'x')
IPC = {'run.sh': b'#!/bin/sh\n./camera_gcu.sh &\n', 'camera_gcu.sh': b'#!/bin/sh\n'}

if not (MAVLINK / 'all/mavlink.h').exists():
    subprocess.run(['make', '-C', str(ROOT / 'camera_app'), 'CAMERA_BACKEND=z1mini',
                    'build/mavlink/all/include/all/mavlink.h'], check=True, stdout=subprocess.DEVNULL, close_fds=False)

with tempfile.TemporaryDirectory(prefix='z1mini-upgrade-test-') as directory:
    root = Path(directory)
    exchange_test = root / 'exchange-test'
    subprocess.run(['c++', '-std=gnu++17', '-Wno-missing-field-initializers', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-Wno-address-of-packed-member', '-ffunction-sections', '-fdata-sections',
                    '-Wl,--gc-sections', '-DAPCAM_TARGET=APCAM_TARGET_Z1_MINI',
                    '-D__CYGWIN__', '-DWEB_PORTABLE_SITL', f'-DWEBROOT_PATH="{Path(__file__).resolve().parents[1] / "webroot"}"', '-DMT11_WEB_TEST', '-DMT11_WEB_SITL',
                    f'-I{MAVLINK}', str(WEB / 'tests/test_z1mini_exchange.cpp'),
                    '-o', str(exchange_test), '-lm'], check=True)
    subprocess.run([str(exchange_test), str(root)], check=True)
    gcu, settings, run, media = root / 'gcu', root / 'settings', root / 'run', root / 'mnt'
    for path in (gcu / 'ap', gcu / 'ipc', settings, run, media):
        path.mkdir(parents=True)
    (gcu / 'ap/camera-app').write_bytes(b'old camera\n')
    (gcu / 'ipc/lib-old').write_bytes(b'stale vendor file\n')
    (settings / 'web.pass').write_text('test-password\n')
    (settings / 'web.pass').chmod(0o600)
    config = (ROOT / 'packaging/z1mini/camera.ini').read_bytes()
    (settings / 'camera.ini').write_bytes(config)
    shutil.copytree(WEB / 'webroot', gcu / 'ap/webroot')
    binary = root / 'z1mini-web'
    paths = dict(WEBROOT_PATH=gcu / "ap/webroot", GCU_ROOT=gcu, APP_DIR=gcu / 'ap', APP_SELECTION_DIR=settings, MEDIA_ROOT=media,
                 PASSWORD_PATH=settings / 'web.pass', REPLACEMENT_CONFIG_PATH=settings / 'camera.ini',
                 REPLACEMENT_CONFIG_BACKUP_PATH=settings / 'camera.ini.bak', SESSION_PATH=run / 'sessions',
                 UPGRADE_LOCK_PATH=run / 'upgrade.lock', USER_LOCK_PATH=run / 'users.lock',
                 RUNTIME_DIR=run, CAMERA_READY_PATH=run / 'ready', REPLACEMENT_CAMERA_PATH=gcu / 'ap/camera-app',
                 SOC_TEMPERATURE_PATH=run / 'soc_temp')
    exchange_failure = root / 'disable-atomic-exchange'
    subprocess.run(['c++', '-std=gnu++17', '-Wno-missing-field-initializers', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-Wno-address-of-packed-member', '-DAPCAM_TARGET=APCAM_TARGET_Z1_MINI',
                    '-DGCU_PACKAGE_MAX_EXTRACTED=4096',
                    '-DMT11_WEB_TEST', '-DMT11_WEB_SITL', f'-I{MAVLINK}',
                    *[f'-D{name}="{path}"' for name, path in paths.items()],
                    str(WEB / 'mt11-web.cpp'), '-o', str(binary), '-lm'], check=True)
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
        authentication = {} if headers and 'Cookie' in headers else {'Authorization': auth}
        connection.request('GET' if body is None else 'POST', path, body=body,
                           headers={**authentication, **(headers or {})})
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

        # Recovery must work with a partially installed tree as well as no tree.
        (gcu / 'ap/webroot/style.css').unlink()
        status, recovery = request('/')
        assert status == 200 and b'Z1-Mini firmware recovery' in recovery
        shutil.rmtree(gcu / 'ap/webroot')
        status, recovery = request('/')
        assert status == 200 and b'id=firmware-upload' in recovery
        assert b'/style.css' not in recovery
        status, recovery_script = request('/upgrade.js')
        assert status == 200 and recovery_script == script
        check_upgrade_browser(recovery_script, [name], rejected)

        def browser(path, body=None, cookie=None, origin=None):
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
            headers = {'Accept': 'text/html'}
            if body is not None:
                headers['Content-Type'] = 'application/x-www-form-urlencoded'
                body = urllib.parse.urlencode(body)
            if cookie: headers['Cookie'] = cookie
            if origin: headers['Origin'] = origin
            connection.request('GET' if body is None else 'POST', path, body, headers)
            response = connection.getresponse()
            result = response.status, dict(response.getheaders()), response.read()
            connection.close()
            return result

        assert browser('/')[0] == 303
        assert browser('/upgrade.js')[0] == 303  # No unauthenticated uploader.
        status, _, login = browser('/login')
        assert status == 200 and b'Z1-Mini firmware recovery' in login
        assert b'<script' not in login and b'<link' not in login
        token = re.search(rb'name=token value="([a-f0-9]{64})"', login).group(1).decode()
        form = {'token': token, 'username': 'admin', 'password': 'wrong'}
        status, _, error = browser('/login', form)
        assert status == 401 and b'Z1-Mini firmware recovery' in error
        form['password'] = 'test-password'
        assert browser('/login', form | {'token': 'bad'})[0] == 400
        assert browser('/login', form, origin='http://attacker.invalid')[0] == 403
        status, login_headers, _ = browser('/login', form)
        assert status == 303
        cookie = login_headers['Set-Cookie'].split(';')[0]
        assert browser('/', cookie=cookie)[0] == 200
        assert browser('/upgrade.js', cookie=cookie)[0] == 200

        headers = {'Content-Type': 'application/octet-stream', 'X-CSRF-Token': csrf, 'X-Firmware-Name': name}
        for bad in rejected:
            assert request('/upgrade', b'x', headers | {'X-Firmware-Name': bad})[0] == 400, bad
        assert request('/upgrade', b'x', headers | {'X-CSRF-Token': 'no'})[0] == 403
        assert request('/upgrade', b'x', headers | {'Content-Type': 'text/plain'})[0] == 415
        good = package(AP, IPC, manifest=json.dumps({
            'target': 'xfrobot-z1mini', 'vendor_isp_required': False,
            'metadata': {'vendor_isp_required': True, 'values': [None, -1.25e2, 'escaped " quote']},
        }, separators=(',', ':')).encode())
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
            'unlisted web asset': package(AP | {'webroot/unknown.js': b'payload'}, IPC),
            'dot path': raw_zip({'gcu/ap/..': b'x'}),
            'underdeclared extracted size exceeds remaining budget': lie_about_uncompressed_size(
                package(AP | {'camera-app': b'a' * 1200, 'z1mini-web': b'b' * 1200}, IPC),
                ('gcu/ap/camera-app', 'gcu/ap/z1mini-web'), 100),
        }
        manifests = {
            'compact retained ISP': '{"target": "xfrobot-z1mini", "vendor_isp_required":true}',
            'nested false cannot override true': '{"target":"xfrobot-z1mini","vendor_isp_required":true,"metadata":{"vendor_isp_required":false}}',
            'nested field is not top-level': '{"target":"xfrobot-z1mini","metadata":{"vendor_isp_required":false}}',
            'nested target': '{"metadata":{"target":"xfrobot-z1mini"},"vendor_isp_required":false}',
            'duplicate ISP': '{"target":"xfrobot-z1mini","vendor_isp_required":true,"vendor_isp_required":false}',
            'duplicate target': '{"target":"other","target":"xfrobot-z1mini","vendor_isp_required":false}',
            'string boolean': '{"target":"xfrobot-z1mini","vendor_isp_required":"false"}',
            'missing boolean': '{"target":"xfrobot-z1mini"}',
            'trailing JSON': '{"target":"xfrobot-z1mini","vendor_isp_required":false}{}',
            'trailing comma': '{"target":"xfrobot-z1mini","vendor_isp_required":false,}',
            'truncated JSON': '{"target":"xfrobot-z1mini","vendor_isp_required":false',
            'excess nesting': '{"target":"xfrobot-z1mini","vendor_isp_required":false,"extra":' + '['*20 + '0' + ']'*20 + '}',
        }
        for label, manifest in manifests.items():
            rejected_packages[label] = package(AP, IPC, manifest=manifest.encode())
        rejected_packages['local ZIP64'] = package(AP, IPC, force_zip64=True)
        sentinel = bytearray(good)
        position = sentinel.index(b'PK\x01\x02')
        sentinel[position+20:position+24] = b'\xff'*4
        rejected_packages['central ZIP64 compressed size'] = bytes(sentinel)
        for label, data in rejected_packages.items():
            status, message = request('/upgrade', data, headers)
            assert status == 400, (label, status, message)
            if label == 'underdeclared extracted size exceeds remaining budget':
                assert (b'extraction failed' in message or
                        b'extracted data exceeds package limit' in message), (label, message)
            assert (gcu / 'ap/camera-app').read_bytes() == b'old camera\n', label
            assert not (root / 'gcu.new').exists() and not list(run.glob('firmware-upload.*')), label
        exchange_failure.touch()
        status, message = request('/upgrade', good, headers)
        assert status == 500 and b'atomically exchange' in message, (status, message)
        assert (gcu / 'ap/camera-app').read_bytes() == b'old camera\n'
        assert not (root / 'gcu.new').exists()
        exchange_failure.unlink()
        # Use the recovery page's browser session for the successful install.
        status, message = request('/upgrade', good, headers | {'Cookie': cookie})
        assert status == 201, (status, message)
        assert b'rebooting' in message
        status, restored = request('/')
        assert status == 200 and b'Z1-Mini firmware recovery' not in restored
        assert b'id=firmware-upload' in restored
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
        assert (gcu / 'ap/webroot/head.html').read_bytes() == AP['webroot/head.html']
        assert (gcu / 'ap/webroot/style.css').read_bytes() == AP['webroot/style.css']
        assert ((gcu / 'ap/webroot/head.html').stat().st_mode & 0o777) == 0o644
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
        print('PASS Z1-Mini: asset-free recovery login/upload, validation, atomic installation, restored UI and reboot')
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
