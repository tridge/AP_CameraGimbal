#!/usr/bin/env python3
"""Return success when a camera SITL web service is already running."""

import argparse
import base64
import http.client
import json
import pathlib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", type=int)
    parser.add_argument("password_file", type=pathlib.Path)
    args = parser.parse_args()
    try:
        password = args.password_file.read_text(encoding="utf-8").rstrip("\r\n")
        credentials = base64.b64encode(
            f"admin:{password}".encode("utf-8")
        ).decode("ascii")
        connection = http.client.HTTPConnection("127.0.0.1", args.port,
                                                timeout=0.5)
        connection.request("GET", "/live/attitude.json",
                           headers={"Authorization": f"Basic {credentials}"})
        response = connection.getresponse()
        body = response.read()
        connection.close()
        attitude = json.loads(body) if response.status == 200 else {}
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError):
        raise SystemExit(1)
    if not isinstance(attitude, dict) or "yaw_deg" not in attitude or "pitch_deg" not in attitude:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
