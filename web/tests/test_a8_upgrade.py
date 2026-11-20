#!/usr/bin/env python3
"""A8 SD filename and legacy filename uploads, using an isolated host server."""
import base64
import http.client
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time

from test_upgrade_browser import check_upgrade_browser

WEB = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix='a8-upgrade-test-') as directory:
    root = Path(directory)
    app, media = root / 'app', root / 'mnt'
    app.mkdir()
    media.mkdir()
    (app / 'web.pass').write_text('test-password\n')
    (app / 'web.pass').chmod(0o600)
    config = (WEB.parent / 'packaging/a8/camera.ini').read_bytes()
    (app / 'camera.ini').write_bytes(config)
    binary = root / 'a8-web'
    paths = dict(APP_DIR=app, MEDIA_ROOT=media, PASSWORD_PATH=app / 'web.pass',
                 REPLACEMENT_CONFIG_PATH=app / 'camera.ini', SESSION_PATH=root / 'sessions',
                 UPGRADE_LOCK_PATH=root / 'upgrade.lock', USER_LOCK_PATH=root / 'users.lock',
                 RUNTIME_DIR=root, CAMERA_READY_PATH=root / 'ready')
    subprocess.run(['c++', '-std=gnu++17', '-Wno-missing-field-initializers', '-O2', '-Wall', '-Wextra', '-Werror', '-std=gnu++17',
                    '-DAPCAM_TARGET=APCAM_TARGET_A8', f'-DWEBROOT_PATH="{Path(__file__).resolve().parents[1] / "webroot"}"', '-DMT11_WEB_TEST', '-DMT11_WEB_SITL',
                    *[f'-D{name}="{path}"' for name, path in paths.items()],
                    str(WEB / 'mt11-web.cpp'), '-o', str(binary), '-lm'], check=True)
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    process = subprocess.Popen([str(binary), '-p', str(port)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

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
        assert status == 200 and b'SIYI_4K_MINI_UpgradeSD.bin' in page
        assert b'>Restart camera app</button>' in page
        assert b'Restart ArduPilot camera app' not in page
        csrf = re.search(rb'name=csrf value="([a-f0-9]{64})"', page).group(1).decode()
        status, parameters = request('/parameters')
        assert status == 200
        assert b'name="uart_protocol"' not in parameters
        assert b'The external UART is handled by the gimbal controller.' in parameters
        assert b'cannot select its protocol or disable its input' in parameters
        canonical = 'SIYI_4K_MINI_UpgradeSD.bin'
        legacy = 'A8_FW_ArduPilot_v1.0_test.bin'
        rejected = ['ZR10_UpgradeSD.bin', '../' + canonical, canonical + '.tmp',
                    'SIYI_4K_MINI_UpgradeSDXbin', 'MT11_FW_test.bin', 'A8_FW_.bin']
        status, script = request('/upgrade.js')
        assert status == 200
        check_upgrade_browser(script, [canonical, legacy], rejected)
        headers = {'Content-Type': 'application/octet-stream', 'X-CSRF-Token': csrf}
        for name in rejected:
            assert request('/upgrade', b'x', headers | {'X-Firmware-Name': name})[0] == 400
        fixture = os.environ.get('A8_TEST_FIRMWARE')
        firmware = Path(fixture).read_bytes() if fixture else b'firmware upload fixture' * 4096
        installed = media / canonical
        for name in (canonical, legacy):
            status, message = request('/upgrade', firmware, headers | {'X-Firmware-Name': name})
            assert status == 201, (status, message)
            assert installed.read_bytes() == firmware
            assert not list(media.glob('*.tmp'))
            assert request('/upgrade', b'x', headers | {'X-Firmware-Name': name})[0] == 409
            assert installed.read_bytes() == firmware
            installed.unlink()
        assert (app / 'camera.ini').read_bytes() == config
        print('PASS A8: browser and server accept SD and legacy filenames, reject other names, preserve existing uploads and parameters')
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
