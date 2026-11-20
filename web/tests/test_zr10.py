#!/usr/bin/env python3
"""Check ZR10 web capabilities and SD photo paths with an isolated host build."""
import base64
import configparser
import http.client
from html.parser import HTMLParser
import os
from pathlib import Path
import socket
import re
import sys
import zlib
import struct
import subprocess
import tempfile
import time
from urllib.parse import quote, urlencode
from test_upgrade_browser import check_upgrade_browser


web = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(web.parent / "tools"))
from zr10_firmware import pack, SCRIPT_SIZE, CUSTOMER_SIZE

class ParameterForm(HTMLParser):
    """Collect the controls a browser actually submits, including empty values."""
    def __init__(self, page):
        super().__init__()
        self.values = {}
        self.active = False
        self.select = None
        self.feed(page.decode())

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "form":
            self.active = attrs.get("action") == "/parameters"
        if not self.active:
            return
        if tag == "input" and "name" in attrs and "disabled" not in attrs:
            self.values[attrs["name"]] = attrs.get("value", "")
        elif tag == "select":
            self.select = attrs["name"]
        elif tag == "option" and self.select and "disabled" not in attrs:
            if "selected" in attrs or self.select not in self.values:
                self.values[self.select] = attrs.get("value", "")

    def handle_endtag(self, tag):
        if tag == "form":
            self.active = False
        elif tag == "select":
            self.select = None


with tempfile.TemporaryDirectory(prefix="zr10-web-") as directory:
    root = Path(directory)
    app = root / "app"
    app.mkdir()
    photos = root / "mnt/DCIM/capture"
    photos.mkdir(parents=True)
    photo = photos / "zr10-test.jpg"
    photo.write_bytes(b"\xff\xd8test\xff\xd9")
    (app / "web.pass").write_text("test-password\n")
    (app / "web.pass").chmod(0o600)
    config = (web.parent / "packaging/zr10/camera.ini").read_text()
    (app / "camera.ini").write_text(config)
    paths = {
        "APP_DIR": app, "MEDIA_ROOT": root / "mnt",
        "PASSWORD_PATH": app / "web.pass",
        "REPLACEMENT_CONFIG_PATH": app / "camera.ini",
        "REPLACEMENT_CONFIG_BACKUP_PATH": app / "camera.ini.bak",
        "APP_SELECTION_PATH": app / "selection", "APP_SELECTION_DIR": app,
        "APP_REQUEST_PATH": root / "request", "APP_REQUEST_LOCK_PATH": root / "request.lock",
        "APP_STARTED_PATH": root / "started", "SWITCH_LOCK_PATH": root / "switch.lock",
        "UPGRADE_LOCK_PATH": root / "upgrade.lock",
        "SESSION_PATH": root / "sessions", "USER_LOCK_PATH": root / "users.lock",
        "RUNTIME_DIR": root, "CAMERA_READY_PATH": root / "ready",
        "VENDOR_CAMERA_PATH": app / "vendor", "REPLACEMENT_CAMERA_PATH": app / "camera-app",
    }
    binary = root / "zr10-web"
    subprocess.run([
        "c++", "-std=gnu++17", "-Wno-missing-field-initializers", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
        "-std=gnu++17", "-DAPCAM_TARGET=APCAM_TARGET_ZR10", f'-DWEBROOT_PATH="{Path(__file__).resolve().parents[1] / "webroot"}"', "-DMT11_WEB_TEST", "-DMT11_WEB_SITL",
        *[f'-D{name}="{value}"' for name, value in paths.items()],
        str(web / "mt11-web.cpp"), "-o", str(binary),
    ], check=True)
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    process = subprocess.Popen([str(binary), "-p", str(port)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    def request(path, method="GET", authenticated=True, body=None, extra_headers=None, interrupt_after=None):
        headers = {}
        if authenticated:
            token = base64.b64encode(b"admin:test-password").decode()
            headers["Authorization"] = "Basic " + token
        if body is not None:
            headers["Content-Type"] = "application/octet-stream"
        headers.update(extra_headers or {})
        if path == "/upgrade":
            # Exercise the browser's streaming endpoint without sending a large
            # body after an early header rejection.
            if isinstance(body, str):
                body = body.encode()
            headers["Content-Length"] = str(len(body or b""))
            headers["Expect"] = "100-continue"
            with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
                sock.sendall((f"{method} {path} HTTP/1.1\r\nHost: localhost\r\n" +
                              "".join(f"{key}: {value}\r\n" for key, value in headers.items()) +
                              "\r\n").encode())
                stream = sock.makefile("rb")
                def read_response():
                    status = int(stream.readline().split()[1])
                    response_headers = {}
                    while True:
                        line = stream.readline()
                        if line == b"\r\n":
                            break
                        key, value = line.decode().split(":", 1)
                        response_headers[key.lower()] = value.strip()
                    return status, response_headers
                status, response_headers = read_response()
                if status == 100:
                    sock.sendall(body if interrupt_after is None else body[:interrupt_after])
                    if interrupt_after is not None:
                        sock.shutdown(socket.SHUT_WR)
                    status, response_headers = read_response()
                contents = stream.read(int(response_headers.get("content-length", 0)))
                stream.close()
                return status, contents
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        connection.request(method, path, body=body, headers=headers)
        response = connection.getresponse()
        status, contents = response.status, response.read()
        connection.close()
        return status, contents

    try:
        for attempt in range(100):
            try:
                status, page = request("/")
                break
            except ConnectionRefusedError:
                if process.poll() is not None:
                    raise RuntimeError(process.stderr.read().decode())
                time.sleep(0.05)
        else:
            raise AssertionError("Web server did not start")
        assert status == 200 and b"ZR10" in page
        assert b"id=firmware-upload" in page
        assert b"action=/switch" not in page
        csrf = re.search(rb'name=csrf value="([a-f0-9]{64})"', page).group(1).decode()
        assert b'ZR10_UpgradeSD.bin' in page
        status, script = request('/upgrade.js')
        assert status == 200
        check_upgrade_browser(script, ['ZR10_UpgradeSD.bin', 'ZR10_FW_test.bin'],
                              ['SIYI_4K_MINI_UpgradeSD.bin', '../ZR10_UpgradeSD.bin',
                               'ZR10_UpgradeSD.bin.tmp', 'ZR10_FW_.bin'])
        assert request("/switch", "POST", body=b"app=vendor")[0] == 400
        assert request("/switch", "POST", body=f"csrf={csrf}&app=invalid",
                       extra_headers={"Content-Type": "application/x-www-form-urlencoded"})[0] == 404
        assert request("/healthz") == (200, b"camera_app=stopped\ncamera_kind=none\n")
        assert request("/", authenticated=False)[0] == 401
        status, page = request("/parameters")
        assert b'name="uart_protocol"' not in page
        assert b'The external UART is handled by the gimbal controller.' in page
        assert status == 200 and b"2560x1440" in page
        assert b"3840x2160" not in page
        fields = ParameterForm(page).values
        assert fields["proxy_video1_port"] == "0"
        assert fields["proxy_video2_port"] == "0"
        assert "brightness" not in fields and "thermal_palette" not in fields
        def save_parameters(values):
            return request("/parameters", "POST", body=urlencode(values | {"action": "save"}),
                           extra_headers={"Content-Type": "application/x-www-form-urlencoded"})

        # Submit the rendered form, not a hand-built superset containing hidden
        # controls. ZR10 must not require its unsupported image parameters.
        status, page = save_parameters(fields)
        assert status == 200, re.findall(rb'<p class="notice[^>]*>(.*?)</p>', page)
        assert ParameterForm(page).values == fields
        saved_config = (app / "camera.ini").read_text()
        def image_section(text):
            # The camera INI reader permits sections extended later in the file.
            parsed = configparser.ConfigParser(strict=False)
            parsed.read_string(text)
            return dict(parsed["image"])
        assert image_section(saved_config) == image_section(config)

        # A rejected save must retain ALL edits, even fields after the first
        # invalid value and HTML-sensitive passwords, without changing disk.
        edited = fields | {
            "timezone": "invalid timezone", "proxy_enabled": "true",
            "proxy_host": "157.245.83.174", "proxy_mavlink_port": "10009",
            "proxy_signing": "true", "proxy_signing_passphrase": "test<&'secret",
            "proxy_publish_password": "publish<&'secret",
            "network_secondary_address": "", "network_gateway": "192.168.144.20",
            "network_capture": "false",
        }
        status, page = save_parameters(edited)
        assert status == 400 and b"Invalid value for Timezone" in page
        assert ParameterForm(page).values == edited
        assert (app / "camera.ini").read_text() == saved_config
        corrected = ParameterForm(page).values | {"timezone": "GMT-10"}
        status, page = save_parameters(corrected)
        assert status == 200 and ParameterForm(page).values == corrected
        assert image_section((app / "camera.ini").read_text()) == image_section(config)
        missing = corrected.copy()
        del missing["mavlink_system_id"]
        assert save_parameters(missing)[0] == 400
        # Restore fixture before exercising unrelated upload behavior.
        (app / "camera.ini").write_text(config)
        status, page = request("/sensors")
        assert status == 200 and photo.name.encode() in page
        assert b"id=thermal-min" not in page and b"id=lidar" not in page
        assert request("/file?path=" + quote(str(photo))) == (200, photo.read_bytes())
        assert request("/upgrade", method="POST", body=b"invalid")[0] == 403
        headers = {"X-CSRF-Token": csrf, "X-Firmware-Name": "ZR10_FW_test.bin"}
        fixture = os.environ.get("ZR10_TEST_FIRMWARE")
        firmware = (Path(fixture).read_bytes() if fixture else
                    pack(b"\x85\x19" + b"\xff" * (CUSTOMER_SIZE - 2)))
        installed = root / "mnt/ZR10_UpgradeSD.bin"
        def upload(data, expected=400, extra=None):
            status, body = request("/upgrade", "POST", body=data,
                                   extra_headers=headers | (extra or {}))
            assert status == expected, (status, body)
            if expected != 201:
                assert not installed.exists()
            assert not list((root / "mnt").glob("*.tmp"))
        upload(firmware, extra={"X-Firmware-Name": "A8_FW_test.bin"})
        upload(firmware[:-1])
        corrupt = bytearray(firmware)
        corrupt[SCRIPT_SIZE + 100] ^= 1
        upload(corrupt)
        # Even a self-consistent CRC cannot authorize a different flash script.
        corrupt = bytearray(firmware)
        corrupt[100] ^= 1
        struct.pack_into("<I", corrupt, len(corrupt)-28, zlib.crc32(corrupt[:-36]))
        upload(corrupt)
        # A conflict must not delete a pre-existing temporary upload.
        stale = root / "mnt/ZR10_FW_test.bin.tmp"
        stale.write_bytes(b"keep")
        assert request("/upgrade", "POST", body=firmware, extra_headers=headers)[0] == 409
        assert stale.read_bytes() == b"keep"
        stale.unlink()
        assert request("/upgrade", "POST", body=firmware, extra_headers=headers,
                       interrupt_after=32768)[0] == 400
        assert not installed.exists() and not list((root / "mnt").glob("*.tmp"))
        upload(firmware, 201)
        assert installed.read_bytes() == firmware
        assert request("/upgrade", "POST", body=firmware, extra_headers=headers)[0] == 409
        assert installed.read_bytes() == firmware
        installed.unlink()
        upload(firmware, 201, extra={"X-Firmware-Name": "ZR10_UpgradeSD.bin"})
        assert installed.read_bytes() == firmware
        installed.unlink()
        assert (app / "camera.ini").read_text() == config
        print("PASS ZR10 web: authentication, parameter saving and rejected-edit preservation, 1440p settings, SD photo browsing/download, exclusive app controls, checked firmware upload, no thermal")
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        process.stderr.close()
