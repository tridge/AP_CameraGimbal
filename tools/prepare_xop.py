#!/usr/bin/env python3
"""Prepare pinned XOP sources, with native or non-Linux POSIX sockets."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--native', action='store_true', help='retain Linux epoll/socket support')
parser.add_argument('source', type=Path)
parser.add_argument('destination', type=Path)
args = parser.parse_args()
source, destination = args.source, args.destination
destination.mkdir(parents=True, exist_ok=True)
# Patch a complete temporary copy. A failed patch must never publish a partial
# source file, and unchanged output keeps its mtime for incremental builds.
with tempfile.TemporaryDirectory(prefix='.xop-', dir=destination.parent) as directory:
    staging = Path(directory)
    for original in source.rglob('*'):
        if not original.is_file():
            continue
        path = staging / original.relative_to(source)
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.suffix in ('.h', '.cpp'):
            path.write_text(original.read_text())
        else:
            path.write_bytes(original.read_bytes())
    subprocess.run(['patch', '-s', '-t', '-N', '-p1', '-d', str(staging), '-i',
                    str(Path(__file__).with_name('xop_warnings.patch').resolve())], check=True)
    for path in staging.rglob('*'):
        if not path.is_file():
            continue
        if not args.native and path.suffix in ('.h', '.cpp'):
            text = path.read_text()
            text = text.replace('defined(__linux) || defined(__linux__)',
                                'defined(__linux) || defined(__linux__) || defined(__CYGWIN__) || defined(__APPLE__)')
            if path.name in ('EpollTaskScheduler.h', 'EpollTaskScheduler.cpp'):
                text = text.replace(' || defined(__CYGWIN__) || defined(__APPLE__)', '')
            if path.name == 'EventLoop.cpp':
                text = text.replace('new EpollTaskScheduler(n)', 'new SelectTaskScheduler(n)')
            if path.name == 'Socket.h':
                for header in ('netinet/ether.h', 'netpacket/packet.h', 'net/ethernet.h', 'net/route.h'):
                    text = text.replace(f'#include <{header}>', '')
            path.write_text(text)
        output = destination / path.relative_to(staging)
        output.parent.mkdir(parents=True, exist_ok=True)
        if not output.exists() or output.read_bytes() != path.read_bytes():
            os.replace(path, output)
