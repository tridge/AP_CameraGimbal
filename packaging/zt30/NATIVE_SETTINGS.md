# Native ZT30 settings

The native application exposes the shared MT11 parameter groups through the web
Parameters page and MAVLink camera definition. The Video group includes
brightness, contrast, saturation, exposure compensation, ISO, shutter, metering
and white balance. Image controls apply to both RGB sensors without restarting.

Main and secondary RGB resolution/codec, recording resolution, network addresses
and SupportProxy connection settings apply after restarting the application.
RGB formats run from 720p to 2160p, using H.264 or H.265. The wide sensor remains
2560×1440; choosing a larger output scales that image. Browser RGB video uses an
independent 720p H.264 encoder. Thermal video stays 640×512 at 15 fps, including
when selected as the primary camera; its RGB companion uses the corresponding
main or secondary RGB format. Source changes do not restart the application.

Video capture saves paired RGB and thermal H.264 MP4 files and a raw thermal
FFV1 recording at the configured raw recording rate. Crosshairs and the thermal
FOV box are live settings; "Overlays in recordings" controls their inclusion
in saved video. The raw thermal plane always retains the original counts.
Thermal palette and raw streaming/recording rates also apply live.

## Field of view

The target uses SIYI's published horizontal FOV values:

| Camera | Horizontal FOV |
| --- | --- |
| Wide RGB | 84.5° |
| Zoom RGB at 1× | 58.1° |
| Zoom RGB at 30× | 2.14° |
| Thermal | 24.2°, assumed equal to MT11 |

RGB values come from the [SIYI ZT30 specifications](https://www.siyi.biz/en/product/tri-axis-multi-sensor-gimbal-pod/zt30/spec/).
The thermal value is an explicit project assumption, not a ZT30 measurement.
`THERMAL_HFOV_CD` (`[thermal] hfov_cd`) defaults to `2420`, in hundredths of a
degree. Set it to a measured value to refine thermal overlays, stream metadata
and survey footprints. Setting it to `0` disables thermal geometry; turn off the
thermal FOV overlay before doing so. The FOV box assumes aligned optical axes.
Intermediate zoom FOV uses provisional interpolation between the published
endpoints and requires fresh, valid zoom feedback.

## Remaining hardware limits

Automatic upright mounting is the supported native orientation. Inverted
coordinate transforms, optical zoom/focus actuation, image tracking,
side-by-side composition, lidar and calibrated thermometry are not qualified.
Raw thermal values are counts, not temperatures. Geographic ROI and survey
settings use the shared camera controllers; entering survey on a stationary
bench does not qualify flight capture or its georeferencing.

Thermal USB capture can stop producing frames following an application restart.
RGB continues running. A Linux reboot and a standard USB device reset have both
failed to recover this condition during bench tests; a camera power cycle
restored capture. The startup attempt budget remains enforced.

The native application starts the USB receive worker before palette and encoder
initialization, keeping submitted transfers serviced during that setup. Encoder
failures still fence ambiguous MI leases by exiting the process, but first allow
the independent USB worker up to two seconds to drain transfers and return the
interface to alternate zero. Startup/shutdown logs include negotiated UVC bytes,
transfer/empty/byte counts, maximum reap gap, packet/frame errors and cleanup
results. These changes address observed lifetime weaknesses; the intermittent
module stall's root cause is not yet confirmed.
