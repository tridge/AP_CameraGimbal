#!/usr/bin/env python3
"""Run the actual Z1-Mini supervisor with isolated paths and fake hardware apps."""
from pathlib import Path
import os
import base64
import http.client
import re
import socket
import subprocess
import tempfile
import time
import sys

native = "--native" in sys.argv

repo = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='z1mini-service-') as directory:
    root = Path(directory)
    app = root / 'opt/bin/gcu/ap'
    customer = root / 'opt/bin/gcu'
    runtime = root / 'tmp'
    commands = root / 'commands'
    for path in (app, customer, runtime, commands):
        path.mkdir(parents=True, exist_ok=True)
    config = root / 'opt/ap_cameragimbal'
    config.mkdir()
    (app / 'camera.ini.default').write_bytes((repo/'packaging/z1mini/camera.ini').read_bytes())
    (app / 'web.pass.default').write_text('test-password\n')
    if native:
        (app/'ax-capture').write_text('#!/bin/sh\nexit 1\n')
        (app/'ax-capture').chmod(0o755)
    source = root / 'fake.c'
    # Both processes take the same exclusive "hardware" lock. A start overlap
    # is a test failure even if the supervisor later recovers.
    source.write_text(r'''
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
static volatile sig_atomic_t stopping;
static void stop_leader(int sig) { (void)sig; stopping=1; pthread_exit(NULL); }
static void *media_worker(void *arg) {
    (void)arg;
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
    while (!stopping) usleep(10000);
    sleep(2); /* SDK teardown outlives the thread-group leader. */
    return NULL;
}
int main(int argc, char **argv) {
    (void)argc;
    int isp = strstr(argv[0], "/ipc/main") != NULL;
    int vendor = strstr(argv[0], "gb_control") != NULL;
    if (isp || (!vendor && atoi(getenv("TEST_NATIVE")))) {
        int media = open(getenv("TEST_MEDIA"), O_CREAT|O_RDWR, 0600);
        if (media < 0 || flock(media, LOCK_EX|LOCK_NB)) {
            FILE *bad = fopen(getenv("TEST_OVERLAP"), "w");
            if (bad) fclose(bad);
            return 5;
        }
    }
    if (isp) {
        pthread_t worker;
        signal(SIGTERM, stop_leader);
        pthread_create(&worker, NULL, media_worker, NULL);
        while (1) pause();
    }
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
        fprintf(ready, "pid=%d\nbackend=z1mini\n", getpid());
        fclose(ready);
    }
    while (1) pause();
}
''')
    subprocess.run(['c++', '-std=gnu++17', '-Wno-missing-field-initializers', '-O2', '-pthread', str(source), '-o', str(app / 'camera-app')], check=True)
    (customer / 'gb_control').write_bytes((app / 'camera-app').read_bytes())
    (customer / 'gb_control').chmod(0o755)
    (customer/'ipc').mkdir()
    (customer/'ipc/main').write_bytes((app/'camera-app').read_bytes())
    (customer/'ipc/main').chmod(0o755)
    # System actions are mocked. All process matching stays within this fixture.
    for command in ('ifconfig', 'killall'):
        (commands / command).write_text('#!/bin/sh\nexit 0\n')
    (commands / 'pidof').write_text('''#!/usr/bin/env python3
import os, sys
from pathlib import Path
pids = []
for path in Path('/proc').glob('[0-9]*/exe'):
    try:
        if (path.parent/'stat').read_text().split(') ')[1].startswith('Z '):
            continue
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
        "APP_DIR": app, "MEDIA_ROOT": root/'mnt',
        "APP_SELECTION_DIR": config,
        "APP_SELECTION_PATH": config/'app_selection',
        "PASSWORD_PATH": config/'web.pass',
        "REPLACEMENT_CONFIG_PATH": config/'camera.ini',
        "REPLACEMENT_CONFIG_BACKUP_PATH": config/'camera.ini.bak',
        "APP_REQUEST_PATH": runtime/'camera-app.request',
        "APP_REQUEST_LOCK_PATH": runtime/'camera-app.request.lock',
        "APP_STARTED_PATH": runtime/'camera-app.started',
        "CAMERA_READY_PATH": runtime/'camera-app.ready',
        "VENDOR_CAMERA_PATH": customer/'gb_control',
        "SWITCH_LOCK_PATH": runtime/'switch.lock',
        "UPGRADE_LOCK_PATH": runtime/'upgrade.lock',
        "USER_LOCK_PATH": runtime/'users.lock',
        "SESSION_PATH": runtime/'sessions', "RUNTIME_DIR": runtime,
    }
    subprocess.run(['c++', '-std=gnu++17', '-Wno-missing-field-initializers', '-O2', '-std=gnu++17', '-DAPCAM_TARGET=APCAM_TARGET_Z1_MINI',
                    '-DMT11_WEB_TEST', '-Wno-address-of-packed-member',
                    '-I'+str(repo/'camera_app/build/mavlink/all/include'),
                    *[f'-D{key}="{value}"' for key, value in paths.items()],
                    str(repo/'web/mt11-web.cpp'), '-o', str(app/'z1mini-web'), '-lm'], check=True)
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    mounts = root / 'mounts'
    mounts.write_text(f'/dev/mmcblk0p1 {root}/mnt/mmc vfat rw 0 0\n')
    service = (repo / 'packaging/z1mini/service.sh').read_text()
    # Change paths only; execute the supervisor's real switching/fallback logic.
    service = re.sub(r'/(mnt|opt|tmp)(?=/|[ \n"])' ,
                     lambda match: str(root)+match[0], service)
    service = service.replace('/proc/mounts', str(mounts)).replace('WEB_PORT=8080', f'WEB_PORT={port}').replace('WEB_PORT=80\n', f'WEB_PORT={port}\n')
    script = app / 'service.sh'
    script.write_text(service)
    env = dict(os.environ, PATH=str(commands)+':'+os.environ['PATH'],
               TEST_ROOT=str(root), TEST_HARDWARE=str(root/'hardware'),
               TEST_MEDIA=str(root/'media'), TEST_NATIVE=str(int(native)),
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
        assert bool(pids('main')) != native, 'wrong media owner at AP startup'
        first_pid = ready.read_text()
        wait_for(lambda: pids('z1mini-web'), 'web did not start')
        web_pid = pids('z1mini-web')[0]
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
        assert not pids('gb_control')
        (root/'fail').touch()
        select('replacement')
        wait_for(lambda: not ready.exists() and not pids('camera-app'), 'failed camera did not stop')
        time.sleep(2)
        assert not pids('gb_control'), 'startup failure launched vendor app'
        assert web_request('/')[0] == 200
        (root/'fail').unlink()
        assert web_request('/restart', f'csrf={csrf}')[0] == 303
        wait_for(ready.exists, 'restart did not recover camera')
        os.kill(int(pids('camera-app')[0]), 9)
        wait_for(lambda: not ready.exists(), 'crash left stale readiness')
        time.sleep(2)
        assert not pids('gb_control'), 'crash launched vendor app'
        assert web_request('/')[0] == 200
        old_web = pids('z1mini-web')[0]
        os.kill(int(old_web), 9)
        wait_for(lambda: pids('z1mini-web') and pids('z1mini-web')[0] != old_web, 'web did not respawn')
        csrf = re.search(rb'name=csrf value="([a-f0-9]{64})"', web_request('/')[1]).group(1).decode()
        assert web_request('/restart', f'csrf={csrf}')[0] == 303
        assert not (root/'overlap').exists(), 'concurrent hardware owners'
        assert bool(pids('main')) != native, 'wrong ISP owner'
        if not native:
            os.kill(int(pids('main')[0]), 9)
            wait_for(lambda: not pids('camera-app'), 'controller left running after ISP exit')
            assert web_request('/')[0] == 200
        print('PASS z1mini: AP-only startup, legacy selection ignored, failure/crash recovery, web respawn')
    finally:
        import signal
        # Kill only the test session, including any restored vendor child.
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()
        log.close()
