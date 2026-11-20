#!/usr/bin/env python3
"""Run the actual ZR10 supervisor with isolated paths and fake hardware apps."""
from pathlib import Path
import os
import base64
import http.client
import re
import socket
import subprocess
import tempfile
import time

repo = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='zr10-service-') as directory:
    root = Path(directory)
    app = root / 'mnt/AP_CameraGimbal/zr10'
    customer = root / 'customer'
    runtime = root / 'tmp'
    commands = root / 'commands'
    for path in (app, customer, runtime, commands):
        path.mkdir(parents=True)
    config = app / 'config'
    config.mkdir()
    (app / 'camera.ini.default').write_bytes((repo/'packaging/zr10/camera.ini').read_bytes())
    (app / 'web.pass.default').write_text('test-password\n')
    source = root / 'fake.c'
    # Both processes take the same exclusive "hardware" lock. A start overlap
    # is a test failure even if the supervisor later recovers.
    source.write_text(r'''
#include "apcam/config_status.h"
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    (void)argc;
    int vendor = strstr(argv[0], "sycamera") != NULL;
    int fd = open(getenv("TEST_HARDWARE"), O_CREAT|O_RDWR, 0600);
    if (fd < 0 || flock(fd, LOCK_EX|LOCK_NB)) {
        FILE *bad = fopen(getenv("TEST_OVERLAP"), "w");
        if (bad) fclose(bad);
        return 3;
    }
    if (!vendor && access(getenv("TEST_FAIL"), F_OK) == 0) return 2;
    if (!vendor) {
        FILE *ready = fopen(getenv("CAMERA_APP_READY_PATH"), "w");
        if (!ready) return 4;
        fprintf(ready, "backend=zr10\npid=%d\n", getpid());
        fclose(ready);
        uint64_t hash;
        if (apcam_config_file_hash(getenv("CAMERA_APP_CONFIG"), &hash) < 0) return 5;
        char status_path[4096];
        snprintf(status_path, sizeof(status_path), "%s.config", getenv("CAMERA_APP_READY_PATH"));
        FILE *status = fopen(status_path, "w");
        if (!status) return 6;
        fprintf(status, "%llx %ld ok\nConfiguration applied.\n", (unsigned long long)hash, (long)getpid());
        fclose(status);
    }
    while (1) pause();
}
''')
    subprocess.run(['c++', '-std=gnu++17', '-Wno-missing-field-initializers', '-O2', '-I'+str(repo/'include'), str(source), '-o', str(app / 'camera-app')], check=True)
    (customer / 'sycamera.vendor').write_bytes((app / 'camera-app').read_bytes())
    (customer / 'sycamera.vendor').chmod(0o755)
    # System actions are mocked. All process matching stays within this fixture.
    for command in ('ifconfig', 'killall'):
        (commands / command).write_text('#!/bin/sh\nexit 0\n')
    (commands / 'pidof').write_text('''#!/usr/bin/env python3
import os, sys
from pathlib import Path
pids = []
for path in Path('/proc').glob('[0-9]*/exe'):
    try:
        exe = os.readlink(path)
        if exe.startswith(os.environ['TEST_ROOT']+'/') and Path(exe).name in sys.argv[1:]:
            pids.append(path.parent.name)
    except OSError:
        pass
print(' '.join(pids)) if pids else None
sys.exit(0 if pids else 1)
''')
    for path in commands.iterdir():
        path.chmod(0o755)
    # Run the real web server against the actual shell supervisor. Aliases
    # exercise executable identification for both SD and flash-style paths.
    paths = {
        "APP_DIR": runtime/'zr10-app', "MEDIA_ROOT": root/'mnt',
        "APP_SELECTION_DIR": runtime/'zr10-config',
        "APP_SELECTION_PATH": runtime/'zr10-config/app_selection',
        "PASSWORD_PATH": runtime/'zr10-config/web.pass',
        "REPLACEMENT_CONFIG_PATH": runtime/'zr10-config/camera.ini',
        "REPLACEMENT_CONFIG_BACKUP_PATH": runtime/'zr10-config/camera.ini.bak',
        "APP_REQUEST_PATH": runtime/'camera-app.request',
        "APP_REQUEST_LOCK_PATH": runtime/'camera-app.request.lock',
        "APP_STARTED_PATH": runtime/'camera-app.started',
        "CAMERA_READY_PATH": runtime/'camera-app.ready',
        "VENDOR_CAMERA_PATH": customer/'sycamera.vendor',
        "SWITCH_LOCK_PATH": runtime/'switch.lock',
        "UPGRADE_LOCK_PATH": runtime/'upgrade.lock',
        "USER_LOCK_PATH": runtime/'users.lock',
        "SESSION_PATH": runtime/'sessions', "RUNTIME_DIR": runtime,
    }
    subprocess.run(['c++', '-std=gnu++17', '-Wno-missing-field-initializers', '-O2', '-std=gnu++17', '-DAPCAM_TARGET=APCAM_TARGET_ZR10',
                    f'-DWEBROOT_PATH="{Path(__file__).resolve().parents[1] / "webroot"}"', '-DMT11_WEB_TEST', '-DMT11_WEB_SITL', '-DWEB_SUPERVISED_TEST',
                    *[f'-D{key}="{value}"' for key, value in paths.items()],
                    str(repo/'web/mt11-web.cpp'), '-o', str(app/'zr10-web')], check=True)
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    mounts = root / 'mounts'
    mounts.write_text(f'/dev/mmcblk0p1 {root}/mnt vfat rw 0 0\n')
    service = (repo / 'packaging/zr10/service.sh').read_text()
    # Change paths only; execute the supervisor's real switching/fallback logic.
    service = re.sub(r'/(mnt|customer|tmp)(?=/|[ \n])',
                     lambda match: str(root)+match[0], service)
    service = service.replace('/proc/mounts', str(mounts)).replace(' -p 80 ', f' -p {port} ')
    script = app / 'service.sh'
    script.write_text(service)
    env = dict(os.environ, PATH=str(commands)+':'+os.environ['PATH'],
               TEST_ROOT=str(root), TEST_HARDWARE=str(root/'hardware'),
               TEST_OVERLAP=str(root/'overlap'), TEST_FAIL=str(root/'fail'))
    log = open(root/'service.log', 'w+')
    process = subprocess.Popen(['sh', str(script)], env=env, stdout=log, stderr=log,
                               start_new_session=True)
    request_file = runtime / 'camera-app.request'
    ready = runtime / 'camera-app.ready'
    generation = 0

    def wait_for(predicate, message, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            if process.poll() is not None:
                break
            time.sleep(0.1)
        log.flush()
        raise AssertionError(message+'\n'+(root/'service.log').read_text())

    def pids(name):
        result = subprocess.run([str(commands/'pidof'), name], env=env,
                                capture_output=True, text=True)
        return result.stdout.split()

    def select(kind):
        global generation
        generation += 1
        # Simulate the web's atomic selection/request publication, but omit
        # signaling entirely to exercise the supervisor's missed-PID race path.
        import fcntl
        with open(runtime/'camera-app.request.lock', 'w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            (config/'app_selection').write_text(kind+'\n')
            request_file.write_text(str(generation)+'\n')

    try:
        wait_for(ready.exists, 'replacement did not start')
        first_pid = ready.read_text()
        wait_for(lambda: pids('zr10-web'), 'web did not start')
        web_pid = pids('zr10-web')[0]
        def web_request(path, body=None):
            headers = {"Authorization": "Basic " + base64.b64encode(b'admin:test-password').decode()}
            if body is not None:
                headers['Content-Type'] = 'application/x-www-form-urlencoded'
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=90)
            connection.request('GET' if body is None else 'POST', path, body, headers)
            response = connection.getresponse()
            result = response.status, response.read()
            connection.close()
            return result
        status, page = web_request('/')
        assert status == 200
        csrf = re.search(rb'name=csrf value="([a-f0-9]{64})"', page).group(1).decode()
        assert b'name=app value=vendor' not in page
        assert web_request('/switch', f'csrf={csrf}&app=vendor')[0] == 404
        assert web_request('/restart', f'csrf={csrf}')[0] == 303
        generation = int(request_file.read_text())
        # Legacy choices cannot select a different application.
        old = ready.read_text()
        select('vendor')
        wait_for(lambda: ready.exists() and ready.read_text() != old, 'restart request was lost')
        assert not pids('sycamera.vendor')
        (root/'fail').touch()
        select('replacement')
        wait_for(lambda: not ready.exists() and not pids('camera-app'), 'failed camera did not stop')
        time.sleep(2)
        assert not pids('sycamera.vendor'), 'startup failure launched vendor app'
        assert web_request('/')[0] == 200
        (root/'fail').unlink()
        assert web_request('/restart', f'csrf={csrf}')[0] == 303
        wait_for(ready.exists, 'restart did not recover camera')
        os.kill(int(pids('camera-app')[0]), 9)
        wait_for(lambda: not ready.exists(), 'crash left stale readiness')
        time.sleep(2)
        assert not pids('sycamera.vendor'), 'crash launched vendor app'
        assert web_request('/')[0] == 200
        old_web = pids('zr10-web')[0]
        os.kill(int(old_web), 9)
        wait_for(lambda: pids('zr10-web') and pids('zr10-web')[0] != old_web, 'web did not respawn')
        csrf = re.search(rb'name=csrf value="([a-f0-9]{64})"', web_request('/')[1]).group(1).decode()
        assert web_request('/restart', f'csrf={csrf}')[0] == 303
        assert not (root/'overlap').exists(), 'concurrent hardware owners'
        # With no SD, settings use a temporary directory and media paths
        # cannot write into the root filesystem. Camera and web remain usable.
        saved_password = (config/'web.pass').read_bytes()
        import signal
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()
        mounts.write_text('')
        ready.unlink(missing_ok=True)
        process = subprocess.Popen(['sh', str(script)], env=env, stdout=log, stderr=log,
                                   start_new_session=True)
        wait_for(lambda: ready.exists() and pids('zr10-web'), 'no-SD camera/web startup failed')
        assert not pids('sycamera.vendor')
        assert web_request('/')[0] == 200
        assert (config/'web.pass').read_bytes() == saved_password
        assert (runtime/'zr10-config').resolve() == runtime/'zr10-config-nosd'
        print('PASS zr10: AP-only startup, legacy selection ignored, failure/crash recovery, web respawn')
    finally:
        import signal
        # Kill only the test session, including any restored vendor child.
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()
        log.close()
