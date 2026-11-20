#!/usr/bin/env python3
"""Capture and validate paired FFV1 radiometric frames from MT11 or SITL."""
import argparse
import json
from pathlib import Path
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('uri', help='http://CAMERA:8556/thermal.mkv')
    parser.add_argument('--mavproxy', type=Path, help='MAVProxy checkout containing the thermal reader')
    parser.add_argument('--frames', type=int, default=10)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--test-pattern', action='store_true', help='check every SITL sample bit')
    args = parser.parse_args()
    if args.frames < 1:
        parser.error('--frames must be positive')
    if args.mavproxy:
        sys.path.insert(0, str(args.mavproxy.resolve()))
    from MAVProxy.modules.mavproxy_camera.thermal_stream import ThermalReader, projection_pose
    import numpy as np
    args.output.mkdir(parents=True, exist_ok=True)
    reader = ThermalReader(args.uri)
    previous = 0
    started = time.monotonic()
    try:
        count = 0
        for pixels, metadata in reader.frames():
            if metadata['capture_monotonic_us'] <= previous:
                raise ValueError('non-increasing capture timestamp')
            previous = metadata['capture_monotonic_us']
            if int(pixels.min()) != metadata['minimum_raw'] or int(pixels.max()) != metadata['maximum_raw']:
                raise ValueError('pixel range does not match frame metadata')
            if args.test_pattern:
                expected = (np.arange(640*512, dtype=np.uint32) + metadata['frame_id']*257).astype(np.uint16).reshape(512,640)
                if not metadata.get('simulated') or not np.array_equal(pixels, expected):
                    raise ValueError('SITL pixels differ from the full-depth test pattern')
            stem = args.output / ('%u_%u' % (previous, metadata['frame_id']))
            stem.with_suffix('.bin').write_bytes(pixels.astype('<u2', copy=False).tobytes())
            stem.with_suffix('.json').write_text(json.dumps(metadata, indent=2)+'\n')
            if metadata.get('minimum_c') is None or metadata.get('maximum_c') is None:
                print('frame %u: %u..%u sensor counts (temperature conversion unverified), fresh projection pose: %s' % (
                    metadata['frame_id'], metadata['minimum_raw'], metadata['maximum_raw'], projection_pose(metadata) is not None))
            else:
                print('frame %u: %.3f..%.3f C, fresh projection pose: %s' % (
                    metadata['frame_id'], metadata['minimum_c'], metadata['maximum_c'], projection_pose(metadata) is not None))
            count += 1
            if count >= args.frames:
                break
        if count != args.frames:
            raise RuntimeError('stream ended after %u frames' % count)
        print('Saved %u native 16-bit frames and metadata in %.2f seconds: %s' % (
            count, time.monotonic()-started, args.output))
    finally:
        reader.close()


if __name__ == '__main__':
    main()
