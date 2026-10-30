# AP_CameraGimbal camera application

`camera-app` provides camera and gimbal services for ArduPilot-compatible
payloads. The reusable SIYI and MAVLink services are separated from camera
backends; the MT11 is the first supported backend.

Current milestone:

- initial MT11, A8 mini and ZR10 UniGCS support: multicast discovery on UDP 37258, private
  control on TCP 37256, shared camera controls and private gimbal forwarding.
  RTSP supplies encoder parameters in SDP and periodic RTCP sender reports;
  H.265 video display has been confirmed in UniGCS against MT11 and A8 SITL.
  The service accepts MT11 v3 and A8 legacy CRC32 network framing.
  SITL emulates native gimbal rate and centre/look-down preset commands.
  See [UniGCS protocol coverage](../docs/unigcs.md) for implemented commands,
  SITL tests and remaining compatibility work;
- SIYI External SDK UDP and TCP service on port 37260, including CRC-16
  validation, fragmented/concatenated TCP framing and simultaneous clients;
- transport-aware response routing: immediate replies return to the requesting
  client, while delayed gimbal-UART replies are matched to pending requests by
  opcode and request order;
- MT11 private UART v3 framing, checksums, startup/shutdown exchange and
  `6b/16` public-protocol tunnelling;
- reply relay for gimbal mode, rate, absolute-angle and attitude commands used
  by MAVProxy and the ArduPilot Lua script;
- local SIYI `settime` (`0x30`) using the MAVProxy-compatible little-endian
  epoch-microseconds payload;
- asynchronous SIYI function feedback (`0x0b`) for photo success/failure,
  unsupported-HDR rejection and recording failure/start/stop, including
  last-result queries;
- an MT11 SS928 media backend running both IMX586 inputs with the preserved
  factory ISP profiles, configurable visible-camera CSC, exposure, metering
  and white-balance controls, and H.264/H.265 RTSP over UDP or interleaved TCP;
- direct acquisition of the MT11 `0bda:5830` thermal module's stacked
  640x1024 UVC bulk stream, retaining its 640x512 radiometric Y16 plane and
  serving the display plane as 1280x720/25 in the configured substream codec at
  `rtsp://CAMERA:8554/video2`, with runtime and persistent palette selection;
- one-shot and 5 Hz continuous SIYI full-frame thermal range replies (`0x14`),
  including live minimum/maximum temperatures and native pixel locations;
- MAVProxy-compatible thermal gain query/selection through SIYI `0x37`/`0x38`:
  mode `0` is low gain/high-temperature range and mode `1` is high
  gain/low-temperature range. The first readback can take several seconds
  while the module loads its gain-specific calibration;
- configurable capture for SIYI photo action `0x0c 00`. Thermal-only mode
  writes the latest complete 640x512 little-endian Y16 plane as a
  vendor-compatible `_I.bin`. All-lens mode additionally uses the hardware
  JPEG encoder to save configured-main-size wide `_C.jpg` and zoom `_Z.jpg`, and
  1280x720 thermal-display `_I.jpg` images. Files are placed beneath
  `/mnt/DCIM/capture/YYYY-MM-DD/` and the command returns `0x0b` feedback. The
  raw filename and mtime use the USB frame-completion timestamp (exFAT may
  round mtime below its supported precision);
- an in-process raw-thermal server on TCP 7345. Each connection receives a
  128-byte filename, little-endian `double` capture timestamp and the latest
  655,360-byte little-endian Y16 plane directly from USB capture memory. It
  deliberately performs no zlib compression and does not depend on an SD
  write. The MT11 MAVProxy client auto-detects this format and the vendor
  helper's legacy compressed format;
- an in-process, loopback-only native live-video server on TCP 8555. It takes
  the already encoded H.264 access units from `/video1` and `/video2`, emits a
  fresh fragmented-MP4 stream for each viewer, and adds no proxy daemon or
  transcoding. The authenticated web service is the only intended client;
  H.265 streams remain available over RTSP but are explicitly rejected by the
  browser transport;
- MAVProxy-compatible RGB slot negotiation, wide/zoom lens switching and
  end-stop-homed E5739 optical/hybrid zoom control through SIYI `0x05`,
  `0x0f`, `0x10`, `0x11`, `0x16` and `0x18`;
- thermal-main `(2,0)` image mode, which routes thermal in the configured main
  codec to `/video1`
  and the zoom RGB stream to `/video2`, including matching RTP clock rates;
- truthful rejection of unsupported SIYI HDR requests through `0x0b` HDR-off
  feedback, with `0x0a` always reporting HDR disabled;
- SIYI autofocus `0x04`, using the requested 1920x1080 video pixel (or image
  centre by default), the zoom sensor's live SS928 contrast grid and
  a factory-calibration-bounded E5739 focus search;
- SIYI manual focus `0x06`, moving continuously nearer/farther until stopped
  or until the current factory calibration row's safe endpoint is reached;
- SIYI `0x0c` action 2 recording control and configurable automatic startup,
  reflected in `0x0a` status replies,
  writing simultaneous finalized RGB `_A.mp4` and thermal `_I.mp4` files
  beneath `/mnt/DCIM/record/YYYY-MM-DD/`;
- timestamped output in `/run/camera_app.log`, with each known implementation
  gap reported only once per process;
- independent 720p, 1080p or 4K RGB sizes for the main/sub RTSP outputs,
  independently selected H.264/H.265 codecs, and independent H.264 recording
  encoders at the selected recording resolution; thermal output remains 720p;
- automatic/upright/inverted mounting configuration. Automatic mode follows
  the MCU mounting notification, retries if the startup reply is missed, and
  detects the A8's inverted-mount attitude representation as a fallback. It
  rotates all visible and thermal outputs;
- a typed, ArduPilot-app-owned `/app/camera.ini` configuration exposed by the
  ArduPilot camera app's Parameters and Raw config web tabs; and
- a runtime readiness file at `/run/camera-app.ready`.

The media backend covers both RGB sensors on `/video1`, thermal on `/video2`,
and simultaneous `A`/`I` recording. `imode wide` and `imode zoom` explicitly
select a physical sensor.
Cold-start thermal acquisition disconnects the boot-bound `uvcvideo` driver,
claims both USB interfaces through `usbfs`, and explicitly negotiates the
640x1024 temperature frame rather than depending on state left by the vendor
application. Graceful shutdown releases the interfaces and restores
`uvcvideo`.
SIYI vendor-protocol zoom-factor commands reproduce the stock hybrid scale: 1x through 3.44x crop
the wide sensor, then the app switches to the zoom sensor and maps the reported
factor to E5739 optical travel relative to the 3.44x crossover. The focus motor
follows the factory infinity-focus curve on zoom changes. `autofocus [X Y]`
then searches only the focus interval represented by that zoom row, so it
cannot drive outside the calibrated near-to-infinity envelope. The wide sensor
is fixed-focus; autofocus always operates the E5739-equipped zoom sensor even
when wide is selected for `/video1`. MAVProxy exposes bounded manual movement
as `focus near|far [SECONDS]`, with a 0.2-second default pulse, and
`focus stop`.
The stock optical-plus-thermal composite, telemetry subtitles,
additional `B`/`Z` recording channels, interrupted-file recovery
and SD-card lifecycle management are also pending.

## Configuration

The timezone defaults to `GMT` (UTC+0). The web dropdown selects a built-in
IANA name, which is saved in `[general] timezone`. All cameras and SITL share
the [supported names and current-rule limitations](../timezones/README.md);
no system zoneinfo installation is required. Existing POSIX settings remain
valid during upgrades.

The ArduPilot camera app reads `/app/camera.ini`; it does not read or attempt to match
the vendor `/app/config.ini` format. The repository default is
[`camera.ini`](camera.ini):

```ini
[general]
timezone = GMT

[capture]
photo_scope = all

[mount]
orientation = auto

[uart]
protocol = none

[mavlink]
system_id = 0
position_targeting = true
tcp_port = 14550
udp_port = 14550

[thermal]
palette = white_hot

[recording]
autorecord = false
resolution = 3840x2160

[stream.main]
resolution = 1920x1080
codec = h264
alias = "main.264"

[stream.sub]
resolution = 1280x720
codec = h264
alias = ""

[image]
brightness = 50
saturation = 50
contrast = 50
exposure_compensation = 0
iso = auto
shutter = auto
metering = average
white_balance = auto
```

`[stream.main] alias` and `[stream.sub] alias` serve `/video1` and `/video2`
under an additional RTSP path on the same port, so ground stations configured
for the vendor firmware's `rtsp://CAMERA:8554/main.264` keep working. SIYI
targets default the main alias to `main.264`; an empty alias adds nothing.
Aliases are a single path segment (letters, digits, `.`, `-`, `_`) and apply
on camera-app restart.

`timezone` accepts a supported built-in IANA name (or an existing explicit
POSIX TZ setting) and is applied before media filenames and normal startup
logging are generated.
`photo_scope` is `thermal` or `all`. `thermal` writes the current radiometric
plane. `all` writes wide, zoom, and thermal-display JPEGs plus the radiometric
thermal plane. Every JPEG carries an EXIF block (capture time with
sub-seconds and UTC offset, digital zoom ratio, and GPS position, altitude,
UTC time and image direction when the autopilot has supplied them) and an
XMP block with the vehicle roll/pitch/yaw, gimbal roll/pitch/yaw, relative
altitude and zoom, using the `drone-dji` field names that photogrammetry
tools already read plus an `apcg` namespace with the full set. Position and
attitude come from the MAVLink `GLOBAL_POSITION_INT`, `ATTITUDE` and
`AUTOPILOT_STATE_FOR_GIMBAL_DEVICE` streams and are dropped from the metadata
once older than 10 seconds. The camera requests `SYSTEM_TIME` at 1 Hz from the
selected flight controller and uses it to correct any camera date earlier than
1 September 2026 UTC. Times before that date from the flight controller are
ignored, allowing it to acquire valid GPS time after startup. A camera clock
already at or after that date is left unchanged.

MT11, A8 and ZR10 also request time from a connected UniGCS client using
private command `0x91` once per second while the camera clock is invalid.
UniGCS sends local calendar time, so the camera's configured timezone must
match the GCS timezone. Validated replies are converted to epoch time using
that timezone, including daylight saving when the configured zone supports it.
MAVLink and public SDK epoch timestamps remain independent of timezone.
Requests stop once any supported time source sets a valid date.
This works with both private TCP wire formats
and does not rename or retimestamp files created before time was available.

Automatic Recording offers **Disabled**, **Enabled**, and **While Armed**.
The INI values are `false`, `true`, and `while_armed`; existing boolean
settings retain their meaning. Enabled starts recording at app startup.
While Armed waits for a HEARTBEAT from the selected MAVLink system ID and
`MAV_COMP_ID_AUTOPILOT1` (component 1), then follows the
`base_mode & MAV_MODE_FLAG_SAFETY_ARMED` bit: armed starts recording,
disarmed stops and finalizes it. Other systems and components are ignored.
Automatic system-ID selection still uses the first flight-controller heartbeat.
An already-armed first heartbeat starts recording, including after an app restart.
Missing heartbeats do not stop recording. Each matching heartbeat reconciles
recording with armed state, so a manual recording change in While Armed mode
lasts only until the next matching heartbeat. Web UI changes require a restart;
MAVLink recording-policy changes apply immediately.
Recording transitions also broadcast MAVLink `CAMERA_CAPTURE_STATUS`; its elapsed
timer covers automatic and web-triggered recording as well as MAVLink commands.

Stream and recording resolutions are target-specific: MT11 and A8 offer
`1280x720`, `1920x1080` and `3840x2160`; ZR10 offers `1280x720`, `1920x1080`
and `2560x1440`; Z1-Mini streams at `1920x1080` and records at `1920x1080`
or `3840x2160`. Each RTSP codec accepts `h264` or `h265`. Recording uses separate
H.264 encoders, so its resolution does not silently follow either RTSP stream.
Fresh installations and SITL use the vendor recording resolution defaults:
MT11 and A8 use `3840x2160`, ZR10 uses `2560x1440`, and Z1-Mini uses
`1920x1080`. Existing saved settings are preserved on upgrade. These defaults
match resolution; the recording codec remains H.264 with our encoder bitrates.
These resolution settings apply to RGB imagery. The MT11 module's 640x512
thermal image has a proven one-stage VPSS ceiling of 1280x720, so thermal RTSP
and thermal MP4 output remain 1280x720 whichever stream slot thermal occupies.
The recording defaults have configuration and simulator test coverage. MT11
4K and ZR10 1440p recording still need hardware verification with all live
streams active, including sustained microSD write throughput at 12 Mbps.

The visible-image values use the vendor ranges and presets: image-level values
are 0--100, exposure compensation is -10--10 tenths of an EV, ISO is `auto` or
100--3200, shutter is `auto` or `1/30` through `1/2000`, metering is
`average`, `center` or center-point `spot`, and white balance is `auto`,
`daylight`, `cloudy`, `fluorescent` or `incandescent`.

Known values are validated and duplicate known keys are rejected. Missing
known values retain compiled defaults, while unknown sections and keys are
preserved and ignored so a configuration can grow without breaking an older
binary. Add new runtime settings to the typed descriptor/parser in
`src/config.cpp`, then add their UI descriptors to `web/mt11-web.cpp`.

`mavlink.tcp_port` and `mavlink.udp_port` independently enable the native
MAVLink 2 listeners. TCP and UDP can use the same numeric port; set either
value to `0` to disable that transport. Both values are available on the web
Parameters page and take effect when `camera-app` restarts.

`mavlink.position_targeting` controls whether the gimbal advertises and accepts
native geographic ROI targets. It defaults to `true`, causing ArduPilot to send
`MAV_CMD_DO_SET_ROI_LOCATION` and leaving the camera to calculate the pitch and
yaw target from vehicle position and attitude. Set it to `false` to omit the
location capability and test ArduPilot-calculated angle targeting instead.
After a camera-app restart, its device information is announced again so a
connected ArduPilot instance sees the updated capability.

Vehicle boot timestamps are mapped into the camera's monotonic clock using the
minimum-delay estimator from ArduPilot `AP_RTC/JitterCorrection`. Separate
estimators for `AUTOPILOT_STATE_FOR_GIMBAL_DEVICE`, fallback `ATTITUDE`, and
`GLOBAL_POSITION_INT` remove variable transport delay and follow clock drift.
They use a 500 ms maximum estimated lag and 100-sample convergence window.
Duplicate and reordered timestamps do not refresh state; 32-bit timestamp wraps
are unwrapped, and a backwards clock after a stream outage is reacquired.
Legacy streams with zero timestamps use arrival time until a timestamp is known.
The estimator cannot measure a constant one-way delay or a device's internal
sampling delay.

For earth-frame gimbal status, the camera interpolates vehicle yaw and yaw rate
at the gimbal feedback timestamp, with wrapped-angle interpolation and at most
250 ms of extrapolation on either side of the history. The yaw
history retains older samples when buffered telemetry shares a corrected
millisecond, replacing only the newest value. The outgoing
`GIMBAL_DEVICE_ATTITUDE_STATUS.time_boot_ms` identifies that feedback sample;
resending cached feedback keeps its timestamp. MT11 feedback has no device clock,
so its receive timestamp remains the best available sample-time estimate. If
matching vehicle history is unavailable, status uses the vehicle frame and marks
it accordingly. Yaw and position extrapolation are limited to 250 ms, after
which the last samples are held (attitude for 1 s, position for 1.5 s) so a
short telemetry gap does not flip the feedback frame or stop rate tracking.
`ATTITUDE` takes over as soon as the primary attitude exceeds the prediction
horizon. Near vertical pitch, where its Euler yaw rate is undefined, it still
updates attitude metadata but cannot supply control prediction. Metadata ages
also include the estimated transport lag.

Rate-based ROI tracking derives earth-frame pitch and yaw LOS rates directly
from `GLOBAL_POSITION_INT` position and NED velocity for a stationary geographic
ROI. It differentiates the great-circle bearing and elevation geometry without
rounding a future position to integer latitude/longitude. Motor yaw feed-forward
is the earth-frame LOS rate minus the vehicle yaw rate. The existing pointing
correction, rate limits, motor quantisation and lag still apply. At an undefined
bearing (within 10 cm horizontally of the ROI, or at a geographic pole), rate
tracking stops instead of producing a singular rate.

On A8 and MT11, the external connector is handled by the gimbal MCU;
ZR10 is assumed to use the same arrangement pending a hardware check.
Read-only SIYI queries on A8 and MT11 respond at 115200 baud, 8N1, even when
`uart.protocol` selects another protocol. MT11 Linux UART4 receives no bytes
from that connector; A8 has no `/dev/ttyAMA4` device.
`uart.protocol = none` therefore does **not** disable the external connector
or prevent the MCU from reacting to a connected flight controller. These
cameras' web UIs omit the protocol selector and explain the limitation.
Existing INI values remain readable; an explicitly supplied
`CAMERA_APP_EXTERNAL_UART` can still select a separate Linux serial device
or a test PTY (230400 baud, 8N1). Network listeners remain independent.

The A8, MT11 and ZR10 backends handle MCU-forwarded camera-local SIYI SDK
requests to Linux through private `6b/35` frames. These use the same camera handlers as network requests (including
configuration, A8 digital zoom, photo capture and recording), with replies returned
to the MCU as `6b/35` acknowledgements (outer flags `0x0a`). The `6b/16`
network tunnel does not return replies to the external UART. Replies stay on
the MCU path rather than going to a
network client. Unsupported delegated requests are not sent back as requests,
preventing a forwarding loop. This path works independently of `uart.protocol`;
it does not disable or select the MCU's external serial protocol.

Raw external UART bytes arrive through `6b/3f`. A dedicated stream parser
accepts MAVLink 1 and MAVLink 2 across private-frame boundaries and dispatches
camera commands through the existing MAVLink service. Replies and camera
telemetry return as raw bytes in `6b/35` acknowledgements: A8 hardware testing
found that `6b/3f` replies lose camera responses, whereas `6b/35` delivers them.
A bounded queue paces complete MAVLink packets for 115200 baud. Packets longer
than 255 bytes use consecutive private frames without a pause inside the
MAVLink packet, which could allow native MCU telemetry to interleave.
SIYI bytes in this raw stream do
not execute SDK commands; delegated `6b/35` requests execute them once.
The MCU continues to own the UART gimbal component: Linux neither repeats
its gimbal controls nor publishes a competing gimbal identity on this route.
Camera requests still use the configured camera component (normally 100),
including capture, recording, zoom, tracking, parameters and MAVFTP.

A8 uses private v2 framing (MCU `0x2e`, camera `0x2c`); MT11 uses private v3
(camera `0x34`). A8 hardware tests through CubeOrange serial passthrough
verified MAVLink 1/2 camera discovery, capture, recording, zoom, SIYI
coexistence, native MCU gimbal discovery and a complete MAVFTP camera
definition download. The stock MCU gates incoming MAVLink forwarding on
received heartbeats. Direct clients should send a heartbeat every second
and allow several seconds for forwarding to start, including after an idle
timeout. The default automatic system ID also requires an autopilot
heartbeat before the camera service can reply; standalone clients can
instead configure a nonzero MAVLink system ID. SIYI requests do not need
this handshake.
The complete hardware test used ArduPilot SITL to supply the telemetry
requested by the MCU. Heartbeat-only trials also worked, but occasionally
lost replies while native MCU request bursts remained unserviced; reliable
standalone operation under that traffic pattern is not yet established.
MT11's delegated SIYI input is captured on hardware; its new MAVLink return
path has host coverage only. ZR10 uses the A8 arrangement by
assumption. `make mcu-uart-test` exercises all three backends with fragmented
MAVLink, FTP reply splitting, SIYI coexistence and independent network routes.

When the ArduPilot camera app is running, the web Parameters and Raw config tabs edit
`/app/camera.ini`; when the vendor is running they continue to edit
`/app/config.ini`. `make install` installs the repository default only when
`/app/camera.ini` does not already exist. Firmware packages install the default
as `/app/camera.ini.default`; early startup copies it to `/app/camera.ini` only
when the persistent file is absent. Both paths preserve camera-local changes.

## Video telemetry metadata

`VIDEO_META_FMT` selects the interoperable video telemetry output on every
hardware target and SITL port. Its persistent values are **0: APCG JSON**
(the default) and **1: MISB KLV**. Set it through MAVLink or the web Parameters
page, or use `[video] telemetry = apcg` / `misb` in `camera.ini`. Changes apply
after restarting camera-app; saving a change preserves the active stream and
recording formats until then. Future formats will receive new enum values.

With MISB selected, open `rtsp://CAMERA:8554/video1.misb` or, on two-stream
cameras, `rtsp://CAMERA:8554/video2.misb` in a MISB-capable viewer. These
endpoints carry MPEG-TS over RTP (MP2T, payload type 33), containing
the configured H.264/H.265 video and a separate, frame-synchronous KLV stream.
Recordings use H.264 plus KLV in `.ts` files, including both visible and thermal
recordings where supported. Files can be downloaded for playback in an FMV
viewer; the Files page does not offer HTML video playback for `.ts` files.
The regular `/video1`, `/video2`, configured aliases and browser previews retain
the existing APCG JSON SEI format described below. MAVLink stream discovery
advertises the selected format as the primary streams (IDs 1 and 2), with codec,
geometry, FOV, thermal flags and independent start/stop status. In MISB mode the
APCG compatibility previews follow raw thermal (whose ID 3 is preserved); an
MT11 with raw thermal advertises APCG previews as streams 4 and 5. Live KLV/TS
packaging and APCG SEI insertion run only for formats with a playing viewer.
Codec headers and source clocks are retained while idle so a viewer can join
at the next keyframe. An enabled SupportProxy publisher remains a consumer of
APCG video even without a local viewer. SupportProxy currently relays
and advertises only the regular previews and raw thermal.

MISB output implements the ST 0601.8 UAS Datalink Local Set and ST 1402
synchronous MPEG-TS carriage: metadata stream type `0x15`, PES stream ID `0xfc`,
metadata descriptors and access-unit cells, with the same PTS as the associated
video frame. Every local set includes a microsecond UTC timestamp, version and
checksum. Available fields include sensor position/MSL altitude, platform
heading/pitch/roll, horizontal/vertical FOV and sensor-relative azimuth,
elevation and roll. Stabilized gimbal feedback is transformed relative to the
full platform attitude. Unavailable/non-finite measurements are omitted; values
outside ST 0601 angle ranges use its out-of-range representation. Full-range
pitch and roll tags 90/91 use 32-bit values across +/-90 degrees; legacy tags
6/7 are retained for older viewers and report out-of-range beyond +/-20/50.

MISB video also carries the complete APCG JSON snapshot in unregistered SEI,
using the same metadata snapshot, UTC and video PTS as KLV. This preserves
inverted attitudes, zoom, velocities, source ages and rates for our tools,
without redefining standard MISB tags. MISB viewers can ignore the SEI and read
KLV; they retain the range and precision of the standard fields they support.
MAVProxy/mavvidplay prefer APCG SEI when present, including in MISB recordings.
This adds hundreds of metadata bytes per frame; idle streams still do no
telemetry packaging.

Encoding precision varies by field (it does not imply sensor accuracy):

| Field | APCG JSON step | Standard MISB KLV step |
| --- | --- | --- |
| UTC | 1 microsecond | 1 microsecond |
| Latitude/longitude | 1e-7 degree source integers | About 4.2e-8 / 8.4e-8 degree |
| Platform pitch/roll | 1e-6 radian (about 0.000057 degree) | About 0.000000042 degree in tags 90/91 |
| Platform heading | About 0.000057 degree | About 0.00549 degree |
| Horizontal FOV | 0.0001 degree | About 0.00275 degree |
| MSL altitude | 0.001 metre | About 0.304 metre |

The accompanying SEI retains APCG precision for every field it carries.

Live image dimensions are read from the encoder SPS, including crop, to derive
vertical FOV from horizontal FOV. Both FOVs remain nominal pinhole estimates.
UTC and pose use the existing streaming/recording handoff snapshot rather than
hardware exposure time; recording uses the configured constant frame rate, as
with MP4. Ground intersections, terrain-derived corners and slant range are not
generated. The independent lossless 16-bit thermal stream and its Matroska
recordings retain their existing format.

The shared muxer uses the existing encoded frames without re-encoding. MISB
selection and encoding do not depend on the vendor SDK or platform libraries.

Every regular H.264/H.265 RTSP preview frame carries an unregistered user-data SEI NAL
with telemetry, immediately before its first video slice. Both streams use
this path on MT11 and A8. H.264 MP4 recordings and the browser's fragmented
MP4 video also carry one telemetry SEI per frame. Original encoder SEI is
retained; all slices and SEI belonging to a picture remain one MP4 sample.
RTP NALs belonging to a frame share a timestamp, and only the last packet of
the frame has the marker bit set.

The SEI payload type is 5; the NAL type is 6 for H.264 and prefix SEI type 39
for H.265. Its 16-byte UUID is `8d646b4e-556f-4a90-8b7c-35e629510321`, followed
by UTF-8 JSON without a trailing NUL. The schema is `apcg.telemetry.v1`:

| Field | Meaning |
| --- | --- |
| `pts90k` | Frame timestamp in 90 kHz ticks. RTSP uses the 32-bit RTP timestamp (wraps); MP4 uses its video sample timeline starting at zero. |
| `utc_us` | Camera system UTC time in microseconds since the Unix epoch, when telemetry was sampled. |
| `position` | `lat_e7`, `lon_e7` in degrees × 10⁷; `alt_amsl_m`, `alt_relative_m` in metres; `age_ms`. |
| `vehicle_attitude` | `roll_rad`, `pitch_rad`, `yaw_rad`, `age_ms` and optional `yaw_rate_rad_s` (earth-frame Euler yaw rate). Prefers `AUTOPILOT_STATE_FOR_GIMBAL_DEVICE`; falls back to `ATTITUDE` after the 250 ms prediction horizon expires. |
| `velocity` | `vn_m_s`, `ve_m_s`, `vd_m_s` (North/East/Down metres per second), and `age_ms`, from `GLOBAL_POSITION_INT` or `AUTOPILOT_STATE_FOR_GIMBAL_DEVICE`. |
| `gimbal_attitude` | Mounting-corrected `roll_rad`, `pitch_rad`, `yaw_rad`, `age_ms` and optional measured `yaw_rate_rad_s`; yaw and its rate are relative to the vehicle. |
| `heading_rad` | Vehicle heading from the position message, in radians. |
| `zoom` | Camera zoom factor, or null if unknown. |
| `hfov_deg` | Effective horizontal field of view of this video stream in degrees, including optical zoom and digital crop; null if unknown. |

FOV follows the selected visible lens and its current zoom, using a pinhole
model: `2 * atan(tan(native_hfov / 2) / magnification)`. The nominal wide FOV
is 88 degrees. MT11 tele magnification includes the 3.44x lens crossover,
the motor's quantized optical zoom and the selected lens's digital crop.
A8 uses its digital zoom. The MT11 thermal FOV remains 24.2 degrees when
the visible camera zooms. These are nominal lens estimates, not a measured
calibration for distortion or stabilization. MAVLink stream information and
status report the same effective FOV, rounded to whole degrees.

Each frame carries its stream's FOV, including when the live visible and
thermal streams are swapped. Players should use `hfov_deg` directly without
applying `zoom` again. Older recordings without this field still require a
user-supplied FOV.

Unavailable, non-finite or stale telemetry is represented by JSON `null`.
Position and attitude sources expire after 10 seconds, matching JPEG metadata.
The snapshot is taken at the streaming/muxing handoff; `utc_us` is not a
hardware exposure timestamp. UTC accuracy follows the camera clock. MP4
continues to use the configured constant frame rate, with telemetry aligned
to that timeline; platform-specific encoder PTS is not used for MP4 timing.

Recordings use fragmented MP4: the movie header is written with the first
keyframe, then each frame is appended as a complete `moof`/`mdat` fragment.
The video is playable without stopping recording; a crash can leave an
incomplete final fragment, but earlier complete fragments remain playable.
The optional total fragment duration (`mehd`) is omitted while the duration
is unknown. Writing it as zero prevents VLC 3 from deriving the recording
length and seek range from the fragments, including in mid-recording downloads.
A 64 KiB `free` box is reserved after the movie header. Closing a recording
ends it with a `sidx` of keyframe-aligned references covering the whole file,
placed directly before the first fragment, and writes the final duration into
the movie and track headers. FFmpeg-based
players, including Chrome's `<video>` element on the Files page, otherwise
read every fragment before playback starts; with the index they start at once
and can seek. A recording cut short by a crash keeps the empty box and still
plays after that scan.
The Files endpoint snapshots the file length between fragment writes and
downloads that immutable prefix while recording continues.

All four hardware backends (MT11, A8, ZR10 and Z1-Mini) share filesystem-aware
recording rollover. FAT/vfat files roll at the next H.264 IDR after **4 GiB minus
64 MiB**. The spare space accommodates the next GOP; a hard limit of **4 GiB
minus one byte** prevents overflow if an encoder stops delivering IDRs.
**exFAT and other filesystems remain a single file**, using 64-bit file offsets.
The filesystem is detected from the opened recording file, not its pathname.

The original filename is retained for the first part; subsequent files use
`<stem>_part0002.mp4`, `_part0003.mp4`, etc. Each part is independently playable,
with cached SPS/PPS decoder headers and per-frame telemetry. Its MP4/telemetry
sample timeline starts at zero; UTC and pose metadata continue to be sampled
normally. MT11 visible and thermal files roll independently at their respective
sizes. Recording status and live streams continue through rollover. Existing
files are never overwritten; an open/write/sync error stops recording and is
reported through the normal backend error path.

The previous part's final sync runs in the background, with at most one retired
file pending. If it is still flushing at the next rollover, splitting is deferred;
the hard FAT limit still applies. For short FAT bench tests only,
`CAMERA_APP_RECORD_SEGMENT_BYTES` can lower the rollover threshold (minimum
65536 bytes). It cannot raise the limit or enable splitting on exFAT. Run
`make -C camera_app recording-test` for decoded segment/telemetry tests and sparse
tests at the real 4 GiB boundary.

Each recording has a background worker which requests `fdatasync` after the
first frame and waits at least one second after each completed flush before
syncing further data. Requests coalesce into a single dirty flag, so slow
syncs do not create a backlog. The worker never holds the frame-write or
download-snapshot locks while syncing. It also syncs the parent directory
once to persist the new filename. Normal close waits for the worker's final
flush and reports any asynchronous I/O error.

Frame processing does not wait for the sync worker, but writes still share
the card's bandwidth and can block under kernel dirty-page pressure. The
one-second interval is not a maximum loss window: slow flushes, SD-card
hardware caches and filesystem damage from a physical power cut can still
affect what reaches storage.

Telemetry remains in every frame's SEI, including FOV, position and attitude.
New recordings omit the duplicate `mett` JSON track: VLC 3's fragmented MP4
reader mishandles unknown metadata tracks and disrupts playback timing.
The extractor still supports timed metadata in older recordings. Recordings
remain H.264; H.265 is supported by the RTSP telemetry path.

With libVLC installed, test the timeline and forward/backward seeking on
downloads taken during recording and after killing the recorder:

```sh
python3 web/tests/test_recording_download.py --vlc
python3 web/tests/check_vlc_video.py recording.mp4
```

Extract per-frame telemetry from a recording, raw stream or RTSP URL:

```sh
python3 tools/video_telemetry.py recording.mp4 --output telemetry.jsonl
python3 tools/video_telemetry.py rtsp://CAMERA:8554/video1 --output telemetry.jsonl
```

The utility uses only Python's standard library for Annex-B `.h264`, `.h265`
and `.hevc` files, and uses `ffprobe`/`ffmpeg` stream copy for MP4 and RTSP.
Use `--stream N` to select another video track in a container. To extract the
periodic metadata track as consecutive JSON objects:

```sh
ffmpeg -i recording.mp4 -map 0:d:0 -c copy -f data telemetry.json
```

## MAVLink camera and gimbal services

Builds with `APCAM_HAVE_THERMAL` (MT11 hardware and MT11 SITL) advertise
`CAMERA_CAP_FLAGS_HAS_THERMAL_RANGE` and publish `CAMERA_THERMAL_RANGE` from
the configured camera component. RGB-only targets do not advertise or send
thermal range and reject its request/interval commands as unsupported.

Reports contain minimum and maximum temperatures in degrees Celsius and their
normalized positions in the thermal image. The positions use the radiometric
sensor dimensions, not the upscaled RTSP resolution, and follow the image's
mounting rotation. MAVLink preserves negative temperatures and values above
655.35°C; SIYI's legacy unsigned fields retain their saturating encoding.
Samples expire after 250 ms without a new frame, including when a SITL renderer
stops. The report's stream ID
and `VIDEO_STREAM_STATUS_FLAGS_THERMAL_RANGE_ENABLED` follow the thermal image
when main/secondary sources are swapped. No report is sent before thermal
data is available; a one-shot request then returns temporarily rejected.

The default rate is 5 Hz. `MAV_CMD_REQUEST_MESSAGE` with message ID 277 returns
one sample (param2: stream ID, or 0 for the thermal stream; param3: camera
device ID, which must be 0). `MAV_CMD_SET_MESSAGE_INTERVAL` uses message ID 277
in param1, interval in microseconds in param2, stream ID (0 for all) in param3,
and camera device ID 0 in param4. Interval -1 disables streaming, 0 restores
the default, and positive intervals are limited to the thermal frame rate.
These runtime settings do not change the SIYI thermal telemetry subscription.
MT11 SITL currently supplies fixed synthetic extrema (12°C minimum, 42°C maximum).

Run `make -C camera_app thermal-mavlink-test` for thermal/RGB protocol checks,
or run `python3 camera_app/tests/test_thermal_mavlink.py build/sitl/camera-app sitl/gimbal_sim.py thermal`
against an MT11 SITL build.

The app presents one MAVLink system with a camera component defaulting to ID
100 (`MAV_COMP_ID_CAMERA`) and a gimbal-device component defaulting to ID 154
(`MAV_COMP_ID_GIMBAL`). Both use `mavlink.system_id`, configurable in the web
Parameters page as **MAVLink system ID**. The default, 0, waits for the first
flight controller HEARTBEAT and adopts its system ID for the lifetime of the
process. GCS heartbeats and heartbeats with `MAV_AUTOPILOT_INVALID` cannot
select the ID. A second flight controller cannot replace the first. No MAVLink
messages are emitted or commands accepted before automatic selection. Set
1–255 for a fixed ID, including standalone operation without a flight controller.
Changes take effect after restarting camera-app.

**MAVLink camera component ID** selects Camera 1–6, IDs 100–105
(`MAV_COMP_ID_CAMERA` through `MAV_COMP_ID_CAMERA6`). The equivalent parameter
is `MAV_CAM_COMP_ID`, or `[mavlink] camera_component_id` in the INI file.
Use a distinct camera component for each camera on the same vehicle. This
setting applies after restart; the save acknowledgment uses the current ID.
Camera heartbeats, commands, parameters and the MAVFTP camera-definition URI
all use the selected component. The associated gimbal component changes with it:
Camera 1–6 map to gimbal IDs **154, 171, 172, 173, 174, 175** respectively.
`CAMERA_INFORMATION.gimbal_device_id`, gimbal heartbeats, telemetry requests,
status messages and command addressing all use that gimbal component. The
`gimbal_device_id` extension in `GIMBAL_DEVICE_INFORMATION` and
`GIMBAL_DEVICE_ATTITUDE_STATUS` stays zero, as required for a device with its
own MAVLink component.

It accepts MAVLink 1 input for interoperability and
emits MAVLink 2. TCP is a listening server with up to four clients. UDP learns
the most recent peer from an incoming MAVLink packet, so the ArduPilot side
must be configured as a UDP client rather than a UDP listener.

The configured camera component exposes all numeric camera-app settings through
`PARAM_REQUEST_LIST`, `PARAM_REQUEST_READ` (by name or index), and `PARAM_SET`.
Requests may target that camera component or component 0 (all); the gimbal component
uses the same configuration and does not publish a duplicate parameter table.
Names are uppercase and at most 16 characters. Values use `MAV_PARAM_TYPE_INT32`
with C-cast encoding, advertised in `AUTOPILOT_VERSION.capabilities`. All values
are exactly representable in the protocol's float field. Booleans use 0/1;
enums use the numeric choices below. String settings, including
`general.timezone` and SupportProxy names and credentials, are available only
through the web UI/INI. The additional proxy parameters are listed in
[SupportProxy](#supportproxy).

| MAVLink name | INI setting | Values |
| --- | --- | --- |
| `MAV_SYSID` | `mavlink.system_id` | 0 automatic; 1–255 fixed |
| `MAV_CAM_COMP_ID` | `mavlink.camera_component_id` | 100–105: Camera 1–6; default 100 |
| `MAV_TCP_PORT` | `mavlink.tcp_port` | 0 disabled; 1–65535 port |
| `MAV_UDP_PORT` | `mavlink.udp_port` | 0 disabled; 1–65535 port |
| `MAV_POS_TARGET` | `mavlink.position_targeting` | 0 disabled, 1 enabled |
| `TRACK_METHOD` | `mavlink.tracking_method` | 0 angle, 1 rate |
| `LOG_DISARMED` | `logging.disarmed` | 0 log while armed, 1 also log while disarmed |
| `PHOTO_SCOPE` | `capture.photo_scope` | 0 thermal, 1 all lenses |
| `MOUNT_ORIENT` | `mount.orientation` | 0 auto, 1 upright, 2 inverted |
| `UART_PROTOCOL` | `uart.protocol` | 0 none, 1 SIYI, 2 MAVLink |
| `THERMAL_PALETTE` | `thermal.palette` | 0 white hot, 2 sepia, 3 ironbow, 4 rainbow, 5 night, 6 aurora, 7 red hot, 8 jungle, 9 medical, 10 black hot, 11 glory hot |
| `REC_AUTOSTART` | `recording.autorecord` | 0 disabled, 1 enabled at startup, 2 while armed |
| `REC_RESOLUTION` | `recording.resolution` | 0 720p, 1 1080p, 2 2160p, 3 1440p; target availability below |
| `VIDEO_MAIN_RES` | `stream.main.resolution` | 0 720p, 1 1080p, 2 2160p, 3 1440p; target availability below |
| `VIDEO_MAIN_CODEC` | `stream.main.codec` | 0 H.264, 1 H.265 |
| `VIDEO_SUB_RES` | `stream.sub.resolution` | 0 720p, 1 1080p, 2 2160p, 3 1440p; target availability below |
| `VIDEO_SUB_CODEC` | `stream.sub.codec` | 0 H.264, 1 H.265 |
| `IMG_BRIGHTNESS` | `image.brightness` | 0–100 |
| `IMG_SATURATION` | `image.saturation` | 0–100 |
| `IMG_CONTRAST` | `image.contrast` | 0–100 |
| `IMG_EXPOSURE` | `image.exposure_compensation` | −10 to 10, in tenths of an EV |
| `IMG_ISO` | `image.iso` | 0 auto, 1 ISO 100, 2 ISO 200, 3 ISO 400, 4 ISO 800, 5 ISO 1600, 6 ISO 3200 |
| `IMG_SHUTTER` | `image.shutter` | 0 auto; 1–8: 1/30, 1/50, 1/100, 1/250, 1/500, 1/750, 1/1000, 1/2000 s |
| `IMG_METERING` | `image.metering` | 0 average, 1 center, 2 spot |
| `IMG_WHITE_BAL` | `image.white_balance` | 0 auto, 1 daylight, 2 cloudy, 3 fluorescent, 4 incandescent |

For the resolution parameters, MT11 and A8 support values 0, 1 and 2;
ZR10 supports 0, 1 and 3. Z1-Mini supports only 1 for streaming and 1 or 2
for recording. MT11 thermal output remains 720p regardless of these RGB settings.

Writes validate ranges and enum choices and persist atomically to the same INI
file as the web UI. Camera-definition settings described below also apply
immediately through ordinary `PARAM_SET`. Other settings, including `MAV_SYSID`, `MAV_CAM_COMP_ID`
and transports, require a restart, preserving the original acknowledgment link
and system ID. Parameter reads reflect saved configuration, including web edits;
`PARAM_EXT` reads report running camera state. Launcher environment overrides
for TCP/UDP ports take precedence on restart.

For a camera using system ID 42 and the default camera component, connect MAVProxy to component 100:

```sh
mavproxy.py --master=tcp:127.0.0.1:14550 --target-system=42 --target-component=100 --mav20 --nowait
```

```text
set param_ftp 0
param fetch
param fetch IMG_BRIGHTNESS
param set IMG_BRIGHTNESS 63
param fetch IMG_BRIGHTNESS
```

`param fetch NAME` performs a wire read; `param show NAME` displays MAVProxy's
cached value. Restart the app for settings reported as requiring a restart. In
automatic mode, a flight controller must send its heartbeat first.

Implemented camera protocol coverage includes discovery and camera
information; single, finite-interval and capture-until-stopped stills;
capture events and status; video recording; percent and continuous zoom;
percent, continuous and automatic focus; photo/video mode; visible/thermal
source selection; microSD capacity; and two RTSP stream descriptors. The
camera information links the camera to gimbal component 154. Implemented
gimbal-device coverage includes discovery, limits and capabilities, 10 Hz
attitude polling with 5 Hz status, vehicle- and earth-frame angle/rate
commands, neutral, and retained global-location targeting. Earth-frame control uses
`AUTOPILOT_STATE_FOR_GIMBAL_DEVICE` to translate between North-relative
MAVLink yaw and the MT11 controller's vehicle-relative SIYI yaw. Native
location targeting combines that attitude with `GLOBAL_POSITION_INT` at 10 Hz
to command the MT11 controller. The camera requests the required position
stream from ArduPilot with `MAV_CMD_SET_MESSAGE_INTERVAL`.

ArduPilot should initiate the connection through its `NET_` subsystem. These
are the settings used by the CI test, with `CAMERA_IP` replaced by the MT11's
address:

| Parameter | TCP client | UDP client |
|---|---:|---:|
| `NET_ENABLE` | 1 | 1 |
| `NET_P1_TYPE` | 3 | 1 |
| `NET_P1_PROTOCOL` | 2 (MAVLink 2) | 2 (MAVLink 2) |
| `NET_P1_IP0` ... `NET_P1_IP3` | `CAMERA_IP` octets | `CAMERA_IP` octets |
| `NET_P1_PORT` | `mavlink.tcp_port` | `mavlink.udp_port` |
| `MNT1_TYPE` | 6 (MAVLink) | 6 (MAVLink) |
| `CAM1_TYPE` | 6 (MAVLink Camera v2) | 6 (MAVLink Camera v2) |

Current ArduPilot and protocol limitations are deliberately reflected in the
advertised capabilities:

- `AP_Camera_MAVLinkCamV2` currently discovers `CAMERA_INFORMATION` and emits
  shutter, recording, zoom and focus commands. It does not expose the MT11's
  visible/thermal source selection, stream/storage/status data, thermal
  palette or gain, radiometric extrema, LiDAR, raw thermal planes, or choice
  of still-image products. Those remain available through the web and SIYI
  interfaces; several need future ArduPilot camera-backend APIs.
- `AP_Mount_MAVLink` can command angles, rates and retract/neutral, but has no
  MT11-specific mode or face-down command. The MT11 has no mechanical retract,
  so a MAVLink retract request is safely mapped to neutral.
- Earth-frame yaw and geographic targeting are supported while fresh
  `AUTOPILOT_STATE_FOR_GIMBAL_DEVICE` and `GLOBAL_POSITION_INT` data are
  available. Geographic targets currently accept only the AMSL
  `MAV_FRAME_GLOBAL` frame and its deprecated `MAV_FRAME_GLOBAL_INT` synonym;
  relative-home and terrain-relative altitudes are rejected. Roll control
  remains unavailable.
- Starting or stopping a MAVLink stream changes the reported logical stream
  state; it does not tear down the shared RTSP encoders used by the web UI.
  Stream URIs use the camera-side IPv4 address of the MAVLink connection, so a
  routed ground station can normally open them without separate configuration.
- `CAMERA_IMAGE_CAPTURED` reports time, sequence, success, and the vehicle
  position and gimbal attitude last received, but not a file URL.
- MAVLink signing is not configured. Signed input frames are parsed using the
  normal packet CRC but their signatures are not authenticated, and output is
  unsigned.

The native protocol test covers every operation above over TCP and performs a
second UDP discovery/query pass. The ArduPilot integration test launches
ArduCopter SITL twice and proves `AP_Mount_MAVLink` and
`AP_Camera_MAVLinkCamV2` operation through `NET_P1` as both a TCP client and a
UDP client.

### MAVProxy generic camera control

The MAVProxy `mavproxy_camera` module discovers the MT11 camera component 100,
its associated gimbal component 154, and both RTSP streams from standard
MAVLink messages. The camera app puts the camera-side address selected for the
MAVLink peer into each `VIDEO_STREAM_INFORMATION.uri`; it also reports stream
name, RGB/thermal flag, encoding, resolution, frame rate and horizontal FOV.
This lets `camera view rgb`, `camera view thermal`, and `camera view all` work
without an MT11-specific IP setting when the GCS can route to the camera.

Use one `CAM1_TYPE=6` and one `MNT1_TYPE=6` backend. The visible and thermal
feeds are streams belonging to one camera component, so configuring
`CAM2_TYPE=6` would incorrectly search for component 101 rather than expose the
second MT11 stream. The MAVProxy module sends camera commands directly to
component 100 through MAVLink routing and sends gimbal commands to ArduPilot's
gimbal manager, preserving the mount backend's arbitration and frame handling.
The module's own README documents commands and the current protocol gaps.

## MAVProxy SIYI protocol coverage

This is the living compatibility checklist for the `pr-siyi-mt11` MAVProxy
branch. Update it whenever an opcode, payload variant or corresponding media
operation is added or changed. "MCU relay" means that `camera-app` preserves
the stock design by tunnelling the public SIYI packet over UART3 and relaying
the gimbal controller's reply; it does not mean the operation is implemented
by the Linux media backend.

### Implemented locally

| Opcode | MAVProxy operation | ArduPilot camera app behaviour |
|---:|---|---|
| `0x04` | `autofocus [X Y]` | Runs an asynchronous contrast search on the zoom sensor; omitted/zero coordinates use the centre, while nonzero coordinates are pixels in the 1920x1080 RGB stream |
| `0x05` | Manual zoom | Adjusts zoom in 0.1x steps; currently not emitted by the MAVProxy CLI |
| `0x06` | `focus near\|far [SECONDS]`, `focus stop` | Moves the E5739 focus motor within the current zoom row's calibrated near-to-infinity envelope |
| `0x0A` | `getconfig` | Reports HDR disabled, recording, gimbal mode and the effective mounting direction; the video field is synthetic |
| `0x0B` | Function feedback | Reports and retains photo and recording outcomes; rejects HDR requests with HDR-off feedback; an empty request returns the latest result |
| `0x0C 00` | `photo` | With `photo_scope=thermal`, saves the latest raw `_I.bin`; with `all`, also saves hardware-encoded wide `_C.jpg`, zoom `_Z.jpg`, and thermal-display `_I.jpg`; emits `0x0B` success/failure feedback |
| `0x0C 01` | `hdr` | HDR is unsupported; leaves it disabled and emits `0x0B` HDR-off feedback |
| `0x0C 02` | `recording` | Toggles simultaneous RGB `_A.mp4` and thermal `_I.mp4` recording and emits failure/started/stopped `0x0B` feedback |
| `0x0F` | `zoom ZOOM` | Sets the 1.0x--10.0x stock-compatible hybrid/optical zoom factor |
| `0x10` | Image-slot query | Reports zoom or wide RGB as main and thermal as secondary |
| `0x11 00 02` | `imode zoom` | Selects the zoom RGB sensor |
| `0x11 01 02` | `imode wide` | Selects the wide RGB sensor |
| `0x11 02 00` | `imode thermal` | Serves thermal as `/video1` and zoom RGB as `/video2` |
| `0x14` | Full-frame temperature | Supports stop, one-shot and continuous extrema reporting |
| `0x18` | Zoom query | Reports the current user-visible zoom factor |
| `0x30` | `settime` | Sets `CLOCK_REALTIME` from little-endian epoch microseconds |
| `0x37` / `0x38` | Thermal gain query/set | Reads and writes the actual thermal-module gain |
| `0x1A` / `0x1B` | Thermal palette query/set | Reads and writes the module palette, including SIYI numbering translation |

The ArduPilot camera app also implements maximum-zoom query `0x16`, although the
current MAVProxy module does not define or emit it.

### Gimbal-controller relay

The following MAVProxy elements use the bidirectional UART tunnel and do not
need duplicate Linux implementations:

| Opcode | Operation or sub-command |
|---:|---|
| `0x01`, `0x02` | Firmware version and hardware ID |
| `0x07`, `0x08` | Rotation rate and centre/reset |
| `0x0C 03`, `0x0C 04`, `0x0C 05` | Lock, Follow and FPV modes |
| `0x0D`, `0x0E` | Gimbal attitude query and absolute-angle command |
| `0x15`, `0x17`, `0x19` | Rangefinder, laser target coordinates and gimbal-mode reports |
| `0x22`, `0x3E` | External aircraft attitude and GPS/velocity injection |
| `0x25 01`--`0x25 04` | Continuous attitude, rangefinder, encoder and voltage streams |
| `0x26`--`0x2A` | Encoder, control-mode, threshold and voltage operations |
| `0x32`, `0x71` | Laser state and weak-control setting |

The MT11 MAVProxy profile intentionally avoids the `0x25` continuous streams,
laser polling and threshold/voltage polling because the ArduPilot Lua script
owns MT11 attitude and laser acquisition. The tunnel remains available for
direct clients. Setting MAVProxy `show_lidar_target` to `True` polls the
gimbal-computed `0x17` coordinate at 2 Hz and displays each valid result on the
map as a red cross; it does not enable the laser, which remains owned by Lua.

### Active MT11 implementation gaps

| Opcode | MAVProxy operation | Missing behaviour |
|---:|---|---|
| `0x11 03 02` | `imode split` | RGB/thermal composite; currently reduces to ordinary zoom plus thermal-secondary slots |
| `0x13` | Temperature box | Defined by MAVProxy but not currently emitted by its CLI |

`0x10` cannot report split until that `0x11` mode exists. Opcode `0x0A` still
uses a synthetic video-status field.

### ZT30-only and dormant MAVProxy elements

MAVProxy rejects the following commands when `camera_type=MT11`, and the MT11
backend does not implement them. They remain portability work for a future
ZT30/A8-style backend rather than current MT11 blockers.

| Opcode | MAVProxy commands |
|---:|---|
| `0x33`, `0x34` | `get_thermal_mode`, `thermal_mode` |
| `0x35` | `tempsnap`; MT11 raw capture instead uses `0x0C 00` |
| `0x39`, `0x3A` | `therm_getenv` and all five `therm_set_*` environment fields |
| `0x3B`, `0x3C` | `therm_getswitch`, `therm_setswitch` |
| `0x42`, `0x43` | Thermal-threshold switch get/set |
| `0x44`, `0x45` | Thermal-threshold values get/set |
| `0x46`, `0x47` | Threshold-precision constants; defined but not emitted or parsed by MAVProxy |

MAVProxy-local commands (`set`, `connect`, the three viewers, target tracking
and `autoflag_*`) do not require matching camera protocol handlers.

MAVProxy defaults to `transport=udp`. Use `siyi set transport tcp` before
`siyi connect` to select TCP explicitly. The legacy `auto` setting selects TCP
for MT11 and UDP for ZT30. MAVProxy sends the vendor TCP keepalive, which the
ArduPilot camera app consumes as a transport heartbeat. TCP is preferable when multiple
operators or test clients are active because the ArduPilot camera app keeps
independent stream framing and reply destinations for up to eight simultaneous
TCP clients while continuing to accept UDP clients on the same port.

The tested Ubuntu VLC 3.0.23 package is built without Live555 RTSP support and falls
back to its SAT>IP client for an `rtsp://` URL. The server accepts that client's
aggregate `SETUP`/`PLAY` sequence, but VLC must be told that the resulting
elementary stream is H.264 when that codec is configured:

```sh
vlc --demux=h264 rtsp://CAMERA:8554/video1
```

Players using normal RTSP/RFC 6184 negotiation, including Totem/GStreamer and
FFmpeg, use the URL directly without this option.

## Build and test

```sh
git submodule update --init --recursive
make dependencies
make -C camera_app
make -C camera_app test
```

The MAVLink C bindings are generated from the message definitions in the
`modules/mavlink` submodule, so it must be initialised once after cloning.
Builds that include the MAVLink layer run pymavlink's `mavgen.py` as needed
(from the submodule; it needs only the Python 3 standard library) on
`MAVLINK_DIALECT` (default `all`) and writes the headers to
`camera_app/build/mavlink/<dialect>/include`, which `make clean` removes. The
generator is LGPL-3.0 with an exception that licenses the generated code under
the MIT licence, so the bindings can be linked into the static binary. The app
compiles the generated helpers once, without the message-info tables, and
gives each MAVLink link its own parser state rather than using the channel
globals.

For host simulation the MT11 backend also accepts
`--uart udp://IPv4:PORT` (`localhost` is accepted too). This changes only its
byte transport: private framing, startup exchange and tunneled SIYI handling
remain the production code paths. The repository-level `make sitl` builds that
host application together with a separate simulated gimbal and the
administrative web UI. See
[`../sitl/README.md`](../sitl/README.md) for launch and test instructions.

The normal binary is a static AArch64 executable. Protocol, thermal assembly,
pixel conversion and zoom calibration tests run natively on the host. The
MT11 media build uses the SS928 V2.0.2.2 B090 MPP tree at
`SS928_MPP_ROOT`, the in-tree IMX586 adapter under `src/backends/mt11`, and a
pinned CC0 minimp4 checkout at `MINIMP4_ROOT`. Override the external roots on
the make command line when needed. The repository-level `make dependencies`
fetches and verifies both pinned public dependencies under `build/deps`.
The build leaves downloaded sources untouched and prepares an XOP copy under
`camera_app/build/xop`, applying the reviewed fixes in
`tools/xop_warnings.patch`. This corrects copied-buffer array ownership and
socket options, and removes unused declarations and incomplete C++ aggregate
initializers. The same buffer/socket implementations replace the corresponding
objects in the MT11 vendor archive. `make -C camera_app xop-test` checks buffer
release with AddressSanitizer and verifies TCP_NODELAY. The minimp4 patch also
annotates intentional fall-throughs and checks signed file sizes before
unsigned comparisons; compiler diagnostics remain enabled.

## SupportProxy

Use `camera view rawthermal` in MAVProxy connected through the proxy to view
the raw stream. The proxy also supplies `scripts/view_raw_thermal.py` for
standalone desktop viewing. Its MPEG-TS browser player cannot decode FFV1;
the web page offers the desktop viewer when a raw publisher is active.

The **SupportProxy** section of the web UI's Parameters tab configures an
optional connection to an ArduPilot SupportProxy instance. It is disabled by
default. Set the server hostname or IPv4 address, its user MAVLink UDP port,
and the separate publish ports assigned to video1, video2 and (on thermal
cameras) raw thermal video3. A port of
zero disables that connection. Save the settings and restart camera-app.

Video is published over RTSP/TCP without transcoding. Both H.264 and H.265 are
supported, using the same sources as the local RTSP streams, including their
per-frame telemetry and zoom-adjusted FOV. Each stream has a configurable name.
Raw thermal stream 3 uses HTTP chunked PUT with lossless FFV1/Matroska, keeping
all 16-bit samples and capture metadata. It requires the Matroska-capable
SupportProxy update. Set `PROXY_VID3_PORT` (web: **Raw thermal port**) to the
proxy's third video port; zero disables it. `RAW_STREAM_FPS` controls its rate.
Its worker keeps at most the newest queued frame plus one in flight, so a slow
uplink skips whole frames without delaying SD recording.

Set the optional publish password to match the proxy entry. Without a publish
password, the proxy must allow session-based publishing (`session_ok`) and the
MAVLink connection must remain active. A publish password also permits
video-only operation with the MAVLink port set to zero.

Optional MAVLink 2 signing uses the SHA-256 hash of the configured passphrase,
matching SupportProxy's signing key derivation. Configure the proxy entry for
bidirectional signing (`bidi_sign`) with the same passphrase. Camera, gimbal
and flight-controller messages retain their source IDs and sequence numbers.
When signing is enabled, unsigned, incorrectly signed and replayed incoming
messages are rejected. Verified commands are forwarded to the selected flight
controller over its local link with the proxy signature removed. The local
flight-controller connection therefore does not require the proxy's key.

Camera stream-information requests received through the proxy advertise the
configured stream names and proxy HTTP viewer URLs (`/v1.ts`, `/v2.ts`, and
`/v3.mkv` on their respective ports). Raw thermal uses experimental type 200. Viewer access remains subject to the proxy's access
rules. Requests received locally continue to advertise local RTSP URLs.

Camera networking is configured independently in **Parameters → Network**,
using the `[network]` section below. These settings work even with SupportProxy
disabled. Enabled legacy `[support_proxy]` network settings are imported if the
corresponding new keys are absent; saving the form writes the new keys.

For example, with ports and credentials allocated on your proxy:

```ini
[support_proxy]
enabled = 1
host = support.example.org
mavlink_port = 10001
signing = 1
signing_passphrase = "replace-with-your-proxy-passphrase"
signing_link_id = 1
video1_port = 40001
video2_port = 40002
video1_name = "Front Camera"
video2_name = "Thermal Camera"
video3_port = 40003
video3_name = "Raw Thermal"
publish_password = "replace-with-your-publish-password"
[network]
interface = eth0
primary_address = 192.168.144.25/24
secondary_address = 192.168.20.25/24
gateway = 192.168.20.1
```

The numeric settings are also exposed by the MAVLink parameter service:

| Parameter | Meaning | Default |
| --- | --- | --- |
| `PROXY_ENABLE` | Enable SupportProxy | 0 |
| `PROXY_MAV_PORT` | MAVLink UDP port; 0 disables | 10001 |
| `PROXY_SIGN` | Enable MAVLink 2 signing | 0 |
| `PROXY_SIGN_ID` | MAVLink signing link ID | 1 |
| `PROXY_VID1_PORT` | Video1 RTSP publish port; 0 disables | 0 |
| `PROXY_VID2_PORT` | Video2 RTSP publish port; 0 disables | 0 |
| `PROXY_VID3_PORT` | Raw thermal Matroska publish port; thermal cameras only; 0 disables | 0 |

Hostnames, names, passwords and network addresses are string settings and are
configured through the web UI or INI file. Settings take effect after restart.

The H.264/H.265 publishers each have an independent worker and a queue limited to 60 frames
or 2 MiB, plus its current frame and socket buffers. Queue overflow drops
queued video and resumes at a keyframe on the same TCP connection, allowing
congestion control to recover without blocking local recording or live video.
A lost connection reconnects separately. TCP send buffers use the kernel's
normal autotuning and `tcp_wmem` memory limits. MAVLink has separate queues of
128 messages in each direction.
The camera needs no external streaming daemon or FFmpeg installation.

## Camera IP configuration

Use **Parameters → Network** to set the interface (normally `eth0`), primary
IPv4 address/prefix, optional secondary address/prefix and default gateway.
Include the subnet prefix on addresses, for example `192.168.144.27/24`.
The gateway is an address without a prefix and must be reachable through one
of the configured subnets when a primary address is specified.

**Save** persists address settings without changing the running network.
**Save and restart camera app** applies them. A reconnect link appears for the
new primary address; changing subnets also requires a reachable address or
route on your computer. You may need to log in again at the new address.

An explicit primary replaces the selected interface's global IPv4 addresses
with the configured primary and optional secondary. It also replaces that
interface's default route with the configured gateway, or removes it if the
gateway is blank. IPv6 and other interfaces are left alone. A blank primary
leaves existing addresses in place; by default all optional fields are blank,
so cameras retain their boot-time network configuration.

Clearing a secondary address or gateway removes the setting previously applied
by this app on its next restart. Clearing the primary alone leaves its current
address in place until a camera reboot restores the boot-time address. If an
operation fails, the app attempts to restore the previous IPv4 addresses and
routes, and reports the failure in the web UI and app log. Existing static
routes are preserved when reachable; changes that would strand those routes
are rejected. Interfaces with shared multipath routes are rejected before
changing anything. Correct the settings and restart the app to retry. This cannot detect a syntactically valid but unreachable address;
keep the correct subnet settings for your computer when changing camera IPs.

SITL saves these fields but never modifies the host network. Address and route
handling is tested in an isolated Linux network namespace; see the
[SITL network test](../sitl/README.md#supportproxy-integration-tests) instructions.

## Network packet capture

All camera targets (SIYI A8, MT11, ZR10 and XFRobot Z1-Mini) support
**Parameters → Network → Network capture**, disabled by default. Select Enabled
and **Save parameters** to begin, reproduce the protocol issue, then select
Disabled and save to finish. Capture starts and stops live without restarting
the camera or interrupting video. The Network tab refreshes capture status and
shows startup/storage errors. **Browse network captures** opens the Files page
at `DCIM/network` on the microSD card; download the `.pcap` files and open them
in Wireshark. The INI setting is `[network] capture = false`.

The capture includes incoming and outgoing traffic on all network interfaces,
including loopback, with full packet payloads (up to 262144 bytes including the
Linux cooked header) and kernel receive timestamps at microsecond resolution.
It uses Linux packet sockets on every target; no tcpdump installation is needed.
Serial UART traffic is not network traffic and is not included.

Capture runs in a separate worker and keeps four files of up to 16 MiB each,
`network-0.pcap` through `network-3.pcap`. The oldest slot is replaced when the
ring wraps; disable capture before downloading to preserve a complete snapshot.
The files are synced every second and are readable without a final index.
Storage or permission failures stop capture and appear in the Network tab and
app log; disable and re-enable to retry. The app log reports packet counts and
kernel drops when capture stops. Capture is independent of recording/armed state.

Linux SITL supports the same capture with root or `CAP_NET_RAW` permission.
Windows/macOS SITL reports it unsupported. See the
[isolated capture tests](../sitl/README.md#network-capture-tests).

## Lossless raw thermal video

MT11 and SITL-MT11 now advertise a third, full-depth 640x512 FFV1/Matroska
thermal stream over HTTP (normally port 8556). Every frame includes its own
position, vehicle/gimbal attitudes, timestamps and temperature range. MAVProxy
can display temperatures and save the original 16-bit samples with metadata.
`RAW_STREAM_FPS` and `RAW_RECORD_FPS` independently set streaming and recording
rates through MAVLink or web Parameters (both default to 5 fps; recording 0
disables raw files). Lossless `.raw.mkv` files accompany ordinary video and
follow the same manual/automatic recording policy, including while armed.
Changes apply live. Metadata timestamps have microsecond resolution; container
playback timestamps use millisecond ticks.
SITL terrain mode derives raw samples and thermal display video from the same
rendered sensor image, using synthetic 15–45°C temperatures. Simple mode keeps
the full-depth diagnostic test pattern.
Display RTSP streams and the legacy port-7345 raw service remain available.
See [the raw thermal protocol, limitations and test instructions](RAW_THERMAL.md).
Discovery uses experimental MAVLink stream type 200 pending upstream allocation.
The [MT11 hardware test results](../docs/mt11-raw-thermal-test.md) cover lossless
samples, live telemetry and concurrent video at the default 5 Hz.

## Install

```sh
make -C camera_app install
```

This uses `rsync` over the `MT11` SSH host from the user's normal SSH
configuration. It atomically installs `/app/bin/camera-app`, builds
and installs the web supervisor and persistent startup scripts, and restarts
only the port-80 web process. It does not switch the running camera application
during installation. The installer makes `/app/app_selection.sh` durable before
activating the `app_init.sh` that calls it, so an interrupted deployment cannot
leave the new init script without its companion.

Use the web UI action buttons to switch owners. The switch is deliberately
exclusive because the two applications cannot share UDP/TCP 37260, UART3 or the
raw-thermal TCP port. The web supervisor stops the external `thermal_socket`
for the ArduPilot camera app and starts it for the vendor app. A failed ArduPilot
camera app start is rolled back to the vendor application and helper. Successful switches
are written atomically to `/app/app_selection.txt`, so reboot starts the last
selected app. Missing or invalid selection defaults to vendor, while an
unavailable selected executable causes `app_selection.sh` to use and remember
the available alternative. Once the ArduPilot camera app owns the hardware, the web
UI's Restart action performs a graceful media teardown and starts the newly
installed binary.

The external `thermal_socket` is only a transitional compatibility service for
the vendor application. It is not needed by `camera-app` and should be removed
from the startup and firmware package once migration is complete and vendor-app
fallback is retired.

## Build an MT11 SD-card update

From the repository root, build an update image with:

```sh
make mt11_package
```

The default output is
`build/MT11_FW_ArduPilot_<version>_abcdef.bin`: `<version>` is the highest
`vX.y` or `vX.y.z` version tag reachable from `HEAD`, and `abcdef` is the first
six characters of the current commit hash. For example, latest reachable tag `v1.0` with
`HEAD` at `49f5727...` produces `build/MT11_FW_ArduPilot_v1.0_49f572.bin`.
The builder validates the reviewed kernel and rootfs inputs included under
`packaging/mt11/base`, builds both static applications and the vendor-mode
raw-thermal helper, and creates an `/app` overlay containing:

- `/app/bin/camera-app`, `/app/bin/mt11-web`, `/app/bin/thermal_socket`,
  `/app/bin/mt11-timesync.sh`, and static `rsync`, `strace`, `ltrace` and
  `tcpdump` support utilities;
- static `/app/bin/dropbear` and `/app/bin/dropbearkey`, plus the
  `/app/dropbear/start-dropbear.sh` launcher;
- `/app/camera.ini.default`, `/app/app_init.sh` and `/app/app_selection.sh`;
  startup creates `/app/camera.ini` from that default only when no persistent
  camera-app configuration exists; and
- `/app/web.pass.default`, set from `MT11_WEB_PASSWORD` (default `ardupilot`);
  startup copies it to `/app/web.pass` only when no password exists, so a
  password selected in the Users tab survives upgrades.

Both the web and root accounts default to `ardupilot`, so no password arguments
are required. For camera-specific credentials, use:

```sh
make mt11_package \
    MT11_WEB_PASSWORD='a-camera-specific-password' \
    MT11_ROOT_PASSWORD='a-camera-specific-root-password'
```

Alternatively, `MT11_ROOT_PASSWORD_HASH_FILE` may name a file containing one
SHA-512 crypt verifier beginning with `$6$`; it takes precedence over
`MT11_ROOT_PASSWORD`. Change the defaults before exposing a camera to an
untrusted network.

On first boot the launcher generates a persistent Ed25519 host key under
`/app/dropbear`, then starts Dropbear on TCP port 22. Root password login uses
the root verifier selected during package creation. Local and remote SSH port
forwarding are disabled; interactive shell and rsync-over-SSH access remain
available.

The sparse overlay deliberately omits the vendor application, `/app/libs`,
`/app/cfg` and `/app/boa`; those existing factory files remain in place. In
particular, omitting `siyi_camera_app_tmp` makes the updater preserve the
installed vendor application. The overlay also leaves an existing
`app_selection.txt` untouched; a camera without one defaults to vendor and
creates it during startup. The port-80 UI can switch either way afterward.
The packaged time-sync helper exits on `SIGTERM`; this is required because the
vendor updater aborts if any process launched from `/app` survives its shutdown
retry loop.

The vendor updater rewrites Linux even for an app-only change. The package
therefore verifies and carries the repository's reviewed kernel and base
rootfs. It rebuilds a temporary rootfs with the selected root password and
never modifies the checked-in image. The MCU member is generated as an exactly
zero-byte file so the guarded no-MCU-update path is retained. Do not remove
these checks or substitute different platform images without reviewing and
testing the complete update path.

No external vendor firmware bundle is required. Override `MT11_KERNEL`,
`MT11_ROOTFS`, `MT11_UPDATE_CONFIG` or `MT11_PACKAGE_OUT` only for controlled
testing. `MT11_VERSION` and `MT11_GIT_HASH` are also exposed for reproducible
non-Git builds, but must use a `vX.y` or `vX.y.z` version and a six-lowercase-hex
hash. A package name must retain the `MT11_FW_*.bin` form for the camera's update
scanner. As with a vendor update, loss of power while kernel or rootfs is being
erased remains a recovery risk; the builder does not install or reboot the
camera.

## Camera definition files

The camera advertises `mftp://[;comp=100]/camera.xml` in
`CAMERA_INFORMATION.cam_definition_uri`. The XML is available directly over
MAVLink FTP, including TCP, UDP, UART and SupportProxy links. It needs no web
login, internet connection or separately hosted definition file. The FTP service
is read-only. Besides the immutable definition, its root exports the card's
recording, capture and log roots as `/record`, `/capture` and `/logs`
(`CAMERA_APP_RECORD_ROOT`, `CAMERA_APP_CAPTURE_ROOT` and `CAMERA_APP_LOG_ROOT`),
so recordings, photos and BIN logs can be listed and downloaded with
`ftp list /record` and `ftp get /record/<file>`; nothing outside those roots
is reachable. Directory listings support both standard listings and MAVProxy's
listing-with-time extension (the definition reports an unknown time). Burst
reads send up to 64 packets per request on network links and 8 on the UART.
In MAVProxy, use `set target_component 100` followed by `ftp list` to discover
the exported files; `camera select` alone does not change the FTP module's target.
When connecting through an autopilot, forwarding must be enabled on its camera
link (`MAVn_OPTIONS` must not have the `NO_FORWARD` bit set).

`make camera-definitions` generates `build/camera-definitions/{mt11,a8,zr10,z1mini}.xml`
for inspection or loading into a GCS. `make release` also generates these files.
The exporter and firmware use the same C metadata in
`src/protocol/camera_definition.cpp`, with resolution/codec options from the
configuration validator and capability flags from `include/apcam/target_*.h`.
There are no independently maintained XML copies to become inconsistent with a
camera or its simulator. `CAMERA_INFORMATION.cam_definition_version` is computed
once at startup as CRC-16/CCITT-FALSE (polynomial 0x1021, initial value 0xffff)
of the exact XML bytes served over MAVFTP, excluding the terminating NUL.
A zero CRC is mapped to 0xffff because MAVLink reserves zero for an unknown
version. Definition changes therefore update the cache version automatically;
no manual version bump is required. The XML document revision is separate and
is not replaced with the CRC, avoiding a self-referential checksum.

The camera component implements binary little-endian `PARAM_EXT_REQUEST_LIST`,
`PARAM_EXT_REQUEST_READ`, `PARAM_EXT_VALUE`, `PARAM_EXT_SET` and `PARAM_EXT_ACK`.
Names, types and permitted values match the XML. Lists are paced, reads accept
names or indices, and writes reject unsupported types, out-of-range values and
non-finite floats. The original `PARAM_*` service remains available separately.

Live controls include zoom multipliers on MT11/A8, MT11 RGB lens and
video-source selection, thermal palette/gain and autofocus. Live controls report
running state and are not persisted. Autofocus is an action that returns to Idle
when read. ZR10 has native rate zoom without reliable absolute readback, so its
XML does not advertise absolute zoom. A8 has a fixed-focus lens; focus commands
are compatibility no-ops and are not advertised. Z1-Mini advertises its
available recording/stream profiles and automatic recording. Camera mode is
omitted from the XML because selecting Photo or Video does not change capture
behaviour.

XML zoom controls use lens-relative multipliers in 0.1x steps. A8 exposes
1–6x digital zoom. MT11 exposes `CAM_ZOOM` (wide digital, 1–10x) and
`CAM_OPT_ZOOM` (optical, 1–3.2x); the lens selector shows the applicable slider.
Each lens retains its zoom, including across pipeline reconfiguration. Here 1x
means that lens at its widest setting, not the wide camera's FOV. The zoom
lens's nominal baseline FOV remains an estimate pending optical calibration.
MAVLink range commands and `CAMERA_SETTINGS.zoomLevel` retain the required
0–100 percent scale over the selected lens's range. Range, step and rate zoom
commands preserve both the selected RGB lens and the thermal/RGB stream source.
SITL models MT11's 0.1x optical calibration steps, rounding up like the lens
controller. Continuous zoom accumulates an unquantised target so slow rates
work in both directions; repeated rate commands preserve fractional progress.
Direct zoom commands and lens changes cancel the previous rate command.
SIYI vendor commands retain their original automatic hybrid lens selection.
Optical initialization configures the MT11 lens pinmux, power and SPI before
homing; if initialization fails, optical requests fail rather than substituting
digital zoom. The wide lens remains usable.

Recording policy, photo scope and supported image settings apply immediately
and are persisted. Selecting While Armed uses the latest selected-system,
component-1 heartbeat, including when the vehicle was already armed before the
setting changed. Selecting Enabled starts recording; selecting Disabled stops it.

Stream/recording resolution and codec changes reopen the media pipeline without
restarting camera-app or disconnecting MAVLink. Video clients may need to
reconnect. Changes during a recording are saved and applied after recording
stops, as with web UI saves. Parameter reads report the saved format selection;
a status message explains when application is deferred. Failed immediate
application or persistence returns a failed ACK and attempts to restore the
previous configuration. Image controls are available
on MT11/A8; thermal controls and photo scope are available only on MT11. SITL
simple streams support codec changes; the 3D terrain renderer supports H.264
only and rejects H.265 rather than reporting an unapplied setting.

With a MAVProxy version supporting camera definitions:

```text
module load ftp
module load camera
camera select 42:100
camera definition
camera params
camera custom
camera param REC_AUTOSTART 2
camera param CAM_ZOOM 25
```

Replace `42` with the selected vehicle system ID. Definition discovery and
parameter fetch are automatic once camera information is received. `camera
custom` opens the settings dialog; the CLI commands exercise the same controls.
`camera definition build/camera-definitions/mt11.xml` can load a generated local
copy instead of downloading it.

Run `make camera-definition-test` for XML capability and FTP protocol checks.
After building the four SITL targets, `python3 sitl/test_camera_definition.py`
checks downloads, get/list/set, live state and persistent settings using both
pymavlink and the installed MAVProxy CLI. It uses isolated ports and runtime
files, with transcripts under `build/camera-definition-test/`.

The format and transport follow the [MAVLink camera definition specification](https://mavlink.io/en/services/camera_def.html),
[extended parameter protocol](https://mavlink.io/en/services/parameter_ext.html)
and [FTP protocol](https://mavlink.io/en/services/ftp.html).

### Video overlays

`[overlay] cross` (`OSD_CROSS`) enables an outlined diagonal targeting cross
with an open centre. `[overlay] thermal_fov` (`OSD_THERMAL_FOV`, MT11 only)
shows a dashed thermal field-of-view boundary on the RGB image. Both default
to false and apply immediately through the Video tab, MAVLink parameters,
camera.xml extended parameters, or INI reload, including during recording.
They appear in live streams. On MT11, A8 and ZR10, `[overlay] recording`
(`OSD_RECORD`, "Overlays in recordings") independently enables burning them
into SD recordings. It defaults to false, keeping recordings clean; changes
apply even during recording. The cross also appears on thermal video; the
FOV box appears only on RGB. Z1-Mini currently shares its 1080p live encoder
with recording and does not expose this toggle. The optional native helper
supports a cross in both live video and recordings; the retained vendor-ISP
pipeline cannot draw overlays and logs when `OSD_CROSS` is enabled.

The box uses tangent-space lens projection, current RGB lens/zoom, and the
thermal sensor's native aspect ratio. A stretched thermal stream does not
change its physical FOV. Boundaries outside the RGB view are clipped, rather
than moved onto the edges. This is nominal co-aligned geometry: it does not
compensate for lens misalignment, distortion or near-field parallax.

MT11 uses encoder regions (one small cross bitmap and up to four narrow box
strips); A8 uses scaler output port regions, as the vendor application does,
and ZR10 uses VPE output regions. No video is decoded or re-encoded for
overlays. ZR10 photos using a shared VPE output, and A8 photos taken from the
main stream's scaler port, can also include the cross. The Z1-Mini native helper patches only the cross's
small NV12 region before encoding, using uncached mappings, without copying
whole frames or inserting a processing queue. These hardware integrations
need camera testing; SITL pixel, parameter and recording regressions run in CI.

SITL uses the same C geometry as hardware and caches its sparse overlay pixels.
Separate RGB/thermal recording encoders run only while recording, so stream
overlays can be selected independently without changing the live image.
The hardware/SITL raster comparison is `tests/test_overlay.py`; live encoded
video checks are included in `make sitl-image-controls-test`.

### MAVLink control BIN logging

BIN logging runs while armed, or on the bench with `LOG_DISARMED=1`
(`[logging] disarmed=true`). All incoming `COMMAND_LONG` and `COMMAND_INT`
requests handled by the MAVLink dispatcher now produce one `MAVC` record with
the original parameters and final command result, replacing the old separate
`CMD` request/ACK records. Unsupported commands and requests addressed to other
components are also recorded while logging is active.

Every control request record includes `SS`/`SC` (source system/component) and
`TS`/`TC` (target system/component). These are the IDs from the received packet,
including broadcast targets, regardless of whether it arrived over UART, TCP,
UDP or SupportProxy. They identify the MAVLink sender, not necessarily the
original GCS if a flight controller generates a new command on its behalf.

| Record | Contents |
| --- | --- |
| `MAVC` | Command number, source/target IDs, frame, seven parameters, result and `WL` (1 for LONG, 0 for INT). |
| `GMBC` | `GIMBAL_DEVICE_SET_ATTITUDE` source/target IDs, flags, original quaternion, all three angular rates (rad/s), and local handling result. Includes angle/rate, neutral/retract, manual-control lockout and unavailable vehicle-attitude cases. |
| `MAVP` | `PARAM_SET` / `PARAM_EXT_SET` source/target IDs, name, parameter type (`PT`), extended-protocol flag (`Ext`), requested numeric value (`Val`), raw value bytes and handling result. Includes rejected and unknown parameters. |
| `MAVH` | Flight-controller heartbeats consumed for armed-state recording: source IDs, custom/base mode, vehicle/autopilot type, system status and MAVLink version. Written before the resulting recording change and before stopping a disarmed log. |

`MAVC` follows ArduPilot's field names, with double-precision `X`/`Y` so both
INT coordinates and LONG floating-point parameters are preserved. With `WL=0`,
`X`/`Y` are the original unscaled integers; with `WL=1`, they are the original
P5/P6 values, including fractions and NaNs. `Z` is P7. LONG has no coordinate
frame, so `Fr=255`. `Res` uses `MAV_RESULT`; 255 means ignored/no command ACK.
`GMBC.Res` uses the same result values for local disposition and does not add a
MAVLink ACK. Acceptance describes command handling, not confirmed physical motion.

`MAVP.Res` uses `PARAM_ACK` values (0 accepted, 1 unsupported, 2 failed) and 255
for ignored requests. For ordinary `PARAM_SET` this is a local result; the wire
protocol still replies with `PARAM_VALUE`. `Raw1` and `Raw2` are DataFlash `a`
fields (32 little-endian int16 values each): concatenating their bytes recovers
the entire 128-byte extended value. For ordinary writes, the first four bytes
hold the received float and the remainder is zero. `Val` decodes the supported
UINT8/INT32/REAL32 extended types; other types retain their raw bytes and use NaN.
The write enabling `LOG_DISARMED` is included in the newly started log.

Vehicle `ATT` and `POS` records also include `SS`/`SC`. In `ATT`, `Src` still
identifies the telemetry type (1 autopilot gimbal state, 2 attitude), and the
vehicle boot-time field is named `TBoot` (milliseconds, formerly `BootMS`) to
fit the DataFlash column limit. Existing `GCMD`, `GIMB`, `CAM`, `VID`, `MODE`,
`ROI`, `PARM` and `PRMA` records remain available for backend commands,
feedback and resulting state, including non-MAVLink controls. Those outcome
records do not attribute asynchronous or locally generated actions to a sender.

Run `make -C camera_app mavlink-binlog-test` to exercise the live dispatcher
and decode the resulting BIN file with pymavlink. It checks distinct TCP/UDP
sender identities, precise command parameters, raw extended values,
accepted/rejected/ignored controls, recording and vehicle telemetry.

### SIYI and XFRobot packet BIN logging

While BIN logging is active, `SIIN`/`SIOU` record incoming/outgoing SIYI packets
and `XFIN`/`XFOU` record incoming/outgoing XFRobot packets, relative to the
camera. This covers the public SDK service and the private camera-to-gimbal
link on MT11, A8, ZR10 and Z1Mini. With `LOG_DISARMED=1`, logging starts before
the backend opens so its startup commands are included.

Incoming records contain complete, CRC-valid frames, including keepalives,
unknown commands and commands rejected by control policy. Outgoing records
capture send attempts or queue submissions once per packet, rather than once
per partial write. `Res=0` means received or successfully sent/queued locally;
negative values are send errors (`-errno`), not a remote acknowledgement.
Later failures of queued transmissions do not change the record. Corrupt or
incomplete input is not included; use network capture for those wire bytes.

| Field | Meaning |
| --- | --- |
| `TimeUS`, `Id` | Local monotonic time in microseconds and packet ID shared by all chunks of one packet. |
| `Lnk` | 1 public UDP, 2 public TCP, 3 public UART, 4 private MCU UART, 5 private MCU UDP (SITL). |
| `Proto` | 1 public SIYI, 2 MT11 private, 3 A8/ZR10 private SIYI, 4 public XFRobot, 5 Z1Mini private XFRobot, 6 legacy SIYI network (CRC32). |
| `IP`, `Port` | Public network peer, in host byte order (`127.0.0.1` is `0x7f000001`); zero for UART and private links. |
| `Src`, `Dst` | Private SIYI source/destination IDs; zero where the protocol has no such fields. These are not MAVLink system IDs. |
| `Cmd`, `Seq` | Wire command and sequence, or 65535 when absent. |
| `Len`, `Ofs` | Total frame length and this chunk's byte offset. |
| `D1`, `D2`, `D3` | Three DataFlash `a` arrays holding up to 192 raw bytes, zero padded in the final chunk. |

Unlike MAVProxy's decoded SIYI `P1`/`P2` fields, these records preserve the
entire frame, including headers, payload and CRC, without needing to know its
command layout. For a pymavlink record `m`, recover a chunk with
`struct.pack('<96h', *m.D1, *m.D2, *m.D3)[:min(192, m.Len-m.Ofs)]`.
Group records by `Id` within a log and concatenate chunks in `Ofs` order;
check offsets are contiguous and the result contains exactly `Len` bytes.
Tunneled SDK frames are also retained inside their private transport frames.
All chunks are queued together; overload drops a whole packet and increments
`STAT.Dropped` by its number of chunks. An interrupted file can still end in
an incomplete packet, which should be discarded during reconstruction.

Run `make -C camera_app vendor-binlog-test` to compare reconstructed bytes
against live TCP/UDP/UART traffic on all four simulated camera backends,
including large packets, private frames, startup traffic and send errors.

### System health BIN logging

While BIN logging is active, `SYS` records are written every five seconds by
its background writer. `CPUTemp` is the SoC temperature in degrees Celsius;
`CPULoad` is aggregate CPU busy percentage (0–100) over the preceding sample
interval, excluding idle and I/O wait. `MemFree` and `MemAvail` are Linux free
and available RAM in bytes; `SDFree` is available space in bytes on the volume
holding the log (microSD on hardware, the runtime filesystem in SITL).

`Valid` bits 0–4 mark temperature, CPU load, free RAM, available RAM and storage
space respectively. Unavailable temperature/load values are NaN; unavailable
memory/storage values are zero with their validity bit clear. MT11 uses its
three on-die sensors and Z1-Mini uses its kernel thermal sensor. A8, ZR10 and
SITL currently report temperature unavailable. Older kernels without
`MemAvailable` still report `MemFree`. No sensor or filesystem polling runs on
the gimbal control thread.
