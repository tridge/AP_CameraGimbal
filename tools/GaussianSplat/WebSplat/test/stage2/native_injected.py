#!/usr/bin/env python3
"""Run native extract_frames.py with test injections, so cases the canonical orbit lacks can be
compared with native: INJECT is JSON {"blank": [frame indices], "meta": {index: {"a.b": value}}}.
Blank frames are flat 18000 with pixel 777 at 30000 (as the Node test decoder makes them);
a meta value "__delete__" removes the key. Usage: native_injected.py INJECT extract_frames args...
"""
import copy
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
import extract_frames  # noqa: E402

spec = json.loads(sys.argv[1])
blank = set(spec.get('blank', []))
patches = {int(k): v for k, v in spec.get('meta', {}).items()}


def patch(index, meta):
    if index not in patches:
        return meta
    meta = copy.deepcopy(meta)
    for key, value in patches[index].items():
        *parents, last = key.split('.')
        d = meta
        for p in parents:
            d = d[p]
        if value == '__delete__':
            d.pop(last, None)
        else:
            d[last] = value
    return meta


read, decode = extract_frames.read_mkv_metadata, extract_frames.decode_frames


def read_mkv_metadata(path):
    for i, packet, meta in read(path):
        yield i, packet, patch(i, meta)


def decode_frames(path, wanted):
    for i, meta, raw in decode(path, wanted):
        if i in blank:
            raw = raw.copy()
            raw[:] = 18000
            raw.flat[777] = 30000
        yield i, patch(i, meta), raw


extract_frames.read_mkv_metadata = read_mkv_metadata
extract_frames.decode_frames = decode_frames
sys.argv = ['extract_frames.py'] + sys.argv[2:]
extract_frames.main()
