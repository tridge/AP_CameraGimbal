#!/usr/bin/env python3
"""Decode real BIN files and check SYS cadence, units and session reset."""
import math
from pathlib import Path
import subprocess
import sys
import tempfile
from pymavlink import mavutil

with tempfile.TemporaryDirectory(prefix='apcam-sys-') as tmp:
    subprocess.run([sys.argv[1], tmp], check=True, timeout=30)
    paths = sorted(Path(tmp).glob('*.BIN'))
    assert len(paths) == 2, paths
    for index, path in enumerate(paths):
        reader = mavutil.mavlink_connection(str(path))
        samples = []
        have_format = False
        while True:
            message = reader.recv_match()
            if message is None:
                break
            assert message.get_type() != 'BAD_DATA', message
            if message.get_type() == 'FMT' and message.Name == 'SYS':
                assert message.Format == 'QffQQQB', message
                have_format = True
            if message.get_type() == 'SYS':
                samples.append(message)
        reader.close()
        assert have_format
        assert len(samples) == (2 if index == 0 else 1), samples
        for sample in samples:
            assert sample.Valid == 30, sample # Host test has no camera sensor.
            assert math.isnan(sample.CPUTemp)
            assert 0 <= sample.CPULoad <= 100, sample
            assert sample.MemFree > 0 and sample.MemAvail > 0, sample
            assert sample.SDFree > 0, sample
            assert sample.MemFree % 1024 == 0
            assert sample.MemAvail % 1024 == 0
        if len(samples) > 1:
            interval = (samples[1].TimeUS - samples[0].TimeUS) * 1e-6
            assert 5 <= interval < 6, interval
    print('PASS decoded SYS layout, five-second cadence, validity, units and log restart')
