# Camera targets

`target.h` selects one of the four release targets. Set `APCAM_TARGET` to a
symbol from `target_ids.h`; builds without a target fail. Firmware and the web
service include the same target header. `tools/export_targets.py` compiles a
small host program for each target and exports its resolved properties to
`build/targets/targets.json`. SITL and the PyQt launcher read that file; Windows
bundles it and needs no compiler on the user's machine.

Use capabilities in generic code. Target comparisons belong in hardware or
vendor-protocol adapters, firmware installation code and platform path selection.
Adding a property means defining it in each applicable target header; the export
step discovers property names and asks the C compiler to evaluate their values.
There is no second Python table of lens calibrations or gimbal signs.

## Lenses, streams and recording

Lens numbers are physical optical paths, starting at 1. Lens type, sensor size,
horizontal FOV and optical zoom calibration describe that path. Stream lens masks
use bit 0 for lens 1, bit 1 for lens 2, etc. MT11 routes either RGB lens to visible
video and has a separate thermal path. Its live primary/secondary stream routing
can swap visible and thermal video without changing the physical lenses.

Main, sub and recording resolution masks are independent. The enum identifiers
are stable: 720p=0, 1080p=1, 2160p=2, 1440p=3. Defaults must belong to their own
mask. Web menus and config/MAVLink validation use those masks. SITL encodes at the
configured dimensions, with a separate RGB recording encoder when required;
thermal recording is a second recording channel on MT11. Z1-Mini supports 1080p
live video and 1080p/4K recording through native capture. The older retained-ISP
backend remains limited by the encoded source it receives.

`FRAME_RATE` is the normal simple-source rate. Terrain SITL defaults to 20 Hz
and can be overridden with `CAMERA_GIMBAL_SITL_FPS`. The terrain worker retains
16 imagery workers, six terrain workers, concurrent encoding, and a bounded
presentation queue. It only enables the extra recording encoder while needed.

FOV is scaled using focal length, not by dividing the angle. ZR10 interpolates
between its supplied 71.5-degree wide and 6.7-degree tele endpoints through the
10x optical range; higher zoom remains uncalibrated. MT11's zoom-lens baseline
uses the existing 3.44x crossover and 3.2x optical limit. Its lens-2 FOV is an
initial value derived from that model, not a new bench measurement. MT11 wide
video uses 71.532165 degrees horizontal and 44.113793 degrees vertical at 1x.
These are derived from the [MT11 specification's 84-degree diagonal FOV](https://reebot.com/en/index.php?id=specs2&asd=481)
on the full 4:3 sensor, retaining its width and cropping vertically to 16:9:
`H = 2 atan(tan(84/2) * 4/5)`, `V = 2 atan(tan(H/2) * 9/16)`.
This supersedes the provisional 58.1-degree bench fit; see the flight comparison
in the porting repository's `analysis/MT11/flight-fov-20260828`. Firmware and SITL
consume the same target constant; digital zoom scales focal length. Its tele
calibration still needs measurement. A8 wide remains 88 degrees and MT11 thermal
remains 24.2 degrees. Z1-Mini's 54.7-degree
horizontal FOV and sensor dimensions come from the surveyed vendor gcu_config.
A8 uses its hardware backend's 6x digital zoom limit in SITL and the UI as well.
Thermal calibration remains a separate TODO.
MAVLink stream rates come from the media backend, including the configured
terrain SITL rate, rather than a fixed 30 Hz value.

## Gimbal conventions

The generic backend API, web UI and MAVLink use yaw right positive and pitch up
positive. Vendor packets retain the vendor convention. `gimbal_transform.h`
implements signed joint-coordinate mappings in roll/pitch/yaw order: an
orthogonal matrix followed by a zero-position offset in degrees. Rates use the
matrix without offsets. These are joint-angle mappings, not multiplication of
spatial Euler angles as a vector.

Feedback properties map vendor wire values to the canonical convention;
command properties map canonical values to wire values. Public and private
feedback are separate, as are angle and rate commands. Each has upright and
inverted properties. SITL applies inverse mappings when generating feedback and
decoding commands. Tests include independent known feedback vectors in addition
to round-trip checks.

Initial values preserve the prior hardware evidence: MT11 feedback yaw is left
positive; A8 public feedback changes yaw sign when inverted while private 0x50
feedback stays right positive; A8/ZR10 inverted pitch has a 180-degree zero
position and reversed sign; ZR10 public feedback yaw is left positive, based on
hardware measurements. Its private 0x50 sign and inverted
mounting still need confirmation. Inverted A8 hardware testing found both
absolute-command axes reversed relative to upright: SIYI 0x0e uses right-positive
yaw and down-positive pitch when inverted, with a level pitch zero. Its rate
commands retain their existing signs. These command mappings are separate from
the 180-degree pitch offset in attitude feedback. ZR10 and Z1-Mini command mappings
still need release bench verification. Z1-Mini upright MCU angles retain the surveyed
identity mapping; its inverted mapping and gyro signs have not been measured.
Do not treat matching simulator behaviour as independent hardware validation.

On 2026-09-13, Tridge confirmed A8 web PTZ and MAVProxy camera mount angle/rate
controls on hardware in both inverted and upright orientations, power cycling
between the tests. This validates the corrected inverted angle-command mapping
and the retained upright and rate-command mappings.

On 2026-09-13, Tridge also confirmed that MT11 hardware control tests pass in
both normal (upright) and inverted orientations. No MT11 mapping changes were
needed.

Only SIYI targets open the SIYI server. Z1-Mini uses its separate internal MCU
transport and the public XFRobot A8 E5 server (TCP 2332, UDP 2337, UDP replies to
the sender on 2338). The public implementation supports attitude queries, angle
control, neutral/down commands and recording; unsupported orders return failure.
It does not claim support for the vendor's separate encrypted protocol.

## Startup and validation

AP CameraGimbal is the only selectable camera application. There is no switch
endpoint or automatic vendor fallback. Legacy selection files are ignored.
Supervised targets keep the web UI available after camera failure and wait for
an explicit restart. A retained vendor ISP in the older Z1-Mini media backend is
a media dependency, not an alternate camera application.

Run `make targets-test` for compiler/export consistency, profile defaults,
transform vectors and lens calibration. Existing camera/web tests and the SITL
smoke tests cover protocol and UI behaviour. The camera test target also
exercises the A8 supervisor with isolated paths and mocked hardware, including
crash recovery and preservation of saved settings. `sitl/test_z1mini_sitl.py`
also checks public XFRobot TCP/UDP framing and CRC rejection, and 1080p live
video alongside 4K recording. Use an isolated build/runtime for these tests: they change its configuration.

MAVLink `VIDEO_STREAM_INFORMATION` and `VIDEO_STREAM_STATUS` report the current
HFOV rounded to whole degrees (72 at MT11 wide 1x). Requestable
`CAMERA_FOV_STATUS` (message 271, via `MAV_CMD_REQUEST_MESSAGE`) reports both HFOV
and VFOV as floats for the current main source, including optical/digital zoom.
Thermal VFOV uses the native sensor aspect before encoder stretching/padding;
composite side-by-side views report unknown FOV. Unknown camera or image
positions use the protocol sentinel rather than a fabricated location. Encoded
video/recording telemetry retains the per-frame floating-point `hfov_deg` field.
The supported SIYI/UniGCS control messages have no identified FOV report field;
their video and tracking use the same media calibration.
