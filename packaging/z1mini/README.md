# XFRobot Z1-Mini application overlay

AP CameraGimbal provides MAVLink and XFRobot control, a web UI, video streaming
and recording on the Z1-Mini. Release packages use native capture with separate
1080p live video and 4K recording.

## Build

```sh
make release RELEASE_TARGETS=Z1-Mini \
    Z1MINI_CROSS_COMPILE=/path/to/arm-none-linux-gnueabihf- \
    Z1MINI_AX_SDK_INCLUDE=/path/to/axpi_bsp_sdk/msp/out/include
```

Use an ARMv7 hard-float toolchain compatible with glibc 2.25; Arm GNU
10.2-2020.11 is supported. Supply AX620A BSP headers separately; the native
capture helper has been built with Sipeed axpi_bsp_sdk revision
`d61e665f44ee145e5bb7cda67d64ac6be0b9097e`. These headers and the installed
camera SDK libraries are not redistributed. Common build dependencies provide
the pinned XOP RTSP and minimp4 sources.

The output folder contains the `.gcu` application overlay, installation/update
instructions, build information and checksums. `make z1mini_native_package`
builds the same native overlay directly. `make z1mini_package` builds an
alternative which receives video from the retained vendor ISP service.

## Installation and updates

Use the XFRobot `.gcu` updater and the per-release README. The updater
replaces the whole `/opt/bin/gcu` directory with the package contents, so the
vendor camera programs are removed and the package ships the `ipc/run.sh`
boot hook that `/etc/init.d/rcS` launches. Power-cycle recovery and
interrupted-update handling still need hardware verification.

Once the AP application is running, later packages can be uploaded on the web
**Status** page. The server streams the `.gcu` to RAM, checks the archive
listing, extracts it next to `/opt/bin/gcu` with the camera's `unzip`, verifies
`SHA256SUMS` and the manifest target, exchanges it with the running
installation and reboots. A rejected package leaves the installation untouched.
SD-card boot installation is not implemented for this target. The retained-ISP
package depends on the vendor `ipc/` files and can only be deployed directly
over an intact vendor installation. The web upload refuses retained-ISP
packages because it replaces the complete application tree and would remove
those vendor files.

The package installs the AP application and its startup hooks. It retains the
kernel, root filesystem, bootloader, SDK libraries and calibration. Stop the AP
service before an updater replaces its running executables, and keep power
connected until installation completes. Boot checks reject incomplete payloads;
reinstall the package if needed. The overlay cannot recover damaged platform
partitions. Use the original XFRobot firmware to restore vendor operation.

Settings and web credentials in `/opt/ap_cameragimbal` survive updates. New
native installations default to 4K recording; existing installations retain
their saved resolution. Logs are bounded in `/tmp`. Recordings require a
mounted microSD card and are stored under `/mnt/mmc/DCIM/record`.

## Operation

- Native web UI: `http://CAMERA/`, initial login **admin / ardupilot**.
- MAVLink: TCP/UDP 14550. System ID 0 selects a flight controller automatically.
- XFRobot: TCP 2332 and UDP 2337, with replies on UDP 2338.
- RTSP: `rtsp://CAMERA:8554/video1` and `/video2`.

MAVLink angle, rate, neutral and attitude feedback are supported. Mounting
transforms and FOV are defined in `include/apcam/target_z1mini.h`; release
hardware checks in both orientations are still required. Geographic targeting
is disabled by default. Still capture, tracking, zoom/focus and manual image
adjustments are not implemented.

Native capture owns the sensor/ISP and hardware encoders. It requires a CPU
temperature below 70 C to start and stops at 75 C, leaving web recovery
available. The alternative retained-ISP package uses the vendor's fixed 1080p
stream for both live video and recording, and serves the AP web UI on port 8080.
Both variants ignore legacy app-selection settings and do not offer switching
to the vendor camera application or automatic vendor fallback.

Video includes per-frame telemetry. The recorder splits files below FAT32's
4 GiB limit, starting each part with decoder headers and an IDR frame.
Use `tools/video_telemetry.py` to extract the metadata.

## Validation

Run `make z1mini-test` for software checks. Native 4K recording alongside 1080p
live video, installation and a normal reboot have passed earlier hardware
tests. Sustained SD operation, cold boot, thermal recovery and projection
calibration remain to be verified for release.
