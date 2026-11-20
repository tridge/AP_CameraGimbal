# MT11 FFV1 hardware test — 2026-09-21

## Recording and live rate parameter update

The later build adds `RAW_STREAM_FPS` (1–25) and `RAW_RECORD_FPS` (0–25),
both defaulting to 5, in MAVLink and web Parameters. Raw recording follows the
existing video recording policy, with zero disabling only raw recording.
Installed camera-app SHA-256:
`98b19e5b2d22be09a47a337538df3146a74cca80f695260e972ce59ff8e00ed1`.
Installed web SHA-256:
`453b24a55287c6c736a8cc6b6b9b3f60a79a38c39f11eaf9c7a3a194c160bac9`.
The prior binaries and configuration are in `/mnt/raw-rate-update-20260921/`.

- The MT11's existing automatic recording setting started `.raw.mkv` beside
  the ordinary RGB and thermal MP4s after the update.
- Live MAVLink settings of 2 fps streaming and 3 fps recording measured
  **1.9948 fps** and **2.9791 fps**, respectively. The downloaded SD recording
  decoded successfully and every frame's extrema matched its metadata.
  These independent schedules selected different frames in this interval;
  this run does not constitute a stream-versus-file pixel equality test.
- Disabling raw recording finalized the file without stopping ordinary video;
  re-enabling opened a fresh numbered raw file. Both rates were restored to 5.
- The hardware web page exposes both rate fields. Web integration tests cover
  saving them, and SITL tests cover applying them through INI reload.
- SITL verified 1 fps live streaming, independent rates, lossless recorded
  samples, recording with streaming stopped, slow clients, live rate changes,
  manual recording, and disabled/enabled/while-armed automatic policies.
  Config validation/persistence, both MAVLink parameter protocols, and the
  existing armed-recording suite passed. The latter waits for actual video
  packets before disarming, avoiding empty files on a busy host.

Evidence: `/data/review/raw-rate-hardware-20260921/results.json` and
`hardware.raw.mkv`; `/data/review/raw-recording-sitl-final/` and
`/data/review/raw-recording-armed-final/`. Corresponding logs are under
`/data/buildlogs/raw-rate-hardware-20260921-final.log`,
`raw-recording-sitl-final.log`, and `raw-recording-armed-final.log`.
Higher hardware rates and SD power-loss behavior were not tested in this update.

## Initial streaming build

The `pr-raw-thermal-stream` worktree build was installed on a real MT11 using
an atomic replacement of `/app/bin/camera-app`, followed by the web supervisor's
normal restart. Configuration was preserved, including automatic RGB and
thermal recording. The previous executable and configuration were backed up
under `/mnt/raw-thermal-test-20260921/` on the camera's SD card.

Installed executable SHA-256:
`6d3b31235d8cf149aae2d0aacfb421ab77acb87a25d737beca0939bd66bd5a01`.

## Results

- MAVLink advertised the existing 1920x1080/30 RGB and 1280x720/25 thermal
  RTSP streams, plus stream 3: 640x512/5 FFV1/Matroska, private type 200.
- Sixty consecutive decoded radiometric frames had matching frame extrema,
  increasing sensor sequence/timestamps, native gray16le samples and USB
  receive timestamps. The measured rate was **4.89 frames/s**.
- Mean encoded size was **101,306 bytes/frame**, maximum 104,338: approximately
  **4.0 Mbit/s** for this scene. Compression depends on scene and sensor noise.
- Both ordinary RTSP streams decoded concurrently without FFmpeg errors.
  Existing RGB/thermal recording files continued to grow.
- Reconnects, concurrent raw clients and MAVLink raw stop/start passed; the
  ordinary stream's running state remained enabled.
- The legacy port-7345 service still returned its original header and raw frame.
  Sampling 100 FFV1 and 31 legacy frames found one shared USB capture timestamp;
  **all 655,360 decoded bytes matched the original legacy raw frame exactly**.
- The actual MAVProxy wx viewer opened, displayed pixel temperatures and saved
  native raw samples plus matching metadata.
- After the user's normal telemetry feed connected, all 20 sampled frames had
  fresh position, velocity, vehicle attitude and gimbal attitude. Position ages
  were 17–82 ms, velocity/vehicle attitude 18–83 ms, and gimbal attitude 7–90 ms.
  Every frame passed the reader's capture-pose freshness validation. No synthetic
  vehicle state was required. This validates metadata transport, not geographic
  accuracy of an image projected onto surveyed ground.

The camera initially booted with a 1970 clock; it was synchronized to host UTC
before the timed stream tests. Missing vehicle telemetry was correctly encoded
as null before the telemetry feed connected.

## On-camera codec benchmark

The static AArch64 probe encoded and decoded 100 frames per input and checked
every sample:

| Input | Bit-exact frames | Encode median / p95 / maximum | Mean encoded size |
| --- | --- | --- | --- |
| All-65536-codes pattern | 100/100 | 14.13 / 17.52 / 26.18 ms | 7,470 bytes |
| Real captured sensor frame | 100/100 | 28.63 / 30.36 / 33.29 ms | 100,281 bytes |

The real frame compressed by 6.54:1. On-camera decoding averaged 50.21 ms;
normal camera streaming only encodes, with decoding on the ground station.

During the streaming test, camera-app CPU rose from about 59% to 87% of one
core (four cores total), with total process RSS around 34 MiB. This comparison
also includes the two additional RTSP receivers, ongoing recording and the
configured support proxy; it is not an isolated encoder CPU measurement.

The sampled sensor frame's two least significant bits were zero, while bits
2 and 3 varied. The stream preserves the entire 16-bit output word. This
observation does not establish the sensor ADC's internal precision.

Only the default 5 Hz streaming cap was validated. Sustained 25 Hz operation,
calibrated exposure latency and surveyed map projection remain untested.

Local test scripts, raw frames, per-frame metadata and detailed results:
`/data/review/mt11-thermal-hardware-20260921/`. Logs are in
`/data/buildlogs/mt11-thermal-*.log` on the test host.
