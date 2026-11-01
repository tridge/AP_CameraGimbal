#!/usr/bin/env python3

import base64
import http.client
import hashlib
import json
import html
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.parse

from test_parameters_browser import check_parameters_browser
from test_reboot_browser import check_reboot_browser
from test_terrain_upload import check_terrain_upload


binary = Path(sys.argv[1]).resolve()
root = Path(sys.argv[2]).resolve()
shutil.rmtree(root, ignore_errors=True)
(root / "app" / "dropbear").mkdir(parents=True)
(root / "mnt").mkdir()
shutil.copytree(Path(__file__).resolve().parents[1] / "webroot", root / "webroot")
(root / "app" / "web.pass").write_text("initial-password\n", encoding="utf-8")
os.chmod(root / "app" / "web.pass", 0o600)
(root / "app" / "dropbear" / "authorized_keys").write_text("", encoding="utf-8")
(root / "app" / "config.ini").write_text(
    "[misc]\nautorecord = n\n", encoding="utf-8"
)
(root / "app" / "camera.ini").write_text(
    """# Configuration retained from an installation before MAVLink support.
[general]
timezone = GMT-10
[capture]
photo_scope = all
[mount]
orientation = auto
[thermal]
palette = white_hot
[recording]
autorecord = false
resolution = 1920x1080
[stream.main]
resolution = 1920x1080
codec = h264
[stream.sub]
resolution = 1280x720
codec = h264
[image]
brightness = 50
saturation = 50
contrast = 50
exposure_compensation = 0
iso = auto
shutter = auto
metering = average
white_balance = auto
""",
    encoding="utf-8",
)
temperature_registers = bytearray(4096)
for offset, raw in ((0x008, 600), (0x108, 590), (0x208, 610)):
    struct.pack_into("<I", temperature_registers, offset, raw | (raw << 16))
(root / "soc-temperature").write_bytes(temperature_registers)


def siyi_crc(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def siyi_packet(sequence, opcode, payload=b""):
    packet = struct.pack("<BBBHHB", 0x55, 0x66, 2, len(payload), sequence, opcode)
    packet += payload
    return packet + struct.pack("<H", siyi_crc(packet))


camera_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
camera_socket.bind(("127.0.0.1", 0))
camera_socket.settimeout(0.2)
camera_port = camera_socket.getsockname()[1]
camera_stop = threading.Event()
camera_events = {
    "captures": 0,
    "controls": [],
    "attitude_requests": 0,
    "lidar_enabled": False,
    "lidar_controls": [],
}


def camera_api_mock():
    while not camera_stop.is_set():
        try:
            packet, peer = camera_socket.recvfrom(4096)
        except socket.timeout:
            continue
        except OSError:
            break
        if len(packet) < 10 or packet[:2] != b"\x55\x66":
            continue
        payload_length, sequence, opcode = struct.unpack_from("<HHB", packet, 3)
        if len(packet) != payload_length + 10 or siyi_crc(packet[:-2]) != struct.unpack_from("<H", packet, len(packet) - 2)[0]:
            continue
        if opcode == 0x15 and camera_events["lidar_enabled"]:
            response_opcode = opcode
            payload = struct.pack("<H", 123)
        elif opcode == 0x14:
            response_opcode = opcode
            payload = struct.pack("<HHHHHH", 4242, 1234, 320, 240, 10, 20)
        elif opcode == 0x0D:
            camera_events["attitude_requests"] += 1
            response_opcode = opcode
            payload = struct.pack("<hhhhhh", 1234, -456, 78, 19, -20, 3)
        elif opcode == 0x0C and packet[8:-2] == b"\x00":
            camera_events["captures"] += 1
            capture = root / "mnt" / "DCIM" / "capture" / "2026-09-01"
            capture.mkdir(parents=True, exist_ok=True)
            for suffix in ("C", "Z", "I"):
                (capture / f"2026-09-01_12-00-00_000_{suffix}.jpg").write_bytes(
                    b"\xff\xd8integration-" + suffix.encode() + b"\xff\xd9"
                )
            response_opcode = 0x0B
            payload = b"\x00"
        elif opcode in (0x07, 0x08, 0x0F):
            camera_events["controls"].append((opcode, packet[8:-2]))
            continue
        elif opcode == 0x32 and packet[8:-2] in (b"\x00", b"\x01"):
            enabled = packet[8] == 1
            camera_events["lidar_enabled"] = enabled
            camera_events["lidar_controls"].append(enabled)
            continue
        else:
            continue
        camera_socket.sendto(siyi_packet(sequence, response_opcode, payload), peer)


camera_thread = threading.Thread(target=camera_api_mock, daemon=True)
camera_thread.start()

# Mock the private camera-app control endpoint independently of SIYI.
manual_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
manual_socket.bind(("127.0.0.1", 0))
(root / "camera-app.ready").write_text(
    f"backend=mt11\nmanual_port={manual_socket.getsockname()[1]}\n")
manual_events = []
manual_token = bytes(range(16))


def manual_mock():
    while True:
        data, peer = manual_socket.recvfrom(1024)
        magic, action, token, value, result = struct.unpack("=II16sfi", data)
        assert magic == 0x4d434131
        assert action == 1 or token == manual_token
        manual_events.append((action, value))
        manual_socket.sendto(struct.pack("=II16sfi", magic, action, manual_token, value, 0), peer)


threading.Thread(target=manual_mock, daemon=True).start()

live_socket = socket.socket()
live_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
live_socket.bind(("127.0.0.1", 0))
live_socket.listen()
live_socket.settimeout(0.2)
live_port = live_socket.getsockname()[1]
live_stop = threading.Event()
live_payload = b"\x00\x00\x00\x18ftypisomnative-test-moov-moof-mdat"


def live_video_mock():
    while not live_stop.is_set():
        try:
            client, _ = live_socket.accept()
        except socket.timeout:
            continue
        except OSError:
            break
        try:
            selection = client.recv(1)
            if selection in (b"\x00", b"\x01"):
                client.sendall(b"\x00" + live_payload + selection)
        finally:
            client.close()


live_thread = threading.Thread(target=live_video_mock, daemon=True)
live_thread.start()

listener = socket.socket()
listener.bind(("127.0.0.1", 0))
port = listener.getsockname()[1]
listener.close()

server = subprocess.Popen(
    [str(binary), "-p", str(port)],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
    env={
        **os.environ,
        "MT11_WEB_CAMERA_PORT": str(camera_port),
        "MT11_WEB_LIVE_PORT": str(live_port),
    },
)


def authorization(password):
    encoded = base64.b64encode(f"admin:{password}".encode()).decode()
    return f"Basic {encoded}"


def request(method, path, password, body=None, headers=None):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=120 if path == "/survey/terrain" else 5)
    all_headers = {"Authorization": authorization(password)}
    if headers:
        all_headers.update(headers)
    connection.request(method, path, body=body, headers=all_headers)
    response = connection.getresponse()
    contents = response.read()
    result = response.status, contents, dict(response.getheaders())
    connection.close()
    return result


def form(path, password, csrf, values):
    fields = {"csrf": csrf, **values}
    body = urllib.parse.urlencode(fields).encode()
    return request(
        "POST",
        path,
        password,
        body,
        {"Content-Type": "application/x-www-form-urlencoded"},
    )


def get_csrf(password):
    status, body, _ = request("GET", "/", password)
    assert status == 200, (status, body)
    match = re.search(rb'data-csrf="([0-9a-f]{64})"', body)
    assert match, body[:500]
    return match.group(1).decode()


def test_large_recording_download():
    video = root / "mnt" / "large-recording.mp4"
    offset = 2**32 + 123
    payload = b"recording beyond 4 GiB"
    with video.open("wb") as output:
        output.seek(offset)
        output.write(payload)
    path = "/file?" + urllib.parse.urlencode({"path": str(video), "download": "1"})
    status, body, headers = request("HEAD", path, "initial-password")
    assert status == 200 and not body
    assert int(headers["Content-Length"]) == offset + len(payload)
    status, body, headers = request("GET", path, "initial-password",
                                    headers={"Range": f"bytes={offset}-"})
    assert status == 206 and body == payload
    assert headers["Content-Range"] == f"bytes {offset}-{offset+len(payload)-1}/{offset+len(payload)}"
    video.unlink()
    print("PASS recording HEAD and range download beyond 4 GiB")


def raw_request(method, path, headers=None, body=None):
    """request without HTTP Basic credentials, as a browser would send it"""
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    connection.request(method, path, body=body, headers=headers or {})
    response = connection.getresponse()
    contents = response.read()
    result = response.status, contents, response.getheaders()
    connection.close()
    return result


def set_cookies(headers):
    """name -> (value, attributes) from every Set-Cookie header"""
    cookies = {}
    for name, value in headers:
        if name.lower() != "set-cookie":
            continue
        pair, _, attributes = value.partition(";")
        cookie, _, cookie_value = pair.partition("=")
        cookies[cookie.strip()] = (cookie_value.strip(), attributes.strip())
    return cookies


def cookie_header(cookies):
    return "; ".join(f"{name}={value}" for name, value in cookies.items())


def login_form(password, extra=None):
    status, body, _ = raw_request("GET", "/login")
    assert status == 200, (status, body)
    token = re.search(rb'name=token value="([0-9a-f]{64})"', body).group(1).decode()
    fields = {"token": token, "username": "admin", "password": password}
    fields.update(extra or {})
    return raw_request(
        "POST",
        "/login",
        {"Content-Type": "application/x-www-form-urlencoded",
         "Accept": "text/html"},
        urllib.parse.urlencode(fields).encode(),
    )


def test_login_and_languages(csrf):
    session_file = root / "sessions"

    # Favicons must load before authentication, including on the login page.
    assets = Path(__file__).resolve().parents[2] / "assets"
    for path, source, mime in (("/favicon.svg", "camera-gimbal.svg", "image/svg+xml"),
                               ("/favicon.ico", "favicon.ico", "image/vnd.microsoft.icon")):
        status, body, headers = raw_request("GET", path)
        assert status == 200 and dict(headers)["Content-Type"] == mime
        assert body == (assets / source).read_bytes()
    status, body, _ = raw_request("GET", "/login")
    assert status == 200 and b"href=/favicon.svg" in body and b"href=/favicon.ico" in body

    # browsers are sent to the login form; scripted clients keep Basic
    status, _, headers = raw_request("GET", "/", {"Accept": "text/html,*/*"})
    assert status == 303 and dict(headers)["Location"] == "/login"
    status, _, headers = raw_request("GET", "/sensors.json")
    assert status == 401 and dict(headers)["WWW-Authenticate"].startswith("Basic")
    status, _, headers = raw_request("GET", "/live/video1.mp4", {"Accept": "*/*"})
    assert status == 401
    # only a real text/html media range counts as a browser navigation
    status, _, _ = raw_request("GET", "/", {"Accept": "text/html;q=0, */*"})
    assert status == 401
    status, _, _ = raw_request("GET", "/", {"Accept": "application/json, text/html-ish"})
    assert status == 401
    status, _, _ = raw_request("GET", "/", {"Accept": "text/html;Q=0"})
    assert status == 401
    status, _, _ = raw_request("GET", "/", {"Accept": 'application/json;note="x, text/html;q=1"'})
    assert status == 401
    status, _, _ = raw_request("GET", "/", {"Accept": "text/html;q=2"})
    assert status == 401
    status, _, _ = raw_request("GET", "/", {"Accept": "text/html;q=0.8, */*;q=0.1"})
    assert status == 303
    for malformed in ("text/html;q =0", "text/html;q=0 q=1", 'text/html;note="unterminated',
                      "text/html q=1", "text/html;=1"):
        status, _, _ = raw_request("GET", "/", {"Accept": malformed})
        assert status == 401, malformed
    status, _, _ = raw_request("GET", "/", {"Accept": 'text/html;level="1, 2";q=1'})
    assert status == 303
    # a failed Basic attempt is answered with the Basic challenge even for a browser
    status, _, headers = request("GET", "/", "wrong-password",
                                 headers={"Accept": "text/html"})
    assert status == 401 and "WWW-Authenticate" in dict(headers)

    status, body, headers = raw_request("GET", "/login")
    assert status == 200 and b"<html lang=en>" in body
    assert b"name=token value=" in body and b"id=lang-login" in body
    assert b"<code>admin</code>" in body and b"<code>ardupilot</code>" in body
    assert str(root / "app" / "web.pass").encode() in body
    assert b"src=/language.js" in body and b'data-csrf="' not in body
    assert b"Log in" in body
    status, body, headers = raw_request("GET", "/language.js")
    assert status == 200 and b"lang-login" in body
    subprocess.run(["node", "--check"], input=body, check=True)

    status, stylesheet, style_headers = raw_request("GET", "/style.css")
    assert status == 200 and b".live-video" in stylesheet
    assert dict(style_headers)["Content-Type"].startswith("text/css")
    assert "style-src 'self'" in dict(style_headers)["Content-Security-Policy"]
    for private_asset in ("/head.html", "/webroot/head.html", "/../head.html", "/%2e%2e/head.html"):
        status, _, _ = request("GET", private_asset, "initial-password")
        assert status == 404, private_asset

    for route, name, mime in (("/mpegts.js", "mpegts.js", "application/javascript"),
                              ("/recording.js", "recording.js", "application/javascript"),
                              ("/mpegts-license", "mpegts-license.html", "text/html")):
        assert raw_request("GET", route)[0] == 401, route
        status, body, headers = request("GET", route, "initial-password")
        assert status == 200 and headers["Content-Type"].startswith(mime), route
        assert body == (root / "webroot" / name).read_bytes()
        assert "media-src 'self' blob:" in headers["Content-Security-Policy"]
        assert "script-src 'self'" in headers["Content-Security-Policy"]
    assert hashlib.sha256((root / "webroot/mpegts.js").read_bytes()).hexdigest() == "e998f7a6cec5fcab56ac14e8c22d61a42565c1ebf7174b3fda078343eda6cab0"
    notices = (root / "webroot/mpegts-license.html").read_text()
    for attribution in ("Apache License", "Bilibili", "Copyright Joyent", "Yehuda Katz",
                        "Roman Shtylman", "Boris Sirota", "JS Foundation", "Dailymotion", "StarBrilliant"):
        assert attribution in notices, attribution
    for suffix, mime in (("ts", "video/mp2t"), ("mp4", "video/mp4")):
        recording = root / "mnt" / ("viewer-test." + suffix)
        payload = b"original telemetry and video bytes" * 20
        recording.write_bytes(payload)
        query = urllib.parse.urlencode({"path": str(recording)})
        status, body, _ = request("GET", "/view?" + query, "initial-password")
        assert status == 200 and b"download=1" in body
        assert b"href=/mpegts-license" not in body
        assert (b"id=recording-player" in body) == (suffix == "ts")
        assert (b"controls preload=metadata" in body) == (suffix == "mp4")
        status, body, headers = request("GET", "/file?" + query + "&download=1", "initial-password")
        assert status == 200 and body == payload and headers["Content-Type"] == mime
        assert headers["Content-Disposition"].startswith("attachment;")
        status, body, headers = request("GET", "/file?" + query, "initial-password", headers={"Range": "bytes=7-31"})
        assert status == 206 and body == payload[7:32]
        assert headers["Content-Range"] == f"bytes 7-31/{len(payload)}"
        recording.unlink()
    print("PASS authenticated TS player assets, CSP, MP4/TS viewing and original range downloads")

    # A partial/missing install must report an asset failure, never empty 200
    # or a misleading allocation error. Keep the source tree untouched.
    head = root / "webroot/head.html"
    head.rename(root / "saved-head.html")
    for route in ("/", "/login", "/parameters", "/rebooting"):
        status, body, _ = request("GET", route, "initial-password") if route != "/login" else raw_request("GET", route)
        assert status == 503 and b"assets unavailable" in body, (route, status, body)
        assert b"Out of memory" not in body
    (root / "saved-head.html").rename(head)
    assert request("GET", "/rebooting", "initial-password")[0] == 200

    # wrong password, expired form token
    status, body, headers = login_form("wrong-password")
    assert status == 401 and b"Incorrect username or password" in body
    assert "session" not in set_cookies(headers)
    assert "WWW-Authenticate" not in dict(headers)
    status, body, _ = raw_request(
        "POST", "/login",
        {"Content-Type": "application/x-www-form-urlencoded"},
        urllib.parse.urlencode({"token": "0" * 64, "username": "admin",
                                "password": "initial-password"}).encode(),
    )
    assert status == 400 and b"login form has expired" in body
    assert not session_file.exists() or session_file.read_text() == ""
    # a cross-site form post carries a foreign Origin and is refused
    status, body, _ = raw_request("GET", "/login")
    token = re.search(rb'name=token value="([0-9a-f]{64})"', body).group(1).decode()
    status, body, headers = raw_request(
        "POST", "/login",
        {"Content-Type": "application/x-www-form-urlencoded",
         "Origin": "http://evil.example", "Host": f"127.0.0.1:{port}"},
        urllib.parse.urlencode({"token": token, "username": "admin",
                                "password": "initial-password"}).encode(),
    )
    assert status == 403 and b"Cross-site login" in body
    assert "session" not in set_cookies(headers)
    status, _, headers = raw_request(
        "POST", "/login",
        {"Content-Type": "application/x-www-form-urlencoded",
         "Origin": f"http://127.0.0.1:{port}", "Host": f"127.0.0.1:{port}"},
        urllib.parse.urlencode({"token": token, "username": "admin",
                                "password": "initial-password"}).encode(),
    )
    assert status == 303 and "session" in set_cookies(headers)
    same_origin_session = set_cookies(headers)["session"][0]

    # successful login sets an HttpOnly session and the chosen language
    status, body, headers = login_form("initial-password", {"lang": "zh"})
    assert status == 303 and dict(headers)["Location"] == "/"
    cookies = set_cookies(headers)
    session, attributes = cookies["session"]
    assert re.fullmatch(r"[0-9a-f]{64}", session), session
    assert "HttpOnly" in attributes and "SameSite=Strict" in attributes
    assert "Path=/" in attributes and "Max-Age=86400" in attributes
    assert cookies["lang"][0] == "zh" and "SameSite=Strict" in cookies["lang"][1]
    assert session_file.stat().st_mode & 0o777 == 0o600
    assert session in session_file.read_text()
    boot_id = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
    assert session_file.read_text().split("\n")[0] == "boot " + boot_id
    jar = {"session": session, "lang": "zh"}

    # a session file left over from another boot is not trusted
    saved = session_file.read_text()
    session_file.write_text(saved.replace(saved.split("\n")[0], "boot 0-0-0-0-0"))
    status, _, headers = raw_request(
        "GET", "/", {"Cookie": cookie_header(jar), "Accept": "text/html"}
    )
    assert status == 303 and dict(headers)["Location"] == "/login"
    session_file.write_text(saved)
    # a record with a signed expiry is ignored
    forged = "f" * 64
    session_file.write_text(saved + forged + " -1\n")
    status, _, _ = raw_request("GET", "/", {"Cookie": "session=" + forged, "Accept": "text/html"})
    assert status == 303
    session_file.write_text(saved)
    # a file another user could have planted is ignored
    os.chmod(session_file, 0o644)
    status, _, _ = raw_request("GET", "/", {"Cookie": cookie_header(jar), "Accept": "text/html"})
    assert status == 303
    os.chmod(session_file, 0o600)

    status, body, _ = raw_request("GET", "/", {"Cookie": cookie_header(jar)})
    assert status == 200 and b"<html lang=zh-CN>" in body
    assert "状态".encode() in body and "退出登录".encode() in body
    assert "重启相机".encode() in body and "SoC 温度".encode() in body
    assert b"action=/logout" in body and b"id=lang-nav" in body
    status, body, _ = raw_request("GET", "/parameters", {"Cookie": cookie_header(jar)})
    assert status == 200 and "MAVLink TCP 端口".encode() in body
    assert "安装方向".encode() in body and b"[mount] orientation" in body
    assert b'name="orientation" id="orientation">' in body
    assert "自动（由云台上报）".encode() in body
    status, body, _ = raw_request("GET", "/sensors.js", {"Cookie": cookie_header(jar)})
    assert status == 200 and "无有效回波".encode() in body
    subprocess.run(["node", "--check"], input=body, check=True)

    # a wrong-language cookie in a browser session may not use a stale
    # session token
    status, _, headers = raw_request(
        "GET", "/", {"Cookie": "session=" + "0" * 64, "Accept": "text/html"}
    )
    assert status == 303 and dict(headers)["Location"] == "/login"

    # switching language from the nav returns to the page and sets the cookie
    status, _, headers = raw_request(
        "POST", "/language",
        {"Content-Type": "application/x-www-form-urlencoded",
         "Cookie": cookie_header(jar)},
        urllib.parse.urlencode({"csrf": csrf, "lang": "ja", "next": "/users"}).encode(),
    )
    assert status == 303 and dict(headers)["Location"] == "/users"
    assert set_cookies(headers)["lang"][0] == "ja"
    jar["lang"] = "ja"
    status, body, _ = raw_request("GET", "/users", {"Cookie": cookie_header(jar)})
    assert status == 200 and b"<html lang=ja>" in body
    assert "管理者パスワード".encode() in body and "ログアウト".encode() in body
    assert "登録済みの SSH 鍵".encode() in body
    status, body, _ = raw_request("GET", "/live", {"Cookie": cookie_header(jar)})
    assert status == 200 and "ジンバルの手動操作を有効にする".encode() in body
    status, body, _ = raw_request("GET", "/live.js", {"Cookie": cookie_header(jar)})
    assert status == 200 and "接続中".encode() in body
    subprocess.run(["node", "--check"], input=body, check=True)
    status, body, _ = raw_request("GET", "/log", {"Cookie": cookie_header(jar)})
    assert status == 200 and "デバッグ".encode() in body
    status, body, _ = raw_request("GET", "/files", {"Cookie": cookie_header(jar)})
    assert status == 200 and "ファイルシステム".encode() in body
    status, _, headers = raw_request(
        "POST", "/language",
        {"Content-Type": "application/x-www-form-urlencoded",
         "Cookie": cookie_header(jar)},
        urllib.parse.urlencode({"csrf": csrf, "lang": "zh",
                                "next": "http://example.invalid/"}).encode(),
    )
    assert status == 303 and dict(headers)["Location"] == "/"
    status, _, _ = raw_request(
        "POST", "/language",
        {"Content-Type": "application/x-www-form-urlencoded",
         "Cookie": cookie_header(jar)},
        urllib.parse.urlencode({"csrf": csrf, "lang": "xx", "next": "/"}).encode(),
    )
    assert status == 400

    # Accept-Language chooses the language when there is no cookie, and the
    # login page can switch before logging in
    status, body, _ = request("GET", "/", "initial-password",
                              headers={"Accept-Language": "zh-CN,zh;q=0.9,en;q=0.8"})
    assert status == 200 and b"<html lang=zh-CN>" in body
    status, body, _ = request("GET", "/", "initial-password",
                              headers={"Accept-Language": "fr"})
    assert status == 200 and b"<html lang=en>" in body
    status, body, _ = request("GET", "/", "initial-password",
                              headers={"Accept-Language": "en;q=0.1, ja;q=0.9"})
    assert status == 200 and b"<html lang=ja>" in body
    status, body, _ = request("GET", "/", "initial-password",
                              headers={"Accept-Language": "ja;q=0, zh-CN"})
    assert status == 200 and b"<html lang=zh-CN>" in body
    status, body, _ = request("GET", "/", "initial-password",
                              headers={"Accept-Language": "en123, ja"})
    assert status == 200 and b"<html lang=ja>" in body
    status, body, _ = request("GET", "/", "initial-password",
                              headers={"Accept-Language": "en@bad;q=0, ja"})
    assert status == 200 and b"<html lang=ja>" in body
    status, body, _ = request("GET", "/", "initial-password",
                              headers={"Accept-Language": "en;foo=bar, ja"})
    assert status == 200 and b"<html lang=ja>" in body
    status, body, _ = request("GET", "/", "initial-password",
                              headers={"Accept-Language": "zh-Hans-CN ; q=0.5 , ja;q=0.4"})
    assert status == 200 and b"<html lang=zh-CN>" in body
    status, body, headers = raw_request("GET", "/login?lang=ja")
    assert status == 200 and b"<html lang=ja>" in body
    assert "ログイン".encode() in body and set_cookies(headers)["lang"][0] == "ja"
    status, body, _ = raw_request("GET", "/login", {"Cookie": "lang=zh"})
    assert status == 200 and b"<html lang=zh-CN>" in body and "登录".encode() in body

    # logout drops the session on the server as well as in the browser
    status, _, headers = raw_request(
        "POST", "/logout",
        {"Content-Type": "application/x-www-form-urlencoded",
         "Cookie": cookie_header(jar)},
        urllib.parse.urlencode({"csrf": csrf}).encode(),
    )
    assert status == 303 and dict(headers)["Location"] == "/login"
    assert set_cookies(headers)["session"][0] == ""
    assert "Max-Age=0" in set_cookies(headers)["session"][1]
    assert session not in session_file.read_text()
    status, _, headers = raw_request(
        "GET", "/", {"Cookie": cookie_header(jar), "Accept": "text/html"}
    )
    assert status == 303 and dict(headers)["Location"] == "/login"
    # logging out with Basic credentials still revokes the cookie's session
    assert same_origin_session in session_file.read_text()
    status, _, _ = request(
        "POST", "/logout", "initial-password",
        urllib.parse.urlencode({"csrf": csrf}).encode(),
        {"Content-Type": "application/x-www-form-urlencoded",
         "Cookie": "session=" + same_origin_session},
    )
    assert status == 303 and same_origin_session not in session_file.read_text()
    status, _, headers = raw_request(
        "POST", "/logout",
        {"Content-Type": "application/x-www-form-urlencoded", "Cookie": "lang=en",
         "Accept": "text/html"},
        urllib.parse.urlencode({"csrf": csrf}).encode(),
    )
    assert status == 303 and dict(headers)["Location"] == "/login"

    # Basic auth continues to work exactly as before, in English by default
    status, body, _ = request("GET", "/", "initial-password")
    assert status == 200 and b"<html lang=en>" in body and b">Status</h2>" in body
    assert request("GET", "/", "wrong-password")[0] == 401


def test_reboot_redirect(csrf):
    # a confirmed reboot redirects so a page refresh cannot reboot again
    status, body, _ = form("/reboot", "initial-password", csrf, {})
    assert status == 400 and b"Reboot confirmation was not checked" in body
    status, body, headers = form("/reboot", "initial-password", csrf, {"confirm": "yes"})
    assert status == 303 and headers["Location"] == "/rebooting", (status, body)
    status, body, page_headers = request("GET", "/rebooting", "initial-password")
    assert status == 200 and b"<h1 id=reboot-heading>Camera rebooting</h1>" in body
    script_status, script, _ = raw_request("GET", "/reboot.js")
    assert script_status == 200  # It still loads after the login session expires.
    auth_status, _, auth_headers = raw_request("GET", "/upgrade-status",
        headers={"Cookie": "session=" + "a" * 64, "Accept": "*/*"})
    assert auth_status == 401
    assert "www-authenticate" not in {key.lower() for key, _ in auth_headers}
    check_reboot_browser(body, script, page_headers["Content-Security-Policy"], dict(auth_headers))
    status, token, _ = request("GET", "/upgrade-status", "initial-password")
    assert status == 200 and token.decode() == csrf
    # Reloading the GET page never submits another reboot request.
    assert request("GET", "/rebooting", "initial-password")[0] == 200


try:
    deadline = time.monotonic() + 5
    while True:
        try:
            csrf = get_csrf("initial-password")
            break
        except (ConnectionRefusedError, OSError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.05)

    test_large_recording_download()
    check_terrain_upload(request, root, csrf, port)
    test_login_and_languages(csrf)
    test_reboot_redirect(csrf)

    status, body, _ = request("GET", "/users", "initial-password")
    assert status == 200 and b"Authorized SSH keys" in body
    assert b"id=public-key-files type=file" in body
    assert b"<textarea" not in body
    assert b"src=/users.js" in body
    status, body, headers = request("GET", "/users.js", "initial-password")
    assert status == 200 and b"public-key-files" in body
    assert headers["Content-Type"].startswith("application/javascript")
    subprocess.run(["node", "--check"], input=body, check=True)
    status, body, _ = request("GET", "/", "initial-password")
    assert status == 200 and b"SoC temperature" in body and b"64.3 &deg;C" in body
    assert b"Firmware version</th><td id=firmware-version>" in body
    assert b" (<span id=firmware-git-hash>" in body
    assert b"id=time-sync" in body and b">Sync</button>" in body
    # the configured [general] timezone (GMT-10 is fixed UTC+10) applies to page times
    assert b" GMT (UTC+1000)<form id=time-sync" in body
    assert b"ArduPilot camera app" in body and b"replacement app" not in body
    status, script, headers = request("GET", "/status.js", "initial-password")
    assert status == 200 and b"Date.now()" in script
    assert headers["Content-Type"].startswith("application/javascript")
    subprocess.run(["node", "--check"], input=script, check=True)

    status, body, _ = request("GET", "/parameters", "initial-password")
    assert status == 200 and b"<h1>Parameters</h1>" in body
    assert b'name="mavlink_tcp_port"' in body and b'value="14550"' in body
    assert b'name="mavlink_system_id" id="mavlink_system_id" type=number min="0" max="255" step="1" value="0"' in body
    assert b'<option value="100" selected>Camera 1 (100)</option>' in body
    assert b'<option value="105">Camera 6 (105)</option>' in body
    for name in (b"proxy_video1_port", b"proxy_video2_port", b"proxy_video3_port"):
        assert re.search(rb'<input[^>]*name="' + name + rb'"[^>]*value="0"', body)
    assert b'name="position_targeting"' in body
    assert b'name="osd_cross"' in body and b'name="osd_thermal_fov"' in body
    assert b'value="true" selected' in body
    assert b'name="uart_protocol"' not in body
    assert b'The external UART is handled by the gimbal controller.' in body
    assert b'<option value="while_armed">While Armed</option>' in body
    assert b'<select required name="timezone" id="timezone">' in body
    assert b'<option selected value="GMT-10">GMT-10</option>' in body
    for zone in (b"GMT", b"Europe/London", b"America/New_York", b"Australia/Sydney", b"Asia/Kathmandu"):
        assert b'<option value="' + zone + b'">' + zone + b'</option>' in body
    replacement_parameters = {
        "survey_pattern": "both", "survey_lens": "thermal", "survey_overlap": "50", "survey_fore_pct": "50",
        "survey_aft_pct": "50", "survey_burst": "1", "survey_burst_ms": "100",
        "survey_dwell_ms": "1200", "survey_settle_ms": "200", "survey_error_cd": "100",
        "survey_rate_cd": "100", "survey_terrain_ms": "3000", "survey_terrain_m": "500",
        "survey_terrain_tol": "25", "survey_min_speed": "5", "survey_turn_deg": "15",
        "timezone": "Asia/Kathmandu",
        "photo_scope": "all",
        "orientation": "auto",
        "mavlink_system_id": "42",
        "mavlink_camera_component_id": "105",
        "mavlink_tcp_port": "14600",
        "mavlink_udp_port": "14601",
        "position_targeting": "false",
        "tracking_method": "angle",
        "osd_cross": "true",
        "osd_recording": "true",
        "osd_thermal_fov": "true",
        "log_disarmed": "false",
        "thermal_palette": "white_hot",
        "raw_stream_fps": "8",
        "raw_record_fps": "3",
        "autorecord": "while_armed",
        "recording_resolution": "1920x1080",
        "main_resolution": "1920x1080",
        "main_codec": "h264",
        "video_telemetry": "apcg",
        "sub_resolution": "1280x720",
        "sub_codec": "h264",
        "main_alias": "main.264",
        "sub_alias": "",
        "brightness": "50",
        "saturation": "50",
        "contrast": "50",
        "exposure_compensation": "0",
        "iso": "auto",
        "shutter": "auto",
        "metering": "average",
        "white_balance": "auto",
        "proxy_enabled": "false",
        "proxy_host": "",
        "proxy_mavlink_port": "10001",
        "proxy_signing": "false",
        "proxy_signing_passphrase": "",
        "proxy_signing_link_id": "1",
        "proxy_video1_port": "40001",
        "proxy_video1_name": "video1",
        "proxy_video2_port": "40002",
        "proxy_video3_port": "40003",
        "proxy_video3_name": "Raw Thermal",
        "proxy_video2_name": "video2",
        "proxy_publish_password": "",
        "network_capture": "false",
        "network_interface": "eth0",
        "network_primary_address": "",
        "network_secondary_address": "",
        "network_gateway": "",
        "action": "save",
    }
    status, body, _ = form(
        "/parameters", "initial-password", csrf, replacement_parameters
    )
    assert status == 200 and b"Parameters saved" in body, (status, body.decode())
    status, _, headers = form("/survey", "initial-password", csrf, {"survey_mode": "2"})
    assert status == 303 and headers["Location"] == "/parameters#survey"
    assert (root / "app" / "camera.ini.survey.command").read_text().strip() == "2"
    assert form("/survey", "initial-password", csrf, {"survey_mode": "9"})[0] == 400
    # Match the shared runtime filename and verify both live and stale status.
    config_hash = 14695981039346656037
    for byte in os.fsencode(root / "app" / "camera.ini"):
        config_hash = ((config_hash ^ byte) * 1099511628211) & 0xffffffffffffffff
    runtime_root = os.environ.get("TMPDIR") or (os.environ.get("TEMP", ".") if os.name == "nt" else "/tmp")
    status_path = Path(runtime_root) / f"apcam-survey-{config_hash:016x}.json"
    try:
        status_path.write_text('{"state":"surveying","captured":17}')
        status, body, _ = request("GET", "/survey/status", "initial-password")
        assert status == 200 and json.loads(body)["captured"] == 17, (status, body)
        stale = time.time() - 10
        os.utime(status_path, (stale, stale))
        status, body, _ = request("GET", "/survey/status", "initial-password")
        assert status == 200 and json.loads(body)["state"] == "camera unavailable"
    finally:
        status_path.unlink(missing_ok=True)
    saved_config = (root / "app" / "camera.ini").read_text(encoding="utf-8")
    assert saved_config.count("[mavlink]") == 1
    assert "[uart]" not in saved_config
    assert 'system_id = 42' in saved_config
    assert 'camera_component_id = 105' in saved_config
    assert 'tcp_port = 14600' in saved_config
    assert 'udp_port = 14601' in saved_config
    assert 'position_targeting = false' in saved_config
    assert 'cross = true' in saved_config and 'thermal_fov = true' in saved_config
    assert 'recording = true' in saved_config
    assert 'autorecord = while_armed' in saved_config
    assert 'stream_fps = 8' in saved_config
    assert 'record_fps = 3' in saved_config
    assert 'alias = main.264' in saved_config and 'alias = ""' in saved_config
    assert "retained from an installation" in saved_config
    status, body, _ = request("GET", "/parameters", "initial-password")
    assert status == 200
    assert b'name="mavlink_tcp_port" id="mavlink_tcp_port" type=number' in body
    assert b'value="14600"' in body and b'value="14601"' in body
    assert b'<option value="while_armed" selected>While Armed</option>' in body

    assert b'<h2>SupportProxy</h2>' in body
    assert b'name="proxy_publish_password" id="proxy_publish_password" type=password' in body
    assert b'<input required name="proxy_publish_password"' not in body
    assert 'timezone = Asia/Kathmandu' in (root / "app/camera.ini").read_text()
    status, timezone_page, _ = request("GET", "/parameters", "initial-password")
    assert status == 200 and b'<option value="Asia/Kathmandu" selected>' in timezone_page
    status, timezone_status, _ = request("GET", "/", "initial-password")
    assert status == 200 and b"(UTC+0545)" in timezone_status
    status, _, _ = form("/parameters", "initial-password", csrf,
                        dict(replacement_parameters, timezone="Asia/Typo"))
    assert status == 400
    assert 'timezone = Asia/Kathmandu' in (root / "app/camera.ini").read_text()

    valid_proxy = dict(replacement_parameters, proxy_enabled="true", proxy_host="localhost",
                       proxy_signing="true", proxy_signing_passphrase=" a signing phrase ",
                       proxy_publish_password="publish&?=secret", proxy_video1_name="Front Camera",
                       network_secondary_address="192.0.2.25/24", network_gateway="192.0.2.1")
    status, proxy_body, _ = form("/parameters", "initial-password", csrf, valid_proxy)
    assert status == 200 and b"Parameters saved" in proxy_body
    proxy_config = (root / "app" / "camera.ini").read_text()
    assert 'signing_passphrase = " a signing phrase "' in proxy_config
    assert 'video1_name = Front Camera' in proxy_config

    def input_value(page, name):
        match = re.search(r'<input[^>]* name="' + re.escape(name) +
                          r'"[^>]* value="([^"]*)"', page.decode())
        assert match, name
        return html.unescape(match.group(1))

    # Reload and resubmit what the browser actually displays, including spaces
    # in secrets. Reusing the original POST dictionary hid reload corruption.
    _, loaded_proxy, _ = request("GET", "/parameters", "initial-password")
    assert b'src=/parameters.js defer' in loaded_proxy
    status, validation_script, headers = request("GET", "/parameters.js", "initial-password")
    assert status == 200 and headers["Content-Type"].startswith("application/javascript")
    subprocess.run(["node", "--check"], input=validation_script, check=True)
    check_parameters_browser(loaded_proxy, validation_script)
    browser_proxy = dict(valid_proxy)
    for name in ("proxy_signing_passphrase", "proxy_publish_password", "proxy_video1_name"):
        browser_proxy[name] = input_value(loaded_proxy, name)
        assert browser_proxy[name] == valid_proxy[name]
    _, saved_proxy, _ = form("/parameters", "initial-password", csrf, browser_proxy)
    assert b"Parameters saved" in saved_proxy
    assert (root / "app" / "camera.ini").read_text() == proxy_config
    assert b'Secondary IPv4 address/prefix' in loaded_proxy
    assert b'placeholder="192.168.2.97/24"' not in loaded_proxy
    assert b'For example 192.168.144.27/24' in loaded_proxy
    for changes in ({"network_primary_address": "192.168.2.97"},
                    {"network_primary_address": "192.0.2.0/24"},
                    {"network_primary_address": "192.0.2.255/24"},
                    {"network_primary_address": "192.0.2.25/24"},
                    {"network_primary_address": "198.51.100.27/24", "network_gateway": "203.0.113.1"},
                    {"network_secondary_address": "192.168.2.97"},
                    {"network_secondary_address": "192.0.2.25/33"},
                    {"network_gateway": "bad"}, {"proxy_host": "bad/host"},
                    {"proxy_signing_passphrase": ""}, {"proxy_video2_port": "40001"},
                    {"proxy_video3_port": "40002"}, {"proxy_video3_name": ""},
                    {"proxy_video1_name": ""}, {"proxy_mavlink_port": "65536"}):
        _, invalid_body, _ = form("/parameters", "initial-password", csrf, dict(valid_proxy, **changes))
        assert b"Parameters saved" not in invalid_body
        if "network_secondary_address" in changes:
            assert b"Optional second address, for example 192.168.20.25/24" in invalid_body
        assert (root / "app" / "camera.ini").read_text() == proxy_config
        for name, value in dict(valid_proxy, **changes).items():
            if name.startswith("proxy_") and name not in ("proxy_enabled", "proxy_signing"):
                assert input_value(invalid_body, name) == value, name
    # Preserve even values after the first invalid field, without injecting HTML.
    rejected = dict(valid_proxy, brightness="101", proxy_host="changed.example.org",
                    proxy_video1_name='<script>alert("camera")</script>', action="save_restart")
    _, rejected_body, _ = form("/parameters", "initial-password", csrf, rejected)
    assert b"Parameters saved" not in rejected_body
    assert input_value(rejected_body, "proxy_host") == rejected["proxy_host"]
    assert input_value(rejected_body, "proxy_video1_name") == rejected["proxy_video1_name"]
    assert b'<script>alert("camera")</script>' not in rejected_body
    assert (root / "app" / "camera.ini").read_text() == proxy_config
    # Network configuration is independent of SupportProxy and survives a reload.
    network_parameters = dict(replacement_parameters, network_primary_address="198.51.100.27/24",
                              network_secondary_address="192.0.2.25/24", network_gateway="192.0.2.1")
    _, network_body, _ = form("/parameters", "initial-password", csrf, network_parameters)
    assert b"Parameters saved" in network_body
    _, network_body, _ = request("GET", "/parameters", "initial-password")
    for name in ("network_primary_address", "network_secondary_address", "network_gateway"):
        assert input_value(network_body, name) == network_parameters[name]
    # Old enabled SupportProxy fields appear in the new Network panel, but
    # explicit empty new fields must suppress legacy fallback after removal.
    legacy = "[support_proxy]\nenabled=true\nhost=localhost\nnetwork_interface=eth1\nnetwork_address=192.0.2.25/24\nnetwork_gateway=192.0.2.1\n"
    (root / "app" / "camera.ini").write_text(legacy)
    _, legacy_body, _ = request("GET", "/parameters", "initial-password")
    assert input_value(legacy_body, "network_interface") == "eth1"
    assert input_value(legacy_body, "network_secondary_address") == "192.0.2.25/24"
    assert input_value(legacy_body, "network_gateway") == "192.0.2.1"
    (root / "app" / "camera.ini").write_text(legacy + '[network]\nsecondary_address=""\ngateway=""\n')
    _, legacy_body, _ = request("GET", "/parameters", "initial-password")
    assert input_value(legacy_body, "network_secondary_address") == ""
    assert input_value(legacy_body, "network_gateway") == ""
    (root / "app" / "camera.ini").write_text(proxy_config)
    # Restore disabled defaults, including clearing both optional secrets.
    status, _, _ = form("/parameters", "initial-password", csrf, replacement_parameters)
    assert status == 200 and (root / "app" / "camera.ini").read_text() == saved_config

    for invalid_id in ("-1", "256", "1.5"):
        invalid_parameters = dict(replacement_parameters, mavlink_system_id=invalid_id)
        _, invalid_body, _ = form(
            "/parameters", "initial-password", csrf, invalid_parameters
        )
        assert b"Parameters saved" not in invalid_body
        assert (root / "app" / "camera.ini").read_text() == saved_config

    for invalid_id in ("99", "106", "100.5"):
        invalid_parameters = dict(replacement_parameters, mavlink_camera_component_id=invalid_id)
        _, invalid_body, _ = form("/parameters", "initial-password", csrf, invalid_parameters)
        assert b"Parameters saved" not in invalid_body
        assert (root / "app" / "camera.ini").read_text() == saved_config

    replacement_parameters["mavlink_tcp_port"] = "14700"
    replacement_parameters["mavlink_udp_port"] = "14701"
    status, body, _ = form(
        "/parameters", "initial-password", csrf, replacement_parameters
    )
    assert status == 200 and b"Parameters saved" in body
    saved_config = (root / "app" / "camera.ini").read_text(encoding="utf-8")
    assert saved_config.count("[mavlink]") == 1
    assert 'tcp_port = 14700' in saved_config
    assert 'udp_port = 14701' in saved_config
    status, _, _ = form(
        "/time/sync", "initial-password", "0" * 64, {"time_ms": "1767225600123"}
    )
    assert status == 400 and not (root / "time-sync").exists()
    status, _, _ = form(
        "/time/sync", "initial-password", csrf, {"time_ms": "not-a-time"}
    )
    assert status == 400 and not (root / "time-sync").exists()
    status, body, _ = form(
        "/time/sync", "initial-password", csrf, {"time_ms": "1767225600123"}
    )
    assert status == 200 and b"Camera time synchronized with browser" in body
    assert (root / "time-sync").read_text() == "1767225600123\n"
    assert b"id=firmware type=file accept=.bin hidden" in body
    assert b"id=select-firmware type=button" in body
    assert b"class=danger type=submit>Upgrade Firmware" not in body
    status, body, headers = request("GET", "/upgrade.js", "initial-password")
    assert status == 200 and b"waitTimeout = 120000" in body
    assert b"check.status === 401" in body and b"/login" in body
    assert b"automatic firmware upgrade" in body
    assert b"/upgrade-status" in body and b"progress.value = 100" in body
    assert headers["Content-Type"].startswith("application/javascript")
    subprocess.run(["node", "--check"], input=body, check=True)
    status, body, headers = request("GET", "/upgrade-status", "initial-password")
    assert status == 200 and body == csrf.encode()
    assert headers["Cache-Control"] == "no-store"

    status, body, _ = request("GET", "/sensors", "initial-password")
    assert status == 200 and b"LiDAR range" in body and b"Capture photo" in body
    assert b"id=lidar-toggle" in body and b">Enable</button>" in body
    assert b"data-csrf=" in body and b"turns on the camera's laser" in body
    assert b">Sensors</a>" in body and b">Debug</a>" in body
    assert b">Camera output</a>" not in body
    status, body, headers = request("GET", "/sensors.js", "initial-password")
    assert status == 200 and b"setInterval(refresh, 500)" in body
    assert b"/sensors/lidar" in body and b"'Disabled'" in body
    assert headers["Content-Type"].startswith("application/javascript")
    subprocess.run(["node", "--check"], input=body, check=True)
    status, body, headers = request("GET", "/sensors.json", "initial-password")
    assert status == 200, (status, body)
    assert b'"lidar_m":null' in body and b'"lidar_enabled":false' in body
    assert b'"minimum_c":12.34' in body and b'"maximum_c":42.42' in body
    assert b'"cpu_c":64.3' in body
    assert headers["Cache-Control"] == "no-store"

    status, _, _ = form(
        "/sensors/lidar", "initial-password", "0" * 64, {"action": "enable"}
    )
    assert status == 403 and not camera_events["lidar_controls"]
    status, body, _ = form(
        "/sensors/lidar", "initial-password", csrf, {"action": "enable"}
    )
    assert status == 200 and b"LiDAR enabled" in body
    deadline = time.monotonic() + 1
    while len(camera_events["lidar_controls"]) < 3 and time.monotonic() < deadline:
        time.sleep(0.01)
    assert camera_events["lidar_controls"] == [True, True, True]
    status, body, _ = request("GET", "/sensors.json", "initial-password")
    assert status == 200
    assert b'"lidar_m":12.3' in body and b'"lidar_enabled":true' in body
    status, body, _ = form(
        "/sensors/lidar", "initial-password", csrf, {"action": "disable"}
    )
    assert status == 200 and b"LiDAR disabled" in body
    deadline = time.monotonic() + 1
    while len(camera_events["lidar_controls"]) < 6 and time.monotonic() < deadline:
        time.sleep(0.01)
    assert camera_events["lidar_controls"][-3:] == [False, False, False]
    status, body, _ = request("GET", "/sensors.json", "initial-password")
    assert status == 200
    assert b'"lidar_m":null' in body and b'"lidar_enabled":false' in body

    status, body, _ = request("GET", "/live", "initial-password")
    assert status == 200 and b"Native H.264 preview" in body
    assert b"running ArduPilot camera app" in body
    assert b"Enable manual gimbal control" in body and b"bounded 180 ms pulses" in body
    assert b"Gimbal attitude" in body and b"id=attitude-dial" in body
    assert b"id=live-rate type=range min=5 max=60 step=1 value=30" in body
    assert b"id=attitude-roll-rate" in body
    assert b"id=attitude-pitch-rate" in body
    assert b"id=attitude-yaw-rate" in body
    assert b">Live</a>" in body
    status, body, headers = request("GET", "/live.js", "initial-password")
    assert status == 200 and b"video.src = '/live/video'" in body
    assert b"fetch('/live/attitude.json'" in body
    assert b"setInterval(refreshAttitude, 250)" in body
    assert b"sendControl(direction, Number(rate.value).toFixed(0))" in body
    assert b"Number(data.pitch_rate_dps).toFixed(1) + '\xc2\xb0/s'" in body
    assert b"MediaSource" not in body
    assert headers["Content-Type"].startswith("application/javascript")
    subprocess.run(["node", "--check"], input=body, check=True)
    status, body, headers = request(
        "GET", "/live/attitude.json", "initial-password"
    )
    assert status == 200
    assert b'"yaw_deg":-123.4' in body and b'"pitch_deg":-45.6' in body
    assert b'"roll_deg":7.8' in body and b'"yaw_rate_dps":-1.9' in body
    assert headers["Cache-Control"] == "no-store"
    assert camera_events["attitude_requests"] == 1
    status, body, headers = request("GET", "/live/video1.mp4", "initial-password")
    assert status == 200 and body == live_payload + b"\x00"
    assert headers["Content-Type"] == "video/mp4"
    assert request("GET", "/live/video2.mp4", "wrong-password")[0] == 401

    status, _, _ = form(
        "/live/control", "initial-password", "0" * 64, {"action": "left"}
    )
    assert status == 403 and not camera_events["controls"]
    status, body, _ = form(
        "/live/control", "initial-password", csrf, {"action": "left"}
    )
    assert status == 400 and b"Missing gimbal rate" in body
    assert not camera_events["controls"]
    assert form("/live/control", "initial-password", csrf,
                {"action": "left", "value": "30"})[0] == 400
    status, body, _ = form("/live/control", "initial-password", csrf,
                           {"action": "acquire"})
    assert status == 200 and body.strip().decode() == manual_token.hex()
    for action, value in (("left", "30"), ("zoom", "3.7"), ("renew", "0"), ("release", "0")):
        assert form("/live/control", "initial-password", csrf,
                    {"action": action, "value": value, "lease": manual_token.hex()})[0] == 200
    assert [a for a, v in manual_events] == [1, 4, 9, 2, 3]
    assert manual_events[1][1] == 30
    assert not camera_events["controls"]

    status, _, _ = form("/sensors/capture", "initial-password", "0" * 64, {})
    assert status == 400 and camera_events["captures"] == 0
    status, body, _ = form("/sensors/capture", "initial-password", csrf, {})
    assert status == 200 and b"Photo captured and saved" in body
    assert camera_events["captures"] == 1
    assert b"2026-09-01_12-00-00_000_C.jpg" in body
    captured_path = root / "mnt" / "DCIM" / "capture" / "2026-09-01" / "2026-09-01_12-00-00_000_C.jpg"
    encoded_path = urllib.parse.quote(str(captured_path), safe="")
    status, body, headers = request("GET", f"/file?path={encoded_path}", "initial-password")
    assert status == 200 and body == captured_path.read_bytes()
    assert headers["Content-Type"] == "image/jpeg"

    status, _, _ = form(
        "/users/password",
        "initial-password",
        csrf,
        {"password": "new-secure-password", "confirmation": "wrong-password"},
    )
    assert status == 400
    assert (root / "app" / "web.pass").read_text() == "initial-password\n"

    status, body, _ = form(
        "/users/password",
        "initial-password",
        csrf,
        {"password": "new-secure-password", "confirmation": "new-secure-password"},
    )
    assert status == 200 and b"Admin password changed" in body
    password_path = root / "app" / "web.pass"
    assert password_path.read_text() == "new-secure-password\n"
    assert password_path.stat().st_mode & 0o777 == 0o600
    assert request("GET", "/users", "initial-password")[0] == 401
    assert request("GET", "/users", "new-secure-password")[0] == 200

    csrf = get_csrf("new-secure-password")
    key_blob = base64.b64encode(b"integration-test-public-key-material").decode()
    public_key = f"ssh-ed25519 {key_blob} integration@test"
    second_blob = base64.b64encode(b"second-integration-test-key-material").decode()
    second_key = f"ssh-rsa {second_blob} second@test"
    status, body, _ = form(
        "/users/keys/add",
        "new-secure-password",
        csrf,
        {"public_key": f"{public_key}\r\n{second_key}\n"},
    )
    assert status == 200 and b"2 SSH public keys added" in body
    persistent = root / "app" / "dropbear" / "authorized_keys"
    runtime = root / "runtime-auth" / "authorized_keys"
    assert persistent.read_text() == public_key + "\n" + second_key + "\n"
    assert runtime.read_bytes() == persistent.read_bytes()
    assert persistent.stat().st_mode & 0o777 == 0o600
    assert runtime.stat().st_mode & 0o777 == 0o600

    status, _, _ = form(
        "/users/keys/add",
        "new-secure-password",
        csrf,
        {"public_key": public_key},
    )
    assert status == 400
    before_bad_batch = persistent.read_bytes()
    status, _, _ = form(
        "/users/keys/add",
        "new-secure-password",
        csrf,
        {"public_key": "ssh-ed25519 malformed\n" + public_key},
    )
    assert status == 400
    assert persistent.read_bytes() == before_bad_batch
    status, body, _ = request("GET", "/users", "new-secure-password")
    assert status == 200 and b"integration@test" in body

    identity = f"ssh-ed25519 {key_blob}"
    status, _, _ = form(
        "/users/keys/remove",
        "new-secure-password",
        csrf,
        {"public_key": identity},
    )
    assert status == 400
    assert persistent.read_text() == public_key + "\n" + second_key + "\n"
    status, body, _ = form(
        "/users/keys/remove",
        "new-secure-password",
        csrf,
        {"public_key": identity, "confirm": "yes"},
    )
    assert status == 200 and b"SSH public key removed" in body
    assert persistent.read_text() == second_key + "\n"
    assert runtime.read_bytes() == persistent.read_bytes()

    second_identity = f"ssh-rsa {second_blob}"
    status, body, _ = form(
        "/users/keys/remove",
        "new-secure-password",
        csrf,
        {"public_key": second_identity, "confirm": "yes"},
    )
    assert status == 400 and b"Refusing to remove the last usable SSH key" in body
    assert persistent.read_text() == second_key + "\n"
    assert runtime.read_bytes() == persistent.read_bytes()

    firmware = bytes(range(256)) * 4096
    rejected_probe = b"x"
    upload_headers = {
        "Content-Type": "application/octet-stream",
        "X-CSRF-Token": csrf,
        "X-Firmware-Name": "bad.bin",
    }
    status, _, _ = request(
        "POST", "/upgrade", "new-secure-password", rejected_probe, upload_headers
    )
    assert status == 400
    assert not (root / "mnt" / "bad.bin").exists()

    upload_headers["X-Firmware-Name"] = "MT11_FW_Integration.bin"
    bad_csrf_headers = {**upload_headers, "X-CSRF-Token": "0" * 64}
    status, _, _ = request(
        "POST", "/upgrade", "new-secure-password", rejected_probe, bad_csrf_headers
    )
    assert status == 403
    assert not (root / "mnt" / "MT11_FW_Integration.bin").exists()

    status, _, _ = request(
        "POST", "/upgrade", "wrong-password", rejected_probe, upload_headers
    )
    assert status == 401
    assert not (root / "mnt" / "MT11_FW_Integration.bin").exists()

    status, body, _ = request(
        "POST", "/upgrade", "new-secure-password", firmware, upload_headers
    )
    assert status == 201, (status, body)
    assert b"checks about every 5 seconds" in body
    final_firmware = root / "mnt" / "MT11_FW_Integration.bin"
    assert final_firmware.read_bytes() == firmware
    assert not (root / "mnt" / "MT11_FW_Integration.bin.tmp").exists()

    status, _, _ = request(
        "POST", "/upgrade", "new-secure-password", rejected_probe, upload_headers
    )
    assert status == 409

    interrupted_name = "MT11_FW_Interrupted.bin"
    raw = socket.create_connection(("127.0.0.1", port), timeout=5)
    headers = (
        "POST /upgrade HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        f"Authorization: {authorization('new-secure-password')}\r\n"
        "Content-Type: application/octet-stream\r\n"
        f"X-CSRF-Token: {csrf}\r\n"
        f"X-Firmware-Name: {interrupted_name}\r\n"
        "Content-Length: 4096\r\nConnection: close\r\n\r\n"
    ).encode()
    raw.sendall(headers + b"partial")
    raw.close()
    time.sleep(0.2)
    deadline = time.monotonic() + 3
    temporary = root / "mnt" / f"{interrupted_name}.tmp"
    while temporary.exists() and time.monotonic() < deadline:
        time.sleep(0.05)
    assert not temporary.exists()
    assert not (root / "mnt" / interrupted_name).exists()

    print("PASS login sessions, languages, parameter migration, time sync, users, native live video/PTZ, sensors, capture gallery, SSH keys and firmware upload")
finally:
    server.terminate()
    try:
        server.wait(timeout=3)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait()
    if server.returncode not in (0, -15):
        print(server.stderr.read(), file=sys.stderr)
    camera_stop.set()
    camera_socket.close()
    camera_thread.join(timeout=1)
    live_stop.set()
    live_socket.close()
    live_thread.join(timeout=1)
    shutil.rmtree(root, ignore_errors=True)
