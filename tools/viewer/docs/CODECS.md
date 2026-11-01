# Local software decoder

Native WebCodecs is the preferred route for supported H264 and HEVC configurations.
The software fallback uses the pinned single-thread `@ffmpeg/core` 0.12.10 in a
classic worker. Assets are served from `BASE_URL/vendor/ffmpeg/<pinned-version>/`; no CDN, server
video conversion, SharedArrayBuffer or cross-origin isolation is required.

The fallback does not call `writeFile` with a recording. Container indexing
provides compressed packet extents, PTS, keyframe flags and codec initialization.
Only the required group from a keyframe through the requested frame becomes a
small seekable Matroska Blob, mounted read-only with Emscripten WORKERFS. Annex-B
H264/HEVC packets become length-prefixed NALs when paired with avcC/hvcC metadata.
Each synthetic Cluster preserves packet PTS to microsecond precision. Original
source PTS remains on the returned frame; these timestamps are not assumed UTC.

Budgets are 180 compressed packets, 32 MiB compressed data, 1 MiB initialization,
and 16 megapixels per frame. Output is one gray16le or RGBA frame, removed from
MEMFS immediately after transfer. Unsupported dimensions, missing initialization,
missing preceding keyframes, excess budgets, and an unidentified requested PTS
fail explicitly. A decode or file replacement can terminate the worker; stale
work cannot replace a later frame. HEVC fallback is CPU intensive and does not
promise real-time playback at every resolution.

FFV1 gray16 sources return a numeric Uint16Array without rescaling or palette
conversion. The decoder checks FFmpeg's input pixel format before labeling a
frame gray16. Calibrated temperatures additionally require valid thermal metadata;
a uint16 buffer alone does not establish Celsius. H264/HEVC output is display RGBA
and cannot supply calibrated pixel temperatures.

## Native playback

H264/HEVC configurations are probed once per track/initialization signature;
unsupported configurations go directly to the software fallback thereafter.
Supported WebCodecs configurations keep one incremental decoder across sequential
requests. A timestamp-keyed VideoFrame cache retains only nearby future output,
with conservative RGBA-sized accounting capped at 32 MiB. Actual output PTS selects
B-frames; decode-order indices are never treated as presentation order. Consumed,
evicted and cancelled VideoFrames are closed. A seek behind retained output or a
flush at the current index boundary starts another bounded keyframe group. Native
packet submission uses the same 180-packet / 32 MiB per-request limits, and gives a
clear error/fallback when no nearby keyframe exists rather than accumulating an
unbounded GOP. Cancellation and file replacement close the active decoder.

Visible Chrome decoded 90 sequential frames of the real 1080p H264 TS with median
9.4 ms and mean 13.6 ms decode/conversion time. The deliberately truncated
96-frame index caused three decoder configurations and 198 packet submissions;
normal incremental requests reused the decoder rather than replaying a whole
GOP. A generated H264 clip with three B-frames decoded all 30 presentation frames
using one configuration and 30 packet reads, with 15 cache hits. These measurements
exclude the rest of the review UI and do not guarantee a particular display rate.

## Qualification evidence, 1 November 2026

Visible Chrome in Xephyr decoded the actual 2,759,203,562-byte flight MKV at PTS
0, 3000.554 and 7000.841 s. All 655,360 output bytes of every frame matched native
FFmpeg SHA-256 references. The last compressed packet begins at source offset
2,670,824,239, beyond 2 GiB. The application indexed-packet decoder read only
144,979, 156,373 and 150,448 compressed bytes respectively. Cold decode including
WASM load took about 205 ms; warm late seeks took about 40 ms. The standalone
WORKERFS proof kept the WASM heap at 32 MiB and input read requests below 204 KiB.
The real container parser plus application decoder also matched all bytes of the
first three thermal frames before completing the recording's telemetry index.
A separate full parser qualification indexed 16,080 frames through the late
target and decoded PTS 0, 3000.554 and 7000.841 s with the same exact hashes; its
largest Blob.arrayBuffer read was 156,373 bytes, including the index scan.

Directly mounting the complete File also decoded exact late frames with bounded
memory, but FFmpeg sought this uncued MKV by scanning about 1.17/2.67 GB for the
3000/7000 s targets (roughly 6/14 seconds). That route is therefore not used for
interactive seeking. The official FFmpeg.wasm FAQ currently states a general
2 GB input limit; the measured File-backed result is narrower evidence for this
pinned core and test recording, rather than a promise for arbitrary large files.

Real 1920x1080 H264 TS packets, including Annex-B conversion, decoded correctly
at three tested PTS positions. Warm software GOP decodes took about 183–255 ms;
4,096 sampled RGBA bytes differed from native FFmpeg by at most 1/255. Real
1920x1080 HEVC MP4 frames 0, 6 and 15 decoded with correct requested PTS in about
250–410 ms; sampled RGBA differed by at most 2/255. Display conversion can differ
slightly between FFmpeg/swscale versions; only gray16 claims byte equality.
The real 3840x2160 HEVC Z recording also decoded frames 0 and 6: about 626 ms
including startup and 1,243 ms for a seven-packet group, with sampled RGBA error
at most 2/255. This is functional fallback, not real-time 4K software playback.

The tests recorded local GET requests for application/codec assets and no file
upload requests. Proof JSON, native-reference hashes, fixture metadata and
screenshots are retained outside the repository under
`/data/review/web-viewer/codec-proof/`.

To repeat using a dedicated Xephyr display and the Vite dev server:

```
DISPLAY=:99 VIEWER_URL=http://127.0.0.1:5173/viewer/ \
  node scripts/qualify-codec.mjs /path/to/recording /path/to/references.json /tmp/codec-result.json
```

Reference JSON has `frames` entries containing original `pts` seconds and native
rawvideo SHA-256 as `expected`. Display references may also provide
`pixelSampleOffsets` and `pixelSampleValues` for a conversion tolerance check.
The helper rejects whole/large Blob reads, off-origin requests and non-GET
requests. It scans only far enough to locate requested frames; late qualification
may require indexing most of the file. Use `CHROME` to choose the browser binary;
`CHROME_NO_SANDBOX=1` is available only where the test environment requires it.

Upstream documentation:

- https://ffmpegwasm.netlify.app/docs/faq/
- https://github.com/ffmpegwasm/ffmpeg.wasm/blob/main/packages/ffmpeg/src/worker.ts
- https://github.com/emscripten-core/emscripten/blob/main/src/lib/libworkerfs.js

The actual Vite production build was served at a `/viewer/` prefix and opened the
real thermal recording in visible Chrome. It loaded the hashed classic software
worker and same-origin vendor core assets, then decoded a 640x512 thermal frame
in about 194 ms. With the current self-contained worker, `worker.format: 'es'`
emitted no module-only syntax and did not prevent classic `importScripts`; this
was tested against built assets rather than inferred from the Vite dev server.
