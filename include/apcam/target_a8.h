#ifndef APCAM_TARGET_A8_H
#define APCAM_TARGET_A8_H

/* Target properties shared by firmware, web UI and generated SITL data.
 * FOV calibration retains existing measurements/nominal values; see README. */
#define APCAM_NAME "a8"
#define APCAM_PRODUCT_NAME "A8 mini"
#define APCAM_MODEL_NAME "SIYI A8 mini"
#define APCAM_MANUFACTURER "SIYI"
#define APCAM_NUM_LENSES 1
#define APCAM_NUM_STREAMS 2
#define APCAM_STREAM1_LENS_MASK 1
#define APCAM_STREAM2_LENS_MASK 1
#define APCAM_HAVE_PHOTO 1
#define APCAM_HAVE_ZOOM 1
#define APCAM_NUM_RECORDING_CHANNELS 1
#define APCAM_VENDOR_PROTOCOL APCAM_PROTOCOL_SIYI
#define APCAM_HAVE_THERMAL 0
#define APCAM_HAVE_LIDAR 0
#define APCAM_ZOOM_NATIVE_RATE 0
#define APCAM_ZOOM_CONTROL_MAX APCAM_ZOOM_MAX
#define APCAM_WEB_CONTROL_MAVLINK 0
#define APCAM_HAVE_OVERLAY_RECORDING_SELECT 1
#define APCAM_HAVE_GIMBAL_RATES 1
#define APCAM_SUPPRESS_DUPLICATE_ANGLES 1
#define APCAM_WEB_CENTER_COMMAND 3
#define APCAM_HAVE_OPTICAL_ZOOM 0
/* Fixed-focus lens: the vendor-compatible focus commands are no-ops. */
#define APCAM_HAVE_FOCUS 0
#define APCAM_HAVE_IMAGE_CONTROLS 1
#define APCAM_HAVE_EXTERNAL_UART 1
#define APCAM_HAVE_SSH_KEYS 0
#define APCAM_HAVE_SOC_TEMPERATURE 0
#define APCAM_VENDOR_PORT 37260
#define APCAM_WEB_PORT 80
#define APCAM_LENS1_TYPE APCAM_LENS_TYPE_RGB
#define APCAM_LENS1_NAME "RGB"
#define APCAM_LENS1_FOV_H 88.0f
#define APCAM_LENS1_FOV_H_TELE 0.0f
#define APCAM_LENS1_FOV_MODEL APCAM_FOV_FOCAL_LENGTH
#define APCAM_LENS1_OPTICAL_ZOOM_MAX 1
#define APCAM_LENS1_WIDTH 3840
#define APCAM_LENS1_HEIGHT 2160
#define APCAM_ZOOM_MAX 6.0f
#define APCAM_FRAME_RATE 25
#define APCAM_THERMAL_FRAME_RATE 0
#define APCAM_THERMAL_STREAM_WIDTH 0
#define APCAM_THERMAL_STREAM_HEIGHT 0
#define APCAM_MAIN_RESOLUTIONS 7
#define APCAM_SUB_RESOLUTIONS 7
#define APCAM_RECORDING_RESOLUTIONS 7
#define APCAM_STREAM_CODECS 3
#define APCAM_DEFAULT_MAIN_RESOLUTION 1
#define APCAM_DEFAULT_SUB_RESOLUTION 0
#define APCAM_DEFAULT_RECORDING_RESOLUTION 1
#define APCAM_DEFAULT_SYSTEM_ID 0
#define APCAM_DEFAULT_POSITION_TARGETING 1
#define APCAM_DEFAULT_ORIENTATION 0
#define APCAM_DEFAULT_PHOTO_SCOPE 1
#define APCAM_DEFAULT_TIMEZONE "GMT-10"
#define APCAM_GIMBAL_PITCH_MIN -90.0f
#define APCAM_GIMBAL_PITCH_MAX 25.0f
#define APCAM_GIMBAL_YAW_CONTINUOUS 0
#define APCAM_GIMBAL_YAW_MIN -135.0f
#define APCAM_GIMBAL_YAW_MAX 135.0f
#define APCAM_GIMBAL_RATE_MAX 60.0f
#define APCAM_VENDOR_YAW_RATE_FULL_SCALE 90.0f
#define APCAM_VENDOR_PITCH_RATE_FULL_SCALE 74.0f
/* Inverted bench, September 2026: command +/-5 is stationary. Signed yaw
 * response is asymmetric. See docs/a8-rate-calibration.md for measurements. */
#define APCAM_VENDOR_PITCH_RATE_CURVE {-100, -74, -20, -14.6, -10, -7.3, -6, -4.3, -5, 0, 5, 0, 6, 4.3, 10, 7.3, 20, 14.6, 100, 74}
#define APCAM_VENDOR_YAW_RATE_CURVE {-100, -65, -80, -52, -60, -39.5, -40, -27, -20, -14.4, -10, -7.6, -6, -4.6, -5, 0, 5, 0, 6, 5, 10, 8.6, 20, 17.8, 40, 35.5, 60, 53.5, 80, 72, 100, 89}
#define APCAM_SIM_PITCH_RATE_CURVE {-100, -74, -20, -14.6, -10, -7.3, -6, -4.3, -5, 0, 5, 0, 6, 4.3, 10, 7.3, 20, 14.6, 100, 74}
#define APCAM_SIM_YAW_RATE_CURVE {-100, -65, -80, -52, -60, -39.5, -40, -27, -20, -14.4, -10, -7.6, -6, -4.6, -5, 0, 5, 0, 6, 5, 10, 8.6, 20, 17.8, 40, 35.5, 60, 53.5, 80, 72, 100, 89}
#define APCAM_SIM_RATE_TIME_CONSTANT 0.12f
#define APCAM_TRACKING_RATE_I 0.4f
#define APCAM_LOG_ROOT "/mnt/mmc/logs"
#define APCAM_GIMBAL_FEEDBACK_UPRIGHT_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, -1}
#define APCAM_GIMBAL_FEEDBACK_UPRIGHT_OFFSET {0, 0, 0}
#define APCAM_GIMBAL_FEEDBACK_INVERTED_MATRIX {1, 0, 0, 0, -1, 0, 0, 0, 1}
#define APCAM_GIMBAL_FEEDBACK_INVERTED_OFFSET {0, 180, 0}
#define APCAM_GIMBAL_PRIVATE_FEEDBACK_UPRIGHT_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, 1}
#define APCAM_GIMBAL_PRIVATE_FEEDBACK_UPRIGHT_OFFSET {0, 0, 0}
#define APCAM_GIMBAL_PRIVATE_FEEDBACK_INVERTED_MATRIX {1, 0, 0, 0, -1, 0, 0, 0, 1}
#define APCAM_GIMBAL_PRIVATE_FEEDBACK_INVERTED_OFFSET {0, 180, 0}
#define APCAM_GIMBAL_ANGLE_COMMAND_UPRIGHT_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, -1}
#define APCAM_GIMBAL_ANGLE_COMMAND_UPRIGHT_OFFSET {0, 0, 0}
/* Inverted A8 hardware reverses both absolute-command axes relative to
 * upright. SIYI 0x0e uses a level pitch zero (unlike attitude feedback's
 * 180-degree zero); rate commands retain their existing signs. */
#define APCAM_GIMBAL_ANGLE_COMMAND_INVERTED_MATRIX {1, 0, 0, 0, -1, 0, 0, 0, 1}
#define APCAM_GIMBAL_ANGLE_COMMAND_INVERTED_OFFSET {0, 0, 0}
#define APCAM_GIMBAL_RATE_COMMAND_UPRIGHT_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, 1}
#define APCAM_GIMBAL_RATE_COMMAND_UPRIGHT_OFFSET {0, 0, 0}
#define APCAM_GIMBAL_RATE_COMMAND_INVERTED_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, 1}
#define APCAM_GIMBAL_RATE_COMMAND_INVERTED_OFFSET {0, 0, 0}

/* Platform paths are consumed only by the web service. */
#ifdef APCAM_WEB_BUILD
#define FIRMWARE_PREFIX "A8_FW_"
/* U-Boot looks for this name on the card */
#define FIRMWARE_INSTALL_NAME "SIYI_4K_MINI_UpgradeSD.bin"
/* Persistent settings survive application updates. */
#ifdef MT11_WEB_SITL
#define APP_SELECTION_DIR APP_DIR
#else
#define APP_SELECTION_DIR "/config/camera-app"
#endif
#define APP_REQUEST_PATH "/tmp/camera-app.request"
#define APP_REQUEST_LOCK_PATH "/tmp/camera-app.request.lock"
#define APP_STARTED_PATH "/tmp/camera-app.started"
#define SWITCH_LOCK_PATH "/tmp/a8-web-switch.lock"
#define APP_STORAGE_PATH "/customer"
#define SETTINGS_STORAGE_PATH "/config"
#ifndef APP_DIR
#define APP_DIR "/customer/camera-app"
#endif
#ifndef MEDIA_ROOT
#define MEDIA_ROOT "/mnt/mmc"
#endif
#ifndef CAPTURE_ROOT
#define CAPTURE_ROOT MEDIA_ROOT "/capture"
#endif
#ifndef REPLACEMENT_CONFIG_PATH
#define REPLACEMENT_CONFIG_PATH "/config/camera-app/camera.ini"
#endif
#ifndef REPLACEMENT_CONFIG_BACKUP_PATH
#define REPLACEMENT_CONFIG_BACKUP_PATH "/config/camera-app/camera.ini.web.bak"
#endif
#ifndef PASSWORD_PATH
#define PASSWORD_PATH "/config/camera-app/web.pass"
#endif
#ifndef USER_LOCK_PATH
#define USER_LOCK_PATH "/tmp/a8-web-users.lock"
#endif
#ifndef UPGRADE_LOCK_PATH
#define UPGRADE_LOCK_PATH "/tmp/a8-web-upgrade.lock"
#endif
#ifndef TIME_SYNC_TEST_PATH
#define TIME_SYNC_TEST_PATH "/tmp/a8-web-time-sync-test"
#endif
#ifndef RUNTIME_DIR
#define RUNTIME_DIR "/tmp"
#endif
#ifndef CAMERA_READY_PATH
#define CAMERA_READY_PATH "/tmp/camera-app.ready"
#endif
#ifndef SESSION_PATH
#define SESSION_PATH "/tmp/a8-web-sessions"
#endif
#ifndef REPLACEMENT_CAMERA_PATH
#define REPLACEMENT_CAMERA_PATH "/customer/camera-app/camera-app"
#endif
#ifndef WEB_PATH
#define WEB_PATH "/customer/camera-app/a8-web"
#endif
#ifndef REPLACEMENT_LOG_PATH
#define REPLACEMENT_LOG_PATH "/tmp/camera-app.log"
#endif
#ifndef REPLACEMENT_LOG_OLD_PATH
#define REPLACEMENT_LOG_OLD_PATH "/tmp/camera-app.log.1"
#endif
#endif

#endif
