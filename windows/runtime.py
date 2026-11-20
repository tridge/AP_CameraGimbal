"""Standalone Windows process supervisor; configuration lives outside the bundle."""
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[1]
_JOB = None


def worker_command(role):
    if getattr(sys, 'frozen', False):
        return [str(Path(sys.executable).with_name('SITLWorker.exe')), '--' + role]
    return [sys.executable, str(REPO / 'windows/entry.py'), '--' + role]


def posix_path(path):
    path = Path(path).resolve()
    if path.drive.startswith('\\\\'):
        return path.as_posix()
    return '/cygdrive/' + path.drive[0].lower() + path.as_posix()[2:]


def package_root():
    return Path(os.environ.get('CAMERA_GIMBAL_SITL_PACKAGE',
                               str(REPO if getattr(sys, 'frozen', False) else REPO / 'build/windows/payload')))


def own_children():
    """OS-enforced cleanup, including camera children detached by web restart."""
    global _JOB
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    kernel.CreateJobObjectW.restype = wintypes.HANDLE
    kernel.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    class Basic(ctypes.Structure):
        _fields_ = [('process_time', ctypes.c_int64), ('job_time', ctypes.c_int64),
                    ('flags', wintypes.DWORD), ('minimum', ctypes.c_size_t),
                    ('maximum', ctypes.c_size_t), ('active', wintypes.DWORD),
                    ('affinity', ctypes.c_size_t), ('priority', wintypes.DWORD),
                    ('scheduling', wintypes.DWORD)]
    class Extended(ctypes.Structure):
        _fields_ = [('basic', Basic), ('io', ctypes.c_uint64 * 6),
                    ('process_memory', ctypes.c_size_t), ('job_memory', ctypes.c_size_t),
                    ('peak_process', ctypes.c_size_t), ('peak_job', ctypes.c_size_t)]
    _JOB = kernel.CreateJobObjectW(None, None)
    limits = Extended()
    limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if not _JOB or not kernel.SetInformationJobObject(_JOB, 9, ctypes.byref(limits), ctypes.sizeof(limits)) or not kernel.AssignProcessToJobObject(_JOB, kernel.GetCurrentProcess()):
        raise ctypes.WinError(ctypes.get_last_error())
    # Keep the raw handle until process exit, when Windows closes it and kills
    # any remaining descendants. No Python finalizer should close it early.


def ready(path, process, timeout=30):
    deadline = time.monotonic() + timeout
    while True:
        try:
            if path.is_file() and path.stat().st_size > 0:
                return
        except FileNotFoundError:
            pass
        if process.poll() is not None:
            raise RuntimeError(f'{process.args[0]} exited with status {process.returncode}')
        if time.monotonic() > deadline:
            raise RuntimeError(f'Timed out waiting for {path.name}')
        time.sleep(.05)


def main():
    import msvcrt
    from sitl.prepare_runtime import main as prepare
    own_children()
    backend = os.environ['CAMERA_GIMBAL_SITL_BACKEND']
    from sitl.target_properties import TARGETS
    if backend not in TARGETS:
        raise ValueError('Unknown camera')
    bundle = package_root().resolve()
    native = bundle / 'native'
    root = Path(os.environ['CAMERA_GIMBAL_SITL_BUILD']).resolve() / 'runtime'
    run = root / 'run'
    run.mkdir(parents=True, exist_ok=True)
    lock = (run / 'launcher.lock').open('a+b')
    lock.seek(0); lock.write(b'0'); lock.flush(); lock.seek(0)
    msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
    prefix = backend.upper() + '_SITL_'
    web_port = int(os.environ.get(prefix + 'WEB_PORT', '8081'))
    # Refuse an occupied endpoint before resetting any saved settings.
    import socket
    from sitl.launcher_config import vendor_ports
    vendor_tcp, vendor_udp = vendor_ports(TARGETS[backend],
        int(os.environ.get(prefix + 'CAMERA_PORT', str(int(TARGETS[backend]['vendor_port'])))))
    for port, kind in ((web_port, socket.SOCK_STREAM), (vendor_tcp, socket.SOCK_STREAM),
                       (vendor_udp, socket.SOCK_DGRAM),
                       (int(os.environ.get(prefix + 'RTSP_PORT', '8554')), socket.SOCK_STREAM),
                       (int(os.environ.get(prefix + 'RTSP_PORT', '8554')) + 1, socket.SOCK_STREAM)):
        with socket.socket(socket.AF_INET, kind) as probe:
            probe.bind(('127.0.0.1', port))
    sys.argv = ['prepare_runtime', str(root), str(bundle / 'configs' / (backend + '.ini'))]
    if os.environ.get('CAMERA_GIMBAL_SITL_RESET_PARAMETERS') == '1':
        sys.argv.append('--reset-parameters')
    prepare()
    env = os.environ.copy()
    env['PATH'] = str(native) + os.pathsep + str(Path(os.environ['SystemRoot']) / 'System32')
    # PyInstaller alters the DLL search path. External native services must
    # load their own runtime; frozen Python children set up their own loader.
    ctypes.windll.kernel32.SetDllDirectoryW(None)
    env['CAMERA_GIMBAL_SITL_RUNTIME'] = posix_path(root)
    camera_exe = native / ('camera-app-' + backend + '.exe')
    web_exe = native / ('web-' + backend + '.exe')
    env['CAMERA_GIMBAL_SITL_CAMERA_EXE'] = posix_path(camera_exe)
    env['CAMERA_GIMBAL_SITL_WEB_EXE'] = posix_path(web_exe)
    env['CAMERA_GIMBAL_SITL_WEBROOT'] = posix_path(bundle / 'webroot')
    paths = {'CAMERA_APP_CONFIG': 'app/camera.ini', 'CAMERA_APP_READY_PATH': 'run/camera-app.ready',
             'CAMERA_APP_LOG_ROOT': 'mnt/logs', 'CAMERA_APP_RECORD_STATE': 'run/recording.state', 'CAMERA_APP_RECORD_ROOT': 'mnt/DCIM/record',
             'CAMERA_APP_CAPTURE_ROOT': 'mnt/DCIM/capture'}
    env.update({key: posix_path(root / value) for key, value in paths.items()})
    env.update(CAMERA_APP_BACKEND=backend,
               CAMERA_APP_PORT=os.environ.get(prefix + 'CAMERA_PORT', str(int(TARGETS[backend]['vendor_port']))),
               CAMERA_APP_RTSP_PORT=os.environ.get(prefix + 'RTSP_PORT', '8554'),
               CAMERA_APP_SITL_VIDEO1=str(bundle / 'fixtures/rgb.h264'),
               CAMERA_APP_SITL_VIDEO2=str(bundle / ('fixtures/thermal.h264' if TARGETS[backend]['have_thermal'] else 'fixtures/rgb.h264')),
               CAMERA_APP_SITL_PHOTO=str(bundle / 'fixtures/photo.jpg'))
    for transport in ('TCP', 'UDP'):
        key = prefix + 'MAVLINK_' + transport + '_PORT'
        target = 'CAMERA_APP_MAVLINK_' + transport + '_PORT'
        env.pop(target, None)
        if key in os.environ:
            env[target] = os.environ[key]
    env['MT11_WEB_CAMERA_PORT'] = env['CAMERA_APP_PORT']
    env['MT11_WEB_LIVE_PORT'] = str(int(env['CAMERA_APP_RTSP_PORT']) + 1)
    if getattr(sys, 'frozen', False):
        env['CAMERA_GIMBAL_SITL_PYTHON'] = str(Path(sys.executable).with_name('SITLWorker.exe'))
        env['CAMERA_APP_SITL_RENDERER'] = '--terrain'
    else:
        env['CAMERA_GIMBAL_SITL_PYTHON'] = sys.executable
        env['CAMERA_APP_SITL_RENDERER'] = str(REPO / 'sitl/terrain_video.py')
    if os.environ.get('CAMERA_GIMBAL_SITL_VIDEO', 'simple') == 'terrain':
        env['CAMERA_APP_SITL_TERRAIN'] = env['CAMERA_APP_SITL_RENDERER']
    else:
        env.pop('CAMERA_APP_SITL_TERRAIN', None)
    children, logs = [], []
    def launch(command, name):
        log = (run / (name + '.log')).open('wb')
        logs.append(log)
        process = subprocess.Popen(command, env=env, cwd=root, stdin=subprocess.DEVNULL,
                                   stdout=log, stderr=log, creationflags=subprocess.CREATE_NO_WINDOW)
        children.append(process)
        return process
    def signal_cygwin(pid, signal='TERM'):
        return subprocess.run([str(native / 'kill.exe'), '-' + signal, str(pid)], env=env,
                       creationflags=subprocess.CREATE_NO_WINDOW, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for name in ('gimbal.ready', 'camera-app.ready', 'web.pid'):
        (run / name).unlink(missing_ok=True)
    try:
        gimbal = launch(worker_command('gimbal') + ['--backend', backend, '--port', os.environ.get(prefix + 'GIMBAL_PORT', '0'),
                        '--orientation', os.environ.get(prefix + 'ORIENTATION', 'upright'),
                        '--ready-file', str(run / 'gimbal.ready')], 'gimbal')
        ready(run / 'gimbal.ready', gimbal)
        env['CAMERA_APP_UART'] = 'udp://' + (run / 'gimbal.ready').read_text().strip()
        camera = launch([str(camera_exe), '--backend', backend], 'camera_app')
        ready(run / 'camera-app.ready', camera)
        web = launch([str(web_exe), '-p', str(web_port)], 'web')
        ready(run / 'web.pid', web)
        print(f'{backend.upper()} SITL running', flush=True)
        print(f'  Web UI: http://127.0.0.1:{web_port}/ (admin / ardupilot)', flush=True)
        print(f'  Data: {root}', flush=True)
        while not (run / 'stop').exists():
            if web.poll() is not None or gimbal.poll() is not None:
                raise RuntimeError('A SITL service stopped; see the logs')
            # Web restart replaces the camera process; its ready file is authoritative.
            time.sleep(.1)
    finally:
        pid_file = run / 'web.pid'
        if pid_file.exists():
            signal_cygwin(int(pid_file.read_text()))
        ready_file = run / 'camera-app.ready'
        if ready_file.exists():
            info = dict(line.split('=', 1) for line in ready_file.read_text().splitlines() if '=' in line)
            camera_pid = int(info['pid'])
            signal_cygwin(camera_pid)
            deadline = time.monotonic() + 7
            # The ready file is removed before encoders and recordings close.
            # Wait for process exit, including a camera replaced by web restart.
            while time.monotonic() < deadline and signal_cygwin(camera_pid, '0').returncode == 0:
                time.sleep(.1)
        for process in reversed(children):
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=5)
        for log in logs:
            log.close()
        lock.close()
    print('SITL stopped', flush=True)
