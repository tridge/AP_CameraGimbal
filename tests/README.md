# ArduPilot camera integration tests

The integration test starts the MT11 camera app and simulated gimbal as separate
processes, then connects an ArduCopter SITL serial port to the camera's SIYI TCP
port. It verifies camera discovery, gimbal attitude and control, zoom, and still
capture through ArduPilot's `AP_Mount_Siyi` and `AP_Camera_Mount` backends.

The MAVLink test first exercises the camera and gimbal-device components
directly over TCP and UDP. It then starts ArduCopter once per transport with
`NET_P1` configured as a MAVLink 2 TCP or UDP client and verifies discovery,
mount angle control, zoom, focus, still capture and video recording through
`AP_Mount_MAVLink` and `AP_Camera_MAVLinkCamV2`. This intentionally does not
use a simulated serial port for the camera link. Stream information and capture
status are requested from the camera component through ArduPilot MAVLink routing.
Earth-frame gimbal feedback is checked directly on the device link because it is
addressed to the autopilot. Upstream ArduPilot does not yet relay camera
stream/capture state or convert earth-frame gimbal feedback into its manager's
body-frame convention. Physical pointing and the device's feedback quaternion/frame
flags are both checked for angle and geographic ROI commands.
ROI uses an absolute AMSL altitude because upstream `AP_Mount_MAVLink` does not
yet convert relative-home ROI locations to AMSL when forwarding them to a gimbal.

To run it locally:

```sh
make dependencies
tests/clone_ardupilot.sh
tests/install_ardupilot_prereqs.sh       # first run on Ubuntu only
tests/run_ardupilot_siyi_test.sh
tests/run_ardupilot_mavlink_test.sh
```

Pass an alternate checkout path as the first argument to each script. Set
`ARDUPILOT_REPOSITORY` or `ARDUPILOT_REF` when cloning to test a fork or branch.
The default is a shallow recursive clone of ArduPilot `master` in
`build/ardupilot`. SIYI logs are retained in
`build/sitl/ardupilot-test/runtime/run`; MAVLink logs are retained below
`build/sitl/ardupilot-mavlink-test`.

To additionally load and exercise the in-development MAVProxy generic camera
module against the MT11 and both ArduPilot `NET_P1` transports, run:

```sh
make mavproxy-camera-test \
    MAVPROXY_REPO=$HOME/project/UAV/MAVProxy.wt/mavcamera
```

This verifies automatic camera, gimbal, and two-stream discovery; routable RTSP
URLs and FOV metadata; zoom, still capture, recording, source switching, and
gimbal-manager angle control. The normal CI test remains independent of a
particular MAVProxy checkout unless the module is added to CI later.
