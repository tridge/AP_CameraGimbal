#ifndef APCAM_TARGET_H
#define APCAM_TARGET_H
#include "target_ids.h"
#ifndef APCAM_TARGET
#error "Set APCAM_TARGET in the build (for example APCAM_TARGET_MT11)"
#endif
#if APCAM_TARGET == APCAM_TARGET_MT11
#include "target_mt11.h"
#elif APCAM_TARGET == APCAM_TARGET_ZT30
#include "target_zt30.h"
#elif APCAM_TARGET == APCAM_TARGET_A8
#include "target_a8.h"
#elif APCAM_TARGET == APCAM_TARGET_ZR10
#include "target_zr10.h"
#elif APCAM_TARGET == APCAM_TARGET_Z1_MINI
#include "target_z1mini.h"
#elif APCAM_TARGET == APCAM_TARGET_RPI_LIBCAM_CADDX
#include "target_rpi_libcam_caddx.h"
#else
#error "Unsupported APCAM_TARGET"
#endif
#define APCAM_HAVE_SIYI (APCAM_VENDOR_PROTOCOL == APCAM_PROTOCOL_SIYI)
#define APCAM_HAVE_XFROBOT (APCAM_VENDOR_PROTOCOL == APCAM_PROTOCOL_XFROBOT)
#ifndef APCAM_HAVE_SITL
#define APCAM_HAVE_SITL 1
#endif
/* Targets whose gimbal link is optional set this to 1: without a gimbal
 * the camera advertises no gimbal component (ca_backend_has_gimbal()). */
#ifndef APCAM_GIMBAL_OPTIONAL
#define APCAM_GIMBAL_OPTIONAL 0
#endif
/* Targets with selectable photo sizes (needing a sensor mode switch) set this
 * to 1 and define APCAM_PHOTO_{BINNED,FULL}_{WIDTH,HEIGHT,NAME}. */
#ifndef APCAM_HAVE_PHOTO_RESOLUTION
#define APCAM_HAVE_PHOTO_RESOLUTION 0
#endif
/* Targets with a selectable recording resolution (REC_RESOLUTION, offered on the
 * web page). Targets that record the stream they serve set this to 0 and record
 * at the main stream's resolution. */
#ifndef APCAM_HAVE_RECORDING_RESOLUTION
#define APCAM_HAVE_RECORDING_RESOLUTION 1
#endif
/* Main stream encoder settings. A target opts in to each separately:
 *   APCAM_HAVE_VIDEO_BITRATE     VIDEO_MAIN_KBPS    ([stream.main] bitrate_kbps)
 *   APCAM_HAVE_VIDEO_FRAME_RATE  VIDEO_MAIN_FPS     ([stream.main] frame_rate)
 * The settings are in struct ca_config as main_bitrate_kbps and main_frame_rate.
 * To adopt one, define its flag, then have the backend read the value where it
 * opens its encoder instead of a fixed rate (the Pi backend does this in
 * rpi_libcam/media.cpp). Both are saved in the ini and applied after a restart,
 * except that a backend can apply a new bitrate while running by acting on it in
 * apply_image(); also add VIDEO_MAIN_KBPS to camera_definition.cpp so ground
 * stations see it as a live setting. A target with a separate sub stream adds the
 * same pair for it. The defaults are APCAM_DEFAULT_MAIN_BITRATE_KBPS and
 * APCAM_FRAME_RATE, which is also the highest frame rate offered.
 * A backend with its own bitrate rule (for example by resolution) can define
 * APCAM_MAIN_BITRATE_MIN_KBPS and APCAM_DEFAULT_MAIN_BITRATE_KBPS as 0, meaning
 * automatic: it then applies its rule when the setting is 0. */
#ifndef APCAM_HAVE_VIDEO_BITRATE
#define APCAM_HAVE_VIDEO_BITRATE 0
#endif
#ifndef APCAM_HAVE_VIDEO_FRAME_RATE
#define APCAM_HAVE_VIDEO_FRAME_RATE 0
#endif
#ifndef APCAM_DEFAULT_MAIN_BITRATE_KBPS
#define APCAM_DEFAULT_MAIN_BITRATE_KBPS 4096
#endif
#ifndef APCAM_MAIN_BITRATE_MIN_KBPS
#define APCAM_MAIN_BITRATE_MIN_KBPS 1000
#endif
#ifndef APCAM_MAIN_BITRATE_MAX_KBPS
#define APCAM_MAIN_BITRATE_MAX_KBPS 20000
#endif
#ifndef APCAM_MAIN_FPS_MIN
#define APCAM_MAIN_FPS_MIN 5
#endif
/* Targets on a host OS which owns networking set this to 0. */
#ifndef APCAM_HAVE_NETWORK_CONFIG
#define APCAM_HAVE_NETWORK_CONFIG 1
#endif
/* Targets whose gimbal link can be set in the configuration (RPi only) set this to 1. */
#ifndef APCAM_HAVE_UART_DEVICE_OVERRIDE
#define APCAM_HAVE_UART_DEVICE_OVERRIDE 0
#endif
#ifndef APCAM_LOG_ROOT
#define APCAM_LOG_ROOT "/mnt/logs"
#endif
#ifndef APCAM_TRACKING_RATE_I
#define APCAM_TRACKING_RATE_I 0.0f
#endif
#ifndef APCAM_TRACKING_MIN_PITCH_RATE
#define APCAM_TRACKING_MIN_PITCH_RATE 0.0f
#endif
#ifndef APCAM_TRACKING_MIN_YAW_RATE
#define APCAM_TRACKING_MIN_YAW_RATE 0.0f
#endif
#ifndef APCAM_SIM_OPTICAL_ZOOM_STEP
#define APCAM_SIM_OPTICAL_ZOOM_STEP 0.0f
#endif
/* Some modules retain zoom independently from the selected RGB source. */
#ifndef APCAM_ZOOM_SELECTS_LENS
#define APCAM_ZOOM_SELECTS_LENS 1
#endif
#ifndef APCAM_DEFAULT_RGB_LENS
#define APCAM_DEFAULT_RGB_LENS 0
#endif
#ifndef APCAM_LENS1_DIGITAL_ZOOM_MAX
#define APCAM_LENS1_DIGITAL_ZOOM_MAX (!APCAM_HAVE_ZOOM || (APCAM_HAVE_OPTICAL_ZOOM && APCAM_NUM_LENSES == 1) ? 1.0f : APCAM_ZOOM_MAX)
#endif
#ifndef APCAM_LENS2_DIGITAL_ZOOM_MAX
#define APCAM_LENS2_DIGITAL_ZOOM_MAX 1.0f
#endif
#ifndef APCAM_LENS2_FOV_MODEL
#define APCAM_LENS2_FOV_MODEL APCAM_FOV_FOCAL_LENGTH
#endif
#ifndef APCAM_LENS2_FOV_H_TELE
#define APCAM_LENS2_FOV_H_TELE 0.0f
#endif
#ifndef APCAM_LENS3_DIGITAL_ZOOM_MAX
#define APCAM_LENS3_DIGITAL_ZOOM_MAX 1.0f
#endif
/* String forms for configuration defaults; numeric IDs remain stable. */
#define APCAM_RESOLUTION_NAME(value) ((value) == APCAM_RES_720P ? "1280x720" : \
    (value) == APCAM_RES_1080P ? "1920x1080" : (value) == APCAM_RES_1440P ? "2560x1440" : \
    (value) == APCAM_RES_540P ? "960x540" : (value) == APCAM_RES_480P ? "854x480" : \
    (value) == APCAM_RES_360P ? "640x360" : (value) == APCAM_RES_240P ? "426x240" : \
    (value) == APCAM_RES_180P ? "320x180" : "3840x2160")
#define APCAM_STRING_VALUE_(value) #value
#define APCAM_STRING_VALUE(value) APCAM_STRING_VALUE_(value)
/* A fixed thermal video stream need not expose module controls or calibrated
 * temperature data (for example the initial native ZT30 backend). */
#ifndef APCAM_HAVE_RAW_THERMAL
#define APCAM_HAVE_RAW_THERMAL APCAM_HAVE_THERMAL
#endif
#ifndef APCAM_HAVE_THERMAL_PALETTE
#define APCAM_HAVE_THERMAL_PALETTE APCAM_HAVE_THERMAL
#endif
#ifndef APCAM_HAVE_THERMAL_VIDEO
#define APCAM_HAVE_THERMAL_VIDEO APCAM_HAVE_THERMAL
#endif
#endif
