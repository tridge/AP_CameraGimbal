# MT11 administrative web service

`mt11-web` is a small static AArch64 HTTP server for the MT11 camera. It does
not require Python or additional target libraries.

The header uses the official ArduPilot logo. Selecting it returns to the top
of the main status page.

Features:

- a login page with browser sessions, HTTP Basic authentication for scripted
  clients, and a user interface in English, Simplified Chinese or Japanese;
- live process, memory, storage, network and uptime status, with a button to
  synchronize the camera clock to the browser;
- firmware version and six-character Git hash on the Status page, embedded
  at build time using the same version/hash as package filenames; builds with
  tracked local modifications append `-dirty` to the hash;
- live SS928V100 on-die temperature using the vendor conversion formula and
  the average of its three hardware sensor channels;
- a Users tab for changing the `admin` password and adding or removing
  persistent SSH public keys without restarting Dropbear;
- a Sensors tab with 2 Hz LiDAR range, thermal extrema and CPU-temperature
  telemetry, authenticated shutter capture, and a recent-photo gallery;
- a Live tab which plays either H.264 output directly in the browser's native
  video element and provides guarded pan, tilt, centre and zoom controls;
- a Debug tab showing camera output and refreshing every three seconds;
- an authenticated filesystem browser with inline image/video viewing,
  range-capable file downloads and media-directory navigation;
- permanent file and recursive-directory deletion restricted to canonical
  targets strictly beneath `/mnt`, with a separate confirmation page;
- a typed parameter page with codec/resolution menus, decoded imaging-path
  masks, ISP controls, network checks, thermal controls and calibration guards;
- an optional SupportProxy section for MAVLink and video forwarding, signing
  and publishing credentials, stream names and ports; see
  [SupportProxy configuration](../camera_app/README.md#supportproxy);
- independent primary/secondary IPv4 addresses and a default gateway under
  Parameters → Network, applied on app restart with validation and reconnect
  guidance; see [Camera IP configuration](../camera_app/README.md#camera-ip-configuration);
- raw editing of the AP CameraGimbal configuration, with syntax validation,
  atomic saves and a backup;
- restart of AP CameraGimbal while the web service remains available for recovery;
- streamed, atomic upload of reviewed `MT11_FW_*.bin` packages to the microSD
  card for the vendor automatic updater; and
- camera reboot with explicit confirmation.

All routes except the login page and its small script require
authentication. The username is `admin`; the password is the trimmed contents
of `/app/web.pass`, reread for every request. The password file should be
mode 0600. A per-process random CSRF token protects all state-changing
requests; the only exception is the language preference, which
`/login?lang=` may also set on a GET.

Browsers are sent to `/login`, an HTML form with the username, the password
and a language selector. The page tells first-time users that the username is
`admin` and the password is `ardupilot` unless it was changed on the Users
page, and where the password file lives. A successful login stores a random
256-bit token in a small session file in RAM (`/run/mt11-web-sessions`,
owned by the service with mode 0600 and flock-protected because every request
runs in its own forked worker; sessions last 24 hours on the monotonic clock
of the boot that created them, so a clock change cannot log anyone out) and
sets it as an `HttpOnly; SameSite=Strict` cookie. **Log out** in the nav
removes the session on the camera; it cannot clear HTTP Basic credentials a
browser has cached. Requests that carry an `Authorization` header are still
judged by HTTP Basic exactly as before, so `curl`, scripts and the tests keep
working, and a failed Basic attempt on a protected route always gets the 401
challenge (`/login` itself judges the posted form instead); unauthenticated
requests that do not accept HTML get the same 401 rather than a redirect.
Browsers that omit the `Origin` header are not protected against login CSRF;
scripted clients should use Basic authentication rather than the form. Sessions
created before this version of the service are not recognised and need one
new login. The login form uses its own per-process token so the
unauthenticated page never exposes the CSRF token of the authenticated
actions, refuses posts whose `Origin` is another site, and delays a failed
login by half a second (this is not a rate limit). Passwords and session
cookies travel in clear text on this HTTP-only service; the cookie flags do
not protect against interception on the network.

The interface is available in English, Simplified Chinese and Japanese. The
language comes from the `lang` cookie, otherwise from the browser's
`Accept-Language`, and defaults to English; it can be chosen on the login
page or from the selector in the nav of every page. All user-visible text of
both the MT11 and A8 builds is translated, including parameter labels and
help, notices, error replies and the text in the page scripts. Technical
identifiers such as parameter keys, paths, ports and codec names, and the
captured application output on the Debug page, stay as they are. The strings
live in one table in `mt11-web.c`, indexed by a string id with one column per
language.

The service deliberately implements HTTP rather than TLS. It is intended only
for the MT11's isolated management network.

## Native live video

The ArduPilot camera app feeds its existing encoded H.264 access
units to a small in-process fragmented-MP4 writer. Its private TCP service is
bound only to `127.0.0.1:8555`; it is neither an additional public camera port
nor an RTSP proxy. `mt11-web` authenticates `/live/video1.mp4` and
`/live/video2.mp4`, selects the corresponding private stream, and relays the
bytes without decoding or transcoding. The native `<video>` element consumes
that URL directly; JavaScript handles status, reconnect, controls and keeping
playback near the live edge. While playing, a backlog exceeding 1.5 seconds
reopens the feed at a fresh IDR instead of seeking through stale buffered
video. Pausing still holds the picture; resuming returns to live automatically.
Each connection gets five seconds to settle before checking for a backlog.
Each fragment carries a `tfdt` decode timestamp and uses moof-relative
addressing, matching Chromium's fragmented-MP4 requirements. Stream geometry
changes close the private feed so the page reconnects with a new MP4
initialization segment. A viewer waits for an IDR before decoding, so initial
display can take up to the configured one-second GOP.

This path supports H.264 only. H.265 remains supported by RTSP but is rejected
with an explicit HTTP error because browser support is not sufficiently
portable. No MediaMTX, FFmpeg, GStreamer, JavaScript decoder or other target
runtime is installed.

With a 20 Hz terrain SITL stream running, `python3 web/tests/test_live_firefox.py`
checks pause, automatic catch-up and presented frame rate using a temporary
Firefox profile. Use `--url http://127.0.0.1:8081/live` for a different server
and `--headful` for an unobscured test window instead of headless playback.

Manual pan/tilt is disabled until explicitly enabled on the page. Each held
direction request uses the selected 5--60 degrees/second command rate (30 by
default) for 180 ms and the HTTP worker then sends three stop packets. This
bounds motion even if the browser disconnects. Centre and 1.0x--10.0x zoom are
one-shot operations. The live gimbal panel updates roll, pitch and yaw angles
and angular rates at 4 Hz, with yaw positive to the right. Aircraft, MAVLink
and Lua gimbal controllers can
still conflict with manual commands, which is why the page presents an
explicit warning and enable control.

The Status page's **SoC temperature** is the average of the SS928V100's three
on-die sensor channels. The stock kernel has no thermal/hwmon driver, so the
service maps the documented status-register page read-only through `/dev/mem`
and applies the vendor boot SDK conversion. No sensor control registers are
written; failed mappings or implausible raw values display as unavailable.

The Sensors tab sends the read-only SIYI rangefinder (`0x15`) and one-shot
full-frame temperature (`0x14`) queries to the running camera application every
500 ms. The MT11 stops replying to range requests while its laser is disabled,
so the page reports **Disabled** when the thermal query succeeds but no range
reply arrives. The adjacent **Enable/Disable** button sends laser-state command
`0x32` explicitly; enabling it turns on the physical laser. A zero range means
the laser is enabled but there is no valid return. Thermal minimum and maximum
values include their frame coordinates. CPU temperature is the same read-only
three-channel SS928V100 measurement used on Status. **Capture photo** sends
SIYI shutter command `0x0C 00`, waits for explicit `0x0B` success feedback, and
then shows the newest JPEG captures from `/mnt/DCIM/capture`. The ArduPilot
package defaults `capture.photo_scope` to `all`, producing wide, zoom and
thermal-display JPEGs plus raw radiometry.

The Files tab starts at `/mnt`, but can browse and download regular files from
other absolute paths for diagnosis. It serves common image and video types with
their correct MIME types and supports HTTP byte ranges so browser video players
can seek. File transfers run in short-lived worker processes so a large video
download does not block status and control requests. Click a Name, Type, Size,
Modified or Mode column header to sort; click again to reverse direction. Size
and date use their numeric values. Parent navigation and directories stay above
files, and the selected sort order is remembered within the browser tab.

Deletion has a deliberately narrower boundary than reading: `/mnt` itself and
everything outside it are rejected. The server resolves the target again at
deletion time, does not follow symlinks while recursively walking directories,
and removes an in-tree symlink itself rather than its destination. Deletion is
permanent and has no trash/recovery layer.

Typed saves update only known keys and preserve the rest of the vendor
configuration file, including comments and unknown settings. The raw editor is
provided for vendor-specific values that are outside the public camera-app
configuration API.

Browse to `http://192.168.144.25/`, which opens the login page, and log in as
`admin`. To inspect or change the password, open the **Users** tab. The save is atomic and fsynced;
the new password applies to the next login or Basic request and is preserved by
subsequent custom firmware overlays. The same page lists the current SSH public
keys and uses a file picker to atomically add one or more selected `.pub` files.
The browser reads and combines the files locally before submitting them to the
server-side batch validator. It writes
both persistent `/app/dropbear/authorized_keys` and the live
`/dev/dropbear-auth/authorized_keys` copy atomically with mode 0600. Removal
requires an explicit checkbox, and the server refuses to remove the last usable
key; add and test a replacement first.

The command-line equivalents for password inspection or replacement are:

```sh
ssh MT11 'cat /app/web.pass'
ssh MT11 'printf "%s\n" "new-password" >/app/web.pass && chmod 0600 /app/web.pass'
```

The password change applies to the next login and to the next HTTP Basic
request; browser sessions that are already logged in stay logged in. No
service restart is needed. The length limits (8 to 128) are UTF-8 bytes.

The blue **Upgrade Firmware…** button on the Status page opens a native file
picker for packages named `MT11_FW_*.bin` up to 128 MiB. The authenticated
browser sends the binary body
directly rather than wrapping it in a memory-heavy multipart form. The server
checks its per-process CSRF token before reading the upload, streams bytes to
`/mnt/NAME.bin.tmp`, fsyncs and globally syncs the complete temporary file,
then atomically renames it to `/mnt/NAME.bin`, fsyncs the directory and syncs
again. Only the final rename makes the package visible to `product_upgrade`.
Interrupted uploads remove the temporary file; existing temporary or final
packages and concurrent uploads are refused. Once publication completes, the
browser continues the progress display while polling the camera. A temporary
network loss is treated as the expected reboot, and a new web-server process
token confirms that the upgraded camera has returned. The wait times out after
60 seconds with a power/network diagnostic rather than claiming success.
The stock updater scans `/mnt` approximately every five seconds. Uploading
firmware can erase the kernel and rootfs, so stable power is mandatory.

On the XFRobot Z1-Mini the same button accepts `Z1Mini_AP_*.gcu` overlays and
the web server installs them itself. The upload is streamed to `/tmp`, the ZIP
listing is checked against the generated package's file allowlist (required
files present, bounded central directory and total extracted size), the archive
is extracted one file at a time with the camera's `unzip` into
`/opt/bin/gcu.new`, `SHA256SUMS` and the manifest target are verified, and the
new tree is atomically exchanged with `/opt/bin/gcu` before the camera reboots.
If the filesystem does not support atomic directory exchange, the upload fails
while preserving the existing installation. Rejected packages leave the
installation untouched.

Custom firmware packages carry `/app/camera.ini.default`, not
`/app/camera.ini`. Early startup copies the default only on a first install or
after deliberate removal of the persistent file, so Parameters-page settings
survive later firmware upgrades.

Camera stdout/stderr is piped through the same binary's
`--capture-app-log` mode. Vendor output is written to
`/run/siyi_camera_app.log`; ArduPilot camera app output is written to
`/run/camera_app.log`. Each log rotates at 512 KiB and retains one previous
segment, so verbose output cannot consume the application flash or grow
without bound. ArduPilot camera app lines without the application's millisecond
timestamp, such as SS928 SDK output, are timestamped by the collector. The web
page displays the newest 256 KiB for the running app.

## SIYI A8 mini build

The same source builds `a8-web` for the SIYI A8 mini with `make -C web
a8-web` (`-DAPCAM_TARGET=APCAM_TARGET_A8`, ARMv7 hard-float, dynamically linked against
the camera's glibc 2.30). That variant serves the Status, Parameters, Raw
config, Users, Files, Live, Photos and Debug pages against the A8 layout:
`camera-app` and this server under `/customer/camera-app`, the live
`camera.ini` and `web.pass` under `/config/camera-app`, login sessions in
`/tmp/a8-web-sessions`, media on `/mnt/mmc` and logs in `/tmp`. The MT11-only features are compiled out: LiDAR and
thermal readouts, SSH key management and the SoC temperature. Camera restarts use the A8 package supervisor's request counter. A failed camera
leaves the web service available for diagnosis and restart. Vendor switching
and automatic fallback are no longer supported.
Firmware upload accepts `A8_FW_*.bin` and writes it to the card as
`SIYI_4K_MINI_UpgradeSD.bin`, which U-Boot installs on the next reboot.
`make -C web test` also compiles the A8 variant on the host.

## Build

```sh
make -C web
make -C web test
```

The repository-level `make sitl` also builds a host web binary whose writable
paths are confined to `build/sitl/runtime`. It runs against the real host
`camera-app`, synthetic native video and the separate UDP gimbal simulator;
see [`../sitl/README.md`](../sitl/README.md).

The normal build is statically linked for AArch64. The host integration test
covers the login form, session cookies and logout, language selection and
rendering in all three languages, HTTP Basic authentication and its password
rollover, CSRF rejection, multi-key atomic add,
duplicate rejection, last-key removal protection, mocked SIYI LiDAR/thermal
decoding, native video relay, bounded variable-rate PTZ packets, attitude and
angular-rate rendering, JavaScript syntax, shutter feedback, photo-gallery
serving, firmware-name checks,
atomic publication, collision handling and interrupted upload cleanup.

## Deploy an update

```sh
rsync -av web/mt11-web MT11:/app/bin/mt11-web.new
ssh MT11 \
    'chmod 0755 /app/bin/mt11-web.new && mv /app/bin/mt11-web.new /app/bin/mt11-web'
```

Restart only the web service after replacing the executable. The persistent
`/app/app_init.sh` starts the selected camera through the matching log capture
mode, then starts the control server with:

```sh
/app/bin/mt11-web -p 80 >/run/mt11-web.log 2>&1 &
```

Runtime output goes to `/run/mt11-web.log` to avoid writing routine logs to
flash.

AP CameraGimbal is the only selectable application. Legacy application-selection
files are ignored. Runtime restarts preserve the web service and settings;
failures never start the vendor camera application. The AP app serves raw thermal
frames on TCP 7345 on targets with that capability.

Target properties and platform paths come from [the target headers](../include/apcam/README.md).
