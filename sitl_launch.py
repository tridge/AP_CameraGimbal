#!/usr/bin/env python3
"""PyQt launcher for up to four independent camera/gimbal simulators."""
import codecs
import os
from pathlib import Path
from sitl.target_properties import TARGETS
from sitl.launcher_config import MAX_SIMULATORS, mavlink_settings
import signal
import subprocess
import sys
import time
import uuid
import queue
import threading

try:
    from PyQt6 import QtCore, QtGui, QtWidgets
except ImportError:
    try:
        from PyQt5 import QtCore, QtGui, QtWidgets
    except ImportError:
        sys.exit('Install PyQt6 (for example: sudo apt install python3-pyqt6) to use this launcher.')

REPO = Path(__file__).resolve().parent
OWNER_VARIABLE = 'CAMERA_GIMBAL_SITL_LAUNCH_ID'


def owned_processes(token):
    """Include detached web-restart children, without touching other SITL runs."""
    if os.name == 'nt':
        import psutil
        result = []
        for process in psutil.process_iter(['pid']):
            try:
                if process.environ().get(OWNER_VARIABLE) == token:
                    result.append(process.pid)
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass
        return result
    marker = (OWNER_VARIABLE + '=' + token).encode()
    found = []
    for process in Path('/proc').iterdir():
        if not process.name.isdigit():
            continue
        try:
            if process.stat().st_uid != os.getuid():
                continue
            if marker in (process / 'environ').read_bytes().split(b'\0'):
                found.append(int(process.name))
        except (OSError, ProcessLookupError):
            pass
    return found


class SimulatorPanel(QtWidgets.QWidget):
    def __init__(self, repo=REPO, instance=1):
        super().__init__()
        self.repo = Path(repo)
        self.instance = instance
        self.process = None
        self.token = None
        self.phase = 'idle'
        self.closing = False
        self.stop_started = None
        self.last_cleanup = 0
        self.log_files = {}
        self.runtime = None
        self.launch_output = ''
        self.setWindowIcon(QtGui.QIcon(str(REPO / 'assets/camera-gimbal.ico')))
        self.setWindowTitle('Camera / Gimbal SITL')
        self.resize(820, 570)

        layout = QtWidgets.QVBoxLayout(self)
        form = QtWidgets.QFormLayout()
        self.camera = QtWidgets.QComboBox()
        for backend, properties in TARGETS.items():
            self.camera.addItem(properties['product_name'], backend)
        self.camera.setCurrentIndex(self.camera.findData('mt11'))
        self.orientation = QtWidgets.QComboBox()
        self.orientation.addItem('Normal', 'upright')
        self.orientation.addItem('Inverted', 'inverted')
        self.video = QtWidgets.QComboBox()
        self.video.addItem('Simple test patterns', 'simple')
        self.video.addItem('3D terrain and imagery', 'terrain')
        form.addRow('Camera', self.camera)
        form.addRow('Orientation', self.orientation)
        form.addRow('Video source', self.video)
        self.clear_parameters = QtWidgets.QCheckBox('Clear parameters on launch')
        self.clear_parameters.setToolTip('Restore camera parameter defaults; keep recordings and web login settings.')
        form.addRow('', self.clear_parameters)
        layout.addLayout(form)
        self.hint = QtWidgets.QLabel()
        self.hint.setWordWrap(True)
        layout.addWidget(self.hint)
        buttons = QtWidgets.QHBoxLayout()
        self.start_button = QtWidgets.QPushButton('Start SITL')
        self.stop_button = QtWidgets.QPushButton('Stop')
        self.web_button = QtWidgets.QPushButton('Open Web UI')
        self.stop_button.setEnabled(False)
        self.web_button.setEnabled(False)
        for button in (self.start_button, self.stop_button, self.web_button):
            buttons.addWidget(button)
        buttons.addStretch()
        layout.addLayout(buttons)
        self.status = QtWidgets.QLabel('Stopped')
        layout.addWidget(self.status)
        self.log = QtWidgets.QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(5000)
        layout.addWidget(self.log)
        self.start_button.clicked.connect(self.start)
        self.stop_button.clicked.connect(self.stop)
        self.web_button.clicked.connect(self.open_web)
        self.camera.currentIndexChanged.connect(self.update_hint)
        self.video.currentIndexChanged.connect(self.update_hint)
        self.timer = QtCore.QTimer(self)
        self.timer.setInterval(100)
        self.timer.timeout.connect(self.poll)
        self.timer.start()
        self.update_hint()

    def update_hint(self):
        hint = ('Start launches the selected gimbal, camera app and web UI.' if os.name == 'nt' else
                'Start builds the selected camera, then launches its gimbal, camera app and web UI.')
        if self.video.currentData() == 'terrain':
            hint += ' The 3D view needs vehicle MAVLink telemetry and initially downloads terrain and imagery.'
        try:
            env, build = self.environment()
            prefix = self.camera.currentData().upper() + '_SITL_'
            settings = mavlink_settings(build, self.instance, self.clear_parameters.isChecked())
            tcp = env.get(prefix + 'MAVLINK_TCP_PORT', settings['tcp_port'])
            udp = env.get(prefix + 'MAVLINK_UDP_PORT', settings['udp_port'])
            hint += (f"\nWeb: {env[prefix + 'WEB_PORT']} · MAVLink TCP/UDP: {tcp}/{udp}"
                     f" · Vendor: {env[prefix + 'CAMERA_PORT']} · RTSP: {env[prefix + 'RTSP_PORT']}"
                     f"\nCamera component: {settings['camera_component_id']} · Data: {build / 'runtime'}")
        except (ValueError, OSError) as error:
            hint += f'\nInvalid configuration: {error}'
        self.hint.setText(hint)

    def environment(self):
        backend = self.camera.currentData()
        env = os.environ.copy()
        default_build = self.repo / 'build' / ('sitl' if backend == 'mt11' else backend + '-sitl')
        if os.name == 'nt':
            default_build = Path(env.get('LOCALAPPDATA', str(Path.home()))) / 'ArduPilot/CameraGimbalSITL' / backend
        build = Path(env.get('CAMERA_GIMBAL_SITL_BUILD', str(default_build))).resolve()
        if self.instance > 1:
            build = build / f'instance-{self.instance}' / backend
        env.update(CAMERA_GIMBAL_SITL_BACKEND=backend,
                   CAMERA_GIMBAL_SITL_BUILD=str(build),
                   CAMERA_GIMBAL_SITL_INSTANCE=str(self.instance),
                   CAMERA_GIMBAL_SITL_VIDEO=self.video.currentData(),
                   CAMERA_GIMBAL_SITL_RESET_PARAMETERS='1' if self.clear_parameters.isChecked() else '0',
                   PYTHONUNBUFFERED='1')
        prefix = backend.upper() + '_SITL_'
        for name, default, stride in (('WEB_PORT', 8081, 1), ('RTSP_PORT', 8554, 10),
                                      ('CAMERA_PORT', int(TARGETS[backend]['vendor_port']), 1)):
            env[prefix + name] = str(int(env.get(prefix + name, default)) + (self.instance - 1) * stride)
        # Explicit environment overrides remain supported, with slot offsets.
        for name in ('MAVLINK_TCP_PORT', 'MAVLINK_UDP_PORT'):
            if prefix + name in env:
                port = int(env[prefix + name])
                env[prefix + name] = str(port + (self.instance - 1) * 10 if port else 0)
        if prefix + 'GIMBAL_PORT' in env:
            port = int(env[prefix + 'GIMBAL_PORT'])
            env[prefix + 'GIMBAL_PORT'] = str(port + self.instance - 1 if port else 0)
        env[f'{backend.upper()}_SITL_ORIENTATION'] = self.orientation.currentData()
        python = self.repo / 'build/terrain-venv/bin/python'
        if not env.get('CAMERA_GIMBAL_SITL_PYTHON'):
            env['CAMERA_GIMBAL_SITL_PYTHON'] = str(python) if python.is_file() else sys.executable
        # A GUI selection always wins over a previously exported terrain mode.
        env.pop('CAMERA_APP_SITL_TERRAIN', None)
        return env, build

    def append(self, text):
        if text:
            self.log.moveCursor(QtGui.QTextCursor.MoveOperation.End if hasattr(QtGui.QTextCursor, 'MoveOperation')
                                else QtGui.QTextCursor.End)
            self.log.insertPlainText(text)
            self.log.ensureCursorVisible()

    def start(self):
        if self.phase != 'idle':
            return
        try:
            self.env, build = self.environment()
        except (ValueError, OSError) as error:
            self.finish(f'Invalid configuration: {error}')
            return
        self.runtime = build / 'runtime/run'
        self.token = uuid.uuid4().hex
        self.env[OWNER_VARIABLE] = self.token
        self.launch_output = ''
        self.log_files.clear()
        self.log.clear()
        self.stop_started = None
        self.closing = False
        self.web_button.setEnabled(False)
        self.set_controls(False)
        backend = self.camera.currentData()
        target = 'sitl' if backend == 'mt11' else backend + '_sitl'
        variable = 'SITL_BUILD' if backend == 'mt11' else backend.upper() + '_SITL_BUILD'
        self.build_command = ['make', target, f'{variable}={build}']
        try:
            port = int(self.env.get(f'{backend.upper()}_SITL_WEB_PORT', '8081'))
            if not 1 <= port <= 65535:
                raise ValueError
        except ValueError:
            self.finish('Invalid SITL web port; expected 1–65535')
            return
        self.web_url = f'http://127.0.0.1:{port}/'
        if os.name == 'nt':
            from windows.runtime import worker_command
            self.runtime.mkdir(parents=True, exist_ok=True)
            (self.runtime / 'stop').unlink(missing_ok=True)
            self.spawn(worker_command('run'), 'launching', 'Starting SITL…')
        elif self.video.currentData() == 'terrain':
            self.spawn([self.env['CAMERA_GIMBAL_SITL_PYTHON'], str(self.repo / 'sitl/terrain_video.py'), '--check'],
                       'checking', 'Checking terrain dependencies…')
        else:
            self.spawn(self.build_command, 'building', 'Building SITL…')

    def set_controls(self, idle):
        for control in (self.camera, self.orientation, self.video, self.clear_parameters, self.start_button):
            control.setEnabled(idle)
        self.stop_button.setEnabled(not idle)

    def spawn(self, command, phase, status):
        self.phase = phase
        self.status.setText(status)
        self.append('$ ' + ' '.join(command) + '\n')
        self.decoder = codecs.getincrementaldecoder('utf-8')(errors='replace')
        try:
            self.process = subprocess.Popen(command, cwd=self.repo, env=self.env,
                                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, start_new_session=os.name != 'nt',
                                            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            if os.name == 'nt':
                self.output_queue = queue.Queue()
                def reader(source, output):
                    while True:
                        data = source.read1(65536)
                        output.put(data)
                        if not data:
                            break
                self.reader_thread = threading.Thread(target=reader, args=(self.process.stdout, self.output_queue), daemon=True)
                self.reader_thread.start()
            else:
                os.set_blocking(self.process.stdout.fileno(), False)
        except OSError as error:
            self.process = None
            self.finish(f'Could not start: {error}')

    def read_output(self):
        if not self.process or not self.process.stdout:
            return
        # Bound work per timer tick so a noisy compiler cannot freeze the UI.
        try:
            data = (self.output_queue.get_nowait() if os.name == 'nt' else
                    os.read(self.process.stdout.fileno(), 65536))
        except (BlockingIOError, queue.Empty):
            return
        text = self.decoder.decode(data, final=not data)
        self.append(text)
        if self.phase == 'launching':
            self.launch_output = (self.launch_output + text)[-8192:]
            if f'{self.camera.currentData().upper()} SITL running' in self.launch_output.splitlines():
                self.phase = 'running'
                self.log_files.clear()
                self.status.setText(f'Running — {self.web_url}')
                self.web_button.setEnabled(True)

    def tail_logs(self):
        if self.runtime is None:
            return
        for name in ('camera_app.log', 'gimbal.log', 'web.log'):
            path = self.runtime / name
            try:
                stat = path.stat()
                inode, offset = self.log_files.get(name, (stat.st_ino, 0))
                if inode != stat.st_ino or stat.st_size < offset:
                    offset = 0
                with path.open('rb') as source:
                    source.seek(offset)
                    data = source.read(16384)
                    self.log_files[name] = (stat.st_ino, source.tell())
                if data:
                    self.append(f'[{name}] ' + data.decode('utf-8', errors='replace'))
            except OSError:
                pass

    def close_output(self):
        if os.name == 'nt':
            self.reader_thread.join(timeout=1)
            while not self.output_queue.empty():
                self.read_output()
        else:
            self.read_output()
        self.process.stdout.close()

    def stop(self):
        if self.phase == 'idle' or self.stop_started is not None:
            return
        self.stop_started = time.monotonic()
        self.last_cleanup = 0
        self.phase = 'stopping'
        self.status.setText('Stopping SITL…')
        self.stop_button.setEnabled(False)
        self.web_button.setEnabled(False)
        # Let run.sh close recordings and stop children before cleaning up any
        # remaining build workers or detached camera restart children.
        if os.name == 'nt' and self.runtime is not None:
            self.runtime.mkdir(parents=True, exist_ok=True)
            (self.runtime / 'stop').touch()
        elif self.process and self.process.poll() is None:
            self.process.terminate()

    def cleanup(self):
        now = time.monotonic()
        if now - self.last_cleanup < 0.5:
            return
        self.last_cleanup = now
        remaining = owned_processes(self.token)
        if not remaining:
            if self.process:
                if self.process.poll() is None:
                    return
                self.close_output()
                self.process = None
            self.finish('Stopped')
            return
        elapsed = now - self.stop_started
        if elapsed >= (9 if os.name == 'nt' else 1):
            for pid in remaining:
                try:
                    os.kill(pid, signal.SIGTERM if os.name == 'nt' else (signal.SIGKILL if elapsed >= 8 else signal.SIGTERM))
                except ProcessLookupError:
                    pass

    def poll(self):
        if self.phase == 'idle':
            return
        self.read_output()
        if self.phase in ('running', 'stopping'):
            self.tail_logs()
        if self.stop_started is not None:
            self.cleanup()
            return
        if not self.process or self.process.poll() is None:
            return
        code = self.process.returncode
        self.close_output()
        self.process = None
        if code:
            self.tail_logs()
            self.append(f'Process exited with status {code}.\n')
            self.failure = f'Failed (exit {code}); see log'
            self.stop()
        elif self.phase == 'checking':
            self.spawn(self.build_command, 'building', 'Building SITL…')
        elif self.phase == 'building':
            # Skip historical logs; the launcher replaces them when it starts.
            self.log_files = {}
            for name in ('camera_app.log', 'gimbal.log', 'web.log'):
                try:
                    s = (self.runtime / name).stat()
                    self.log_files[name] = (s.st_ino, s.st_size)
                except OSError:
                    pass
            self.spawn(['sh', str(self.repo / 'sitl/run.sh')], 'launching', 'Starting SITL…')
        else:
            self.stop()

    def finish(self, status):
        self.phase = 'idle'
        self.token = None
        self.stop_started = None
        self.status.setText(getattr(self, 'failure', status))
        if hasattr(self, 'failure'):
            del self.failure
        self.set_controls(True)
        self.web_button.setEnabled(False)
        if self.closing:
            self.close()

    def open_web(self):
        QtGui.QDesktopServices.openUrl(QtCore.QUrl(self.web_url))

    def closeEvent(self, event):
        if self.phase == 'idle':
            event.accept()
        else:
            self.closing = True
            self.stop()
            event.ignore()


class Launcher(QtWidgets.QWidget):
    """Coordinate a group; serialize builds which share compiler output files."""
    def __init__(self, repo=REPO):
        super().__init__()
        self.phase = 'idle'
        self.closing = False
        self.active = []
        self.pending = []
        self.failure = None
        self.setWindowTitle('Camera / Gimbal SITL')
        self.setWindowIcon(QtGui.QIcon(str(REPO / 'assets/camera-gimbal.ico')))
        self.resize(920, 720)
        layout = QtWidgets.QVBoxLayout(self)
        form = QtWidgets.QFormLayout()
        self.count = QtWidgets.QSpinBox()
        self.count.setRange(1, MAX_SIMULATORS)
        self.count.setValue(1)
        form.addRow('Number of simulators', self.count)
        layout.addLayout(form)
        self.tabs = QtWidgets.QTabWidget()
        self.simulators = [SimulatorPanel(repo, i + 1) for i in range(MAX_SIMULATORS)]
        for panel in self.simulators:
            panel.start_button.hide()
            panel.stop_button.hide()
            panel.clear_parameters.toggled.connect(panel.update_hint)
        layout.addWidget(self.tabs)
        self.count.valueChanged.connect(self.update_tabs)
        self.update_tabs()
        buttons = QtWidgets.QHBoxLayout()
        self.start_button = QtWidgets.QPushButton('Start all')
        self.stop_button = QtWidgets.QPushButton('Stop all')
        self.stop_button.setEnabled(False)
        self.start_button.clicked.connect(self.start)
        self.stop_button.clicked.connect(self.stop)
        buttons.addWidget(self.start_button)
        buttons.addWidget(self.stop_button)
        layout.addLayout(buttons)
        self.status = QtWidgets.QLabel('Stopped')
        layout.addWidget(self.status)
        self.timer = QtCore.QTimer(self)
        self.timer.setInterval(100)
        self.timer.timeout.connect(self.poll)
        self.timer.start()

    def update_tabs(self):
        selected = self.tabs.currentIndex()
        self.tabs.clear()
        for i, panel in enumerate(self.simulators[:self.count.value()]):
            self.tabs.addTab(panel, f'Simulator {i + 1}')
        self.tabs.setCurrentIndex(max(0, min(selected, self.count.value() - 1)))

    def validate(self):
        endpoints, identities, builds = {}, {}, set()
        for panel in self.simulators[:self.count.value()]:
            env, build = panel.environment()
            if build in builds:
                raise ValueError('Simulators must use separate build/data directories')
            builds.add(build)
            settings = mavlink_settings(build, panel.instance, panel.clear_parameters.isChecked())
            component = settings['camera_component_id']
            if not 100 <= component <= 105:
                raise ValueError(f'Simulator {panel.instance}: camera component must be 100–105')
            if component in identities:
                raise ValueError(f'Simulators {identities[component]} and {panel.instance} have camera component '
                                 f'{component}. Change the saved camera component or clear parameters.')
            identities[component] = panel.instance
            prefix = panel.camera.currentData().upper() + '_SITL_'
            rtsp = int(env[prefix + 'RTSP_PORT'])
            ports = [('web', int(env[prefix + 'WEB_PORT']), ('TCP',)),
                     ('RTSP', rtsp, ('TCP',)), ('live video', rtsp + 1, ('TCP',)),
                     ('vendor', int(env[prefix + 'CAMERA_PORT']), ('TCP', 'UDP'))]
            for transport in ('TCP', 'UDP'):
                port = int(env.get(prefix + 'MAVLINK_' + transport + '_PORT', settings[transport.lower() + '_port']))
                if port:
                    ports.append(('MAVLink', port, (transport,)))
            gimbal = int(env.get(prefix + 'GIMBAL_PORT', 0))
            if gimbal:
                ports.append(('gimbal', gimbal, ('UDP',)))
            for name, port, protocols in ports:
                if not 1 <= port <= 65535:
                    raise ValueError(f'Simulator {panel.instance}: invalid {name} port {port}')
                for protocol in protocols:
                    key = (protocol, port)
                    if key in endpoints:
                        raise ValueError(f'{protocol} port {port} is shared by {endpoints[key]} '
                                         f'and simulator {panel.instance} {name}')
                    endpoints[key] = f'simulator {panel.instance} {name}'

    def start(self):
        if self.phase != 'idle':
            return
        try:
            self.validate()
        except (ValueError, OSError) as error:
            self.status.setText(str(error))
            return
        self.closing = False
        self.failure = None
        self.active = []
        self.pending = list(self.simulators[:self.count.value()])
        self.phase = 'launching'
        self.count.setEnabled(False)
        self.start_button.setEnabled(False)
        self.stop_button.setEnabled(True)
        for panel in self.pending:
            panel.set_controls(False)
        self.status.setText('Starting simulators…')
        self.launch_next()

    def launch_next(self):
        if self.pending:
            panel = self.pending.pop(0)
            self.active.append(panel)
            panel.start()
        else:
            self.phase = 'running'
            self.status.setText(f'{len(self.active)} simulators running')

    def stop(self):
        if self.phase == 'idle':
            return
        self.pending.clear()
        self.phase = 'stopping'
        self.stop_button.setEnabled(False)
        self.status.setText('Stopping simulators…')
        for panel in self.active:
            panel.stop()

    def poll(self):
        if self.phase == 'idle':
            return
        if self.phase != 'stopping':
            for panel in self.active:
                if panel.phase in ('idle', 'stopping'):
                    self.failure = f'Simulator {panel.instance} stopped; see its log'
                    self.stop()
                    break
            if self.phase == 'launching' and all(p.phase == 'running' for p in self.active):
                self.launch_next()
        if self.phase == 'stopping' and all(p.phase == 'idle' for p in self.active):
            self.phase = 'idle'
            self.count.setEnabled(True)
            self.start_button.setEnabled(True)
            for panel in self.simulators:
                panel.set_controls(True)
            self.status.setText(self.failure or 'Stopped')
            if self.closing:
                self.close()

    def closeEvent(self, event):
        if self.phase == 'idle':
            event.accept()
        else:
            self.closing = True
            self.stop()
            event.ignore()


def main():
    app = QtWidgets.QApplication(sys.argv)
    window = Launcher()
    window.show()
    signal.signal(signal.SIGINT, lambda *_: window.close())
    signal.signal(signal.SIGTERM, lambda *_: window.close())
    sys.exit(app.exec())


if __name__ == '__main__':
    main()
