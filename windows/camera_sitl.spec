# -*- mode: python ; coding: utf-8 -*-
from pathlib import Path
from PyInstaller.utils.hooks import collect_data_files, collect_submodules
root = Path(SPECPATH).parent
payload = root / 'build/windows/payload'
a = Analysis([str(root / 'windows/entry.py')], pathex=[str(root)],
             datas=[(str(payload / name), name) for name in ('native', 'fixtures', 'configs', 'licenses', 'webroot')] +
                   [(str(payload / 'targets.json'), '.'), (str(root / 'assets/camera-gimbal.ico'), 'assets')] +
                   collect_data_files('MAVProxy.modules.mavproxy_map'),
             hiddenimports=collect_submodules('MAVProxy.modules.mavproxy_map3d') +
                           ['MAVProxy.modules.lib.video_telemetry', 'pymavlink.dialects.v20.ardupilotmega',
                            'pymavlink.dialects.v10.ardupilotmega'],
             excludes=['PyQt5', 'PySide2', 'PySide6', 'tkinter'])
pyz = PYZ(a.pure)
gui = EXE(pyz, a.scripts, exclude_binaries=True, name='CameraGimbalSITL', console=False, upx=False, icon=str(root / 'assets/camera-gimbal.ico'))
worker = EXE(pyz, a.scripts, exclude_binaries=True, name='SITLWorker', console=True, upx=False, icon=str(root / 'assets/camera-gimbal.ico'))
COLLECT(gui, worker, a.binaries, a.datas, name='CameraGimbalSITL', upx=False)
