# Lossless thermal stream (experimental v1)

MT11 and SITL-MT11 expose `http://CAMERA:8556/thermal.mkv` in addition to the
existing display streams. This carries **640x512 gray16le FFV1 level 3** video
in streaming Matroska, with slice CRCs and independently decodable frames.
There is no palette, resizing, rotation, overlay or sample truncation in the
radiometric path. The complete USB radiometric word is retained: unsigned
16-bit Kelvin multiplied by 64 (`Celsius = sample / 64 - 273.15`). This describes
the MT11 sensor's output format; it does not assert its internal ADC precision.

Native ZT30 also exposes this transport, preserving its 16-bit raw counts.
Its temperature conversion is not calibrated: `temperature_calibration` is
`unverified_sensor_counts`, and `temperature_scale_k`, `temperature_offset_k`, `minimum_c`
and `maximum_c` are JSON `null`. Never apply MT11's Kelvin conversion to these
counts. Calibrated MT11 frames report `temperature_calibration: "sensor_kelvin_64"`.
These optional/nullable fields extend `apcg.thermal.v1`; consumers must accept
unknown calibration and null numeric values before displaying temperatures.

`VIDEO_STREAM_INFORMATION` advertises stream **3**, named **Raw Thermal
(16-bit)**, thermal flag, encoding UNKNOWN (FFV1 identified inside Matroska),
and **experimental type 200** for HTTP Matroska. Type 200 is a private agreement
between this firmware and MAVProxy, **not an upstream MAVLink allocation**.
It needs an upstream stream-type/encoding proposal before interoperable release.
Display streams 1 and 2 retain their RTSP types and normal behaviour. The legacy
one-frame-per-TCP-connection service on port 7345 is unchanged. The raw stream
is available directly and through an updated SupportProxy. Configure
`[support_proxy] video3_port` (`PROXY_VID3_PORT`, default 0) and optionally
`video3_name`. Proxy routes advertise `http://PROXY:PORT/v3.mkv`; a disabled
proxy stream has an empty URI and no running flag. The proxy preserves the
Matroska bytes, native samples and metadata, with the same `RAW_STREAM_FPS`.
The proxy publisher is independent of the local HTTP listener (which may be
disabled using `CAMERA_APP_RAW_THERMAL_PORT=0`).

MAVProxy's `camera view rawthermal` works through this route. SupportProxy's
`scripts/view_raw_thermal.py` provides a standalone greyscale viewer with
optional palettes, temperature readout and native frame/metadata saves.

## Transport and per-frame metadata

Each HTTP connection receives an EBML header, an unknown-length Matroska
Segment, one FFV1 track with CodecPrivate configuration, and one Cluster per
frame. TimestampScale is 1 ms. Cluster timestamps derive from local monotonic
capture time relative to the encoder session. There are no dependent frames,
so clients may join or drop frames without losing decoder state.

Each video BlockGroup contains the same frame's UTF-8 JSON in BlockAdditional
ID 2. Its track BlockAdditionMapping specifies the **experimental FourCC APCG
(0x41504347)** and name `apcg.thermal.v1 JSON`. FFmpeg resolves this mapping,
so `AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL` has an eight-byte big-endian **APCG**
type followed by the JSON (rather than the on-wire BlockAddID 2). Generic players
can show FFV1, but applications must retain this side data to preserve georeferencing.
Remuxing with a generic tool may discard the custom mapping/metadata.

The `apcg.thermal.v1` JSON contains:

| Field | Meaning |
| --- | --- |
| `frame_id` | Sequence of received sensor frames; gaps are normal at reduced streaming rate |
| `capture_monotonic_us` | Camera monotonic receive timestamp, microseconds |
| `simulation_source` | `terrain` or `test_pattern` in SITL; empty on hardware |
| `timestamp_source`, `simulated` | `usb_receive` on hardware; `simulation`, true in SITL |
| `width`, `height`, `pixel_format`, `bits_per_sample` | 640, 512, gray16le, 16 |
| `temperature_calibration` | `sensor_kelvin_64` for calibrated MT11; `unverified_sensor_counts` for uncalibrated sources |
| `temperature_scale_k`, `temperature_offset_k` | Kelvin = raw * scale + offset; 1/64, 0 for MT11, otherwise null |
| `gain` | MT11 gain value (0 low, 1 high); -1 until known |
| `minimum_raw`, `maximum_raw`, `minimum_c`, `maximum_c` | Actual raw extrema; Celsius extrema are null without temperature calibration |
| `rotation_deg` | Rotation to upright display, 0 or 180; native pixels remain untouched |
| `hfov_deg`, `calibration` | Native thermal nominal horizontal FOV and `nominal_pinhole`; unknown geometry is `null` with calibration `unknown` |
| `altitude_datum`, `gimbal_frame` | AMSL; `roll_pitch_level_yaw_vehicle` |
| `telemetry` | Existing `apcg.telemetry.v1` capture snapshot, including UTC microseconds, position, NED velocity, vehicle and gimbal attitudes, yaw rates and individual source ages |

Missing telemetry stays null and source ages are retained. The snapshot is taken
when the frame callback publishes pixels, before compression or network delivery.
Hardware timestamps describe completed USB frame reception, not sensor exposure;
unknown USB/exposure latency remains. UTC follows the camera clock. Gimbal roll
and pitch are level-referenced and yaw is vehicle-relative. The nested video
telemetry's `zoom`/`hfov_deg` may describe RGB: use the **outer thermal HFOV**.
MAVProxy rejects projection when any required source is missing or older than
250 ms, and extrapolates position and known yaw rates over their reported ages.
Roll/pitch interpolation, lens distortion, boresight calibration, terrain error
and sensor exposure latency are not corrected. Metadata supports approximate
map projection, not surveyed georeferencing accuracy.

## Streaming and recording rates

Both rates are persistent, live settings in the web **Parameters → Video** tab
and in MAVLink `PARAM_*` and `PARAM_EXT_*` (including camera-definition XML):

| Parameter | INI key in `[thermal]` | Range | Default |
| --- | --- | --- | --- |
| `RAW_STREAM_FPS` | `stream_fps` | 1–25 fps | 5 |
| `RAW_RECORD_FPS` | `record_fps` | 0–25 fps; 0 disables raw recording | 5 |

They replace the earlier `CAMERA_APP_RAW_THERMAL_FPS` environment setting and
apply without restarting. These are sampling-rate targets, subject to sensor,
encoder and storage throughput; the hardware streaming tests used 5 fps.

Raw recording follows the existing video recording state: manual start/stop
(including MAVLink and web controls), and `REC_AUTOSTART` / `[recording] autorecord`
(`false`, `true`, `while_armed`). Thus existing installations with automatic video
recording enabled also record raw thermal at the default 5 fps. Set
`RAW_RECORD_FPS=0` to retain display-video-only recording. While Armed uses the
same selected-flight-controller heartbeat and disarm behavior as ordinary video.

Files are named `<RGB video stem>.raw.mkv` alongside the MP4 recordings, normally
under `/mnt/DCIM/record/YYYY-MM-DD/`. Each contains full-depth FFV1 pixels and the
same per-frame metadata as the stream. Recording needs no connected viewer.
Changing a nonzero rate applies within the current file; zero closes the raw
file while ordinary video continues. Re-enabling during that video session
creates `.raw-1.mkv`, etc., without overwriting prior data. Downloads snapshot
complete Matroska clusters; stopping recording flushes and closes the file.
A raw storage-write error closes that raw file and logs the error. Earlier
complete frames remain readable; ordinary video retains its own error handling.

`CAMERA_APP_RAW_THERMAL_PORT` defaults to RTSP port + 2; zero disables the HTTP
endpoint and its advertisement, while raw recording remains available.
`VIDEO_START_STREAMING`/`VIDEO_STOP_STREAMING` for ID 3 enables/disables streaming,
and ID 0 includes it; these commands do not stop recording. Both outputs share
an encoder, with independent schedules and a cached intra frame. Encoding runs
only when recording or a ready streaming client needs a frame. Four connections
share the stream; slow clients skip frames, retain at most one pending frame,
and disconnect after two seconds without send progress. Incomplete HTTP requests
also time out. Network backpressure does not gate the recording schedule.
Dropping frames never changes retained sensor samples.

Metadata timestamps (`capture_monotonic_us` and `telemetry.utc_us`) have **1 µs
resolution**; Matroska playback timestamps have **1 ms resolution**. These mark
USB receive completion / callback publication, not the sensor's exposure time.
Sensor frames arrive about 40 ms apart at 25 Hz. Clock synchronization and USB
latency limit absolute timing accuracy independently of the timestamp resolution.

## Build and test

```sh
make sitl
make -C camera_app all thermal-codec-bench
python3 -m pip install 'av>=18.1'
python3 sitl/test_raw_thermal_stream.py --mavproxy /path/to/MAVProxy --viewer
```

The final command starts isolated SITL processes and exercises MAVLink discovery,
all 16 bits, metadata pairing, reconnects, multiple readers, incomplete requests,
start/stop, ordinary RTSP video, independent recording rates, arm/disarm, manual
and automatic recording, live parameter/INI changes, lossless recorded samples,
and optionally the wx viewer. Omit `--viewer`
without a desktop. It stops only its own processes. Simple test-pattern mode
uses all 65536 codes every frame. Add `--terrain` for an offline integration test
using textured terrain meshes, with decoded samples checked against the renderer.

In **3D terrain and imagery** mode, the raw stream and thermal display video
share a native 640x512 rendered image and predicted camera pose. Imagery luminance
maps to synthetic 15–45°C Kelvin×64 samples. Display gain, palettes, resizing and
overlays apply only to the display encodings. This is a visual simulation, not a
physical temperature model. The source frame rate follows the terrain renderer;
recording and streaming rates remain independently limited by their parameters.
Terrain pixels are already upright (`rotation_deg: 0`). Their metadata carries
the rendered position and orientation, presentation time, and original telemetry
ages in `telemetry.render_source_age_ms`, frozen with the pixels before queuing.
If pose telemetry is missing or stale, raw frames are uniform with a null pose.

The build downloads a SHA-256-pinned FFmpeg 8.0.1 source release and builds minimal
static libavcodec/libavutil under `build/deps/thermal-codecs`. No external encoder
process runs on the camera. FFmpeg is built under LGPL-2.1-or-later, without GPL
or nonfree options; its license is copied beside each installation and the source
and build recipe are retained for rebuilding/relinking.

MAVProxy needs PyAV with packet side-data access (18.1 tested), NumPy, OpenCV and
wxPython. In the requested MAVProxy checkout, load `camera`, then select
**Camera → Video → Raw Thermal (16-bit)** or run `camera view rawthermal`.
The text panel below the image shows labelled minimum/maximum temperatures and
the hovered pixel's temperature in a larger font. The hover reading follows the
same pixel as new frames arrive. The popup menu pauses or saves the native `.bin` plus its
`.json` under the MAVProxy log directory's `raw_thermal/`. A paused left click
prints approximate terrain coordinates if the terrain module and fresh pose
are available. The viewer defaults to white-hot greyscale, scaled to each
frame's minimum and maximum. Its **Palette** popup menu offers Greyscale,
Inferno and Turbo, including while paused. This is independent of the camera's
RTSP thermal palette. Palette and display rotation only affect the screen copy.

## Hardware testing

The [2026-09-21 MT11 hardware test](../docs/mt11-raw-thermal-test.md) passed
lossless sample preservation, discovery, telemetry, GUI and concurrent video
checks at the default 5 Hz.

Built artifacts are `camera_app/camera-app` and
`camera_app/build/mt11/thermal-codec-bench`, both static AArch64 executables.
Installing camera-app uses the existing normal deployment workflow; the benchmark
can run separately without taking ownership of the camera hardware. It encodes
and decodes 100 frames, checks every sample, and reports latency, size, CPU and RSS:

```sh
./thermal-codec-bench                     # all-code test pattern
./thermal-codec-bench saved-frame.bin     # representative real sensor frame
```

After the new app is running, capture on the host:

```sh
python3 tools/raw_thermal_probe.py http://CAMERA:8556/thermal.mkv \
    --mavproxy /path/to/MAVProxy --frames 100 --output /tmp/mt11-thermal
```

This validates geometry, range/metadata pairing and monotonic timestamps and
saves native samples plus matching metadata. Add `--test-pattern` only for SITL.
Use real frames for meaningful compression and CPU measurements: the all-code
ramp is deliberately easy to compress. Check normal RGB/thermal video and gimbal
control while streaming before increasing the raw rate toward 25 Hz. No camera
installation or hardware test is performed by the build or SITL test commands.

## Converting older raw captures

`tools/thermal_to_video.py CAPTURE_DIRECTORY OUTPUT.mkv` converts the old
640x512 `_I.bin` files to this FFV1/Matroska format with original filenames,
timestamps and per-frame SHA-256 checksums. Reverse it with
`tools/thermal_to_video.py --extract INPUT.mkv OUTPUT_DIRECTORY`.
Extraction checks every archived frame's checksum and restores its name and
modification time. Old captures carry no vehicle/gimbal telemetry; reconstructed
relative timing is explicitly labelled. Add `--bin FLIGHT.bin` to reconstruct
each frame's `apcg.telemetry.v1` pose from an ArduPilot dataflash log, matched by
absolute UTC (needs `pymavlink`). See the
[converter options and timing semantics](../tools/README.md#legacy-thermal-directories-to-lossless-video).

## Interrupted recordings

The live Matroska layout does not require a closing index or footer. Complete
FFV1 Clusters remain playable after an interrupted write; a truncated final
frame may be discarded or reported as an end-of-file error. Cuts inside the
header and payload of a later frame in an actual MT11 file were tested with
PyAV: the preceding complete frames still decoded as gray16le.

This is a container property, not a power-loss guarantee for the filesystem.
The camera calls `fdatasync` at recording stop; unsynchronised kernel/card
buffers can be lost if power is removed. Stopping before power-off remains
safest. The amount of lost tail data is not bounded by this format.
