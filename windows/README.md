# Camera/Gimbal SITL for Windows

Run `CameraGimbalSITL-Setup.exe`, or unpack the portable ZIP and double-click
`CameraGimbalSITL.exe`. Choose **1–4 simulators**, then select **MT11**, **A8**,
**ZR10** or **Z1-Mini**, the mounting orientation and simple video patterns or
3D terrain imagery in each tab. Click **Start all**. Each tab's **Open Web UI**
opens its camera controls (initial login `admin` / `ardupilot`).

Python, Qt, the camera/web services, the POSIX runtime, FFmpeg and the terrain
renderer are included. Users do not need Python, Cygwin, WSL, a compiler,
MAVProxy or an ArduPilot checkout installed. The simulator connects to an
external flight-controller SITL or MAVLink source; it does not launch a plane
or copter itself. SIYI listens on port 37260, RTSP on 8554, and MAVLink TCP/UDP
on 14550 by default. Settings can be changed in the web UI.

The 3D source uses the same MAVProxy map3d terrain meshes and imagery provider,
with 16 background imagery download threads by default. Internet access is
needed to fetch uncached terrain/imagery. This source needs vehicle MAVLink
telemetry and OpenGL-capable graphics. Simple patterns work offline.

Settings, photos, recordings and logs are stored under
`%LOCALAPPDATA%\ArduPilot\CameraGimbalSITL\<camera>\runtime`. Reinstalling or
moving the package does not erase them. **Clear parameters on launch** is off
by default and resets only camera parameters, retaining a backup and media.
Additional slots have separate runtime directories under
`<camera>\instance-<number>\<camera>\runtime`. Web ports default to 8081–8084;
MAVLink ports to 14550/14560/14570/14580 and RTSP to 8554/8564/8574/8584.
Vendor ports increase by one per slot. Fresh configurations also have distinct
camera/gimbal component IDs; existing saved settings are retained.
See the [full defaults](../sitl/README.md). The existing environment
variables `<CAMERA>_SITL_WEB_PORT`, `_CAMERA_PORT`, `_RTSP_PORT`,
`_MAVLINK_TCP_PORT` and `_MAVLINK_UDP_PORT` override the base ports when needed;
later slots add their normal offsets. Port 0 leaves MAVLink disabled.
If a MAVLink port cannot be opened, its transport and error are logged and
startup continues with the other available transports. The web UI remains
available even if both MAVLink ports fail: change the ports in Parameters,
save, then restart the camera app to retry them.

Stop using the launcher's **Stop all** button so recordings are closed cleanly.
The Windows supervisor also cleans up camera processes started by a web-app
restart. SupportProxy streams use the Windows host network; configuring an
additional host address/default route from the camera is not supported.

## Building

The GitHub Actions Windows job follows MAVProxy's Python/PyInstaller and Inno
Setup packaging approach. Native services are built with Cygwin GCC and their
runtime DLLs are copied into the package. The GUI and terrain renderer use
native Windows Python and VTK. The finished package never invokes a shell or
compiler.

For a local build, install native Python 3.10 x64, Git and Cygwin with
`gcc-core,gcc-g++,make,python3,python3-lxml,patch,git,curl,tar,xz`. Fetch this
repository's submodules, then run `windows/build.ps1` in PowerShell. It installs
its Python build dependencies in the selected Python environment; a virtual
environment is recommended. The installer and ZIP appear in `windows/Output`.

Run `dist\CameraGimbalSITL\SITLWorker.exe --self-test` to exercise all four
cameras through the bundled PyQt launcher, SIYI, MAVLink, RTSP, web live video,
recording and restart. Add `--orientation inverted` or `--video terrain` for
those modes. These tests remove external tools from PATH, use separate runtime
directories, and save logs/screenshots. `--show-window` displays the test GUI.
