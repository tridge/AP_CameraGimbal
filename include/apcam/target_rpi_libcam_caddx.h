#ifndef APCAM_TARGET_RPI_LIBCAM_CADDX_H
#define APCAM_TARGET_RPI_LIBCAM_CADDX_H

/* Target properties shared by firmware, web UI and generated SITL data.
 * RPi (CM4) + libcamera IMX477 CSI camera + CADDX gimbal, same as
 * XFRobot C20-T. Video uses src/backends/rpi_libcam and the Pi 4 / CM4 V4L2
 * H.264 encoder. The gimbal uses src/backends/caddx. */
#define APCAM_NAME "rpi_libcam_caddx"
#define APCAM_PRODUCT_NAME "RPi CADDX"
#define APCAM_MODEL_NAME "RPi IMX477 CADDX"
#define APCAM_MANUFACTURER "RPi"
#define APCAM_NUM_LENSES 1
/* One encoded stream, served as /video1 and used for recording. */
#define APCAM_NUM_STREAMS 1
/* The recording is that stream, so it must use the main resolution and has no
 * resolution setting of its own. */
#define APCAM_HAVE_RECORDING_RESOLUTION 0
#define APCAM_STREAM1_LENS_MASK 1
#define APCAM_STREAM2_LENS_MASK 1
#define APCAM_HAVE_PHOTO 1
/* Photos from a video frame, or from the IMX477 2x2 binned or full sensor
 * modes after a brief switch away from video. */
#define APCAM_HAVE_PHOTO_RESOLUTION 1
#define APCAM_PHOTO_BINNED_WIDTH 2028
#define APCAM_PHOTO_BINNED_HEIGHT 1520
#define APCAM_PHOTO_BINNED_NAME "2028x1520"
#define APCAM_PHOTO_FULL_WIDTH 4056
#define APCAM_PHOTO_FULL_HEIGHT 3040
#define APCAM_PHOTO_FULL_NAME "4056x3040"
#define APCAM_HAVE_ZOOM 0
#define APCAM_NUM_RECORDING_CHANNELS 1
/* MAVLink only; the companion computer has no vendor control service. */
#define APCAM_VENDOR_PROTOCOL APCAM_PROTOCOL_NONE
#define APCAM_HAVE_THERMAL 0
#define APCAM_HAVE_LIDAR 0
#define APCAM_ZOOM_NATIVE_RATE 0
#define APCAM_ZOOM_CONTROL_MAX APCAM_ZOOM_MAX
#define APCAM_WEB_CONTROL_MAVLINK 1
#define APCAM_HAVE_OVERLAY_RECORDING_SELECT 0
#define APCAM_HAVE_GIMBAL_RATES 0
/* The gimbal needs a UART device ([network] uart_device_override, --uart or
 * CAMERA_APP_UART); without one no gimbal is advertised. */
#define APCAM_HAVE_UART_DEVICE_OVERRIDE 1
#define APCAM_GIMBAL_OPTIONAL 1
#define APCAM_SUPPRESS_DUPLICATE_ANGLES 0
#define APCAM_WEB_CENTER_COMMAND 3
#define APCAM_HAVE_OPTICAL_ZOOM 0
#define APCAM_HAVE_FOCUS 0
#define APCAM_HAVE_IMAGE_CONTROLS 1
#define APCAM_HAVE_EXTERNAL_UART 0
#define APCAM_HAVE_SSH_KEYS 0
#define APCAM_HAVE_SOC_TEMPERATURE 1
/* Hardware only for now: no simulated camera, web UI or SITL launcher entry. */
#define APCAM_HAVE_SITL 0
/* The host OS (for example Rpanion-server) owns the network interfaces. */
#define APCAM_HAVE_NETWORK_CONFIG 0
#ifndef SOC_TEMPERATURE_PATH
#define SOC_TEMPERATURE_PATH "/sys/class/thermal/thermal_zone0/temp"
#endif
#define APCAM_VENDOR_PORT 0
#define APCAM_WEB_PORT 8080
#define APCAM_LENS1_TYPE APCAM_LENS_TYPE_RGB
#define APCAM_LENS1_NAME "RGB"
/* Nominal C20-T lens value; not yet measured. */
#define APCAM_LENS1_FOV_H 70.0f
#define APCAM_LENS1_FOV_H_TELE 0.0f
#define APCAM_LENS1_FOV_MODEL APCAM_FOV_FOCAL_LENGTH
#define APCAM_LENS1_OPTICAL_ZOOM_MAX 1
#define APCAM_LENS1_WIDTH 4056
#define APCAM_LENS1_HEIGHT 3040
#define APCAM_ZOOM_MAX 1.0f
/* The Pi 4 / CM4 hardware encoder is limited to 1080p30, so 30 fps is the
 * default and the highest rate; VIDEO_MAIN_FPS can lower it. */
#define APCAM_HAVE_VIDEO_BITRATE 1
#define APCAM_HAVE_VIDEO_FRAME_RATE 1
#define APCAM_FRAME_RATE 30
#define APCAM_THERMAL_FRAME_RATE 0
#define APCAM_THERMAL_STREAM_WIDTH 0
#define APCAM_THERMAL_STREAM_HEIGHT 0
/* One encoded stream serves the RTSP path (/video1) and recording. The hardware
 * encoder stops at 1080p; smaller sizes save bandwidth. */
#define APCAM_MAIN_RESOLUTIONS (APCAM_RES_MASK_1080P | APCAM_RES_MASK_720P | APCAM_RES_MASK_540P | \
    APCAM_RES_MASK_480P | APCAM_RES_MASK_360P | APCAM_RES_MASK_240P | APCAM_RES_MASK_180P)
#define APCAM_SUB_RESOLUTIONS APCAM_RES_MASK_1080P
#define APCAM_RECORDING_RESOLUTIONS APCAM_RES_MASK_1080P
#define APCAM_STREAM_CODECS 1
#define APCAM_DEFAULT_MAIN_RESOLUTION APCAM_RES_1080P
#define APCAM_DEFAULT_SUB_RESOLUTION APCAM_RES_1080P
#define APCAM_DEFAULT_RECORDING_RESOLUTION APCAM_RES_1080P
#define APCAM_DEFAULT_SYSTEM_ID 0
#define APCAM_DEFAULT_POSITION_TARGETING 0
#define APCAM_DEFAULT_ORIENTATION 1
#define APCAM_DEFAULT_PHOTO_SCOPE 1
#define APCAM_DEFAULT_TIMEZONE "GMT"
/* vendor-compatible RTSP path alias for /video1; the sub alias is unused (one stream) */
#define APCAM_DEFAULT_MAIN_ALIAS ""
#define APCAM_DEFAULT_SUB_ALIAS ""
#define APCAM_GIMBAL_PITCH_MIN -90.0f
#define APCAM_GIMBAL_PITCH_MAX 30.0f
#define APCAM_GIMBAL_YAW_CONTINUOUS 0
#define APCAM_GIMBAL_YAW_MIN -180.0f
#define APCAM_GIMBAL_YAW_MAX 180.0f
#define APCAM_GIMBAL_RATE_MAX 60.0f
#define APCAM_VENDOR_YAW_RATE_FULL_SCALE 60.0f
#define APCAM_VENDOR_PITCH_RATE_FULL_SCALE 60.0f
#define APCAM_GIMBAL_FEEDBACK_UPRIGHT_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, 1}
#define APCAM_GIMBAL_FEEDBACK_UPRIGHT_OFFSET {0, 0, 0}
#define APCAM_GIMBAL_FEEDBACK_INVERTED_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, 1}
#define APCAM_GIMBAL_FEEDBACK_INVERTED_OFFSET {0, 0, 0}
#define APCAM_GIMBAL_PRIVATE_FEEDBACK_UPRIGHT_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, 1}
#define APCAM_GIMBAL_PRIVATE_FEEDBACK_UPRIGHT_OFFSET {0, 0, 0}
#define APCAM_GIMBAL_PRIVATE_FEEDBACK_INVERTED_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, 1}
#define APCAM_GIMBAL_PRIVATE_FEEDBACK_INVERTED_OFFSET {0, 0, 0}
#define APCAM_GIMBAL_ANGLE_COMMAND_UPRIGHT_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, 1}
#define APCAM_GIMBAL_ANGLE_COMMAND_UPRIGHT_OFFSET {0, 0, 0}
#define APCAM_GIMBAL_ANGLE_COMMAND_INVERTED_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, 1}
#define APCAM_GIMBAL_ANGLE_COMMAND_INVERTED_OFFSET {0, 0, 0}
#define APCAM_GIMBAL_RATE_COMMAND_UPRIGHT_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, 1}
#define APCAM_GIMBAL_RATE_COMMAND_UPRIGHT_OFFSET {0, 0, 0}
#define APCAM_GIMBAL_RATE_COMMAND_INVERTED_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, 1}
#define APCAM_GIMBAL_RATE_COMMAND_INVERTED_OFFSET {0, 0, 0}
/* Pi OS has no /mnt media card; the service may override these. */
#define APCAM_LOG_ROOT "/var/lib/ap_cameragimbal/logs"

/* Platform paths are consumed only by the web service. systemd supervises
 * both services: the web stops camera-app and systemd starts a fresh one. */
#ifdef APCAM_WEB_BUILD
/* Updates come from the source tree (git pull, make, install), not packages. */
#define WEB_HAVE_FIRMWARE_UPLOAD 0
#define FIRMWARE_PREFIX "RPi_"
#ifndef APP_DIR
#define APP_DIR "/etc/ap_cameragimbal"
#endif
#ifndef MEDIA_ROOT
#define MEDIA_ROOT "/var/lib/ap_cameragimbal"
#endif
/* Recordings live on the root filesystem, not a separately mounted card. */
#define WEB_MEDIA_IS_MOUNT 0
#ifndef CAPTURE_ROOT
#define CAPTURE_ROOT MEDIA_ROOT "/capture"
#endif
#ifndef REPLACEMENT_CONFIG_PATH
#define REPLACEMENT_CONFIG_PATH APP_DIR "/camera.ini"
#endif
#ifndef REPLACEMENT_CONFIG_BACKUP_PATH
#define REPLACEMENT_CONFIG_BACKUP_PATH APP_DIR "/camera.ini.web.bak"
#endif
#ifndef PASSWORD_PATH
#define PASSWORD_PATH APP_DIR "/web.pass"
#endif
#ifndef REPLACEMENT_CAMERA_PATH
#define REPLACEMENT_CAMERA_PATH "/usr/local/bin/ap-camera-app"
#endif
#ifndef WEB_PATH
#define WEB_PATH "/usr/local/bin/ap-camera-web"
#endif
#ifndef WEBROOT_PATH
#define WEBROOT_PATH "/usr/local/share/ap_cameragimbal/webroot"
#endif
#ifndef APP_SELECTION_DIR
#define APP_SELECTION_DIR APP_DIR
#endif
#ifndef APP_REQUEST_PATH
#define APP_REQUEST_PATH "/run/camera-app.request"
#endif
#ifndef APP_REQUEST_LOCK_PATH
#define APP_REQUEST_LOCK_PATH "/run/camera-app.request.lock"
#endif
#ifndef APP_STARTED_PATH
#define APP_STARTED_PATH "/run/camera-app.started"
#endif
#define APP_STORAGE_PATH "/usr/local/bin"
#define SETTINGS_STORAGE_PATH APP_DIR
#ifndef REBOOT_COMMAND
#define REBOOT_COMMAND "systemctl reboot"
#endif
/* camera-app logs to the journal of its systemd unit. */
#ifndef APP_LOG_COMMAND
#define APP_LOG_COMMAND "journalctl --unit=ap-cameragimbal --boot --lines=2000 --output=cat --no-pager"
#endif
#endif

#endif
