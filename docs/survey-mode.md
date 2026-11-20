# Survey mode (MT11)

The default **Both** pattern captures a repeating nine-view sequence during straight
flight: front-left, front-centre, front-right, down-right, down-centre, down-left,
rear-left, rear-centre, rear-right. Each target is a fixed latitude/longitude on a
terrain surface (or a local plane when only flight-controller terrain is available).
Successive sweeps revisit ground rows from different angles.
The camera compensates for aircraft translation and yaw while exposing each burst.

This implementation needs the matching ArduPilot
[`pr-mavlink-camera2`](https://github.com/tridge/ardupilot/tree/pr-mavlink-camera2)
and MAVProxy [`pr-camera2`](https://github.com/tridge/MAVProxy/tree/pr-camera2)
branches. The camera branch is `pr-survey-mode`. Plane requests and mission
execution use the normal camera ID selection (0 all, 1–6 configured camera instance, or a MAVLink camera component ID).
No custom MAVLink dialect is required on the camera/GCS link.

## Controls

Open web **Parameters → Survey**, select **Raw thermal**, **Wide**, or **Zoom**,
and save parameters. Enable position targeting in System. Start with **Start /
resume survey**, or choose **Survey** in MAVProxy's camera Mode menu. Selecting
Survey starts capture as soon as telemetry, terrain, straight flight and gimbal
ownership are available. Saving survey settings pauses an active survey; resume
after saving. Start/stop buttons use the saved settings.

The status heartbeat uses temporary storage (`/tmp` on cameras, or `TMPDIR`
when set), with a filename derived from the configuration path. It does not
write periodic status updates to camera flash.

Choose **Survey pattern** before saving:

- **Both** (`SURV_PATTERN=0`, INI `pattern=both`): the existing nine views.
- **Left/right only** (`1`, `left_right`): down-left → down-right, without centre,
  forward or rear views. Forward/rear distance parameters are unused.
- **Fore/aft only** (`2`, `fore_aft`): front-centre → rear-centre, without centre
  or lateral views. Forward/rear distances use the configured AGL percentages.
- **Fore-only (three pitch angles)** (`3`, `fore_only`): far → middle → near,
  all straight ahead along the flight path. Targets are at 3×, 2× and 1× the
  forward distance, rounded to common ground rows. The rear distance is unused.
  At the default 50% AGL, the nominal distances are 150%, 100% and 50% AGL
  (approximately 34°, 45° and 63° down), before row rounding and aircraft motion.
  Successive sweeps revisit each ground row at all three pitch angles, without
  a lateral sweep or a deliberate 180° yaw reversal.
- **Fore-wide (three yaw angles)** (`4`, `fore_wide`): front-left → front-centre → front-right,
  Targets are at 1× the forward distance at 3 yaw angles covering left, centre and right, rounded to common ground rows. The rear distance is unused.

Both remains the default for existing configurations. The reduced patterns budget
only their selected views, giving shorter sweeps and closer ground rows. Saving a
pattern change pauses an active survey; use **Start / resume survey** afterward.
The status shows position 1–2, 1–3 or 1–9 as appropriate. Capture metadata preserves
`slot` IDs 0–8 for the original grid, and uses 9/10/11 for far/middle/near
forward pitch views. It also includes `pattern`, zero-based `position`, and
`positions` for the selected sequence.

Equivalent MAVProxy commands:

```
camera mode survey
camera mode photo
camera coverage show
camera coverage hide
camera coverage clear
camera coverage status
camera coverage alpha 0.25
camera coverage load path/to/flight.tlog
```

`wp cameramode survey 100` appends a SET_CAMERA_MODE mission item for camera
component 100 and uploads the mission; add it before the survey flight segment.
`wp cameramode photo 100` ends the segment. In a mission editor, use
`MAV_CMD_SET_CAMERA_MODE`, param1 camera ID, param2 `2` for Survey (`0` Photo,
`1` Video). Direct camera-addressed commands use param1 `0`, param2 `2`.
`camera.xml` also exposes the runtime `CAM_MODE` choice.

`MAV_CMD_IMAGE_STOP_CAPTURE` pauses surveying while retaining Survey mode.
`MAV_CMD_IMAGE_START_CAPTURE` resumes it; its count limits images, not sweeps,
and its interval is a minimum between images. Photo/Video mode exits surveying.
While the survey holds the gimbal, incoming pointing commands are ignored,
including external ROI, SIYI/UniGCS motion, web PTZ and image tracking. ArduPilot
sends no targets while the camera holds the gimbal manager lease. The pilot or GCS
takes the gimbal back through ArduPilot: RC stick movement, a GCS angle, rate or
ROI command, or a mount mode change ends the lease. The survey then pauses, and
the camera follows the flight controller's targets and other pointing again until
the survey is explicitly resumed. ArduPilot denies a renewal after such a
takeover, which also pauses the survey, and refuses a claim while the mount is
retracted; the survey status reports that while it waits. Pointing is also
followed while a survey is paused for any other reason. While waiting for
ownership, direct MAVLink attitude/rate commands are followed without cancelling
the claim. A grant stops any earlier rate and clears ROI and image tracking
before the survey waits for fresh terrain/telemetry and straight flight.
SIYI/UniGCS motion, web PTZ, a new ROI LOCATION or starting image tracking pauses
a waiting survey. It then stops claiming; a later grant does not resume it.
Explicitly resume the survey to request ownership again.
A camera that stops renewing its ownership releases the ArduPilot gimbal manager
after three seconds, and the mount returns to its previous mode and target.

## Parameters

All settings are persistent, available in the Survey web tab and as MAVLink
parameters. The corresponding INI section is `[survey]`.

| MAVLink parameter | INI key | Default | Meaning |
| --- | --- | --- | --- |
| SURV_PATTERN | pattern | both | 0 both, 1 left/right, 2 fore/aft, 3 fore-only, 4 fore-wide |
| SURV_LENS | lens | thermal | 0 thermal, 1 wide, 2 zoom; INI uses names |
| SURV_OVERLAP | overlap | 50 | Requested overlap percent, 0–90 |
| SURV_FORE | fore_pct | 50 | Forward offset as percent of AGL; nearest offset for fore-only |
| SURV_AFT | aft_pct | 50 | Rear offset as percent of AGL |
| SURV_BURST | burst | 1 | Distinct frames per target, 1–25 |
| SURV_BURST_MS | burst_ms | 100 | Minimum burst frame spacing in ms |
| SURV_DWELL_MS | dwell_ms | 1200 | Per-view settling/capture budget after slew allowance |
| SURV_SETTLE_MS | settle_ms | 200 | Continuous time meeting motion thresholds |
| SURV_ERROR_CD | error_cd | 100 | Pointing threshold in hundredths of a degree |
| SURV_RATE_CD | rate_cd | 100 | Residual LOS rate threshold in hundredths of a degree/s |
| SURV_TERR_MS | terrain_ms | 3000 | Maximum terrain report age in ms |
| SURV_TERR_M | terrain_m | 500 | Maximum distance from terrain report in metres |
| SURV_TERR_TOL | terrain_tol | 25 | Flight-controller height consistency tolerance in metres |
| SURV_MIN_SPEED | min_speed | 5 | Minimum ground speed in m/s |
| SURV_TURN_DEG | turn_deg | 15 | Course departure that ends the current leg |

Overlap determines lateral spacing from the selected lens footprint. Fore/aft
positions are quantised to common ground rows to permit repeat views. Row spacing
increases when the slew and capture cycle cannot keep up with the requested
along-track overlap. Thus realised offsets can exceed the requested AGL factors.
The status displays sweep time, row spacing and **nominal same-view overlap**;
a negative value denotes gaps between successive nadir-sized row footprints.
Oblique views can cover these gaps, but the setting does not guarantee uniform
coverage or three views of every point. Use the captured-footprint overlay to
inspect the result. Turns and the start/end of a leg have incomplete repeat views.

At 400 m AGL the nominal MT11 thermal nadir footprint is about 172 × 137 m.
MT11 firmware and SITL use a 100°/s maximum rate, matching upright hardware bench
measurements. SITL retains the measured low-speed dead zone and stopping lag.
Fore-only avoids large yaw slews, but the dwell and capture budgets still apply;
shorter dwell settings increase cadence. At a deadline the camera accepts a reachable unsettled exposure
and marks it accordingly; it skips invalid geometry or missing telemetry.
Large bursts reduce achievable ground coverage. Nadir overflight can require a
rapid yaw reversal and may miss a view rather than invent a new ground target.

## Terrain, storage and coverage

Upload a ZIP from **terrain.ardupilot.org** in **Parameters → Survey → Upload
terrain**. ArduPilot v1 `.DAT` tiles are supported, including SRTM1 at 30 m spacing.
The archive must contain flat `NxxExxx.DAT` / `SxxWxxx.DAT` names, with stored or
deflated ZIP entries. ZIP64 is not supported. The upload replaces the installed
area; include all required tiles in one ZIP. Activation takes effect within one
second without restarting the camera. An in-progress burst keeps its fixed target.

The browser sends the file directly. The web server streams it to the SD card,
then streams each decompressed tile through block CRC, location, bitmap and spacing
validation. Memory use does not scale with archive size. Limits are 512 MiB ZIP,
256 MiB per tile and 2 GiB extracted; free space is checked before receive and
extraction. Files are staged under `/mnt/TERRAIN`, then an atomic `current` manifest
switch (also supported on FAT/exFAT SD cards) publishes the complete dataset. Failed/interrupted uploads preserve the
previous dataset. The camera's existing `unzip` utility supplies decompression;
no shell command is constructed from uploaded names.

The terrain reader follows AP_Terrain's overlapping 28×32 grids, 2048-byte records,
validity bitmap, CRC and bilinear interpolation, including mean-latitude longitude
scaling. A background worker handles disk reads. A bounded 512-block LRU cache
uses approximately 1 MiB plus bookkeeping; at most 64 reads can be queued. Dataset
replacement invalidates the cache. SITL can override the directory with
`CAMERA_APP_TERRAIN_ROOT` (containing the same `current` manifest).

Uploaded data is preferred both beneath the aircraft and at each target's lat/lon.
The camera still requests TERRAIN_REPORT at 2 Hz and uses fresh reports outside
uploaded coverage. If only aircraft-local terrain is known, target height falls
back to that local plane. No home-altitude fallback is used. A stale report alone
does not suspend capture where uploaded terrain covers the aircraft. Fresh
position, velocity and attitude are always required.

Terrain/AGL and speed changes no longer discard a straight leg's grid. Targets
remain on fixed rows for repeat views; current speed controls sweep timing and
current terrain supplies each new target height. The first sweep starts immediately
after straight flight/ownership acquisition, rather than waiting half a sweep.
Turns, missing terrain or stale telemetry still suspend capture and end the leg.

In ArduPilot AUTO waypoint flight, the camera requests `NAV_CONTROLLER_OUTPUT` at 5 Hz
and `MISSION_CURRENT` at 2 Hz. It retrieves only the active mission item
(`MISSION_REQUEST_INT`/`MISSION_ITEM_INT`, refreshed every second) to
confirm a straight `MAV_CMD_NAV_WAYPOINT` leg. It does not download the mission.
Loiter cross-track error is radial, so loiters and other command types retain
the ground-track fallback.

The intended leg bearing is `target_bearing - asin(xtrack_error / wp_dist)`
when the waypoint is ahead. The grid is anchored on the resulting centreline,
not on the displaced aircraft track. Estimates are filtered and must be stable
for one second. Integer-degree bearings limit accuracy; invalid geometry,
nearby or passed endpoints, stale telemetry, and leaving AUTO use the existing
ground-track method. Source changes realign the grid between bursts; targets
remain fixed within a burst. Actual aircraft position and velocity always drive
the gimbal LOS controller. The Survey status reports the active path source.
This describes the current leg only and cannot anticipate the next leg's turn.
`EXTENDED_SYS_STATE` is requested at 1 Hz to exclude QuadPlane hover and
transition phases; VTOL vehicle types require fresh fixed-wing state. Position
samples used for path reconstruction must be no more than 500 ms old.
Explicit takeover, storage errors and stop commands require resume.

Small course changes also trigger grid realignment when lateral drift, including
the projected offset of the furthest selected view, exceeds half a cross-grid
interval. This limit scales with current AGL and has a 5 m minimum for GPS noise.
Realignment happens between bursts after any pending frame completes, preserving
the active burst's fixed ground target. Future rows restart on the current flight
path without another startup delay when straight-flight conditions still hold.
Repeat-view matching starts a new leg at that boundary. This prevents a small
heading error below the turn threshold from accumulating kilometre-scale drift
on long flight legs. Old captured footprints remain on the map as history.

Coverage polygons remain flat-plane projections, now using the selected target's
height. They are approximate over steep terrain, rather than terrain-mesh ray
intersections. Sidecars retain ground height beneath the aircraft separately from
`footprint_ground_amsl_m` and record `target_terrain_source`.

Raw thermal captures are individual native 640×512 little-endian 16-bit `.bin`
files on the capture SD directory, with JSON sidecars. Exactly those pixel arrays
are queued into the existing lossless FFV1/Matroska raw stream, independent of its
normal FPS decimation. The GCS must connect to that stream to receive them. Network
queues are bounded; a disconnected/slow GCS cannot block SD recording. A queue
overflow is logged, and `stream_queued` records whether the frame entered the
stream queue (it is not a delivery acknowledgement).

Wide/zoom captures are JPEGs for only the chosen lens, with pose metadata and the
same survey sidecar. Sidecars record session/image IDs, ground row and column,
burst ordinal, frame time, target, actual footprint and settling measurements.
The footprint uses the locally flat terrain assumption. Hardware sensor exposure
latency remains uncalibrated: records explicitly mark `latency_unverified` and
use frame-receive timing. SITL cannot establish actual thermal smear performance.

The camera emits paired CAMERA_FOV_STATUS and CAMERA_IMAGE_CAPTURED messages with
the same frame timestamp. MAVProxy shades successful actual footprints at low
alpha without requiring a video viewer. Cyan outlines mark small footprints; the
default fill opacity is 18%, adjustable with `camera coverage alpha` or the menu.
Overlap accumulates shade. Cached footprints appear when a map is opened or
replaced. `camera coverage load` restores captured footprints from a telemetry log
after a GCS restart; repeated log loads do not duplicate images. It keeps the
latest 4000 footprints; hide/show/clear act on the selected camera. This is coverage
geometry, not a quality or sharpness guarantee.

## Tests and builds

```
make -j8 sitl
make -C camera_app -j8 test
make -C web -j8 test
make camera-definition-test
python3 sitl/test_survey_controls.py
python3 sitl/test_survey.py --mavproxy /path/to/MAVProxy --speed 25 --duration 110 --output /tmp/survey25
python3 sitl/test_survey.py --mavproxy /path/to/MAVProxy --speed 18 --duration 100 --terrain-dropout --output /tmp/survey18
python3 tests/test_survey_plane.py --arduplane /path/to/ardupilot/build/sitl/bin/arduplane --output /tmp/survey-plane
make -j8
```

The Plane test requires a rebuilt ArduPlane SITL binary, selected with
`--arduplane PATH`; synthetic tests require the MAVProxy checkout selected with
`--mavproxy PATH`. The Plane test flies
a real AUTO mission at approximately 400 m AGL, supplies terrain tiles to Plane,
checks mission mode round-trip, capture, manual takeover, and lease expiry after
camera termination. Synthetic flight tests verify distinct bursts and exact
stream/SD pixel equality at 18 and 25 m/s. Host tests check geometry, row revisits,
metadata interpolation, camera definition, configuration and web controls.

The MT11 executables are `camera_app/camera-app` and `web/mt11-web` after the
hardware build. Install them using the existing camera update workflow, with the
matching flight-controller build. Begin with a bench check of start/stop and
manual takeover, then inspect footprints and thermal sharpness on a short straight
flight before tuning burst size, overlap and settle thresholds.

## Verification from this worktree

- Plane AUTO, approximately 400 m AGL: 46 captures with zero missed slots in the
  measured segment; mission mode storage/dispatch, explicit takeover and camera
  crash lease expiry passed (`/data/buildlogs/survey-plane-final.log`).
- Synthetic 25 m/s: 68 distinct thermal captures; exact FFV1-to-SD pixel equality;
  consecutive ground rows and front/middle/rear target revisits passed
  (`/data/buildlogs/survey-final25-rows.log`).
- Synthetic 18 m/s with terrain interruption: 30 distinct thermal captures;
  capture suspended while terrain was stale; stream/SD equality passed
  (`/data/buildlogs/survey-final18-dropout.log`).
- Wide and zoom JPEG selection: finite three-image limit passed for each lens
  (`/data/buildlogs/survey-wide-count.log`, `/data/buildlogs/survey-zoom-count2.log`).
- Runtime `CAM_MODE` selection, Survey capability advertisement and pairing of actual
  camera capture messages into MAVProxy footprints passed
  (`/data/buildlogs/survey-map-e2e.log`).
- Camera host suite, 14 camera-definition tests, browser/web suite, MAVProxy camera
  tests, captured-footprint tests and frame-pose interpolation tests passed.

The installable test image is built with:

```
make -j8 mt11_package MT11_PACKAGE_OUT=build/MT11_FW_ArduPilot_survey_test.bin
```

This is a working-tree test build, not a published release. The matching
ArduPilot source changes must be built for the actual aircraft's flight controller;
the provided ArduPlane binary is SITL only.
