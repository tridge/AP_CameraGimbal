#!/usr/bin/env python3
"""A failed network startup must stay visible until a successful app restart."""
import os
from pathlib import Path
import pty
import select
import socket
import subprocess
import sys
import tempfile
import time


def wait_for(path, process, predicate):
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline:
        assert process.poll() is None, process.stderr.read()
        if path.exists() and predicate(path.read_text()):
            return path.read_text()
        time.sleep(0.02)
    raise AssertionError(f'timed out waiting for {path}: {path.read_text() if path.exists() else "missing"}')


def main():
    with tempfile.TemporaryDirectory(prefix='camera-network-status-') as directory:
        root = Path(directory)
        config = root / 'camera.ini'
        ready = root / 'ready'
        status = root / 'ready.config'
        config.write_text('[uart]\nprotocol=none\n[network]\ninterface=no-such-cam\nprimary_address=192.0.2.25/24\n')
        for failed in (True, False):
            master, slave = pty.openpty()
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
                probe.bind(('127.0.0.1', 0))
                port = probe.getsockname()[1]
            env = dict(os.environ, CAMERA_APP_READY_PATH=str(ready), CAMERA_APP_MAVLINK_TCP_PORT='0',
                       CAMERA_APP_MAVLINK_UDP_PORT='0', CAMERA_APP_TEST_SETTIME_FAILURE='1')
            process = subprocess.Popen([sys.argv[1], '--uart', os.ttyname(slave), '--port', str(port),
                                        '--config', str(config)], env=env, stdout=subprocess.DEVNULL,
                                       stderr=subprocess.PIPE, text=True)
            try:
                wait_for(ready, process, lambda text: f'pid={process.pid}\n' in text)
                # Drain startup traffic from the emulated serial port.
                while select.select([master], [], [], 0)[0]:
                    os.read(master, 8192)
                text = wait_for(status, process, lambda text: f' {process.pid} ' in text)
                if failed:
                    assert ' error\n' in text and 'Network configuration failed on no-such-cam' in text, text
                    # Saving corrected settings does not apply network changes
                    # until restart, so an unrelated/live reload must not mask failure.
                    config.write_text('[uart]\nprotocol=none\n[image]\nbrightness=61\n')
                    previous = text
                    text = wait_for(status, process, lambda text: text != previous)
                    assert ' error\n' in text and 'Restart to retry' in text, text
                else:
                    assert ' applied\n' in text and 'Network configuration failed' not in text, text
            finally:
                process.terminate()
                try:
                    process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()
                os.close(master)
                os.close(slave)
        print('PASS startup network failure acknowledgement, persistence across live reload, and restart recovery')


if __name__ == '__main__':
    main()
