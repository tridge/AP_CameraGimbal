# AP_CameraGimbal {version} — XFRobot Z1-Mini

Firmware: **{filename}**. Source revision: `{revision}`.
`BUILD_INFO.json` records whether the build includes uncommitted changes.

This is the native AX620A application overlay: **1080p live video** and a
separate **4K recording** path. It retains the installed XFRobot kernel,
root filesystem, AX libraries and calibration. It is not an SD bootloader image.

## First installation and updates

1. Keep the original XFRobot firmware package available for recovery. Verify
   this package's SHA-256 against `SHA256SUMS`.
2. Use the XFRobot firmware update tool's `.gcu` upload procedure to transfer
   **{filename}**, then reboot. Keep power connected throughout the update.
   The updater replaces the whole `/opt/bin/gcu` directory, removing the vendor
   camera programs; the package includes the boot hook the camera runs.
3. After reboot, open the camera's existing Ethernet IP address in a browser
   on port **80**. Log in as **admin / ardupilot** on a new installation.
4. Check the version/hash on **Status**, then check **Live**. Insert a microSD
   card for recordings. New installations default to 4K recording; existing
   installations preserve their saved recording resolution.

For an update of a running AP installation, stop the AP service before the
vendor updater replaces its executables. Do not extract an overlay over running
AP executables. Existing parameters and web credentials in `/opt/ap_cameragimbal`
survive updates.

**ArduPilot web firmware upload is not yet implemented for Z1-Mini.** Copying
this `.gcu` to a microSD card does not install it. Use the vendor upload procedure
for this target; the SD and ArduPilot-web update instructions for SIYI cameras
do not apply.

MAVLink uses TCP/UDP **14550**; XFRobot control uses TCP **2332**, UDP **2337**
with replies on **2338**. RTSP streams use port **8554**, paths `/video1` and
`/video2`. AP startup failure leaves its web UI available for logs and recovery.
Reinstall the original vendor `.gcu` to restore the vendor application.
