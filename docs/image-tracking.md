# Image rectangle tracking

The camera owns a shared dlib 20.0.1 correlation tracker. SIYI UniGCS and
MAVLink select and cancel the same target. Processing runs on a dedicated
worker, at most 15 updates/s, on unstretched pre-OSD luma up to 320×256.
The motor controller and network handlers never wait for correlation.

## Camera coverage

| Camera | Image source | Interfaces | Tracking box |
|---|---|---|---|
| MT11 | Selected visible or thermal VPSS output | SIYI, MAVLink | Live main stream |
| A8 | Dedicated 320×180 SCL output, following digital crop | SIYI, MAVLink | Both live streams |
| ZR10 | Dedicated 320×180 VPE output | SIYI, MAVLink | Both live streams |
| Z1 Mini | Native AX capture helper luma IPC | MAVLink | Draw from MAVLink status in the client |

Capability is advertised only when the tracking worker and raw-frame path
can be opened. Z1 Mini's retained vendor-RTSP mode does not expose raw frames
and therefore does not advertise tracking. Use its native capture helper.
The tracker has no classification models or pretrained weights.

MT11 has visible, thermal, and combined side-by-side scenes. Combined view
preserves each sensor's aspect ratio with black padding; rectangle tracking
is rejected in that scene because a rectangle can span two optical axes.
Switching sensor, orientation or pipeline cancels tracking. Large zoom
changes may require reselecting the target.

## MAVProxy camera-module interface

Send COMMAND_LONG to the camera's system ID and camera component (normally
100, configurable through 105):

- `MAV_CMD_CAMERA_TRACK_RECTANGLE` (2005): parameters 1–4 are normalized
  left, top, right, bottom in the selected main image. Sort reverse drags
  in the client. Each edge is in [0,1]; the rectangle must have positive
  area and be at least 8 pixels wide/high in the tracking image.
- `MAV_CMD_CAMERA_TRACK_POINT` (2004): parameters 1–2 are the normalized point
  and parameter 3 its radius as a fraction of the main image width. The camera
  tracks a square of ±radius centred on the point, shrunk to fit inside the
  image. It is never smaller than the tracker accepts, so a radius of 0 (one
  pixel in MAVLink) or a point at the very edge gets that minimum box.
- `MAV_CMD_CAMERA_STOP_TRACKING` (2010): cancels either SIYI or MAVLink tracking.
- `CAMERA_INFORMATION.flags` advertises `CAMERA_CAP_FLAGS_HAS_TRACKING_RECTANGLE`
  and `CAMERA_CAP_FLAGS_HAS_TRACKING_POINT`.
- These commands can also be sent to the flight controller, which forwards them
  to the camera when it advertises the matching capability. The flight
  controller accepts them as soon as they are forwarded, so a camera rejection
  (for example while a survey holds the gimbal) does not reach the GCS.
- `CAMERA_TRACKING_IMAGE_STATUS` is sent at 5 Hz by default. Rectangle corners
  are normalized; point/radius fields are NaN. ACTIVE means image-confirmed;
  ACTIVE|COASTING means predicted during a short occlusion; ERROR means lost;
  IDLE means stopped or acquisition pending. Mode is RECTANGLE while active.
- `MAV_CMD_REQUEST_MESSAGE` requests immediate status.
  `MAV_CMD_SET_MESSAGE_INTERVAL` selects the status interval in microseconds
  (minimum 50 ms, maximum 60 s); 0 restores the default, -1 disables periodic
  reporting. The setting is shared across camera clients.

An ACCEPTED command ACK means the asynchronous acquisition was queued, not
that an image target was found. Follow the status message. A textureless or
unavailable image can subsequently report ERROR. Client-side rubber bands
must map the video viewport, excluding letterbox bars, into normalized image
coordinates. No MAVProxy module changes are included here.

## Motion and loss behavior

Confirmed rectangle centers are converted to inertial line-of-sight angles
using camera roll/pitch/yaw and effective field of view. Aircraft yaw and its
rate are added when fresh MAVLink attitude is available; stabilized camera
roll/pitch are not added to aircraft roll/pitch a second time. A bounded
attitude history interpolates the pose at the frame timestamp; body gyro axes
are not treated as Euler angle rates. Without aircraft attitude the prediction
assumes the aircraft is stationary and compensates gimbal motion only.

MT11 visible frames use SDK PTS translated to the monotonic clock; thermal
frames retain the monotonic timestamps assigned by our USB injector. A8 and
ZR10 currently timestamp frames when the MI buffer is read, without mapping
the MI PTS to the monotonic clock. Their pose lookup therefore approximates
exposure time with read time; a depth-one output queue limits buffering but
does not remove capture-to-read latency or its motion-compensation error. Motor
control projects the measured target to the current pose. The encoded box
is projected through processing delay to the live pipeline's exposure time,
so it is not drawn at either the old processed position or ahead of the video.
Samples older than 350 ms cannot drive the motors; confirmation expires after
500 ms, including during prediction. `TRKF` logs image errors, estimated target
rates and frame pose; `TFRA` records MT11 capture/SDK timestamps.

The estimated target LOS rate predicts the next search rectangle and supplies
feed-forward gimbal rates. MT11 tracking realizes rates below its measured
6 degrees/s minimum with zero/minimum-rate pulses, carrying the unsent angle
between control ticks. This replaces the dead-zone integral on MT11, which
otherwise accumulated while the motor stood still and drove past the target
when motion started. Other cameras retain their existing bounded integral.
Pulse state is cleared on acquisition, cancellation, stale data, direction
changes and travel limits. Ordinary SDK/MAVLink rate conversion is unchanged.
Commands are limited to 30 degrees/s and camera travel limits.
Correlation uses a 64×64 spatial filter and 16 scale levels. Low confidence
freezes the learned appearance instead of teaching the tracker the occluder.
Recovery requires a higher confidence than normal tracking. Coasting lasts
at most 500 ms; stale video/pose stops motor output. After loss a new selection
is required: there is no unbounded search or autonomous reacquisition.

Manual SIYI/private controls, MAVLink gimbal controls/ROI, and web manual
control take ownership back. Do not keep sending competing mount-rate or
angle commands while image tracking is active. An ArduPilot mount backend
streaming public SIYI rates can immediately cancel a rectangle; the future
MAVProxy workflow needs to coordinate that ownership as well as send 2005.
SIYI disconnect cancels SIYI-owned tracking, but not a MAVLink-owned target.

## Validation

`make -C camera_app image-tracking-test` covers image translation, rotating
camera compensation during short occlusion, recovery, long loss, stale pose,
stale frames, invalid rectangles and texture rejection.

With the SITL binaries built:

```sh
build/terrain-venv/bin/python sitl/test_image_tracking.py build/a8-sitl/camera-app --backend a8
build/terrain-venv/bin/python sitl/test_image_tracking.py build/sitl/camera-app --backend mt11
build/terrain-venv/bin/python sitl/test_image_tracking.py build/sitl/camera-app --backend mt11 --zoom-scene
build/terrain-venv/bin/python sitl/test_image_tracking.py build/sitl/camera-app --backend mt11 --thermal-scene
```

These tests use real encoded video and dlib image processing, simulated motor
feedback, both network protocols, cross-protocol cancel, manual override,
and MT11 scene/range commands. Add `--tablet --runtime /tmp/tracking-tablet`
to serve an interactive fixture on discovery 37258, private TCP 37256, RTSP
8554 and MAVLink TCP 14550. The public SIYI port stays dynamically allocated
to avoid a pre-existing aircraft simulator commanding the test gimbal.
Android's software decoder displays the H.265 fixture. The renderer omits
access-unit delimiter NALs: including them caused gray inter-frames and
repeated UniGCS decoder resets although FFmpeg decoded the stream correctly.
The fixture predicts pose to its frame presentation time and restores the
thermal sensor aspect before feeding luma to the tracker.

Android A8 and MT11 visible rectangle acquisition, centering, encoded boxes
and cancel were verified. MT11's three scene buttons and lidar on/off were
also verified. Thermal acquisition passes the direct SIYI/MAVLink test;
Android's thermal-view drags did not emit AA in this session, so that specific
client gesture remains unverified.

The original 30 September MT11 zoom-scene runs failed the five-second centering
threshold: normalized error fell from 0.145 to 0.071 and 0.066, against a required
error below 0.04. The calibrated rate-pulse correction initially passed three
consecutive zoom runs plus wide and thermal acquisition. A subsequent six-run
review reproduced three failures of the two-second settling check, so zoom
settling remains unresolved. The regression retains the original
five-second threshold and additionally requires the rectangle to remain within
0.04 of center for two seconds. Unit tests check integrated motion through the
dead zone at both signs and variable control intervals, plus reset/stop handling.
Historical hardware measurements are in the separate analysis archive;
use `--runtime <directory>` with the commands above to retain reproducible
SITL process logs, session details and BIN logs locally.
Short-pulse response still needs confirmation on physical MT11 hardware.

Native builds and SITL establish API/build/UI interoperability, not native
CPU capacity or SDK buffer correctness. Measure capture latency, CPU, memory,
confidence thresholds and occlusion behavior on each physical camera before
flight use. MT11's hardware side-by-side scaler path also needs physical
validation. Detailed captures and measurements belong in the sibling
`AP_CameraGimbal.port/analysis/SIYI/` tree.
