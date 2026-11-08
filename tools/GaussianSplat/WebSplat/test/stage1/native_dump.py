#!/usr/bin/env python3
"""Native reference for stage 1: per-frame metadata, timestamps and raw pixel hashes of an APCG
thermal mkv, read with the native tools' own code (splat_common's PyAV reader).

Writes OUT/native.json (one record per video packet) and, with --raw, OUT/raw/NNNNN.u16
(gray16le) for the Node stage 2 test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

TOOLS = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(TOOLS))
from splat_common import decode_frames, read_mkv_metadata  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('mkv', type=Path)
    ap.add_argument('out', type=Path)
    ap.add_argument('--raw', action='store_true', help='also write the raw frames')
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    records = []
    for index, packet, meta in read_mkv_metadata(args.mkv):
        tb = packet.time_base
        records.append(dict(index=index, pts=packet.pts, time_base=[tb.numerator, tb.denominator],
                            size=packet.size, keyframe=bool(packet.is_keyframe), meta=meta))
    if args.raw:
        (args.out / 'raw').mkdir(exist_ok=True)
    for index, meta, raw in decode_frames(args.mkv, set(range(len(records)))):
        data = raw.astype('<u2').tobytes()
        records[index].update(width=raw.shape[1], height=raw.shape[0],
                              sha256=hashlib.sha256(data).hexdigest())
        if args.raw:
            (args.out / 'raw' / ('%05d.u16' % index)).write_bytes(data)
    (args.out / 'native.json').write_text(json.dumps(dict(source=args.mkv.name, frames=records), indent=1))
    print('%d frames' % len(records))


if __name__ == '__main__':
    main()
