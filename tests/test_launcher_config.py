#!/usr/bin/env python3
"""Check fresh launcher ports and retention of user-chosen listener settings."""
import configparser
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sitl.launcher_config import defaults, initial_config, mavlink_settings

for instance, port in enumerate([16001, 16011, 16021, 16031], 1):
    expected = {'tcp_port': port, 'udp_port': port, 'camera_component_id': 99 + instance}
    assert defaults(instance) == expected
    template = '[mavlink]\ntcp_port=14550\nudp_port=14570\ncamera_component_id=100\ntracking_method=angle\n'
    parser = configparser.ConfigParser()
    parser.read_string(initial_config(template, instance))
    assert {k: parser.getint('mavlink', k) for k in expected} == expected
    assert parser['mavlink']['tracking_method'] == 'angle'
for invalid in [0, 5]:
    try:
        defaults(invalid)
    except ValueError:
        pass
    else:
        raise AssertionError(invalid)
with tempfile.TemporaryDirectory() as temp:
    build = Path(temp)
    assert mavlink_settings(build, 1) == defaults(1)
    path = build / 'runtime/app/camera.ini'
    path.parent.mkdir(parents=True)
    saved = '[mavlink]\ntcp_port=14550\nudp_port=0\ncamera_component_id=104\ntracking_method=angle\n'
    path.write_text(saved)
    assert mavlink_settings(build, 1) == {'tcp_port': 14550, 'udp_port': 0, 'camera_component_id': 104}
    assert mavlink_settings(build, 1, reset=True) == defaults(1)
    assert path.read_text() == saved
print('PASS fresh launcher ports, distinct instances, saved ports/disabled UDP and explicit reset')
