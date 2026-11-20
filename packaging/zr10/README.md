# SIYI ZR10 camera application and firmware

The ZR10 target supports Infinity6B0 / GC4663 hardware. AP CameraGimbal provides
MAVLink, SIYI control, video streaming, recording and the administration web UI,
using the camera's installed kernel, drivers and factory image tuning.

## Build

```sh
git submodule update --init --recursive
make release RELEASE_TARGETS=ZR10
```

This creates `release/<tag>/SIYI_ZR10/ZR10_UpgradeSD.bin` with installation and update
instructions, build information and checksums. The firmware updates only the
customer application partition; it retains the bootloader, kernel, drivers and
gimbal firmware.

Run `python3 tools/install_build_environment.py --targets zr10` on x86_64
Debian/Ubuntu to install the checksum-pinned Bootlin toolchain, SDK dependencies
and build tools. The builder uses checked-in sensor, ISP tuning and network
files from `packaging/zr10/platform`, verified against `SHA256SUMS`. It does not
read or download vendor firmware. `ZR10_PLATFORM_DIR` overrides that directory.

The installed camera must have the compatible platform from camera firmware
**v0.3.9 svn2499** (SIYI's **v0.4.3** firmware bundle). Its bootloader, kernel,
root filesystem and `/config` SDK modules remain in place. Our `demo.sh` loads
those modules and the retained GC4663 sensor driver, then starts AP.

`ZR10_CROSS_COMPILE` and `ZR10_WEB_PASSWORD` override the compiler and initial
web password. The default web login is **admin / ardupilot**. Lower-level
`make zr10_firmware` and `make zr10_package` produce the flash image and an
optional SD application archive respectively; `ZR10_FIRMWARE_OUT` and
`ZR10_PACKAGE_OUT` override those output paths.

## Installation and updates

Use the per-release README for first installation from a FAT32 microSD card.
Subsequent updates use the same SD procedure or **Status → Upgrade Firmware**
in the ArduPilot web UI. The web updater accepts `ZR10_UpgradeSD.bin` and
legacy `ZR10_FW_*.bin` names, validates the image and stages it on the card.
Wait for upload completion, then use **Reboot camera** to install it.

Settings and web credentials live in `/mnt/AP_CameraGimbal/zr10/config` and
survive application updates. Keep the card inserted. Without a mounted card,
a flash installation uses temporary defaults and disables media output.
Application failure leaves the web UI available for logs and restart. There
is no vendor-app selection or automatic vendor fallback. Restore SIYI firmware
with the original vendor SD image and its installation instructions.

The flash image contains no vendor camera application, Boa, sounds or graphics.
Our `zr10-uuid` helper implements the boot-time `sycamera Uuid` operation through
the installed chip UUID API, preserving the signed decimal input to MAC-address
generation. Other vendor utility arguments are rejected.

The optional SD application archive can be installed from a camera shell:

```sh
cd /mnt
tar -xzf ZR10_APP_ArduPilot_*.tar.gz
sh /mnt/zr10/install.sh
/mnt/AP_CameraGimbal/zr10/service.sh
```

Use an exact archive filename if several are present. The installer verifies
the payload and installed SDK compatibility, and preserves existing settings.
It requires executable permissions on the SD mount. After testing, use
`sh /mnt/AP_CameraGimbal/zr10/install.sh --enable-boot` for automatic startup.
`sh /mnt/AP_CameraGimbal/zr10/uninstall.sh` restores the original launch path
only for an SD installation with a saved vendor application. A flash
installation requires the original SIYI SD image to restore vendor operation.

## Operation and validation

- Web: `http://192.168.144.25/`.
- MAVLink: TCP/UDP 16001. SIYI: TCP/UDP 37260.
- RTSP: `rtsp://192.168.144.25:8554/video1` and `/video2`.
- Media: `/mnt/DCIM/capture` and `/mnt/DCIM/record`.
- Logs: `/tmp/camera-app.log` and `/tmp/zr10-web.log`.

The service enables `ALLMULTI` on `eth0` before starting UniGCS discovery,
matching the receive-filter workaround confirmed on A8. This accepts Ethernet
multicast without enabling promiscuous mode; the kernel still selects packets
for each socket. Discovery on physical ZR10 hardware remains to be verified.

Streaming and recording support 720p, 1080p and 1440p. Optical zoom is 1–10x.
Factory image tuning and autofocus are retained; manual image settings and
absolute focus are not implemented. High-resolution combinations may exceed
encoder or memory limits. Target properties, including FOV and mounting
transforms, are defined in `include/apcam/target_zr10.h`.

Run `make -C camera_app zr10-test`, `make -C web zr10-test` and
`make zr10_sitl-test` for software checks. The release hardware checks for
normal/inverted gimbal control, cold boot, SD firmware installation and
sustained recording remain outstanding.
