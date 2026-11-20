#!/usr/bin/env python3
"""Compare a WebSplat stage 1 index (and frame hashes) with native_dump.py's native.json."""
import json
import sys


def same(native, web, where, errors):
    # JS keeps integers beyond 2^53 as decimal strings
    if isinstance(native, int) and not isinstance(native, bool) and isinstance(web, str):
        if str(native) != web:
            errors.append('%s: %r != %r' % (where, native, web))
    elif isinstance(native, dict) and isinstance(web, dict):
        if set(native) != set(web):
            errors.append('%s: keys %s' % (where, sorted(set(native) ^ set(web))))
        for k in native.keys() & web.keys():
            same(native[k], web[k], '%s.%s' % (where, k), errors)
    elif isinstance(native, list) and isinstance(web, list) and len(native) == len(web):
        for i, (a, b) in enumerate(zip(native, web)):
            same(a, b, '%s[%d]' % (where, i), errors)
    elif type(native) is not type(web) and not (isinstance(native, (int, float)) and isinstance(web, (int, float))
                                                  and not isinstance(native, bool) and not isinstance(web, bool)):
        errors.append('%s: type %s != %s' % (where, type(native).__name__, type(web).__name__))
    elif native != web:
        errors.append('%s: %r != %r' % (where, native, web))


def main():
    native = json.load(open(sys.argv[1]))['frames']
    web = json.load(open(sys.argv[2]))
    require = '--require-hashes' in sys.argv[3:]
    frames, hashes = web['index']['frames'], web.get('hashes')
    errors = []
    if require and not (isinstance(hashes, list) and len(hashes) == len(native) and
                        all(isinstance(h, str) and len(h) == 64 for h in hashes)):
        errors.append('expected exactly one SHA-256 per frame, got %s' %
                      (len(hashes) if isinstance(hashes, list) else hashes))
        hashes = None
    if (web['index'].get('format'), web['index'].get('version')) != ('websplat.ingest', 1):
        errors.append('not websplat.ingest v1')
    if len(frames) != len(native):
        errors.append('frame count %d != %d' % (len(frames), len(native)))
    for n, w in zip(native, frames):
        i = n['index']
        if w['index'] != i:
            errors.append('frame %d: index %d' % (i, w['index']))
        # native pts in time_base units; web in Matroska ticks of timescale_ns
        num, den = n['time_base']
        if w['pts'] * web['index']['track']['timescale_ns'] * den != n['pts'] * num * 10 ** 9:
            errors.append('frame %d: pts %r vs %r' % (i, w['pts'], n['pts']))
        if w['size'] != n['size'] or w['keyframe'] != n['keyframe']:
            errors.append('frame %d: size/keyframe' % i)
        same(n['meta'], w['meta'], 'frame %d meta' % i, errors)
        if hashes is not None and hashes[i] != n['sha256']:
            errors.append('frame %d: pixels differ' % i)
    for e in errors[:40]:
        print(e)
    print('%d frames compared (%s): %s' % (len(native), 'metadata, timestamps and pixels' if hashes
          else 'metadata and timestamps', 'FAIL, %d differences' % len(errors) if errors else 'identical'))
    sys.exit(1 if errors else 0)


if __name__ == '__main__':
    main()
