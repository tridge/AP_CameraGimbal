#!/usr/bin/env python3
"""RPi Network parameters: video stream 1 URI shown and saved; host-owned address fields hidden."""
import base64
import html.parser
import http.client
import os
import re
import shutil
import socket
import subprocess
import sys
import time
import urllib.parse
from pathlib import Path

binary = Path(sys.argv[1]).resolve()
root = Path(sys.argv[2]).resolve()
repo = Path(__file__).resolve().parents[2]
shutil.rmtree(root, ignore_errors=True)
(root / "app").mkdir(parents=True)
(root / "mnt" / "capture").mkdir(parents=True)
shutil.copytree(repo / "web" / "webroot", root / "webroot")
(root / "app" / "web.pass").write_text("initial-password\n", encoding="utf-8")
os.chmod(root / "app" / "web.pass", 0o600)
(root / "soc-temperature").write_text("45000\n", encoding="ascii")
# Start from the installed defaults, plus legacy address keys the RPi ignores.
camera_ini = root / "app" / "camera.ini"
camera_ini.write_text(
    (repo / "packaging" / "rpi_libcam_caddx" / "camera.ini").read_text(encoding="utf-8")
    .replace("telemetry = misb\n", "")
    + "interface = eth0\nprimary_address = 192.0.2.5/24\n",
    encoding="utf-8",
)


class Form(html.parser.HTMLParser):
    """Values the browser would submit from the parameters form."""

    def __init__(self):
        super().__init__()
        self.values = {}
        self.select = None
        self.first_option = None

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "input" and a.get("name") and a.get("type") not in ("submit", "button"):
            if a.get("type") in ("checkbox", "radio") and "checked" not in a:
                return
            self.values[a["name"]] = a.get("value") or ""
        elif tag == "select" and a.get("name"):
            self.select, self.first_option = a["name"], None
        elif tag == "option" and self.select:
            if self.first_option is None:
                self.first_option = a.get("value", "")
                self.values.setdefault(self.select, self.first_option)
            if "selected" in a:
                self.values[self.select] = a.get("value", "")

    def handle_endtag(self, tag):
        if tag == "select":
            self.select = None


listener = socket.socket()
listener.bind(("127.0.0.1", 0))
port = listener.getsockname()[1]
listener.close()
server = subprocess.Popen([str(binary), "-p", str(port)], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True)


def request(method, path, body=None):
    headers = {"Authorization": "Basic " + base64.b64encode(b"admin:initial-password").decode()}
    if body is not None:
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    connection.request(method, path, body=body, headers=headers)
    response = connection.getresponse()
    result = response.status, response.read().decode("utf-8", "replace")
    connection.close()
    return result


def parameters():
    status, page = request("GET", "/parameters")
    assert status == 200, (status, page[:500])
    form = Form()
    form.feed(page)
    return page, form.values


def save(values):
    return request("POST", "/parameters", urllib.parse.urlencode(values).encode())


try:
    deadline = time.monotonic() + 5
    while True:
        try:
            status, _ = request("GET", "/")
            break
        except OSError:
            if time.monotonic() > deadline or server.poll() is not None:
                raise
            time.sleep(0.05)

    page, values = parameters()
    assert values["video_telemetry"] == "misb", values
    assert "network_video1_uri" in values, sorted(values)
    assert "network_capture" in values
    for hidden in ("network_interface", "network_primary_address",
                   "network_secondary_address", "network_gateway"):
        assert hidden not in values, hidden
    help_text = re.search(r"<div class=help>([^<]*)<code>\[network\] video1_uri</code>", page)
    assert help_text and "Requires camera app restart." not in help_text.group(1), help_text

    # The gimbal link offers suggestions but accepts any valid value.
    assert "network_uart_device_override" in values, sorted(values)
    assert "list=uart_device_override_choices" in page and "<datalist id=uart_device_override_choices>" in page
    for choice in ("/dev/ttyAMA0", "udp://192.168.144.15:15002"):
        assert f'<option value="{choice}">' in page, choice
    values["network_uart_device_override"] = "udp://192.168.144.15:15002"
    status, body = save(values)
    assert status in (200, 303), (status, re.sub(r"<[^>]+>", " ", body)[:600])
    link_saved = camera_ini.read_text(encoding="utf-8")
    assert re.search(r'^uart_device_override = "?udp://192\.168\.144\.15:15002"?$', link_saved, re.M), link_saved
    for invalid in ("/etc/passwd", "/dev/../etc/passwd", "udp://example.com:15002",
                    "udp://192.168.144.15", "udp://192.168.144.15:0", "tcp://example.com:1",
                    "tcp://192.168.144.15", "tcp://192.168.144.15:0", "http://192.168.144.15:1"):
        _, values = parameters()
        values["network_uart_device_override"] = invalid
        save(values)
        assert camera_ini.read_text(encoding="utf-8") == link_saved, invalid
    _, values = parameters()
    assert values["network_uart_device_override"] == "udp://192.168.144.15:15002"
    values["network_uart_device_override"] = "tcp://192.168.144.15:15002"
    assert save(values)[0] in (200, 303)
    assert re.search(r'^uart_device_override = "?tcp://192\.168\.144\.15:15002"?$', camera_ini.read_text(encoding="utf-8"), re.M)
    _, values = parameters()
    values["network_uart_device_override"] = "/dev/ttyAMA0"
    assert save(values)[0] in (200, 303)
    assert re.search(r'^uart_device_override = "?/dev/ttyAMA0"?$', camera_ini.read_text(encoding="utf-8"), re.M)

    _, values = parameters()
    values["network_video1_uri"] = "rtsp://10.0.2.100:9000/camera/main"
    status, body = save(values)
    assert status in (200, 303), (status, body[:500])
    saved = camera_ini.read_text(encoding="utf-8")
    assert re.search(r'^telemetry = "?misb"?$', saved, re.M), saved
    assert re.search(r"^video1_uri = \"?rtsp://10\.0\.2\.100:9000/camera/main\"?$", saved, re.M), saved
    # Hidden fields are not rewritten: the ignored legacy values stay as they were.
    assert "primary_address = 192.0.2.5/24" in saved, saved

    for invalid in ("10.0.2.100", "http://10.0.2.100/video1", "rtsp://user:pw@10.0.2.100/video1",
                    "rtsp://10.0.2.100/has space", "rtsp://10.0.2.100:99999/video1",
                    "rtsp://h/" + "a" * 151):
        _, values = parameters()
        values["network_video1_uri"] = invalid
        save(values)
        assert camera_ini.read_text(encoding="utf-8") == saved, invalid

    _, values = parameters()
    assert values["network_video1_uri"] == "rtsp://10.0.2.100:9000/camera/main"
    values["network_video1_uri"] = ""
    save(values)
    assert re.search(r'^video1_uri = ("")?$', camera_ini.read_text(encoding="utf-8"), re.M)
finally:
    server.terminate()
    try:
        server.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        server.kill()
print("PASS RPi network parameters: video stream 1 URI saved and validated, host-owned fields hidden")
