# Live-view manual gimbal control

Tick **Enable manual gimbal control** in Live view to give that page exclusive
gimbal control. MAVLink angle/rate commands and vendor gimbal movement commands
are ignored while it is enabled. MAVLink ROI changes are temporarily rejected;
an existing ROI is paused and resumes when manual control is released.
Telemetry, recording and other camera operations remain available.

Untick the checkbox to release control. Closing or leaving the page also releases
it; if the browser or connection disappears, the lease expires after five seconds.
Only one live-view page can own control at a time. Direction buttons send bounded
180 ms rate pulses, stopped by the camera app even if the browser disconnects.
Any active pan/tilt pulse is stopped before a synchronous optical zoom move.
The web control endpoint waits up to five seconds for lens moves and queued
renewals. A rejected command reports an error while keeping a valid lease;
renewal failure still disables control. The zoom display follows camera feedback,
including after a language change, and rapid zoom changes are coalesced.

This setting is not saved in camera.ini. Rebooting or restarting the camera app
clears it, and an already-open page cannot reacquire control without another
explicit tick. Refresh the page after upgrading from older firmware.

The web server uses a private loopback endpoint published in camera-app.ready,
independent of the user-configurable MAVLink and vendor ports. All four targets
use their normal backend transformations for manual movement.

Tests:

```
make -C camera_app manual-control-test host
python3 camera_app/tests/test_manual_mavlink.py camera_app/tests/camera-app-host sitl/gimbal_sim.py
make -C web test
```

The MAVLink test needs pymavlink. The tests cover ownership, stale tokens,
renewal, expiry, bounded movement, competing controls, ROI pause/resume and app
restart. The SITL web smoke test also checks vendor command lockout and release.
