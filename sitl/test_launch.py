#!/usr/bin/env python3
"""Exercise the real Qt launcher with isolated simulated child processes."""
import json
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from unittest import mock

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sitl_launch import SimulatorPanel as Launcher, Launcher as MultiLauncher, QtWidgets, owned_processes, REPO


def until(app, predicate, timeout=15):
    deadline = time.monotonic() + timeout
    while not predicate():
        assert time.monotonic() < deadline, 'launcher timed out'
        app.processEvents()
        time.sleep(0.02)
    app.processEvents()


def real_stack(app):
    """Build/run both backends with isolated ports and writable camera trees."""
    import re
    import socket
    from test_sitl import reserve_port, web_request, web_form, request
    root = Path(tempfile.mkdtemp(prefix='sitl-launch-real-'))
    print(f'Real SITL build trees and logs: {root}', flush=True)
    window = Launcher(REPO)
    try:
        for index, backend in enumerate(('mt11', 'a8')):
            prefix = backend.upper() + '_SITL_'
            web_port = reserve_port(socket.SOCK_STREAM)
            camera_port = reserve_port(socket.SOCK_DGRAM)
            env = {'CAMERA_GIMBAL_SITL_BUILD': str(root / backend),
                   prefix + 'WEB_PORT': str(web_port),
                   prefix + 'RTSP_PORT': str(reserve_port(socket.SOCK_STREAM)),
                   prefix + 'CAMERA_PORT': str(camera_port),
                   prefix + 'MAVLINK_TCP_PORT': '0', prefix + 'MAVLINK_UDP_PORT': '0'}
            with mock.patch.dict(os.environ, env):
                window.camera.setCurrentIndex(window.camera.findData(backend))
                window.orientation.setCurrentIndex(index)
                window.video.setCurrentIndex(index)
                window.start_button.click()
            token = window.token
            until(app, lambda: window.phase in ('running', 'idle'), timeout=90)
            assert window.phase == 'running', window.log.toPlainText()[-5000:]
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                client.connect(('127.0.0.1', camera_port))
                status = request(client, 1, 0x0A)
                assert status[5] == index + 1, status
            with web_request(web_port, '/') as response:
                page = response.read().decode()
            csrf = re.search(r'data-csrf="([0-9a-f]{64})"', page).group(1)
            config = window.runtime.parent / 'app/camera.ini'
            saved = re.sub(r'(?m)^autorecord\s*=.*$', 'autorecord = while_armed', config.read_text())
            with web_form(web_port, '/config', csrf, {'config': saved, 'action': 'save'}, timeout=30) as response:
                assert response.status == 200
            assert 'autorecord = while_armed' in config.read_text()
            saved = config.read_bytes()
            media_file = window.runtime.parent / 'mnt/keep-recording.mp4'
            media_file.write_bytes(b'keep')
            ready = window.runtime / 'camera-app.ready'
            old_pid = re.search(r'pid=(\d+)', ready.read_text()).group(1)
            with web_form(web_port, '/restart', csrf, {}, timeout=30) as response:
                assert response.status == 200
                page = response.read().decode()
            new_pid = re.search(r'pid=(\d+)', ready.read_text()).group(1)
            assert new_pid != old_pid, page
            assert int(new_pid) in owned_processes(token)
            assert config.read_bytes() == saved
            window.stop_button.click()
            until(app, lambda: window.phase == 'idle')
            assert not owned_processes(token)
            (root / f'{backend}-gui.log').write_text(window.log.toPlainText())
            for clear in (False, True):
                window.clear_parameters.setChecked(clear)
                with mock.patch.dict(os.environ, env):
                    window.start_button.click()
                until(app, lambda: window.phase in ('running', 'idle'), timeout=90)
                assert window.phase == 'running', window.log.toPlainText()[-5000:]
                if clear:
                    assert config.read_bytes() != saved
                    assert config.with_suffix('.ini.reset.bak').read_bytes() == saved
                    assert 'autorecord = false' in config.read_text()
                else:
                    assert config.read_bytes() == saved
                assert media_file.read_bytes() == b'keep'
                window.stop_button.click()
                until(app, lambda: window.phase == 'idle')
            window.clear_parameters.setChecked(False)
            print(f'PASS real {backend} GUI, web save/restart, parameter preservation, explicit reset and media preservation', flush=True)
    finally:
        window.close()
        until(app, lambda: window.phase == 'idle')


def main(real=False):
    app = QtWidgets.QApplication([])
    with tempfile.TemporaryDirectory(prefix='sitl-launch-') as temp:
        repo = Path(temp)
        (repo / 'sitl').mkdir()
        (repo / 'Makefile').write_text('sitl a8_sitl zr10_sitl z1mini_sitl:\n\t@echo built $@\n')
        (repo / 'sitl/terrain_video.py').write_text("print('terrain dependencies available')\n")
        (repo / 'sitl/run.sh').write_text('exec python3 "$(dirname "$0")/fake.py"\n')
        (repo / 'sitl/fake.py').write_text('''
import json, os, pathlib, subprocess, sys, time
root = pathlib.Path(os.environ['CAMERA_GIMBAL_SITL_BUILD'])
root.mkdir(parents=True, exist_ok=True)
keys = ('CAMERA_GIMBAL_SITL_BACKEND', 'CAMERA_GIMBAL_SITL_VIDEO',
        'MT11_SITL_ORIENTATION', 'A8_SITL_ORIENTATION')
(root / 'selection.json').write_text(json.dumps({k: os.environ.get(k) for k in keys}))
# A detached child models a web-triggered camera restart.
child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'],
                         start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
(root / 'child.pid').write_text(str(child.pid))
print(os.environ['CAMERA_GIMBAL_SITL_BACKEND'].upper() + ' SITL running', flush=True)
time.sleep(60)
''')
        unrelated = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
        window = Launcher(repo)
        window.show()
        try:
            assert window.video.currentData() == 'simple'
            assert not window.clear_parameters.isChecked()
            assert window.environment()[0]['CAMERA_GIMBAL_SITL_RESET_PARAMETERS'] == '0'
            window.clear_parameters.setChecked(True)
            assert window.environment()[0]['CAMERA_GIMBAL_SITL_RESET_PARAMETERS'] == '1'
            window.clear_parameters.setChecked(False)
            for index in (0, 1):
                backend = ('mt11', 'a8')[index]
                window.camera.setCurrentIndex(window.camera.findData(backend))
                window.orientation.setCurrentIndex(index)
                window.video.setCurrentIndex(index)
                window.start_button.click()
                token = window.token
                assert not window.camera.isEnabled()
                until(app, lambda: window.phase == 'running')
                selection = json.loads((window.runtime.parent.parent / 'selection.json').read_text())
                backend = ('mt11', 'a8')[index]
                assert selection['CAMERA_GIMBAL_SITL_BACKEND'] == backend
                assert selection[backend.upper() + '_SITL_ORIENTATION'] == ('upright', 'inverted')[index]
                assert selection['CAMERA_GIMBAL_SITL_VIDEO'] == ('simple', 'terrain')[index]
                assert len(owned_processes(token)) >= 2
                assert window.web_button.isEnabled()
                if index:
                    window.close()  # closing must clean up too, without a confirmation dialog
                else:
                    window.stop_button.click()
                until(app, lambda: window.phase == 'idle')
                assert not owned_processes(token)
                assert unrelated.poll() is None, 'launcher stopped an unrelated process'
                assert window.start_button.isEnabled()
            print('PASS camera/orientation/video selections, startup, stop, close and detached-child cleanup')

            group = MultiLauncher(repo)
            try:
                assert group.count.value() == 1 and group.tabs.count() == 1
                group.count.setValue(4)
                assert group.tabs.count() == 4
                for backends in (('mt11',) * 4, ('mt11', 'a8', 'zr10', 'z1mini')):
                    for i, panel in enumerate(group.simulators):
                        panel.camera.setCurrentIndex(panel.camera.findData(backends[i]))
                        panel.video.setCurrentIndex(i % 2)
                        panel.orientation.setCurrentIndex(i % 2)
                    group.start_button.click()
                    until(app, lambda: group.phase in ('running', 'idle'))
                    assert group.phase == 'running', group.status.text()
                    assert not group.count.isEnabled()
                    tokens = [p.token for p in group.simulators]
                    builds = [p.runtime.parent.parent for p in group.simulators]
                    assert len(set(builds)) == 4
                    assert len({p.web_url for p in group.simulators}) == 4
                    for i, panel in enumerate(group.simulators):
                        selection = json.loads((builds[i] / 'selection.json').read_text())
                        assert selection['CAMERA_GIMBAL_SITL_BACKEND'] == backends[i]
                        assert selection['CAMERA_GIMBAL_SITL_VIDEO'] == ('simple', 'terrain')[i % 2]
                        assert len(owned_processes(tokens[i])) >= 2
                    if backends[1] == 'a8':
                        group.close()
                    else:
                        group.stop_button.click()
                    until(app, lambda: group.phase == 'idle')
                    assert all(not owned_processes(token) for token in tokens)
                    assert unrelated.poll() is None
                print('PASS four identical/mixed simulators, separate directories/ports, stop-all and close cleanup')

                # A failure after another simulator started must unwind the group.
                (repo / 'Makefile').write_text('sitl a8_sitl zr10_sitl z1mini_sitl:\n'
                    '\t@test "$$CAMERA_GIMBAL_SITL_INSTANCE" != 2\n')
                group.start()
                until(app, lambda: group.phase == 'idle')
                assert 'Simulator 2 stopped' in group.status.text(), group.status.text()
                assert all(p.phase == 'idle' for p in group.simulators)
                assert not (builds[2] / 'launcher.pid').exists()
                print('PASS partial startup failure stops the whole group')

                (repo / 'Makefile').write_text('sitl a8_sitl zr10_sitl z1mini_sitl:\n\t@sleep 60\n')
                group.start()
                until(app, lambda: group.simulators[0].phase == 'building')
                token = group.simulators[0].token
                group.stop()
                until(app, lambda: group.phase == 'idle')
                assert not owned_processes(token)
                assert all(p.phase == 'idle' for p in group.simulators)
                print('PASS stop during build cancels pending simulators')

                with mock.patch.dict(os.environ, {'MT11_SITL_WEB_PORT': '8554'}):
                    group.start()
                    assert group.phase == 'idle' and 'shared' in group.status.text()
                assert group.start_button.isEnabled()
                print('PASS conflicting endpoints rejected before startup')
            finally:
                group.close()
                until(app, lambda: group.phase == 'idle')

            (repo / 'Makefile').write_text('sitl a8_sitl:\n\t@echo deliberate build failure\n\t@false\n')
            window.video.setCurrentIndex(0)
            window.start_button.click()
            until(app, lambda: window.phase == 'idle')
            assert 'Failed' in window.status.text()
            assert 'deliberate build failure' in window.log.toPlainText()
            assert not window.web_button.isEnabled()
            print('PASS build failure is displayed and controls recover')
        finally:
            window.close()
            until(app, lambda: window.phase == 'idle')
            unrelated.terminate()
            unrelated.wait(timeout=3)
    if real:
        real_stack(app)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--real', action='store_true', help='also build and launch the real MT11/A8 stack')
    main(parser.parse_args().real)
