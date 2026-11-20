# SIYI A8 mini packaging

`make a8_package` builds an SD-card update for the SIYI A8 mini that
replaces the vendor application partition with `camera-app`.

For a package ready for end users, use `make release RELEASE_TARGETS=A8`.
It creates `release/<tag>/SIYI_A8/SIYI_4K_MINI_UpgradeSD.bin`, installation and
update instructions in `README.md`, build information and SHA-256 checksums.
This exact filename works both on the SD card and in the AP web updater.

## What the update contains

The A8 mini boots U-Boot, a kernel and an initramfs root filesystem from
SPI-NOR and mounts a 6 MiB JFFS2 "customer" partition holding the SIYI
application, its kernel modules and settings. Only that partition is
rewritten by this update; U-Boot, the kernel and the vendor root filesystem
(including its telnet service and the network upgrade daemon) stay as
shipped in camera firmware v0.3.7.

The new partition holds:

- `camera-app/camera-app`, `camera-app/a8-uuid`, `camera-app/a8-web` (the
  administrative web interface on port 80, see `web/README.md`),
  `camera-app/VERSION`
- `camera-app/web.pass.default`, the initial web password (`A8_WEB_PASSWORD`,
  default `ardupilot`, user `admin`), seeded to `/config/camera-app/web.pass`
- `camera-app/camera.ini.default`, copied to `/config/camera-app/camera.ini`
  on first boot. `/config` is the vendor settings partition, which this
  update does not touch, so edits survive later updates.
- `bin/cardv`: `app_init.sh`. The vendor root filesystem starts
  `/customer/bin/cardv` after loading the MI kernel modules and calls
  `cardv Uuid` to derive the Ethernet MAC, so the launcher lives under that
  name and answers the UUID query with `a8-uuid`.
- `modules/`, `8836_imx678_v6.bin` (ISP tuning) and `network_config.ini` from
  the vendor v0.3.7 image
- `.sys_upgrade_finish_flag`, which the vendor boot script requires before it
  starts the application

The image does not contain the vendor camera application, Boa, sounds, fonts,
OSD graphics, Wi-Fi applications or debugging tools. The retained rootfs still
attempts to execute `/customer/boa/boa`; that file is absent, so the attempt
returns without starting a server. AP owns HTTP port 80.

## Application startup

The launcher starts AP CameraGimbal and ignores legacy app_selection files.
If startup fails or the camera app exits, the web service remains available for
logs, parameter changes and restart. It never starts cardv as a fallback.
The SD watcher remains active independently of camera restarts.

Startup enables `ALLMULTI` on `eth0` for the retained vendor Ethernet driver.
On the tested A8, joining `224.0.0.1` installed the expected multicast group
and MAC entry but discovery packets were still dropped; enabling `ALLMULTI`
restored UniGCS discovery. This does not enable promiscuous reception.

## Digital zoom pipeline

The sensor feeds ISP input port 0 in realtime. ISP output port 1 supplies
buffered YUV420 frames to SCL device 1, whose three output ports feed the main
stream, substream and recording encoders. Overlays attach to that same scaler
device. This allows all three outputs to upscale the digital zoom crop while
retaining their configured resolutions, including 4K recording.

Do not switch this connection back to ISP output 0 and realtime SCL device 0:
upscaling the live sensor feed can overflow its FIFO and corrupt frames. On
the A8 this produced severe flicker at 2x zoom and above with 1080p streaming
and 4K recording. The buffered path was tested on hardware from 1x through
6x, with all three scaler outputs maintaining 25 fps.

`make -C camera_app a8-zoom-test` checks the production ISP/scaler/encoder
routing, zoom crops, orientation and overlay destination. Hardware testing
is still needed to detect pixel corruption and verify scaler throughput.

## Building

Run `python3 tools/install_build_environment.py --targets a8` on x86_64
Debian/Ubuntu to install the toolchain, SDK dependencies and build tools.
The generated `build/environment.mk` configures subsequent Make invocations.
Existing toolchains can still be selected with `A8_CROSS_COMPILE` or
`A8_TOOLCHAIN_DIR`.

The builder reads the checked-in files under `packaging/a8/platform`, checks
their SHA-256 values and constructs a new customer filesystem. It does not
read or download a vendor firmware image. `A8_PLATFORM_DIR` overrides the
platform directory. `make a8_package` produces
`build/A8_FW_ArduPilot_<tag>_<hash>.bin` with a checksum; `make release` uses
the bootloader's filename directly.

## Installing

1. The camera must already run vendor camera firmware v0.3.7 and gimbal
   firmware v0.4.9 (U-Boot environment and root filesystem from that
   release are relied upon).
2. Copy the package to a FAT32 microSD card as `SIYI_4K_MINI_UpgradeSD.bin`
   (the name U-Boot looks for) and power the camera on with the card
   inserted. U-Boot rewrites the customer partition and boots; the launcher
   removes the file from the card afterwards.
3. The camera keeps 192.168.144.25 and the vendor telnet login.
   `camera-app` listens on SIYI 37260, MAVLink 16001, RTSP 8554
   (`/video1`, `/video2`) and writes captures and recordings to the card
   under `/mnt/mmc/DCIM/capture` and `/mnt/mmc/DCIM/record`, the DCIM folder
   the vendor firmware also uses. The web interface is at
   `http://192.168.144.25/`; its login page takes user `admin` with the
   package's web password (`ardupilot` by default) and offers the interface
   in English, Simplified Chinese or Japanese. Later updates can be uploaded
   from its Status page and take effect on reboot. Its log is
   `/tmp/camera-app.log` (truncated once it passes 512 KiB, checked every
   5 s, with the previous tail kept in `camera-app.log.1`).

The update erases the application partition, so a modified
`/customer/network_config.ini` (static IP) is reset to the vendor default
and the MAC-address file is regenerated (the derived address is the same).
For recovery, use the web Debug and restart controls. The launcher itself can
be stopped with `kill` (it stops its child).

The U-Boot on this camera cannot verify the file before flashing. The
script pre-fills the load buffer with 0xff and loads the image before
erasing, so a short read leaves empty JFFS2 blocks rather than garbage,
but a card that fails to read at all still leaves an empty application
partition: the camera then boots the vendor root filesystem with telnet and
Ethernet but without an application, and the SD update simply needs to be
repeated. Check the copied file against its SHA-256 before rebooting.

## Rolling back

Write the vendor v0.3.7 SD image to the card as
`SIYI_4K_MINI_UpgradeSD.bin` and boot; it restores every partition it
covers. This is the preferred path.

If the card path is unavailable, the vendor customer partition can be
restored over telnet from the 0x600000 bytes at offset 0x504000 of the
vendor SD image, but only with the filesystem unmounted: stop the
launcher and application (`kill` the `bin/cardv` shell and `camera-app`),
`umount /customer/config`, `umount /customer`, then
`dd if=customer.jffs2 of=/dev/mtdblock4 bs=65536 && sync` and reboot.
Writing to `/dev/mtdblock4` while `/customer` is mounted corrupts it.
