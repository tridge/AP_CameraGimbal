#!/usr/bin/env python3
"""Gather native services, codecs, fixtures, defaults and distribution notices."""
from pathlib import Path
import shutil
import subprocess
import sys
import imageio_ffmpeg

ROOT = Path(__file__).resolve().parents[1]
PAYLOAD = ROOT / 'build/windows/payload'


def main():
    for name in ('native', 'fixtures', 'configs', 'licenses'):
        (PAYLOAD / name).mkdir(parents=True, exist_ok=True)
    for path in (ROOT / 'build/windows/native').iterdir():
        if path.suffix.lower() in ('.exe', '.dll'):
            shutil.copy2(path, PAYLOAD / 'native' / path.name)
    # Replace the tree to avoid shipping stale templates from an older build.
    shutil.rmtree(PAYLOAD / 'webroot', ignore_errors=True)
    shutil.copytree(ROOT / 'web/webroot', PAYLOAD / 'webroot')
    shutil.copy2(ROOT / 'build/targets/targets.json', PAYLOAD / 'targets.json')
    ffmpeg = PAYLOAD / 'native/ffmpeg.exe'
    shutil.copy2(imageio_ffmpeg.get_ffmpeg_exe(), ffmpeg)
    for backend, source in (('mt11', 'camera_app/camera.ini'), ('a8', 'packaging/a8/camera.ini'),
                            ('zr10', 'sitl/zr10.ini'), ('zt30', 'packaging/zt30/camera.ini'),
                            ('z1mini', 'packaging/z1mini/camera.ini')):
        shutil.copy2(ROOT / source, PAYLOAD / 'configs' / (backend + '.ini'))
    for name, filter_ in [('rgb.h264', 'testsrc2=size=1920x1080:rate=20'),
                           ('thermal.h264', 'testsrc2=size=1280x720:rate=20')]:
        subprocess.run([str(ffmpeg), '-hide_banner', '-loglevel', 'error', '-y',
                        '-f', 'lavfi', '-i', filter_, '-t', '1', '-an', '-c:v', 'libx264',
                        '-threads', '2', '-preset', 'ultrafast', '-tune', 'zerolatency',
                        '-x264-params', 'aud=1:repeat-headers=1:keyint=20:bframes=0',
                        str(PAYLOAD / 'fixtures' / name)], check=True)
    subprocess.run([str(ffmpeg), '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi',
                    '-i', 'testsrc2=size=1920x1080:rate=1', '-frames:v', '1', '-threads', '1',
                    str(PAYLOAD / 'fixtures/photo.jpg')], check=True)
    # Preserve upstream distribution notices, including those of bundled wheels.
    for name in ('LICENSE', 'COPYING', 'COPYING.txt'):
        if (ROOT / name).is_file():
            shutil.copy2(ROOT / name, PAYLOAD / 'licenses' / name)
    cygwin_notices = ROOT / 'build/windows/cygwin-licenses'
    if cygwin_notices.exists():
        shutil.copytree(cygwin_notices, PAYLOAD / 'licenses/Cygwin', dirs_exist_ok=True)
    import importlib.metadata
    for distribution in importlib.metadata.distributions():
        for item in distribution.files or []:
            if any(part.lower().startswith(('license', 'copying', 'notice')) for part in item.parts):
                source = Path(distribution.locate_file(item))
                if source.is_file():
                    target = PAYLOAD / 'licenses' / distribution.metadata['Name'] / Path(*item.parts)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, target)
    (PAYLOAD / 'licenses/README.txt').write_text(
        'Camera/Gimbal SITL includes Cygwin (https://cygwin.com/licensing.html), '
        'FFmpeg (https://ffmpeg.org/legal.html), PyQt6/Qt, VTK, MAVProxy and their dependencies.\n'
        'Source and build instructions: https://github.com/ArduPilot/AP_CameraGimbal\n'
        'The CI source artifact contains the project and pinned C/C++ dependency sources.\n')


if __name__ == '__main__':
    main()
