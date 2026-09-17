# AP_CameraGimbal SITL

The simulation runs the real host-built camera application and web service
with hardware media stubs. A separate Python process models the MT11, A8 or
ZR10 gimbal MCU and exchanges that camera's private UART frames with `camera-app`
over connected UDP. The A8 model reproduces its distinct upright/inverted
pitch, yaw and yaw-rate wire representations, including private `0x50` yaw in
the `0..360` degree interval.
Public SIYI clients still use the normal UDP or TCP camera endpoint, so commands
cross both protocol layers exactly as they do on the camera.

Build and test either camera from the repository root:

```sh
make sitl
make sitl-test
make a8_sitl
make a8_sitl-test
make zr10_sitl
make zr10_sitl-test
make sitl-live-tracking-test
```

The live tracking test uses isolated ports and files, and requires the usual
MAVLink/video test dependencies. It checks web saves against the
running app, rate tracking, stale-data stops and video-format changes deferred
until recording stops.

The **Tracking control method** setting (`TRACK_METHOD` in MAVLink, or
`[mavlink] tracking_method=angle|rate` in `camera.ini`) selects how the camera
tracks a geographic ROI. Angle is the default. Rate combines predicted target
motion with filtered pointing-error correction, and stops on stale vehicle or
gimbal telemetry. It does not change explicit mount-angle/rate commands.

The camera app checks for configuration-file changes every 500 ms. Image
controls, capture scope, thermal palette, automatic recording, timezone and
tracking settings apply without an app restart. Video formats reopen the media
pipeline, briefly reconnecting streams; during recording they remain pending
until recording stops. Mount orientation, transport/identity and SupportProxy
settings require restart. The Parameters page reports the app's acknowledgement
of the saved configuration, including errors and deferred changes. External
editors should replace the INI atomically, as the web UI does.

Both `sitl-test` and `a8_sitl-test` run the complete camera/backend/web stack
once upright and once inverted. The A8 cases begin at a physical yaw of -38
degrees. Every case commands a positive/right yaw rate and requires the web UI
to report both yaw and yaw rate increasing, including the MT11 conversion from
its left-positive `0x0D` wire representation.

Start all three processes in the foreground with:

```sh
make sitl-run
make a8_sitl-run
```

Alternatively, use the PyQt desktop launcher from the repository root:

```sh
./sitl_launch.py
```

Choose **1–4 simulators** (default 1), then select **MT11 / A8 / ZR10 / Z1-Mini**,
**Normal / Inverted**, and **Simple test patterns / 3D terrain and imagery** in
each simulator's tab. Repeated camera types are supported. **Start all** builds
the cameras sequentially, then leaves them running together. **Stop all** or
closing the window stops the entire group, including cameras restarted through
the web UI. A startup failure stops the group and leaves the logs in each tab.
Each tab has its own **Open Web UI**, build output and service logs.

Fresh simulator slots use these defaults:

| Simulator | Web | MAVLink TCP/UDP | RTSP | Live video | Camera/gimbal components |
|---|---:|---:|---:|---:|---|
| 1 | 8081 | 14550 | 8554 | 8555 | 100 / 154 |
| 2 | 8082 | 14560 | 8564 | 8565 | 101 / 171 |
| 3 | 8083 | 14570 | 8574 | 8575 | 102 / 172 |
| 4 | 8084 | 14580 | 8584 | 8585 | 103 / 173 |

Vendor protocol ports start at 37260 for SIYI cameras or 2337 for Z1-Mini,
plus the slot number minus one. Internal gimbal sockets use automatically
allocated ports. Existing MAVLink port and component settings are preserved;
conflicting saved settings are reported before starting the group.

Slot 1 keeps its existing build/runtime directory. Additional slots use
`<build>/instance-<number>/<camera>/runtime`, with separate parameters, login
settings, recordings and logs. **Clear parameters on launch** applies only to
that tab. The launcher number seeds port and component defaults when creating
or explicitly resetting a configuration, so web changes remain effective.

Windows users can run the standalone installer or portable ZIP without installing
Python, Cygwin or FFmpeg. See [Windows packaging and usage](../windows/README.md).

The launcher supports PyQt6 or PyQt5 (for example, install `python3-pyqt6` on
Debian/Ubuntu). Terrain mode automatically uses `build/terrain-venv/bin/python`
when present, or the `CAMERA_GIMBAL_SITL_PYTHON` override. The port and MAVLink
environment overrides described below still apply as slot-1 base ports, with
the same offsets for later slots (MAVLink port 0 stays disabled). A8 launched
from the GUI enables MAVLink; standalone `make a8_sitl-run` retains its existing
disabled default. An explicit `CAMERA_GIMBAL_SITL_BUILD` directory is used for
both build and launch, with numbered subdirectories for additional slots.
Test the GUI without a display using
`QT_QPA_PLATFORM=offscreen python3 sitl/test_launch.py`.
Add `--real` to build and launch both real backends in isolated temporary
directories, checking mounting orientation, web restarts and process cleanup.
`QT_QPA_PLATFORM=offscreen python3 sitl/test_launch_multi.py` builds and runs
four MT11s, then all four camera types, checking concurrent web servers, video,
MAVLink identities and cleanup on isolated test ports.

Keep that command running and press Ctrl-C in the same terminal to stop all
three processes. For MT11, another terminal can use `make sitl-kill`. The kill
target matches this repository's exact MT11 SITL executable and script paths
and is safe when SITL is already stopped. A per-model launcher lock prevents
accidental duplicate instances.

The default endpoints are:

- web UI: `http://127.0.0.1:8081/` (login page; username `admin`, password
  `ardupilot`; English, Simplified Chinese or Japanese);
- public SIYI API: UDP and TCP port 37260; and
- H.264 RTSP test streams: `rtsp://127.0.0.1:8554/video1` and `/video2`;
- MAVLink 2 camera/gimbal service: UDP and TCP port 14550; and
- simulated private gimbal link: an automatically selected free UDP port,
  printed by the launcher.

The writable camera filesystem and logs are under `build/sitl/runtime` for
MT11 and `build/a8-sitl/runtime` for A8. The web UI can change its simulated
`camera.ini`, password and media tree without
touching host `/app`, `/mnt`, `/run` or `/dev`. Rebuilding preserves these
runtime settings; `make sitl-clean` resets the complete simulated filesystem.
Firmware uploads remain inside its simulated microSD tree and reboot requests
are explicitly ignored.

Both Live and RTSP streams are generated H.264 test patterns using the same
in-process RTSP implementation as the hardware build. Both video sources use
the configured stream resolutions: 1920x1080 main and 1280x720 sub by default.
A8 shows the same visible scene in both encodings. MT11 thermal video stays at
1280x720, matching the real camera; selecting thermal as main sends visible
video at the configured substream size. A separate visible encoding keeps its
recording dimensions stable during that swap. Simple fixtures loop through the
same live image-processing and encoding stage as terrain video. The MT11 thermal
stream uses the selected palette; its Sensors tab receives changing simulated
LiDAR ranges and fixed thermal extrema. Shutter captures save processed JPEGs.
Rate, centre and absolute-angle commands update the simulator's bounded gimbal
state and are observable through SIYI attitude queries.

## Optional 3D terrain video

Set `CAMERA_GIMBAL_SITL_VIDEO=terrain` for satellite imagery draped over the
same ArduPilot quantized meshes used by MAVProxy's map3d module. The default
remains `simple`, with generated test streams. All four targets support terrain.
Both sources apply camera image controls before encoding live video and recordings.

Simple video needs NumPy, OpenCV and PyAV, without VTK or map3d. The Windows
package includes them. On Linux, the build-environment installer installs the
system packages, or use a virtual environment:

```sh
python3 -m venv --system-site-packages build/terrain-venv
build/terrain-venv/bin/pip install -r sitl/requirements-video.txt
```

The launcher, `make` and `sitl/run.sh` automatically use this environment when
present; `CAMERA_GIMBAL_SITL_PYTHON` overrides it. Direct camera-app invocations
should set that variable when the dependencies are not in the system Python.

Install a recent MAVProxy with `mavproxy_map3d`, plus the optional renderer
dependencies. A virtual environment can reuse an already installed MAVProxy:

```sh
python3 -m venv --system-site-packages build/terrain-venv
build/terrain-venv/bin/pip install -r sitl/requirements-terrain.txt
# If needed, install your current MAVProxy checkout into this environment:
# build/terrain-venv/bin/pip install -e /path/to/MAVProxy

CAMERA_GIMBAL_SITL_VIDEO=terrain \
CAMERA_GIMBAL_SITL_PYTHON="$PWD/build/terrain-venv/bin/python" make sitl-run
# Use make a8_sitl-run for A8; explicitly enable its MAVLink port as above.
```

The renderer uses map3d's terrain decoder, mesh rendering, imagery fetching
and on-disk caches. Its terrain endpoint currently resolves to
[`https://plot.ardupilot.org/quantized`](https://plot.ardupilot.org/quantized/layer.json),
the quantized representation of ArduPilot terrain. It uses that endpoint from
the installed map3d module, rather than fetching flat SRTM height images.
Imagery defaults to **Esri World Imagery**; `CAMERA_GIMBAL_SITL_IMAGERY` can
select an existing MAVProxy tile service, such as `MicrosoftSat` (map3d's
current default). First use needs network access; subsequent runs reuse the
normal MAVProxy tile cache.

Send the simulated vehicle's MAVLink stream to the camera. Position and AMSL
altitude come from the selected flight controller, and the view uses the
simulated gimbal's level-referenced roll/pitch and vehicle-relative yaw.
Missing or stale telemetry produces a waiting screen. Gimbal controls and
optical/digital zoom alter the rendered view and its advertised FOV. A8's two
streams encode the same visible scene at their configured resolutions; MT11's
thermal view uses synthetic temperature and the selected palette with the
thermal sensor's FOV, **not calibrated temperatures**. Still photographs capture
the processed scene for each requested lens.

The encoded video runs through the ordinary RTSP, web Live, recording and
SupportProxy paths, including per-frame telemetry. The renderer is a separate
child of camera-app and is stopped with it. Web-triggered camera restarts
inherit the selected video mode. Renderer logs report the most recent ten
seconds of frame rate and render/encode p95 and maximum time, so short stalls
are visible rather than hidden by a lifetime average.

Terrain video defaults to **20 fps** for both cameras; override with
`CAMERA_GIMBAL_SITL_FPS` (1–60). The camera requests `GLOBAL_POSITION_INT` and
`AUTOPILOT_STATE_FOR_GIMBAL_DEVICE` at 10 Hz from the selected flight controller,
refreshing the requests every five seconds even when slower telemetry is present.
It also requests `SYSTEM_TIME` at 1 Hz to correct an unset camera date.
The latter provides the preferred vehicle attitude; `ATTITUDE` is a metadata
fallback after one second without gimbal-state updates.

Terrain rendering extrapolates position with NED velocity and orientation with
angular rates to the frame time, without delaying video to interpolate future
samples. Prediction is limited to 250 ms on link loss. Small telemetry corrections are
blended into the velocity prediction over 150 ms, avoiding packet-arrival
jitter without buffering future telemetry. Frames are rendered ahead of their
50 ms presentation deadlines and published on that cadence. Two queued encoded
frames plus the frame awaiting display provide about 150 ms of render headroom
at 20 fps, so a short texture upload does not pause playback. Prediction remains
capped at 250 ms; the frame queue cannot grow without bound. Gimbal Euler rates are
estimated from successive timestamped samples with angle wrapping. Recorded SEI
also includes optional NED `velocity` and vehicle `yaw_rate_rad_s` fields; old
telemetry readers continue to work.

Prefetch and performance settings:

| Environment variable | Default | Meaning |
| --- | --- | --- |
| `CAMERA_GIMBAL_SITL_VIDEO` | `simple` | `simple` or `terrain` |
| `CAMERA_GIMBAL_SITL_PYTHON` | `python3` | Python interpreter with renderer dependencies |
| `CAMERA_GIMBAL_SITL_TILE_THREADS` | `16` | Concurrent imagery downloads, 1–64 |
| `CAMERA_GIMBAL_SITL_PREFETCH_RADIUS` | `2` | Fine terrain tile radius around the view, vehicle and predicted position, 1–4 |
| `CAMERA_GIMBAL_SITL_IMAGERY` | `EsriWorldImagery` | Satellite imagery service |
| `CAMERA_GIMBAL_SITL_FPS` | `20` | Terrain video frame rate, 1–60 |

Six terrain workers fetch/decode meshes and assemble textures, alongside the
imagery download pool. Prefetch covers the current view and a ten-second
flight prediction (limited to 3 km), with coarser terrain beyond the fine
region. Completed imagery is applied progressively, with at most one new mesh
or texture applied per frame, since VTK defers OpenGL uploads until rendering.
Texture sizes use stable 256/512/1024/2048-pixel levels with
hysteresis; flying a few metres does not rebuild textures at a different
single-pixel width. Idle terrain result processing is skipped. A fixed-size
framebuffer with per-stream viewports avoids reallocating OpenGL buffers for
different stream sizes. Thermal grayscale conversion uses OpenCV. Two encoder
workers operate independently of downloads; smaller visible encodings reuse
the larger render. The camera allows one outstanding
render request, with at most two completed frames queued for presentation.
RTSP and web live-video TCP connections use `TCP_NODELAY` so a frame's final
packet is not held back by TCP acknowledgement delays.

VTK needs a working OpenGL implementation; recent VTK wheels can render
offscreen with EGL or OSMesa, while older builds may need an X display/Xvfb.
Hardware acceleration helps at 1080p. On slower software renderers, lower the
frame rate or visible stream resolutions. Initial uncached terrain appears
progressively and cannot be guaranteed complete before imagery downloads.

Optional tests (not part of the ordinary offline SITL tests):

```sh
make sitl-terrain-test SITL_TERRAIN_PYTHON="$PWD/build/terrain-venv/bin/python"
make a8_sitl-terrain-test SITL_TERRAIN_PYTHON="$PWD/build/terrain-venv/bin/python"
build/terrain-venv/bin/python sitl/test_terrain_video.py --network \
    --seconds 35 --output /tmp/terrain-video-check
```

The geometry/encoding test uses synthetic meshes and delayed worker jobs
without downloads. The integration tests use real terrain, camera-app, the
gimbal simulator, RTSP and MP4 recordings. The optional `--network` benchmark
writes two rendered PNGs and frame timing statistics while flying over real
terrain; `--a8 --fps 20 --width 1920 --height 1080` exercises a larger view.

The MT11 launcher accepts these environment overrides:

```text
MT11_SITL_WEB_PORT       default 8081
MT11_SITL_CAMERA_PORT    default 37260
MT11_SITL_GIMBAL_PORT    optional fixed port; default is OS-selected
MT11_SITL_RTSP_PORT      default 8554 (native web video uses the next port)
MT11_SITL_MAVLINK_TCP_PORT default 14550; 0 disables TCP
MT11_SITL_MAVLINK_UDP_PORT default 14550; 0 disables UDP
MT11_SITL_ORIENTATION    upright or inverted; default upright
```

The A8 launcher has the equivalent `A8_SITL_WEB_PORT`,
`A8_SITL_CAMERA_PORT`, `A8_SITL_GIMBAL_PORT`, `A8_SITL_RTSP_PORT`,
`A8_SITL_MAVLINK_TCP_PORT`, `A8_SITL_MAVLINK_UDP_PORT` and
`A8_SITL_ORIENTATION` variables. Its MAVLink ports default to zero so a
standalone A8 simulation cannot accidentally accept gimbal targets broadcast
by another ArduPilot instance on port 14550. Set one or both port variables to
14550 when deliberately testing MAVLink integration.

The MT11 MAVLink ports normally come from the persistent simulated
`app/camera.ini`, whose initial values are 14550. Setting either model's
MAVLink launcher variable explicitly overrides that INI value for the process
and for web-triggered restarts; leave MT11 variables unset when changing its
ports in the web UI. A8 always uses its isolated launcher defaults unless the
variables are explicitly set.

For example:

```sh
MT11_SITL_ORIENTATION=inverted MT11_SITL_WEB_PORT=8081 make sitl-run
A8_SITL_ORIENTATION=inverted A8_SITL_WEB_PORT=8081 make a8_sitl-run
```

`gimbal_sim.py` can also be launched independently with `--backend mt11` or
`--backend a8`. `camera-app` selects it with `--uart udp://IPv4:PORT` (with
`localhost` also accepted), or equivalently with `CAMERA_APP_UART`. Physical
operation continues to use each backend's normal UART device.

Test MAVLink system-ID selection and the numeric parameter service with the
actual MAVProxy CLI:

```sh
make sitl-mavlink-test
make a8_sitl-mavlink-test
```

These require `pymavlink`, `MAVProxy` (`mavproxy.py` on PATH), and `pexpect`.
They run camera-app and the gimbal model with isolated temporary files and
ports, check MAVProxy fetch-all/get/set, TCP/UDP addressing, invalid values,
16-character names, and persistence through fixed/automatic ID restarts.
The console transcript, fetched parameter file, telemetry and process logs
are retained in the temporary directory printed by the test. To choose the
log directory, run `python3 sitl/test_mavlink_parameters.py --output PATH`
(with `--backend a8` for A8).

The default automatic system ID waits for a flight controller heartbeat; a
standalone MAVProxy GCS cannot select it. Set **MAVLink system ID** to 1–255
in the web UI and restart camera-app when testing without a flight controller.
The parameter tests supply simulated flight controller heartbeats themselves.
The **MAVLink camera component ID** setting defaults to Camera 1 (100) and
supports Camera 2–6 (101–105), applying after restart. The parameter test also
checks component 105 and rejection of requests to the old camera ID. To test
MAVProxy camera definitions and extended controls with Camera 6:

```sh
make camera-definitions sitl
python3 sitl/test_camera_definition.py --targets mt11 --component 105
```

See [the parameter table](../camera_app/README.md#mavlink-camera-and-gimbal-services)
for names, enum values and restart behavior.

Armed recording tests exercise all three automatic recording modes, automatic
and configured system IDs, already-armed startup, unrelated systems/components,
repeated heartbeats, rearming, and finalized MP4 recordings:

```sh
make sitl-armed-recording-test
make a8_sitl-armed-recording-test
```

Video telemetry tests use the real camera application, gimbal model, RTP
packetizer and MP4 muxer with generated H.264/H.265 fixtures:

```sh
make sitl-video-telemetry-test
make a8_sitl-video-telemetry-test
```

They require `pymavlink` and an FFmpeg build with libx264/libx265. The tests
check both RTSP streams for one SEI per access unit, correct RTP timestamps
and marker bits, telemetry changes following MAVLink updates, clean video
decoding, absent/stale sources, and the recorded MP4 frame and telemetry
timestamps. Zoom checks cover 1x, 2x, 4x and 5x, the MT11 wide/tele
crossover, constant thermal FOV, and visible/thermal source swapping. They
compare per-frame FOV with MAVLink stream information and status.
Each run retains media and logs in a printed temporary
directory; `sitl/test_video_telemetry.py --output PATH` chooses its location.

SITL recording now writes real MP4 files from its video fixtures (both MT11
streams; the main A8 stream). The first recorded frame is a keyframe.
Recordings use the configured recording resolution and, on MT11, 1280x720 thermal.
A separate H.264 encoder is enabled when the RGB stream resolution or codec
differs from recording. H.265 fixtures and H.265 RGB streams can be recorded
through this encoder as H.264, with the same current image adjustments.
Hardware continues to use its dedicated H.264 recording channels.

`make recording-recovery-test` runs the real MP4 writer and Files HTTP endpoint
in an isolated runtime. It downloads before close, checks growing immutable
prefixes and fragment write locking, kills the recorder with SIGKILL, and tests
truncation inside the final MP4 boxes. FFmpeg verifies decoded frames and their
telemetry. This simulates interrupted writes, not physical SD-card power loss.

`make -C camera_app test` also blocks the background recording sync deliberately
while writing more frames. It verifies that writes and download locks continue,
sync requests coalesce, close waits for the final flush, and I/O errors propagate.

## SupportProxy integration tests

These tests launch a real SupportProxy with a temporary key database and
isolated ports, alongside camera-app and the gimbal simulator. They require a
built SupportProxy checkout (by default the sibling `../SupportProxy`), its
Python key-database dependencies, `pymavlink`, and FFmpeg with libx264/libx265.

```sh
make sitl-supportproxy-test SUPPORTPROXY_ROOT=/path/to/SupportProxy
make a8_sitl-supportproxy-test SUPPORTPROXY_ROOT=/path/to/SupportProxy
```

The MT11 suite checks signed bidirectional MAVLink, local TCP and UDP links,
camera parameters and advertised proxy video URLs, password and session-based
publishing, H.264/H.265 video decoding, and per-frame telemetry/FOV. It also
tests disabled, video-only and single-video configurations, and reconnection
after a proxy restart while local recording continues. The A8 target runs the
signed MAVLink and two-stream H.264 case. Outputs are retained in the printed
temporary directory; `sitl/test_support_proxy.py --output PATH` chooses it.
The host signing unit test additionally rejects forged, unsigned and replayed
MAVLink packets. The video queue regression test delays the RTSP handshake,
forces both frame-count and byte-count limits, and verifies keyframe recovery
without closing the established connection.

Camera SITL skips network address/route changes. Test the production network
configuration code separately inside a disposable network namespace:

```sh
python3 sitl/test_network.py
# If unprivileged network namespaces are unavailable:
python3 sitl/test_network.py --sudo
```

This creates dummy interfaces and checks primary address replacement, optional
secondary addressing, default routes, removal, repeated startup and rollback
after failure. IPv6 and other interfaces must remain unchanged. The `--sudo` variant requires passwordless sudo for
`unshare`; neither variant changes the host network.

SITL preserves the selected backend's `runtime/app/camera.ini` across builds,
web restarts and launcher stop/start. **Clear parameters on launch** is unchecked
by default. Selecting it restores that camera's template on the next launch,
backs up the previous settings to `camera.ini.reset.bak`, and keeps recordings,
photos and web credentials. Command-line equivalent:
`CAMERA_GIMBAL_SITL_RESET_PARAMETERS=1 make sitl-run` (or `make a8_sitl-run`).

`make sitl-buffering-test` verifies frame cadence with injected 140 ms renderer
stalls and verifies that stopping a full render queue cleans up promptly.

## Shared camera properties

The launcher and MCU simulators read the compiler export of the [target headers](../include/apcam/README.md). MT11, A8, ZR10 and Z1-Mini have their own lens and protocol properties. Build Z1-Mini with `make z1mini_sitl` and start it with `make z1mini_sitl-run`, or select it in the PyQt launcher. Live and recording resolutions are independent, including 1080p live plus 4K recording on Z1-Mini.

## Camera definition controls in SITL

The simulator uses the target's real `camera.xml` and the same MAVLink parameter
handlers as hardware. MAVProxy's custom settings sliders update subsequent
frames without restarting the camera or interrupting a recording. The bounded
render queue contributes up to three frames of delay; the viewer adds its own.

- MT11 and A8: brightness, saturation, contrast, exposure compensation, ISO,
  shutter speed, exposure metering and white balance affect RGB pixels.
- MT11: all eleven thermal palettes and both gain modes affect thermal pixels;
  RGB/thermal source selection and wide/zoom lens selection also affect the view.
- Zoom changes terrain FOV and crops simple fixture video using the same target
  lens calibration. MT11 manual focus defocuses RGB; autofocus restores sharpness.
- Recording policy, recording resolution and stream resolutions/codecs use the
  existing live configuration path. Resolution/codec changes rebuild the media
  pipeline; clients reconnect, and changes remain rejected while recording.
  Both software sources support H.264 and H.265 RGB streams, with H.264 recording.
  Thermal stays H.264. Swapping RGB and thermal also reconnects RTSP clients
  when the two sources use different codecs, so the advertised codec stays correct.
- Still captures use the current processed scene at stream resolution, including
  each requested MT11 lens and thermal palette. Photo scope selects thermal only
  or all lenses. Single-lens cameras produce one JPEG; Z1-Mini has no photo control.

Image controls approximate an ISP: brightness adds an offset, contrast scales
around mid-gray, saturation scales chroma, exposure uses one-third-stop steps,
manual ISO scales gain, and manual shutter scales exposure relative to 1/100 s.
Center/spot metering adjusts exposure from the selected region when either ISO
or shutter is automatic. White-balance presets apply RGB gains. Thermal imagery
uses scene luminance as synthetic temperature, with approximate named palettes;
low gain compresses contrast. Focus is a bounded blur, with zero percent sharp.
These models do not reproduce vendor sensor calibration, thermal measurements,
noise, exposure motion blur, focus motors or automatic exposure dynamics. Controls
absent from a target's XML remain absent in its simulator.

Run pixel-level tests of all four cameras (including MAVLink updates, thermal
palette/source changes, autofocus, still captures and recording) with:

```sh
make sitl-image-controls-test
```
