#!/usr/bin/env python3
"""Non-keyframe FFV1 fixture: a small gray16 Matroska with GOPs of 8 (FFV1 level 3 keeps its
context state across non-key frames), and the native reference decode of every frame, decoded
in order with PyAV. Writes OUT/gop.mkv and OUT/gop.json."""
import hashlib
import json
from fractions import Fraction
from pathlib import Path
import sys

import av
import numpy as np

W, H, N, GOP = 96, 64, 40, 8


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    path = out / 'gop.mkv'
    rng = np.random.default_rng(3)
    with av.open(str(path), 'w', format='matroska') as c:
        s = c.add_stream('ffv1', rate=10)
        s.width, s.height, s.pix_fmt = W, H, 'gray16le'
        s.time_base = Fraction(1, 1000)
        s.codec_context.gop_size = GOP
        s.codec_context.options = {'level': '3', 'coder': '1', 'slicecrc': '1'}
        y, x = np.mgrid[0:H, 0:W]
        for i in range(N):
            img = (18000 + 20 * x + 15 * y + 300 * np.sin(i / 3 + x / 9) + rng.integers(0, 40, (H, W)))
            frame = av.VideoFrame.from_ndarray(img.astype(np.uint16), format='gray16le')
            frame.pts = i * 100
            for p in s.encode(frame):
                c.mux(p)
        for p in s.encode():
            c.mux(p)
    frames = []
    with av.open(str(path)) as c:
        st = c.streams.video[0]
        for p in c.demux(st):
            if p.size == 0:
                continue
            frames.append(dict(pts=p.pts, keyframe=bool(p.is_keyframe)))
            for f in p.decode():
                frames[-1]['sha256'] = hashlib.sha256(f.to_ndarray(format='gray16le').astype('<u2').tobytes()).hexdigest()
    assert len(frames) == N and all('sha256' in f for f in frames)
    keys = [i for i, f in enumerate(frames) if f['keyframe']]
    assert keys == list(range(0, N, GOP)), keys
    (out / 'gop.json').write_text(json.dumps(dict(width=W, height=H, gop=GOP, frames=frames), indent=1))
    print('%d frames, keyframes %s' % (N, keys))


if __name__ == '__main__':
    main()
