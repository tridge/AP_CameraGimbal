#!/usr/bin/env python3
"""Independent calibration vectors and consistency of the exported build data."""
import configparser
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from sitl.target_properties import TARGETS, transform
from windows.test_package import check_video_size

assert set(TARGETS) == {'mt11', 'a8', 'zr10', 'z1mini', 'rpi_libcam_caddx', 'zt30'}
recording_defaults = {
    'mt11': '3840x2160',
    'zt30': '3840x2160',
    'a8': '3840x2160',
    'zr10': '2560x1440',
    'z1mini': '1920x1080',
    'rpi_libcam_caddx': '1920x1080',
}
resolutions = ('1280x720', '1920x1080', '3840x2160', '2560x1440', '960x540', '854x480',
               '640x360', '426x240', '320x180')
with tempfile.TemporaryDirectory(prefix='apcam-target-test-') as temp:
    for name, target in TARGETS.items():
        binary = str(Path(temp) / name)
        subprocess.run(['c++', '-std=gnu++17', '-Wno-missing-field-initializers', '-Wall', '-Wextra', '-Werror', '-std=gnu++17',
                        '-DAPCAM_TARGET=' + target['target_id'],
                        '-I' + str(ROOT/'include'), '-I' + str(ROOT/'camera_app/include'),
                        str(ROOT/'tests/test_targets.cpp'), str(ROOT/'camera_app/src/config.cpp'), '-lm', '-o', binary], check=True)
        subprocess.run([binary], check=True)
        for role in ('main', 'sub', 'recording'):
            assert target[role + '_resolutions'] & (1 << target['default_' + role + '_resolution'])
        # Fresh firmware installs and SITL use these INI templates; missing
        # settings use the compiled defaults exported into TARGETS instead.
        templates = [ROOT / ('camera_app/camera.ini' if name == 'mt11'
                             else f'packaging/{name}/camera.ini')]
        # ZR10 uses a separate template in the Linux and Windows simulators.
        if name == 'zr10':
            templates.append(ROOT / 'sitl/zr10.ini')
        if name == 'zt30':
            # Development defaults use a different, conservative resolution.
            dev = configparser.ConfigParser()
            assert dev.read(ROOT / 'packaging/zt30/camera.ini.dev')
            assert dev['video']['telemetry'] == 'misb'
        for template in templates:
            config = configparser.ConfigParser()
            assert config.read(template), template
            assert config['video']['telemetry'] == 'misb', template
            assert config['mavlink']['tracking_method'] == 'rate', template
            assert config['mavlink']['tcp_port'] == config['mavlink']['udp_port'] == '16001', template
            roles = [('main', 'stream.main'), ('sub', 'stream.sub'), ('recording', 'recording')]
            if int(target['num_streams']) < 2:
                roles.remove(('sub', 'stream.sub'))
            for role, section in roles:
                compiled = resolutions[target['default_' + role + '_resolution']]
                assert config[section]['resolution'] == compiled, (template, section)
            assert config['recording']['resolution'] == recording_defaults[name], template
        for lens in range(1, int(target['num_lenses']) + 1):
            fov = target[f'lens{lens}_fov_h']
            assert 0 < fov < 180 or (name == 'zt30' and lens == 3 and fov == 0)
        for channel in ('feedback', 'private_feedback', 'angle_command', 'rate_command'):
            for inverted in (False, True):
                prefix = 'gimbal_' + channel + ('_inverted' if inverted else '_upright')
                matrix = target[prefix + '_matrix']
                for i in range(3):
                    for j in range(3):
                        assert sum(matrix[3*i+k] * matrix[3*j+k] for k in range(3)) == (i == j)
                for rates in (False, True):
                    vector = (12, -34, 56)
                    converted = transform(target, channel, inverted, vector, rates=rates)
                    recovered = transform(target, channel, inverted, converted, rates=rates, inverse=True)
                    assert list(vector) == recovered
print('PASS six-target compiler exports, profile defaults, orthogonal mappings and known calibration vectors')

# The Windows packaged self-test must accept each native thermal geometry
# for both live video and recordings, and reject the other camera's size.
for name,target in TARGETS.items():
    check_video_size(target,False,1280,720)
    if target['have_thermal']:
        check_video_size(target,True,target['thermal_stream_width'],target['thermal_stream_height'])
        wrong=(1280,720) if name=='zt30' else (640,512)
        try:
            check_video_size(target,True,*wrong)
        except AssertionError:
            pass
        else:
            raise AssertionError((name,'accepted wrong thermal stream geometry',wrong))
