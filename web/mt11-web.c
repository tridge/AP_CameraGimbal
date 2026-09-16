#define _GNU_SOURCE
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "build/version.h"
#include "build/icons.h"
#include "../include/apcam/config_status.h"
#include "../include/apcam/network.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <ftw.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifndef __CYGWIN__
#include <sys/reboot.h>
#include <sys/syscall.h>
#endif
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Shared web UI with target-specific paths, capabilities and update formats.
 * Z1-Mini uses the kernel SoC thermal sensor and MAVLink for web controls. */
#define APCAM_WEB_BUILD 1
#include "../include/apcam/target.h"
#include "../include/apcam/gimbal_transform.h"
#include "../include/apcam/manual_control.h"
#define SERVER_NAME APCAM_NAME "-web/2.0"
#define PRODUCT_NAME APCAM_PRODUCT_NAME
#define WEB_HAVE_THERMAL APCAM_HAVE_THERMAL
#define WEB_HAVE_SSH_KEYS APCAM_HAVE_SSH_KEYS
#define WEB_HAVE_SOC_TEMPERATURE APCAM_HAVE_SOC_TEMPERATURE
#if APCAM_TARGET == APCAM_TARGET_ZR10
#include "build/zr10_upgrade.h"
#endif
#ifndef FIRMWARE_SUFFIX
#define FIRMWARE_SUFFIX ".bin"
#endif
#ifdef FIRMWARE_INSTALL_NAME
#define FIRMWARE_NAME_PATTERNS FIRMWARE_INSTALL_NAME " / " FIRMWARE_PREFIX "*" FIRMWARE_SUFFIX
#else
#define FIRMWARE_NAME_PATTERNS FIRMWARE_PREFIX "*" FIRMWARE_SUFFIX
#endif
/* Z1-Mini: the web server verifies and installs the .gcu overlay itself, then reboots. */
#if APCAM_TARGET == APCAM_TARGET_Z1_MINI
#define WEB_INSTALLS_FIRMWARE 1
#else
#define WEB_INSTALLS_FIRMWARE 0
#endif
#if APCAM_HAVE_THERMAL || WEB_INSTALLS_FIRMWARE
#define WEB_UPGRADE_REBOOTS_JS "true"
#else
#define WEB_UPGRADE_REBOOTS_JS "false"
#endif
#define DEFAULT_CAMERA_KIND CAMERA_REPLACEMENT
#if APCAM_HAVE_THERMAL
#define TARGET_TEXT(mt11, a8) mt11
#else
#define TARGET_TEXT(mt11, a8) a8
#endif
#if APCAM_WEB_CONTROL_MAVLINK
#define DEFAULT_PORT APCAM_WEB_PORT
#define CAMERA_API_PORT 14550U
#include "z1mini_mavlink.h"
#else
#define DEFAULT_PORT APCAM_WEB_PORT
#define CAMERA_API_PORT 37260U
#endif
#define LIVE_VIDEO_PORT 8555U
#define ARDUPILOT_LOGO_URL \
    "https://firmware.ardupilot.org/Tools/Logos/ArduPilot-Cleaned-Transparent.png"
#define MAX_HEADER (16U * 1024U)
#define MAX_BODY (256U * 1024U)
#define MAX_CONFIG (64U * 1024U)
#define MAX_AUTHORIZED_KEYS (64U * 1024U)
#define MAX_FIRMWARE_SIZE (128U * 1024U * 1024U)
#ifndef SOC_TSENSOR_DEVICE
#define SOC_TSENSOR_DEVICE "/dev/mem"
#endif
#ifndef SOC_TSENSOR_MAP_OFFSET
#define SOC_TSENSOR_MAP_OFFSET UINT64_C(0x1102e000)
#endif
#define SOC_TSENSOR_MAP_SIZE 4096U
#define SOC_TSENSOR_STATUS0 0x8U
#define SOC_TSENSOR_STRIDE 0x100U
#define SOC_TSENSOR_COUNT 3U
#ifndef APP_DIR
#define APP_DIR "/app"
#endif
#ifndef MEDIA_ROOT
#define MEDIA_ROOT "/mnt"
#endif
#ifndef CAPTURE_ROOT
#define CAPTURE_ROOT MEDIA_ROOT "/DCIM/capture"
#endif
#ifndef REPLACEMENT_CONFIG_PATH
#define REPLACEMENT_CONFIG_PATH "/app/camera.ini"
#endif
#ifndef REPLACEMENT_CONFIG_BACKUP_PATH
#define REPLACEMENT_CONFIG_BACKUP_PATH "/app/camera.ini.web.bak"
#endif
#ifndef PASSWORD_PATH
#define PASSWORD_PATH "/app/web.pass"
#endif
#ifndef AUTHORIZED_KEYS_PATH
#define AUTHORIZED_KEYS_PATH "/app/dropbear/authorized_keys"
#endif
#ifndef RUNTIME_AUTHORIZED_KEYS_PATH
#define RUNTIME_AUTHORIZED_KEYS_PATH "/dev/dropbear-auth/authorized_keys"
#endif
#ifndef RUNTIME_AUTHORIZED_KEYS_DIR
#define RUNTIME_AUTHORIZED_KEYS_DIR "/dev/dropbear-auth"
#endif
#ifndef USER_LOCK_PATH
#define USER_LOCK_PATH "/run/mt11-web-users.lock"
#endif
#ifndef UPGRADE_LOCK_PATH
#define UPGRADE_LOCK_PATH "/run/mt11-web-upgrade.lock"
#endif
#ifndef TIME_SYNC_TEST_PATH
#define TIME_SYNC_TEST_PATH "/run/mt11-web-time-sync-test"
#endif
#ifndef RUNTIME_DIR
#define RUNTIME_DIR "/run"
#endif
#ifndef CAMERA_READY_PATH
#define CAMERA_READY_PATH "/run/camera-app.ready"
#endif
#ifndef SESSION_PATH
#define SESSION_PATH RUNTIME_DIR "/mt11-web-sessions"
#endif
#ifndef REPLACEMENT_CAMERA_PATH
#define REPLACEMENT_CAMERA_PATH "/app/bin/camera-app"
#endif
#ifndef WEB_PATH
#define WEB_PATH "/app/bin/mt11-web"
#endif
#ifndef REPLACEMENT_LOG_PATH
#define REPLACEMENT_LOG_PATH "/run/camera_app.log"
#endif
#ifndef REPLACEMENT_LOG_OLD_PATH
#define REPLACEMENT_LOG_OLD_PATH "/run/camera_app.log.1"
#endif
#define APP_LOG_ROTATE_SIZE (512U * 1024U)
#define APP_LOG_DISPLAY_SIZE (256U * 1024U)
#define RECEIVE_INCOMPLETE (-1)
#define SESSION_LIFETIME_SECONDS (24U * 60U * 60U)
#define MAX_SESSIONS 16U
#define MAX_SESSION_FILE (MAX_SESSIONS * 128U)

static int listen_fd = -1;
static char csrf_token[65];
/* separate token for the unauthenticated login form so the page cannot
 * leak the CSRF token that guards authenticated actions */
static char login_token[65];
#ifdef WEB_PORTABLE_SITL
static char session_epoch[65];
#endif

struct string_buffer {
    char *data;
    size_t len;
    size_t cap;
};

/* ---- user-visible strings ---- */
/* user-visible text, one row per id and one column per language;
 * T() returns the row for the language of the current request */
enum language {
    LANG_EN,
    LANG_ZH,
    LANG_JA,
    LANG_COUNT
};

static const struct {
    const char *code;
    const char *html_lang;
} languages[LANG_COUNT] = {
    [LANG_EN] = {"en", "en"},
    [LANG_ZH] = {"zh", "zh-CN"},
    [LANG_JA] = {"ja", "ja"},
};

#ifdef WEB_PORTABLE_SITL
#undef APP_DIR
static char portable_app_dir[4096];
#define APP_DIR portable_app_dir
#undef MEDIA_ROOT
static char portable_media_root[4096];
#define MEDIA_ROOT portable_media_root
#undef CAPTURE_ROOT
static char portable_capture_root[4096];
#define CAPTURE_ROOT portable_capture_root
#undef REPLACEMENT_CONFIG_PATH
static char portable_replacement_config_path[4096];
#define REPLACEMENT_CONFIG_PATH portable_replacement_config_path
#undef REPLACEMENT_CONFIG_BACKUP_PATH
static char portable_replacement_config_backup_path[4096];
#define REPLACEMENT_CONFIG_BACKUP_PATH portable_replacement_config_backup_path
#undef PASSWORD_PATH
static char portable_password_path[4096];
#define PASSWORD_PATH portable_password_path
#undef AUTHORIZED_KEYS_PATH
static char portable_authorized_keys_path[4096];
#define AUTHORIZED_KEYS_PATH portable_authorized_keys_path
#undef RUNTIME_AUTHORIZED_KEYS_PATH
static char portable_runtime_authorized_keys_path[4096];
#define RUNTIME_AUTHORIZED_KEYS_PATH portable_runtime_authorized_keys_path
#undef RUNTIME_AUTHORIZED_KEYS_DIR
static char portable_runtime_authorized_keys_dir[4096];
#define RUNTIME_AUTHORIZED_KEYS_DIR portable_runtime_authorized_keys_dir
#undef USER_LOCK_PATH
static char portable_user_lock_path[4096];
#define USER_LOCK_PATH portable_user_lock_path
#undef UPGRADE_LOCK_PATH
static char portable_upgrade_lock_path[4096];
#define UPGRADE_LOCK_PATH portable_upgrade_lock_path
#undef TIME_SYNC_TEST_PATH
static char portable_time_sync_test_path[4096];
#define TIME_SYNC_TEST_PATH portable_time_sync_test_path
#undef SOC_TSENSOR_DEVICE
static char portable_soc_tsensor_device[4096];
#define SOC_TSENSOR_DEVICE portable_soc_tsensor_device
#undef APP_SELECTION_DIR
static char portable_app_selection_dir[4096];
#define APP_SELECTION_DIR portable_app_selection_dir
#undef REPLACEMENT_LOG_PATH
static char portable_replacement_log_path[4096];
#define REPLACEMENT_LOG_PATH portable_replacement_log_path
#undef REPLACEMENT_LOG_OLD_PATH
static char portable_replacement_log_old_path[4096];
#define REPLACEMENT_LOG_OLD_PATH portable_replacement_log_old_path
#undef RUNTIME_DIR
static char portable_runtime_dir[4096];
#define RUNTIME_DIR portable_runtime_dir
#undef CAMERA_READY_PATH
static char portable_camera_ready_path[4096];
#define CAMERA_READY_PATH portable_camera_ready_path
#undef SESSION_PATH
static char portable_session_path[4096];
#define SESSION_PATH portable_session_path
#undef REPLACEMENT_CAMERA_PATH
static char portable_replacement_camera_path[4096];
#define REPLACEMENT_CAMERA_PATH portable_replacement_camera_path
#undef WEB_PATH
static char portable_web_path[4096];
#define WEB_PATH portable_web_path
#if WEB_INSTALLS_FIRMWARE
#undef GCU_ROOT
static char portable_gcu_root[4096];
#define GCU_ROOT portable_gcu_root
#endif
#undef APP_LIB_DIR
static char portable_app_lib_dir[4096];
#define APP_LIB_DIR portable_app_lib_dir
static int portable_paths_init(void)
{
    const char *root = getenv("CAMERA_GIMBAL_SITL_RUNTIME");
    if (!root || root[0] != '/') return -1;
    if (snprintf(APP_DIR, sizeof(APP_DIR), "%s/app", root) >= (int)sizeof(APP_DIR)) return -1;
    if (snprintf(MEDIA_ROOT, sizeof(MEDIA_ROOT), "%s/mnt", root) >= (int)sizeof(MEDIA_ROOT)) return -1;
    if (snprintf(CAPTURE_ROOT, sizeof(CAPTURE_ROOT), "%s/mnt/DCIM/capture", root) >= (int)sizeof(CAPTURE_ROOT)) return -1;
    if (snprintf(REPLACEMENT_CONFIG_PATH, sizeof(REPLACEMENT_CONFIG_PATH), "%s/app/camera.ini", root) >= (int)sizeof(REPLACEMENT_CONFIG_PATH)) return -1;
    if (snprintf(REPLACEMENT_CONFIG_BACKUP_PATH, sizeof(REPLACEMENT_CONFIG_BACKUP_PATH), "%s/app/camera.ini.web.bak", root) >= (int)sizeof(REPLACEMENT_CONFIG_BACKUP_PATH)) return -1;
    if (snprintf(PASSWORD_PATH, sizeof(PASSWORD_PATH), "%s/app/web.pass", root) >= (int)sizeof(PASSWORD_PATH)) return -1;
    if (snprintf(AUTHORIZED_KEYS_PATH, sizeof(AUTHORIZED_KEYS_PATH), "%s/app/dropbear/authorized_keys", root) >= (int)sizeof(AUTHORIZED_KEYS_PATH)) return -1;
    if (snprintf(RUNTIME_AUTHORIZED_KEYS_PATH, sizeof(RUNTIME_AUTHORIZED_KEYS_PATH), "%s/runtime-auth/authorized_keys", root) >= (int)sizeof(RUNTIME_AUTHORIZED_KEYS_PATH)) return -1;
    if (snprintf(RUNTIME_AUTHORIZED_KEYS_DIR, sizeof(RUNTIME_AUTHORIZED_KEYS_DIR), "%s/runtime-auth", root) >= (int)sizeof(RUNTIME_AUTHORIZED_KEYS_DIR)) return -1;
    if (snprintf(USER_LOCK_PATH, sizeof(USER_LOCK_PATH), "%s/run/users.lock", root) >= (int)sizeof(USER_LOCK_PATH)) return -1;
    if (snprintf(UPGRADE_LOCK_PATH, sizeof(UPGRADE_LOCK_PATH), "%s/run/upgrade.lock", root) >= (int)sizeof(UPGRADE_LOCK_PATH)) return -1;
    if (snprintf(TIME_SYNC_TEST_PATH, sizeof(TIME_SYNC_TEST_PATH), "%s/run/time-sync", root) >= (int)sizeof(TIME_SYNC_TEST_PATH)) return -1;
    if (snprintf(SOC_TSENSOR_DEVICE, sizeof(SOC_TSENSOR_DEVICE), "%s/run/soc-temperature", root) >= (int)sizeof(SOC_TSENSOR_DEVICE)) return -1;
    if (snprintf(APP_SELECTION_DIR, sizeof(APP_SELECTION_DIR), "%s/app", root) >= (int)sizeof(APP_SELECTION_DIR)) return -1;
    if (snprintf(REPLACEMENT_LOG_PATH, sizeof(REPLACEMENT_LOG_PATH), "%s/run/camera_app.log", root) >= (int)sizeof(REPLACEMENT_LOG_PATH)) return -1;
    if (snprintf(REPLACEMENT_LOG_OLD_PATH, sizeof(REPLACEMENT_LOG_OLD_PATH), "%s/run/camera_app.log.1", root) >= (int)sizeof(REPLACEMENT_LOG_OLD_PATH)) return -1;
    if (snprintf(RUNTIME_DIR, sizeof(RUNTIME_DIR), "%s/run", root) >= (int)sizeof(RUNTIME_DIR)) return -1;
    if (snprintf(CAMERA_READY_PATH, sizeof(CAMERA_READY_PATH), "%s/run/camera-app.ready", root) >= (int)sizeof(CAMERA_READY_PATH)) return -1;
    if (snprintf(SESSION_PATH, sizeof(SESSION_PATH), "%s/run/web-sessions", root) >= (int)sizeof(SESSION_PATH)) return -1;
    const char *replacement_camera_path = getenv("CAMERA_GIMBAL_SITL_CAMERA_EXE");
    if (!replacement_camera_path || snprintf(REPLACEMENT_CAMERA_PATH, sizeof(REPLACEMENT_CAMERA_PATH), "%s", replacement_camera_path) >= (int)sizeof(REPLACEMENT_CAMERA_PATH)) return -1;
    const char *web_path = getenv("CAMERA_GIMBAL_SITL_WEB_EXE");
    if (!web_path || snprintf(WEB_PATH, sizeof(WEB_PATH), "%s", web_path) >= (int)sizeof(WEB_PATH)) return -1;
    if (snprintf(APP_LIB_DIR, sizeof(APP_LIB_DIR), "%s/app/libs", root) >= (int)sizeof(APP_LIB_DIR)) return -1;
#if WEB_INSTALLS_FIRMWARE
    if (snprintf(GCU_ROOT, sizeof(GCU_ROOT), "%s/gcu", root) >= (int)sizeof(GCU_ROOT)) return -1;
#endif
    return 0;
}
#else
#define APP_LIB_DIR APP_DIR "/libs"
#endif

enum string_id {
    S_LANGUAGE_NAME,
    S_LANGUAGE,
    S_APPLY,
    S_NAV_TOP,
    S_NAV_STATUS,
    S_NAV_PARAMETERS,
    S_NAV_RAW,
    S_NAV_USERS,
    S_NAV_FILES,
    S_NAV_LIVE,
#if WEB_HAVE_THERMAL
    S_NAV_SENSORS,
    S_TITLE_SENSORS,
#else
    S_NAV_SENSORS,
    S_TITLE_SENSORS,
#endif
    S_NAV_DEBUG,
    S_NAV_LOGOUT,
    S_UNAVAILABLE,
    S_OUT_OF_MEMORY,
    S_NOT_FOUND,
    S_TITLE_LOGIN,
    S_LOGIN_SUBTITLE,
    S_LOGIN_USERNAME,
    S_LOGIN_PASSWORD,
    S_LOGIN_BUTTON,
    S_LOGIN_FIRST_TIME,
    S_LOGIN_SCRIPTED,
    S_LOGIN_EXPIRED,
    S_LOGIN_INCORRECT,
    S_LOGIN_ORIGIN,
    S_LOGOUT_FAILED,
    S_LOGIN_SESSION_FAILED,
    S_AUTH_REQUIRED,
    S_PASSWORD_FILE_MISSING,
    S_LANGUAGE_INVALID,
    S_OPT_DISABLED,
    S_OPT_ENABLED,
    S_OPT_WHILE_ARMED,
    S_OPT_AUTO,
    S_OPT_ISO_100,
    S_OPT_ISO_200,
    S_OPT_ISO_400,
    S_OPT_ISO_800,
    S_OPT_ISO_1600,
    S_OPT_ISO_3200,
    S_OPT_SHUTTER_30,
    S_OPT_SHUTTER_50,
    S_OPT_SHUTTER_100,
    S_OPT_SHUTTER_250,
    S_OPT_SHUTTER_500,
    S_OPT_SHUTTER_750,
    S_OPT_SHUTTER_1000,
    S_OPT_SHUTTER_2000,
    S_OPT_METER_AVERAGE,
    S_OPT_METER_CENTER,
    S_OPT_METER_SPOT_CENTER,
    S_OPT_WB_DAYLIGHT,
    S_OPT_WB_CLOUDY,
    S_OPT_WB_FLUORESCENT,
    S_OPT_WB_INCANDESCENT,
    S_OPT_SCOPE_THERMAL,
    S_OPT_SCOPE_ALL,
    S_OPT_ORIENT_AUTO,
    S_OPT_ORIENT_UPRIGHT,
    S_OPT_ORIENT_INVERTED,
    S_OPT_UART_NONE,
    S_OPT_UART_SIYI,
    S_OPT_UART_MAVLINK,
    S_OPT_RES_720,
    S_OPT_RES_1080,
    S_OPT_RES_4K,
    S_OPT_RES_1440,
    S_OPT_CODEC_H264,
    S_OPT_CODEC_H265,
    S_OPT_PAL_WHITE_HOT,
    S_OPT_PAL_SEPIA,
    S_OPT_PAL_IRONBOW,
    S_OPT_PAL_RAINBOW,
    S_OPT_PAL_NIGHT,
    S_OPT_PAL_AURORA,
    S_OPT_PAL_RED_HOT,
    S_OPT_PAL_JUNGLE,
    S_OPT_PAL_MEDICAL,
    S_OPT_PAL_BLACK_HOT,
    S_OPT_PAL_GLORY_HOT,
    S_P_BRIGHTNESS,
    S_P_SATURATION,
    S_P_CONTRAST,
    S_P_EXPOSURE_COMP,
    S_P_ISO,
    S_P_METERING,
    S_P_WHITE_BALANCE,
    S_H_CARDV_BRIGHTNESS,
    S_H_CARDV_SATURATION,
    S_H_CARDV_CONTRAST,
    S_P_TIMEZONE,
    S_H_TIMEZONE,
    S_P_PHOTO_SCOPE,
    S_H_PHOTO_SCOPE,
    S_P_ORIENTATION,
    S_H_ORIENTATION,
    S_P_UART_PROTOCOL,
    S_H_UART_PROTOCOL_MT11,
    S_H_UART_PROTOCOL_A8,
    S_P_MAVLINK_CAMERA_COMPID,
    S_H_MAVLINK_CAMERA_COMPID,
    S_OPT_CAMERA_COMP1,
    S_OPT_CAMERA_COMP2,
    S_OPT_CAMERA_COMP3,
    S_OPT_CAMERA_COMP4,
    S_OPT_CAMERA_COMP5,
    S_OPT_CAMERA_COMP6,
    S_P_MAVLINK_SYSID,
    S_H_MAVLINK_SYSID,
    S_P_MAVLINK_TCP,
    S_H_MAVLINK_TCP,
    S_P_MAVLINK_UDP,
    S_H_MAVLINK_UDP,
    S_P_LOG_DISARMED, S_H_LOG_DISARMED,
    S_P_TRACK_METHOD, S_H_TRACK_METHOD, S_OPT_TRACK_ANGLE, S_OPT_TRACK_RATE,
    S_RESTART_REQUIRED,
    S_P_POSITION_TARGETING,
    S_H_POSITION_TARGETING,
    S_P_THERMAL_PALETTE,
    S_H_THERMAL_PALETTE,
    S_P_AUTORECORD,
    S_H_AUTORECORD_APP,
    S_P_RECORDING_RESOLUTION,
    S_H_RECORDING_RESOLUTION_MT11,
    S_H_RECORDING_RESOLUTION_A8,
    S_P_MAIN_RESOLUTION,
    S_H_MAIN_RESOLUTION_MT11,
    S_H_MAIN_RESOLUTION_A8,
    S_P_MAIN_CODEC,
    S_H_MAIN_CODEC,
    S_P_SUB_RESOLUTION,
    S_H_SUB_RESOLUTION_MT11,
    S_H_SUB_RESOLUTION_A8,
    S_P_SUB_CODEC,
    S_H_SUB_CODEC,
    S_H_BRIGHTNESS_MT11,
    S_H_SATURATION_MT11,
    S_H_CONTRAST_MT11,
    S_H_EXPOSURE_COMP_APP,
    S_H_ISO_APP,
    S_P_SHUTTER,
    S_H_SHUTTER_APP,
    S_H_METERING_APP,
    S_H_WB_APP,
    S_E_CONFIG_SIZE,
    S_E_INI_UNTERMINATED_SECTION,
    S_E_INI_TEXT_AFTER_SECTION,
    S_E_INI_NOT_ASSIGNMENT,
    S_E_INI_EMPTY_KEY,
    S_E_INI_NO_SECTION,
    S_E_INI_DUPLICATE_KEY,
    S_E_INI_MISSING_KEY,
    S_E_CONFIG_TOO_LARGE,
    S_E_CONFIG_ALLOC,
    S_E_TOO_MANY_PARAMETERS,
    S_E_INVALID_VALUE,
    S_E_PATH_TOO_LONG,
    S_E_CANNOT_WRITE,
    S_E_CANNOT_CLOSE,
    S_E_CANNOT_INSTALL,
    S_E_CANNOT_SYNC,
    S_E_LOCK_USERS,
    S_E_KEYS_RUNTIME_DIR,
    S_E_KEYS_RUNTIME_ACTIVATION,
    S_E_PASSWORD_LENGTH,
    S_E_PASSWORD_CONTROL,
    S_E_PASSWORD_MISMATCH,
    S_E_BROWSER_TIME,
    S_E_SET_TIME,
    S_E_KEY_REQUIRED,
    S_E_KEYS_TOO_LARGE,
    S_E_KEY_MALFORMED,
    S_E_KEY_DUPLICATE_LINE,
    S_E_KEYS_READ,
    S_E_KEY_ALREADY,
    S_E_KEY_FILE_TOO_LARGE,
    S_E_KEY_REMOVE_UNCONFIRMED,
    S_E_KEY_SELECTION,
    S_E_KEY_ABSENT,
    S_E_KEY_LAST,
    S_E_CANNOT_STAT,
    S_E_CONFIG_PATH_LONG,
    S_E_CONFIG_BACKUP,
    S_E_CONFIG_WRITE,
    S_E_CONFIG_INSTALL,
    S_E_CANNOT_READ,
    S_E_NO_CONFIG_DATA,
    S_APP_REPLACEMENT,
    S_APP_GENERIC,
    S_APP_STOPPED,
    S_E_APP_SELECTION_PATH,
    S_E_CAMERA_NOT_RUNNING,
    S_E_CAMERA_API,
    S_E_MISSING_ACTION,
    S_E_MISSING_RATE,
    S_E_RATE_RANGE,
    S_E_MISSING_ZOOM,
    S_E_ZOOM_RANGE,
    S_E_UNKNOWN_ACTION,
    S_E_SEND_CONTROL,
    S_E_SEND_LIDAR,
    S_E_SEND_SHUTTER,
    S_E_SHUTTER_UNCONFIRMED,
    S_E_CAPTURE_FAILED,
    S_E_APP_STOP,
    S_E_CANNOT_START,
    S_E_NOT_READY,
    S_E_REQUEST_RECORD,
    S_E_REQUEST_LOCK,
    S_E_APP_NOT_UP,
    S_E_SWITCH_LOCK,
    S_NOT_MOUNTED,
    S_E_PATH_ABSOLUTE,
    S_E_PATH_RESOLVE,
    S_E_TARGET_MISSING,
    S_E_DELETE_BENEATH,
    S_E_TARGET_CHANGED,
    S_E_DELETE_FAILED,
    S_E_OPEN_DIRECTORY,
    S_E_VIEW_REGULAR,
    S_E_DELETE_EXISTING,
    S_E_DELETE_RESOLVE,
    S_E_DELETE_UNCONFIRMED,
    S_E_DELETE_PATH,
    S_E_DOWNLOAD_REGULAR,
    S_E_PATH_LONG_OR_INVALID,
    S_E_PATH_MISSING,
    S_PATH_DELETED,
    S_UNKNOWN_CURRENT_VALUE,
    S_TITLE_CONTROL,
    S_STATUS_SUBTITLE,
    S_STATUS_HEADING,
    S_PARAMS_PROXY,
    S_P_PROXY_ENABLED,
    S_H_PROXY_ENABLED,
    S_P_PROXY_HOST,
    S_H_PROXY_HOST,
    S_P_PROXY_MAVLINK_PORT,
    S_H_PROXY_MAVLINK_PORT,
    S_P_PROXY_SIGNING,
    S_H_PROXY_SIGNING,
    S_P_PROXY_SIGNING_PASSPHRASE,
    S_H_PROXY_SIGNING_PASSPHRASE,
    S_P_PROXY_SIGNING_LINK_ID,
    S_H_PROXY_SIGNING_LINK_ID,
    S_P_PROXY_VIDEO1_PORT,
    S_H_PROXY_VIDEO1_PORT,
    S_P_PROXY_VIDEO1_NAME,
    S_H_PROXY_VIDEO1_NAME,
    S_E_PROXY_VIDEO_PORTS,
    S_P_PROXY_VIDEO2_PORT,
    S_H_PROXY_VIDEO2_PORT,
    S_P_PROXY_VIDEO2_NAME,
    S_H_PROXY_VIDEO2_NAME,
    S_P_PROXY_PUBLISH_PASSWORD,
    S_H_PROXY_PUBLISH_PASSWORD,
    S_P_NETWORK_PRIMARY,
    S_H_NETWORK_PRIMARY,
    S_E_NETWORK,
    S_NETWORK_RECONNECT,
    S_NETWORK_CONNECTION_LOST,
    S_NETWORK_SITL,
    S_P_NETWORK_INTERFACE,
    S_H_NETWORK_INTERFACE,
    S_P_NETWORK_ADDRESS,
    S_H_NETWORK_ADDRESS,
    S_P_NETWORK_GATEWAY,
    S_H_NETWORK_GATEWAY,
    S_STATUS_FIRMWARE_VERSION,
    S_STATUS_CAMERA_APP,
    S_STATUS_PID_RSS,
    S_STATUS_WEB_SERVICE,
    S_STATUS_WEB_PID_RSS,
    S_STATUS_TIME,
    S_STATUS_SYNC,
    S_STATUS_UPTIME,
    S_STATUS_UPTIME_FORMAT,
    S_STATUS_CPU,
    S_STATUS_CPU_BUSY,
    S_STATUS_LOAD,
    S_STATUS_SOC_TEMPERATURE,
    S_STATUS_SOC_AVERAGE,
    S_STATUS_MEMORY,
    S_STATUS_MEMORY_VALUE,
    S_STATUS_IPV4,
    S_STORAGE_TMPFS,
    S_STORAGE_ROOTFS,
    S_STORAGE_APPLICATION,
    S_STORAGE_SETTINGS,
    S_STORAGE_MICROSD,
    S_STATUS_REFRESH,
    S_STATUS_ACTIONS,
    S_STATUS_RESTART,
    S_STATUS_UPGRADE_BUTTON,
    S_STATUS_UPGRADE_HELP,
    S_STATUS_UPGRADE_SYNC_MT11,
    S_STATUS_UPGRADE_SYNC_A8,
    S_STATUS_UPGRADE_INSTALL_Z1,
    S_STATUS_REBOOT_CONFIRM,
    S_STATUS_REBOOT_BUTTON,
    S_STATUS_AUTH_NOTE,
    S_TIME_SYNCED,
    S_REBOOT_UNCONFIRMED,
    S_TITLE_REBOOTING,
    S_REBOOTING_HEADING,
    S_REBOOTING_TEXT,
    S_CSRF_RELOAD,
    S_CSRF_INVALID,
    S_UNKNOWN_POST,
    S_SENSORS_SUBTITLE_MT11,
    S_SENSORS_SUBTITLE_A8,
    S_SENSORS_LIVE,
    S_SENSORS_LIDAR,
    S_LOADING,
    S_ENABLE,
    S_DISABLE,
    S_SENSORS_MIN,
    S_SENSORS_MAX,
    S_SENSORS_CPU,
    S_SENSORS_UPDATING,
    S_SENSORS_LASER_NOTICE,
    S_SENSORS_SHUTTER,
    S_SENSORS_SHUTTER_TEXT,
    S_SENSORS_CAPTURE,
    S_SENSORS_SCOPE_NOTE,
    S_SENSORS_RECENT,
    S_SENSORS_NO_PHOTOS,
    S_SENSORS_BROWSE,
    S_PHOTO_CAPTURED,
    S_E_LIDAR_ACTION,
    S_LIDAR_ENABLED,
    S_LIDAR_DISABLED,
    S_JS_UNAVAILABLE,
    S_JS_TEMPERATURE_AT,
    S_JS_NO_RETURN,
    S_JS_UPDATED,
    S_JS_TWICE_PER_SECOND,
    S_JS_SENSOR_FAILED,
    S_JS_UNKNOWN_ERROR,
    S_JS_WAITING_RANGE,
    S_JS_LIDAR_CONTROL_FAILED,
    S_TITLE_LIVE,
    S_LIVE_HEADING,
    S_LIVE_SUBTITLE,
    S_LIVE_STREAM,
    S_LIVE_MAIN,
    S_LIVE_SECONDARY,
    S_LIVE_STARTING,
    S_LIVE_HELP,
    S_LIVE_PTZ,
    S_LIVE_ENABLE_MANUAL,
    S_LIVE_MANUAL_NOTICE,
    S_LIVE_CENTRE,
    S_LIVE_RATE,
    S_LIVE_ZOOM,
    S_LIVE_NO_COMMANDS,
    S_LIVE_ATTITUDE,
    S_LIVE_ATTITUDE_UNAVAILABLE,
    S_LIVE_YAW,
    S_LIVE_ROLL,
    S_LIVE_PITCH,
    S_LIVE_YAW_RATE,
    S_LIVE_ROLL_RATE,
    S_LIVE_PITCH_RATE,
    S_LIVE_WAITING_GIMBAL,
    S_COMMAND_SENT,
    S_JS_CONNECTING,
    S_JS_RETRYING,
    S_JS_LIVE_PREFIX,
    S_JS_STREAM_ENDED,
    S_JS_LIVE_UNAVAILABLE,
    S_JS_ERROR_ABORTED,
    S_JS_ERROR_NETWORK,
    S_JS_ERROR_DECODE,
    S_JS_ERROR_UNSUPPORTED,
    S_JS_ERROR_CODE,
    S_JS_ENABLE_FIRST,
    S_JS_CONTROL_FAILED,
    S_JS_MANUAL_ENABLED,
    S_JS_MANUAL_DISABLED,
    S_JS_ATTITUDE_LABEL,
    S_JS_LIVE_ATTITUDE,
    S_JS_ATTITUDE_FAILED,
    S_TITLE_USERS,
    S_USERS_HEADING,
    S_USERS_SUBTITLE_MT11,
    S_USERS_SUBTITLE_A8,
    S_USERS_PASSWORD_HEADING,
    S_USERS_PASSWORD_TEXT,
    S_USERS_NEW_PASSWORD,
    S_USERS_CONFIRM_PASSWORD,
    S_USERS_CHANGE_BUTTON,
    S_USERS_HTTP_NOTICE,
    S_USERS_ADD_KEYS,
    S_USERS_ADD_KEYS_BUTTON,
    S_USERS_ADD_KEYS_HELP,
    S_USERS_AUTHORIZED,
    S_USERS_KEYS_UNREADABLE,
    S_USERS_KEY_TYPE,
    S_USERS_KEY,
    S_USERS_KEY_COMMENT,
    S_USERS_KEY_ACTION,
    S_USERS_KEY_NO_COMMENT,
    S_USERS_KEY_CONFIRM,
    S_USERS_KEY_REMOVE,
    S_USERS_NO_KEYS,
    S_USERS_UNMANAGED_ONE,
    S_USERS_UNMANAGED_MANY,
    S_USERS_LAST_KEY_NOTICE,
    S_PASSWORD_CHANGED,
    S_KEY_ADDED_ONE,
    S_KEYS_ADDED_MANY,
    S_KEY_REMOVED,
    S_JS_SELECT_PUB,
    S_JS_READING_KEYS,
    S_JS_FILES_EMPTY,
    S_JS_KEYS_TOO_LARGE,
    S_JS_UPLOADING_KEYS,
    S_JS_FILES_UNREADABLE,
    S_TITLE_APP_PARAMETERS,
    S_APP_PARAMETERS_SUBTITLE,
    S_APP_PARAMETERS_NOTICE,
    S_PARAMS_SYSTEM,
    S_PARAMS_NETWORK,
    S_PARAMS_VIDEO,
    S_PARAMS_CATEGORIES,
    S_PARAMS_SAVE_ALL,
    S_PARAMS_SAVE,
    S_PARAMS_SAVE_RESTART,
    S_PARAMS_SAVED_RESTARTED,
    S_PARAMS_SAVED,
    S_E_CONFIG_UNREADABLE,
    S_TITLE_RAW,
    S_RAW_HEADING,
    S_RAW_SUBTITLE,
    S_RAW_SAVE,
    S_RAW_HELP,
    S_CONFIG_SAVED_RESTARTED,
    S_CONFIG_SAVED,
    S_TITLE_FILES,
    S_FILES_HEADING,
    S_FILES_SUBTITLE,
    S_FILES_PATH,
    S_FILES_OPEN,
    S_FILES_NAME,
    S_FILES_TYPE,
    S_FILES_SIZE,
    S_FILES_MODIFIED,
    S_FILES_MODE,
    S_FILES_ACTIONS,
    S_FILES_DIRECTORY,
    S_FILES_LINK,
    S_FILES_DOWNLOAD,
    S_FILES_DELETE,
    S_FILES_TRUNCATED,
    S_FILES_LEGEND,
    S_TITLE_VIEWER,
    S_VIEWER_HEADING,
    S_VIEWER_BACK,
    S_VIEWER_IMAGE_ALT,
    S_VIEWER_NO_VIDEO,
    S_VIEWER_NO_PREVIEW,
    S_TITLE_DELETE,
    S_DELETE_TEXT,
    S_DELETE_RECURSIVE,
    S_DELETE_NO_UNDO,
    S_DELETE_CONFIRM,
    S_DELETE_BUTTON,
    S_TITLE_DEBUG,
    S_DEBUG_HEADING,
    S_DEBUG_SUBTITLE,
    S_DEBUG_PLAIN_TEXT,
    S_DEBUG_EMPTY_MT11,
    S_DEBUG_EMPTY_A8,
    S_DEBUG_FOOTER,
    S_E_LOG_UNREADABLE,
    S_E_FW_SIZE,
    S_E_FW_CONTENT_TYPE,
    S_E_FW_NAME,
    S_E_FW_NAME_LONG,
    S_E_FW_LOCK,
    S_E_FW_MICROSD,
    S_E_FW_SPACE,
    S_E_FW_INSPECT,
    S_E_FW_EXISTS,
    S_E_FW_NAME_EXISTS,
    S_E_FW_TEMP_EXISTS,
    S_E_FW_CREATE,
    S_E_FW_PUBLISH,
    S_E_FW_APPEARED,
    S_E_FW_PUBLISH_SAFE,
    S_E_FW_DIR_SYNC,
    S_FW_UPLOADED_MT11,
    S_FW_UPLOADED_A8,
    S_FW_INSTALLED_Z1,
    S_E_FW_PACKAGE,
    S_E_FW_INSTALL,
    S_E_FW_TMP,
    S_E_FW_TMP_SPACE,
    S_E_FW_FAILED,
    S_JS_FW_TIMEOUT,
    S_JS_FW_TIMEOUT_ALERT,
    S_JS_FW_BACK,
    S_JS_FW_BACK_LOGIN,
    S_JS_FW_BACK_ALERT,
    S_JS_FW_REBOOTING,
    S_JS_FW_WAITING_UPDATER,
    S_JS_FW_NAME,
    S_JS_FW_SIZE,
    S_JS_FW_CONFIRM,
    S_JS_FW_CONFIRM_A8,
    S_JS_FW_CONFIRM_Z1,
    S_JS_FW_INSTALLED,
    S_JS_FW_WRITING,
    S_JS_FW_HTTP,
    S_JS_FW_UPLOADED,
    S_JS_FW_CLOSED,
    S_E_BODY_TOO_LARGE,
    S_E_HEADERS_TOO_LARGE,
    S_E_MALFORMED,
    S_E_LIVE_UNAVAILABLE,
    S_E_LIVE_H264,
    S_COUNT
};

static const char *const strings[S_COUNT][LANG_COUNT] = {
    [S_LANGUAGE_NAME] = {"English", "简体中文", "日本語"},
    [S_LANGUAGE] = {"Language", "语言", "言語"},
    [S_APPLY] = {"Apply", "应用", "適用"},
    [S_NAV_TOP] = {"Go to the top of the %s control page", "回到 %s 控制页面顶部", "%s コントロールページの先頭へ"},
    [S_NAV_STATUS] = {"Status", "状态", "ステータス"},
    [S_NAV_PARAMETERS] = {"Parameters", "参数", "パラメータ"},
    [S_NAV_RAW] = {"Raw config", "原始配置", "設定ファイル"},
    [S_NAV_USERS] = {"Users", "用户", "ユーザー"},
    [S_NAV_FILES] = {"Files", "文件", "ファイル"},
    [S_NAV_LIVE] = {"Live", "实时画面", "ライブ"},
#if WEB_HAVE_THERMAL
    [S_NAV_SENSORS] = {"Sensors", "传感器", "センサー"},
    [S_TITLE_SENSORS] = {"%s sensors", "%s 传感器", "%s センサー"},
#else
    [S_NAV_SENSORS] = {"Photos", "照片", "写真"},
    [S_TITLE_SENSORS] = {"%s photos", "%s 照片", "%s 写真"},
#endif
    [S_NAV_DEBUG] = {"Debug", "调试", "デバッグ"},
    [S_NAV_LOGOUT] = {"Log out", "退出登录", "ログアウト"},
    [S_UNAVAILABLE] = {"unavailable", "不可用", "取得不可"},
    [S_OUT_OF_MEMORY] = {"Out of memory", "内存不足", "メモリ不足です"},
    [S_NOT_FOUND] = {"Not found", "未找到", "見つかりません"},
    [S_TITLE_LOGIN] = {"%s login", "%s 登录", "%s ログイン"},
    [S_LOGIN_SUBTITLE] = {"Administrative interface", "管理界面", "管理インターフェース"},
    [S_LOGIN_USERNAME] = {"Username", "用户名", "ユーザー名"},
    [S_LOGIN_PASSWORD] = {"Password", "密码", "パスワード"},
    [S_LOGIN_BUTTON] = {"Log in", "登录", "ログイン"},
    [S_LOGIN_FIRST_TIME] = {"First time here? The username is <code>admin</code> and the password is <code>ardupilot</code> unless it has been changed on the Users page. The camera keeps the password in <code>%s</code>.", "首次使用？用户名为 <code>admin</code>，密码为 <code>ardupilot</code>（除非已在“用户”页面修改）。相机将密码保存在 <code>%s</code> 中。", "初めてお使いですか？ユーザー名は <code>admin</code>、パスワードは「ユーザー」ページで変更していない限り <code>ardupilot</code> です。パスワードはカメラ内の <code>%s</code> に保存されています。"},
    [S_LOGIN_SCRIPTED] = {"Scripted clients can keep using HTTP Basic authentication with the same credentials. This service is HTTP, not HTTPS; keep it on the isolated camera network.", "脚本客户端可继续使用相同凭据进行 HTTP Basic 认证。本服务使用 HTTP 而非 HTTPS，请仅在隔离的相机网络中使用。", "スクリプトからは同じ認証情報で HTTP Basic 認証を引き続き利用できます。このサービスは HTTPS ではなく HTTP です。隔離されたカメラ用ネットワーク内でのみ使用してください。"},
    [S_LOGIN_EXPIRED] = {"The login form has expired; try again", "登录表单已过期，请重试", "ログインフォームの有効期限が切れました。もう一度お試しください"},
    [S_LOGIN_INCORRECT] = {"Incorrect username or password", "用户名或密码错误", "ユーザー名またはパスワードが正しくありません"},
    [S_LOGIN_ORIGIN] = {"Cross-site login request rejected", "已拒绝跨站登录请求", "他サイトからのログイン要求を拒否しました"},
    [S_LOGOUT_FAILED] = {"Cannot revoke the login session on the camera; try again", "无法在相机上撤销登录会话，请重试", "カメラ上のログインセッションを取り消せません。もう一度お試しください"},
    [S_LOGIN_SESSION_FAILED] = {"Cannot create a login session on the camera", "无法在相机上创建登录会话", "カメラ上にログインセッションを作成できません"},
    [S_AUTH_REQUIRED] = {"Authentication required", "需要认证", "認証が必要です"},
    [S_PASSWORD_FILE_MISSING] = {"Missing or empty %s", "%s 缺失或为空", "%s がないか空です"},
    [S_LANGUAGE_INVALID] = {"Unknown language", "未知的语言", "不明な言語です"},
    [S_OPT_DISABLED] = {"Disabled", "禁用", "無効"},
    [S_OPT_ENABLED] = {"Enabled", "启用", "有効"},
    [S_OPT_WHILE_ARMED] = {"While Armed", "解锁时", "アーム中"},
    [S_OPT_AUTO] = {"Auto", "自动", "自動"},
    [S_OPT_ISO_100] = {"ISO 100", "ISO 100", "ISO 100"},
    [S_OPT_ISO_200] = {"ISO 200", "ISO 200", "ISO 200"},
    [S_OPT_ISO_400] = {"ISO 400", "ISO 400", "ISO 400"},
    [S_OPT_ISO_800] = {"ISO 800", "ISO 800", "ISO 800"},
    [S_OPT_ISO_1600] = {"ISO 1600", "ISO 1600", "ISO 1600"},
    [S_OPT_ISO_3200] = {"ISO 3200", "ISO 3200", "ISO 3200"},
    [S_OPT_SHUTTER_30] = {"1/30 s", "1/30 秒", "1/30 秒"},
    [S_OPT_SHUTTER_50] = {"1/50 s", "1/50 秒", "1/50 秒"},
    [S_OPT_SHUTTER_100] = {"1/100 s", "1/100 秒", "1/100 秒"},
    [S_OPT_SHUTTER_250] = {"1/250 s", "1/250 秒", "1/250 秒"},
    [S_OPT_SHUTTER_500] = {"1/500 s", "1/500 秒", "1/500 秒"},
    [S_OPT_SHUTTER_750] = {"1/750 s", "1/750 秒", "1/750 秒"},
    [S_OPT_SHUTTER_1000] = {"1/1000 s", "1/1000 秒", "1/1000 秒"},
    [S_OPT_SHUTTER_2000] = {"1/2000 s", "1/2000 秒", "1/2000 秒"},
    [S_OPT_METER_AVERAGE] = {"Average", "平均测光", "平均測光"},
    [S_OPT_METER_CENTER] = {"Center-weighted", "中央重点测光", "中央重点測光"},
    [S_OPT_METER_SPOT_CENTER] = {"Spot (center)", "点测光（中央）", "スポット測光（中央）"},
    [S_OPT_WB_DAYLIGHT] = {"Daylight", "日光", "太陽光"},
    [S_OPT_WB_CLOUDY] = {"Cloudy", "阴天", "曇天"},
    [S_OPT_WB_FLUORESCENT] = {"Fluorescent", "荧光灯", "蛍光灯"},
    [S_OPT_WB_INCANDESCENT] = {"Incandescent", "白炽灯", "白熱灯"},
    [S_OPT_SCOPE_THERMAL] = {"Thermal only", "仅热成像", "サーマルのみ"},
    [S_OPT_SCOPE_ALL] = {"All lenses", "全部镜头", "すべてのレンズ"},
    [S_OPT_ORIENT_AUTO] = {"Automatic (gimbal-reported)", "自动（由云台上报）", "自動（ジンバルの報告に従う）"},
    [S_OPT_ORIENT_UPRIGHT] = {"Upright", "正装", "正立"},
    [S_OPT_ORIENT_INVERTED] = {"Inverted", "倒装", "倒立"},
    [S_OPT_UART_NONE] = {"None", "无", "なし"},
    [S_OPT_UART_SIYI] = {"SIYI", "SIYI", "SIYI"},
    [S_OPT_UART_MAVLINK] = {"MAVLink", "MAVLink", "MAVLink"},
    [S_OPT_RES_720] = {"1280 x 720", "1280 x 720", "1280 x 720"},
    [S_OPT_RES_1080] = {"1920 x 1080", "1920 x 1080", "1920 x 1080"},
    [S_OPT_RES_1440] = {"2560 x 1440", "2560 x 1440", "2560 x 1440"},
    [S_OPT_RES_4K] = {"3840 x 2160 / 4K", "3840 x 2160 / 4K", "3840 x 2160 / 4K"},
    [S_OPT_CODEC_H264] = {"H.264 / AVC", "H.264 / AVC", "H.264 / AVC"},
    [S_OPT_CODEC_H265] = {"H.265 / HEVC", "H.265 / HEVC", "H.265 / HEVC"},
    [S_OPT_PAL_WHITE_HOT] = {"White hot", "白热", "ホワイトホット"},
    [S_OPT_PAL_SEPIA] = {"Sepia", "棕褐色", "セピア"},
    [S_OPT_PAL_IRONBOW] = {"Ironbow", "铁红", "アイアンボウ"},
    [S_OPT_PAL_RAINBOW] = {"Rainbow", "彩虹", "レインボー"},
    [S_OPT_PAL_NIGHT] = {"Night", "夜视", "ナイト"},
    [S_OPT_PAL_AURORA] = {"Aurora", "极光", "オーロラ"},
    [S_OPT_PAL_RED_HOT] = {"Red hot", "红热", "レッドホット"},
    [S_OPT_PAL_JUNGLE] = {"Jungle", "丛林", "ジャングル"},
    [S_OPT_PAL_MEDICAL] = {"Medical", "医疗", "メディカル"},
    [S_OPT_PAL_BLACK_HOT] = {"Black hot", "黑热", "ブラックホット"},
    [S_OPT_PAL_GLORY_HOT] = {"Glory hot", "辉光", "グローリーホット"},
    [S_P_BRIGHTNESS] = {"Brightness", "亮度", "明るさ"},
    [S_P_SATURATION] = {"Saturation", "饱和度", "彩度"},
    [S_P_CONTRAST] = {"Contrast", "对比度", "コントラスト"},
    [S_P_EXPOSURE_COMP] = {"Exposure compensation", "曝光补偿", "露出補正"},
    [S_P_ISO] = {"ISO", "ISO", "ISO"},
    [S_P_METERING] = {"Metering mode", "测光模式", "測光モード"},
    [S_P_WHITE_BALANCE] = {"White balance", "白平衡", "ホワイトバランス"},
    [S_H_CARDV_BRIGHTNESS] = {"ISP brightness.", "ISP 亮度。", "ISP の明るさ。"},
    [S_H_CARDV_SATURATION] = {"ISP saturation.", "ISP 饱和度。", "ISP の彩度。"},
    [S_H_CARDV_CONTRAST] = {"ISP contrast.", "ISP 对比度。", "ISP のコントラスト。"},
    [S_P_TIMEZONE] = {"Timezone", "时区", "タイムゾーン"},
    [S_H_TIMEZONE] = {"POSIX TZ string or an installed IANA zone name. GMT-10 is fixed UTC+10.", "POSIX TZ 字符串或已安装的 IANA 时区名称。GMT-10 表示固定的 UTC+10。", "POSIX TZ 文字列、またはインストール済みの IANA タイムゾーン名。GMT-10 は固定の UTC+10 です。"},
    [S_P_PHOTO_SCOPE] = {"Photo capture scope", "拍照范围", "静止画の撮影範囲"},
    [S_H_PHOTO_SCOPE] = {"Thermal saves the radiometric plane only. All saves both visible lenses, the thermal display and the radiometric plane.", "“仅热成像”只保存辐射测温数据；“全部镜头”保存两个可见光镜头的图像、热成像显示图像和辐射测温数据。", "「サーマルのみ」は放射温度データのみを保存します。「すべてのレンズ」は 2 つの可視光レンズの画像、サーマル表示画像、放射温度データをすべて保存します。"},
    [S_P_ORIENTATION] = {"Mounting orientation", "安装方向", "取り付け方向"},
    [S_H_ORIENTATION] = {"Automatic follows the direction reported by the gimbal controller; a forced mode rotates all video paths by 180 degrees when needed.", "“自动”跟随云台控制器上报的方向；强制模式会在需要时将所有视频通路旋转 180 度。", "「自動」はジンバルコントローラーが報告する向きに従います。固定モードでは必要に応じてすべての映像経路を 180 度回転します。"},
    [S_P_UART_PROTOCOL] = {"UART4 protocol", "UART4 协议", "UART4 プロトコル"},
    [S_H_UART_PROTOCOL_MT11] = {"External /dev/ttyAMA4 flight-controller link at 230400 baud, 8 data bits, no parity and one stop bit.", "通过 /dev/ttyAMA4 连接外部飞控，230400 波特率、8 数据位、无校验、1 停止位。", "/dev/ttyAMA4 経由の外部フライトコントローラー接続。230400 baud、データ 8 ビット、パリティなし、ストップ 1 ビット。"},
    [S_H_UART_PROTOCOL_A8] = {"External flight-controller UART link at 230400 baud, 8 data bits, no parity and one stop bit.", "外部飞控 UART 连接，230400 波特率、8 数据位、无校验、1 停止位。", "外部フライトコントローラーとの UART 接続。230400 baud、データ 8 ビット、パリティなし、ストップ 1 ビット。"},
    [S_P_MAVLINK_CAMERA_COMPID] = {"MAVLink camera component ID", "MAVLink 相机组件 ID", "MAVLink カメラコンポーネント ID"},
    [S_H_MAVLINK_CAMERA_COMPID] = {"Select Camera 1–6 (IDs 100–105). Use a different camera ID for each camera on the same vehicle.", "选择相机 1–6（ID 100–105）。同一载具上的每台相机应使用不同的相机 ID。", "カメラ 1–6（ID 100–105）を選択。同じ機体の各カメラには異なる ID を設定してください。"},
    [S_OPT_CAMERA_COMP1] = {"Camera 1 (100)", "相机 1 (100)", "カメラ 1 (100)"},
    [S_OPT_CAMERA_COMP2] = {"Camera 2 (101)", "相机 2 (101)", "カメラ 2 (101)"},
    [S_OPT_CAMERA_COMP3] = {"Camera 3 (102)", "相机 3 (102)", "カメラ 3 (102)"},
    [S_OPT_CAMERA_COMP4] = {"Camera 4 (103)", "相机 4 (103)", "カメラ 4 (103)"},
    [S_OPT_CAMERA_COMP5] = {"Camera 5 (104)", "相机 5 (104)", "カメラ 5 (104)"},
    [S_OPT_CAMERA_COMP6] = {"Camera 6 (105)", "相机 6 (105)", "カメラ 6 (105)"},
    [S_P_MAVLINK_SYSID] = {"MAVLink system ID", "MAVLink 系统 ID", "MAVLink システム ID"},
    [S_H_MAVLINK_SYSID] = {"0 automatically uses the first flight controller heartbeat's system ID (GCS heartbeats are ignored). 1–255 sets a fixed ID. Takes effect after camera-app restarts.", "0 自动使用首个飞控心跳的系统 ID（忽略地面站心跳）。1–255 为固定 ID。重启相机应用后生效。", "0 は最初のフライトコントローラーのハートビートから自動取得（GCS は無視）。1–255 は固定 ID。カメラアプリの再起動後に有効。"},
    [S_P_MAVLINK_TCP] = {"MAVLink TCP port", "MAVLink TCP 端口", "MAVLink TCP ポート"},
    [S_H_MAVLINK_TCP] = {"MAVLink 2 camera and gimbal listener. Set to 0 to disable TCP.", "MAVLink 2 相机与云台监听端口。设为 0 可禁用 TCP。", "MAVLink 2 カメラ／ジンバルの待ち受けポート。0 で TCP を無効にします。"},
    [S_P_MAVLINK_UDP] = {"MAVLink UDP port", "MAVLink UDP 端口", "MAVLink UDP ポート"},
    [S_H_MAVLINK_UDP] = {"MAVLink 2 camera and gimbal listener. Set to 0 to disable UDP.", "MAVLink 2 相机与云台监听端口。设为 0 可禁用 UDP。", "MAVLink 2 カメラ／ジンバルの待ち受けポート。0 で UDP を無効にします。"},
    [S_P_LOG_DISARMED] = {"Log when disarmed", "未解锁时记录日志", "非アーム時にログ記録"},
    [S_H_LOG_DISARMED] = {"Write diagnostic BIN logs to SD card logs/. Armed flights are always logged. Applies on Save.", "诊断日志保存至 SD 卡 logs/。解锁时始终记录，保存后生效。", "診断 BIN ログを SD カードの logs/ に保存。アーム時は常に記録。保存時に反映。"},
    [S_P_TRACK_METHOD] = {"Tracking control method", "跟踪控制方式", "追尾制御方式"},
    [S_H_TRACK_METHOD] = {"Angle sends absolute positions. Rate follows predicted target motion with pointing-error correction. Applies to geographic ROI tracking; changes apply when saved.", "角度模式发送绝对位置；速率模式结合预测运动与指向误差修正。用于地理 ROI 跟踪，保存后生效。", "角度は絶対位置、速度は予測運動と指向誤差補正で制御します。地理 ROI 追尾に使用し、保存時に反映します。"},
    [S_OPT_TRACK_ANGLE] = {"Angle", "角度", "角度"},
    [S_OPT_TRACK_RATE] = {"Rate", "速率", "速度"},
    [S_RESTART_REQUIRED] = {"Requires camera app restart.", "需要重启相机应用。", "カメラアプリの再起動が必要です。"},
    [S_P_POSITION_TARGETING] = {"Position targeting", "位置目标指向", "位置ターゲット指向"},
    [S_H_POSITION_TARGETING] = {"When enabled, advertise and handle geographic ROI targets in the camera. Disable to make ArduPilot calculate and send angle targets. Changes apply when saved; the updated capability is advertised to the flight controller.", "启用时，由相机宣告并处理地理 ROI 目标。禁用时，由 ArduPilot 计算并发送角度目标。保存后生效。", "有効にすると、カメラが地理 ROI ターゲットを通知して処理します。無効にすると、ArduPilot が角度ターゲットを計算して送信します。保存時に反映します。"},
    [S_P_THERMAL_PALETTE] = {"Thermal palette", "热成像调色板", "サーマルパレット"},
    [S_H_THERMAL_PALETTE] = {"Pseudo-colour palette applied by the thermal module to video and still images.", "热成像模块应用于视频和照片的伪彩调色板。", "サーマルモジュールが映像と静止画に適用する疑似カラーパレット。"},
    [S_P_AUTORECORD] = {"Automatic recording", "自动录像", "自動録画"},
    [S_H_AUTORECORD_APP] = {"Enabled starts recording immediately and at startup. While Armed follows the current armed state and stops on disarm, using MAVLink HEARTBEAT from the selected system's autopilot (component 1).", "启用会在应用启动后开始录像。解锁时模式根据所选系统飞控（组件 1）的 MAVLink 心跳，在解锁时开始录像、上锁时停止。", "有効では起動時に録画を開始します。アーム中では選択したシステムのオートパイロット（コンポーネント 1）の MAVLink HEARTBEAT に従い、アームで開始、ディスアームで停止します。"},
    [S_P_RECORDING_RESOLUTION] = {"Recording resolution", "录像分辨率", "録画解像度"},
    [S_H_RECORDING_RESOLUTION_MT11] = {"Resolution used by the visible recording encoder; the MT11 thermal recording remains 1280 x 720.", "可见光录像编码器使用的分辨率；MT11 的热成像录像固定为 1280 x 720。", "可視光録画エンコーダーの解像度。MT11 のサーマル録画は 1280 x 720 固定です。"},
    [S_H_RECORDING_RESOLUTION_A8] = {"Resolution used by the recording encoder.", "录像编码器使用的分辨率。", "録画エンコーダーの解像度。"},
    [S_P_MAIN_RESOLUTION] = {"Main RTSP resolution", "主 RTSP 流分辨率", "メイン RTSP 解像度"},
    [S_H_MAIN_RESOLUTION_MT11] = {"RGB resolution of rtsp://CAMERA:8554/video1; thermal output is limited to 1280 x 720.", "rtsp://CAMERA:8554/video1 的 RGB 分辨率；热成像输出上限为 1280 x 720。", "rtsp://CAMERA:8554/video1 の RGB 解像度。サーマル出力は 1280 x 720 が上限です。"},
    [S_H_MAIN_RESOLUTION_A8] = {"Resolution of rtsp://CAMERA:8554/video1.", "rtsp://CAMERA:8554/video1 的分辨率。", "rtsp://CAMERA:8554/video1 の解像度。"},
    [S_P_MAIN_CODEC] = {"Main RTSP codec", "主 RTSP 流编码格式", "メイン RTSP コーデック"},
    [S_H_MAIN_CODEC] = {"Codec of rtsp://CAMERA:8554/video1.", "rtsp://CAMERA:8554/video1 的编码格式。", "rtsp://CAMERA:8554/video1 のコーデック。"},
    [S_P_SUB_RESOLUTION] = {"Sub RTSP resolution", "子 RTSP 流分辨率", "サブ RTSP 解像度"},
    [S_H_SUB_RESOLUTION_MT11] = {"RGB resolution of rtsp://CAMERA:8554/video2; thermal output is limited to 1280 x 720.", "rtsp://CAMERA:8554/video2 的 RGB 分辨率；热成像输出上限为 1280 x 720。", "rtsp://CAMERA:8554/video2 の RGB 解像度。サーマル出力は 1280 x 720 が上限です。"},
    [S_H_SUB_RESOLUTION_A8] = {"Resolution of rtsp://CAMERA:8554/video2.", "rtsp://CAMERA:8554/video2 的分辨率。", "rtsp://CAMERA:8554/video2 の解像度。"},
    [S_P_SUB_CODEC] = {"Sub RTSP codec", "子 RTSP 流编码格式", "サブ RTSP コーデック"},
    [S_H_SUB_CODEC] = {"Codec of rtsp://CAMERA:8554/video2.", "rtsp://CAMERA:8554/video2 的编码格式。", "rtsp://CAMERA:8554/video2 のコーデック。"},
    [S_H_BRIGHTNESS_MT11] = {"Visible-camera ISP brightness, applied to both RGB sensors.", "可见光相机 ISP 亮度，同时应用于两个 RGB 传感器。", "可視光カメラの ISP の明るさ。両方の RGB センサーに適用されます。"},
    [S_H_SATURATION_MT11] = {"Visible-camera ISP saturation, applied to both RGB sensors.", "可见光相机 ISP 饱和度，同时应用于两个 RGB 传感器。", "可視光カメラの ISP の彩度。両方の RGB センサーに適用されます。"},
    [S_H_CONTRAST_MT11] = {"Visible-camera ISP contrast, applied to both RGB sensors.", "可见光相机 ISP 对比度，同时应用于两个 RGB 传感器。", "可視光カメラの ISP のコントラスト。両方の RGB センサーに適用されます。"},
    [S_H_EXPOSURE_COMP_APP] = {"Tenths of an EV from -1.0 to +1.0 EV.", "以 0.1 EV 为单位，范围 -1.0 至 +1.0 EV。", "0.1 EV 単位で -1.0〜+1.0 EV。"},
    [S_H_ISO_APP] = {"Automatic gain or a fixed visible-camera gain preset.", "自动增益或可见光相机的固定增益预设。", "自動ゲイン、または可視光カメラの固定ゲインプリセット。"},
    [S_P_SHUTTER] = {"Shutter speed", "快门速度", "シャッター速度"},
    [S_H_SHUTTER_APP] = {"Automatic exposure time or a fixed visible-camera shutter speed.", "自动曝光时间或可见光相机的固定快门速度。", "自動で調整する露光時間、または可視光カメラの固定シャッター速度。"},
    [S_H_METERING_APP] = {"Spot metering uses the center of the frame.", "点测光以画面中央为基准。", "スポット測光は画面中央を使用します。"},
    [S_H_WB_APP] = {"Automatic white balance or a fixed RGB-gain preset.", "自动白平衡或固定的 RGB 增益预设。", "オートホワイトバランス、または固定の RGB ゲインプリセット。"},
    [S_E_CONFIG_SIZE] = {"Config size must be between 1 and %u bytes", "配置大小必须在 1 至 %u 字节之间", "設定のサイズは 1〜%u バイトである必要があります"},
    [S_E_INI_UNTERMINATED_SECTION] = {"Line %u has an unterminated section", "第 %u 行的节名未闭合", "%u 行目のセクションが閉じられていません"},
    [S_E_INI_TEXT_AFTER_SECTION] = {"Line %u has text after its section", "第 %u 行的节名之后有多余文本", "%u 行目のセクションの後に余分な文字があります"},
    [S_E_INI_NOT_ASSIGNMENT] = {"Line %u is neither a section nor key=value", "第 %u 行既不是节也不是 key=value", "%u 行目はセクションでも key=value でもありません"},
    [S_E_INI_EMPTY_KEY] = {"Line %u has an empty key", "第 %u 行的键为空", "%u 行目のキーが空です"},
    [S_E_INI_NO_SECTION] = {"Config must contain a section and at least one assignment", "配置必须包含一个节和至少一个赋值", "設定にはセクションと 1 つ以上の代入が必要です"},
    [S_E_INI_DUPLICATE_KEY] = {"Duplicate config key [%s] %s", "配置键 [%s] %s 重复", "設定キー [%s] %s が重複しています"},
    [S_E_INI_MISSING_KEY] = {"Config key [%s] %s is missing", "缺少配置键 [%s] %s", "設定キー [%s] %s がありません"},
    [S_E_CONFIG_TOO_LARGE] = {"Updated config is too large", "更新后的配置过大", "更新後の設定が大きすぎます"},
    [S_E_CONFIG_ALLOC] = {"Cannot allocate updated config", "无法为更新后的配置分配内存", "更新後の設定用のメモリを確保できません"},
    [S_E_TOO_MANY_PARAMETERS] = {"Too many parameters or parameter value too long", "参数过多或参数值过长", "パラメータが多すぎるか、値が長すぎます"},
    [S_E_INVALID_VALUE] = {"Invalid value for %s", "“%s”的值无效", "「%s」の値が無効です"},
    [S_E_PATH_TOO_LONG] = {"Path is too long", "路径过长", "パスが長すぎます"},
    [S_E_CANNOT_WRITE] = {"Cannot write %s: %s", "无法写入 %s：%s", "%s に書き込めません: %s"},
    [S_E_CANNOT_CLOSE] = {"Cannot close %s: %s", "无法关闭 %s：%s", "%s を閉じられません: %s"},
    [S_E_CANNOT_INSTALL] = {"Cannot install %s: %s", "无法安装 %s：%s", "%s を配置できません: %s"},
    [S_E_CANNOT_SYNC] = {"Cannot sync %s: %s", "无法同步 %s：%s", "%s を同期できません: %s"},
    [S_E_LOCK_USERS] = {"Cannot lock user settings: %s", "无法锁定用户设置：%s", "ユーザー設定をロックできません: %s"},
    [S_E_KEYS_RUNTIME_DIR] = {"Keys were saved persistently but runtime directory failed: %s", "密钥已持久保存，但运行时目录操作失败：%s", "鍵は永続保存されましたが、実行時ディレクトリの処理に失敗しました: %s"},
    [S_E_KEYS_RUNTIME_ACTIVATION] = {"Keys were saved persistently but runtime activation failed: %s", "密钥已持久保存，但运行时激活失败：%s", "鍵は永続保存されましたが、実行時への反映に失敗しました: %s"},
    [S_E_PASSWORD_LENGTH] = {"Password must be between 8 and 128 bytes of UTF-8", "密码的 UTF-8 编码长度必须为 8 至 128 字节", "パスワードは UTF-8 で 8〜128 バイトにしてください"},
    [S_E_PASSWORD_CONTROL] = {"Password must not contain control characters", "密码不能包含控制字符", "パスワードに制御文字は使えません"},
    [S_E_PASSWORD_MISMATCH] = {"Password confirmation does not match", "两次输入的密码不一致", "確認用パスワードが一致しません"},
    [S_E_BROWSER_TIME] = {"Browser supplied an invalid time; reload the page and retry", "浏览器提供的时间无效，请刷新页面后重试", "ブラウザから無効な時刻が送られました。ページを再読み込みして再試行してください"},
    [S_E_SET_TIME] = {"Cannot set camera time: %s", "无法设置相机时间：%s", "カメラの時刻を設定できません: %s"},
    [S_E_KEY_REQUIRED] = {"At least one public key is required", "至少需要一个公钥", "公開鍵が少なくとも 1 つ必要です"},
    [S_E_KEYS_TOO_LARGE] = {"Submitted keys exceed %u bytes", "提交的密钥超过 %u 字节", "送信された鍵が %u バイトを超えています"},
    [S_E_KEY_MALFORMED] = {"Key on line %zu is unsupported or malformed; options are not accepted", "第 %zu 行的密钥不受支持或格式错误；不接受密钥选项", "%zu 行目の鍵は未対応か形式が不正です。鍵オプションは受け付けません"},
    [S_E_KEY_DUPLICATE_LINE] = {"The submitted keys contain a duplicate on line %zu", "提交的密钥中第 %zu 行重复", "送信された鍵の %zu 行目が重複しています"},
    [S_E_KEYS_READ] = {"Cannot read authorized keys: %s", "无法读取已授权密钥：%s", "authorized_keys を読み取れません: %s"},
    [S_E_KEY_ALREADY] = {"Submitted key %zu is already authorized", "提交的第 %zu 个密钥已被授权", "送信された %zu 番目の鍵はすでに登録済みです"},
    [S_E_KEY_FILE_TOO_LARGE] = {"Authorized key file would exceed %u bytes", "已授权密钥文件将超过 %u 字节", "authorized_keys ファイルが %u バイトを超えてしまいます"},
    [S_E_KEY_REMOVE_UNCONFIRMED] = {"SSH key removal was not confirmed", "未确认删除 SSH 密钥", "SSH 鍵の削除が確認されていません"},
    [S_E_KEY_SELECTION] = {"Invalid public key selection", "所选公钥无效", "選択された公開鍵が無効です"},
    [S_E_KEY_ABSENT] = {"Public key was already absent", "该公钥已不存在", "その公開鍵はすでに存在しません"},
    [S_E_KEY_LAST] = {"Refusing to remove the last usable SSH key; add its replacement first", "拒绝删除最后一个可用的 SSH 密钥；请先添加替代密钥", "最後の有効な SSH 鍵は削除できません。先に代わりの鍵を追加してください"},
    [S_E_CANNOT_STAT] = {"Cannot stat %s: %s", "无法获取 %s 的状态：%s", "%s の情報を取得できません: %s"},
    [S_E_CONFIG_PATH_LONG] = {"Config path is too long", "配置路径过长", "設定ファイルのパスが長すぎます"},
    [S_E_CONFIG_BACKUP] = {"Cannot create config backup: %s", "无法创建配置备份：%s", "設定のバックアップを作成できません: %s"},
    [S_E_CONFIG_WRITE] = {"Cannot write new config: %s", "无法写入新配置：%s", "新しい設定を書き込めません: %s"},
    [S_E_CONFIG_INSTALL] = {"Cannot install new config: %s", "无法安装新配置：%s", "新しい設定を配置できません: %s"},
    [S_E_CANNOT_READ] = {"Cannot read %s: %s", "无法读取 %s：%s", "%s を読み取れません: %s"},
    [S_E_NO_CONFIG_DATA] = {"Form did not contain config data", "表单中没有配置数据", "フォームに設定データが含まれていません"},
    [S_APP_REPLACEMENT] = {"ArduPilot camera app", "ArduPilot 相机应用", "ArduPilot カメラアプリ"},
    [S_APP_GENERIC] = {"camera application", "相机应用", "カメラアプリ"},
    [S_APP_STOPPED] = {"stopped", "已停止", "停止中"},
    [S_E_APP_SELECTION_PATH] = {"Application selection path is too long", "应用选择文件路径过长", "アプリ選択ファイルのパスが長すぎます"},
    [S_E_CAMERA_NOT_RUNNING] = {"Camera application is not running", "相机应用未运行", "カメラアプリが動作していません"},
    [S_E_CAMERA_API] = {"Cannot contact camera API: %s", "无法连接相机 API：%s", "カメラ API に接続できません: %s"},
    [S_E_MISSING_ACTION] = {"Missing control action", "缺少控制动作", "制御アクションがありません"},
    [S_E_MISSING_RATE] = {"Missing gimbal rate", "缺少云台速率", "ジンバル速度がありません"},
    [S_E_RATE_RANGE] = {"Gimbal rate must be between 5 and 60 degrees per second", "云台速率必须在每秒 5 至 60 度之间", "ジンバル速度は毎秒 5〜60 度の範囲で指定してください"},
    [S_E_MISSING_ZOOM] = {"Missing zoom value", "缺少变焦值", "ズーム値がありません"},
    [S_E_ZOOM_RANGE] = {"Zoom is outside the supported range", "变焦倍率超出支持范围", "ズームが対応範囲外です"},
    [S_E_UNKNOWN_ACTION] = {"Unknown control action", "未知的控制动作", "不明な制御アクションです"},
    [S_E_SEND_CONTROL] = {"Cannot send camera control: %s", "无法发送相机控制命令：%s", "カメラ制御コマンドを送信できません: %s"},
    [S_E_SEND_LIDAR] = {"Cannot send LiDAR control: %s", "无法发送激光测距控制命令：%s", "LiDAR 制御コマンドを送信できません: %s"},
    [S_E_SEND_SHUTTER] = {"Cannot send shutter command: %s", "无法发送快门命令：%s", "シャッターコマンドを送信できません: %s"},
    [S_E_SHUTTER_UNCONFIRMED] = {"Camera did not confirm the shutter command; check recent photos before retrying", "相机未确认快门命令；重试前请先查看最近的照片", "シャッターコマンドに対するカメラの確認応答がありませんでした。再試行の前に最近の写真を確認してください"},
    [S_E_CAPTURE_FAILED] = {"Camera reported photo capture failure", "相机报告拍照失败", "カメラが撮影失敗を報告しました"},
    [S_E_APP_STOP] = {"Camera application did not stop cleanly; refusing to force-kill it", "相机应用未能正常停止；拒绝强制终止", "カメラアプリが正常に停止しませんでした。強制終了は行いません"},
    [S_E_CANNOT_START] = {"Cannot start %s: %s", "无法启动 %s：%s", "%s を起動できません: %s"},
    [S_E_NOT_READY] = {"%s did not report ready", "%s 未报告就绪", "%s が準備完了を報告しませんでした"},
    [S_E_REQUEST_RECORD] = {"Cannot record the application request: %s", "无法记录应用操作请求：%s", "アプリ操作要求を記録できません: %s"},
    [S_E_REQUEST_LOCK] = {"Cannot lock the application request: %s", "无法锁定应用操作请求：%s", "アプリ操作要求をロックできません: %s"},
    [S_E_APP_NOT_UP] = {"%s did not come up; the launcher may have fallen back to the other application", "%s 未能启动；启动器可能已回退到另一个应用", "%s が起動しませんでした。ランチャーがもう一方のアプリに切り替えた可能性があります"},
    [S_E_SWITCH_LOCK] = {"Cannot lock application switching: %s", "无法锁定应用切换：%s", "アプリ切り替えをロックできません: %s"},
    [S_NOT_MOUNTED] = {"not mounted; ", "未挂载；", "未マウント。"},
    [S_E_PATH_ABSOLUTE] = {"Path must be absolute", "路径必须是绝对路径", "パスは絶対パスである必要があります"},
    [S_E_PATH_RESOLVE] = {"Cannot resolve path: %s", "无法解析路径：%s", "パスを解決できません: %s"},
    [S_E_TARGET_MISSING] = {"Target does not exist", "目标不存在", "対象が存在しません"},
    [S_E_DELETE_BENEATH] = {"Deletion is permitted only beneath %s", "只允许删除 %s 之下的内容", "削除できるのは %s 配下のみです"},
    [S_E_TARGET_CHANGED] = {"Target changed while preparing deletion", "准备删除时目标发生了变化", "削除の準備中に対象が変化しました"},
    [S_E_DELETE_FAILED] = {"Deletion failed: %s", "删除失败：%s", "削除に失敗しました: %s"},
    [S_E_OPEN_DIRECTORY] = {"Cannot open directory: %s", "无法打开目录：%s", "ディレクトリを開けません: %s"},
    [S_E_VIEW_REGULAR] = {"View target must be a regular file", "查看的目标必须是普通文件", "表示対象は通常のファイルである必要があります"},
    [S_E_DELETE_EXISTING] = {"Deletion is permitted only for existing targets beneath %s", "只允许删除 %s 之下已存在的目标", "削除できるのは %s 配下に存在する対象のみです"},
    [S_E_DELETE_RESOLVE] = {"Cannot resolve delete target", "无法解析删除目标", "削除対象を解決できません"},
    [S_E_DELETE_UNCONFIRMED] = {"Deletion confirmation was not checked", "未勾选删除确认", "削除の確認にチェックが入っていません"},
    [S_E_DELETE_PATH] = {"Invalid or missing delete path", "删除路径无效或缺失", "削除パスが無効か指定されていません"},
    [S_E_DOWNLOAD_REGULAR] = {"Download target must be a regular file", "下载的目标必须是普通文件", "ダウンロード対象は通常のファイルである必要があります"},
    [S_E_PATH_LONG_OR_INVALID] = {"Invalid or overly long path", "路径无效或过长", "パスが無効か長すぎます"},
    [S_E_PATH_MISSING] = {"Invalid or missing path", "路径无效或缺失", "パスが無効か指定されていません"},
    [S_PATH_DELETED] = {"Path deleted permanently", "路径已永久删除", "パスを完全に削除しました"},
    [S_UNKNOWN_CURRENT_VALUE] = {"Unknown/missing current value", "当前值未知或缺失", "現在値が不明または未設定"},
    [S_TITLE_CONTROL] = {"%s control", "%s 控制", "%s コントロール"},
    [S_STATUS_SUBTITLE] = {"Authenticated administrative interface", "已认证的管理界面", "認証済み管理インターフェース"},
    [S_STATUS_HEADING] = {"Status", "状态", "ステータス"},
    [S_PARAMS_PROXY] = {"SupportProxy", "SupportProxy", "SupportProxy"},
    [S_P_PROXY_ENABLED] = {"SupportProxy", "SupportProxy", "SupportProxy"},
    [S_H_PROXY_ENABLED] = {"Enable video publishing and a bidirectional MAVLink relay. Changes take effect after restarting the camera app.", "启用视频发布和双向 MAVLink 转发。设置在重启相机应用后生效。", "映像送信と双方向 MAVLink 転送を有効にします。変更はカメラアプリの再起動後に反映されます。"},
    [S_P_PROXY_HOST] = {"Proxy host", "代理服务器", "プロキシホスト"},
    [S_H_PROXY_HOST] = {"SupportProxy server IPv4 address or hostname.", "SupportProxy 服务器的 IPv4 地址或主机名。", "SupportProxy サーバーの IPv4 アドレスまたはホスト名。"},
    [S_P_PROXY_MAVLINK_PORT] = {"MAVLink port", "MAVLink 端口", "MAVLink ポート"},
    [S_H_PROXY_MAVLINK_PORT] = {"SupportProxy user-side UDP port. Set to 0 to disable MAVLink forwarding.", "SupportProxy 用户侧 UDP 端口。设为 0 禁用 MAVLink 转发。", "SupportProxy のユーザー側 UDP ポート。0 で MAVLink 転送を無効にします。"},
    [S_P_PROXY_SIGNING] = {"MAVLink signing", "MAVLink 签名", "MAVLink 署名"},
    [S_H_PROXY_SIGNING] = {"Sign outgoing MAVLink 2 packets and require valid signed packets from SupportProxy. Configure bidi_sign on the proxy entry.", "对发出的 MAVLink 2 数据签名，并要求代理的数据具有有效签名。请在代理条目上启用 bidi_sign。", "送信する MAVLink 2 パケットに署名し、プロキシからの有効な署名を必須にします。プロキシの項目で bidi_sign を有効にしてください。"},
    [S_P_PROXY_SIGNING_PASSPHRASE] = {"Signing passphrase", "签名口令", "署名パスフレーズ"},
    [S_H_PROXY_SIGNING_PASSPHRASE] = {"Use the same passphrase as the SupportProxy entry. Applies only to the proxy link.", "使用与 SupportProxy 条目相同的口令。仅用于代理链路。", "SupportProxy の項目と同じパスフレーズを使用します。プロキシ接続にのみ適用されます。"},
    [S_P_PROXY_SIGNING_LINK_ID] = {"Signing link ID", "签名链路 ID", "署名リンク ID"},
    [S_H_PROXY_SIGNING_LINK_ID] = {"MAVLink signing link ID for this camera connection.", "此相机连接的 MAVLink 签名链路 ID。", "このカメラ接続の MAVLink 署名リンク ID。"},
    [S_P_PROXY_VIDEO1_PORT] = {"Video 1 port", "视频 1 端口", "映像 1 ポート"},
    [S_H_PROXY_VIDEO1_PORT] = {"RTSP publishing port for video1. Set to 0 to disable this stream.", "video1 的 RTSP 发布端口。设为 0 禁用此视频流。", "video1 の RTSP 送信ポート。0 でこのストリームを無効にします。"},
    [S_P_PROXY_VIDEO1_NAME] = {"Video 1 stream name", "视频 1 流名称", "映像 1 ストリーム名"},
    [S_H_PROXY_VIDEO1_NAME] = {"Stream name used in the RTSP publishing URL.", "RTSP 发布 URL 中使用的流名称。", "RTSP 送信 URL で使用するストリーム名。"},
    [S_E_PROXY_VIDEO_PORTS] = {"Video 1 and Video 2 must use different ports.", "视频 1 和视频 2 必须使用不同端口。", "映像 1 と映像 2 には異なるポートを指定してください。"},
    [S_P_PROXY_VIDEO2_PORT] = {"Video 2 port", "视频 2 端口", "映像 2 ポート"},
    [S_H_PROXY_VIDEO2_PORT] = {"RTSP publishing port for video2. Set to 0 to disable this stream.", "video2 的 RTSP 发布端口。设为 0 禁用此视频流。", "video2 の RTSP 送信ポート。0 でこのストリームを無効にします。"},
    [S_P_PROXY_VIDEO2_NAME] = {"Video 2 stream name", "视频 2 流名称", "映像 2 ストリーム名"},
    [S_H_PROXY_VIDEO2_NAME] = {"Stream name used in the RTSP publishing URL.", "RTSP 发布 URL 中使用的流名称。", "RTSP 送信 URL で使用するストリーム名。"},
    [S_P_PROXY_PUBLISH_PASSWORD] = {"Video publish password", "视频发布密码", "映像送信パスワード"},
    [S_H_PROXY_PUBLISH_PASSWORD] = {"Optional SupportProxy video publish password. Without it, allow publishing via the MAVLink session on the proxy.", "可选的 SupportProxy 视频发布密码。留空时需在代理上允许通过 MAVLink 会话发布。", "任意の SupportProxy 映像送信パスワード。空欄の場合、プロキシで MAVLink セッション経由の送信を許可してください。"},
    [S_P_NETWORK_PRIMARY] = {"Primary IPv4 address/prefix", "主 IPv4 地址/前缀", "プライマリ IPv4 アドレス/プレフィックス"},
    [S_H_NETWORK_PRIMARY] = {"For example 192.168.144.27/24. Replaces existing IPv4 addresses on this interface with the primary and optional secondary address. Blank leaves existing addresses in place.", "例如 192.168.144.27/24。用主地址及可选的次地址替换此接口的现有 IPv4 地址。留空保留现有地址。", "例：192.168.144.27/24。このインターフェースの IPv4 アドレスをプライマリと任意のセカンダリアドレスに置き換えます。空欄では既存のアドレスを維持します。"},
    [S_E_NETWORK] = {"Use distinct host addresses and a gateway reachable through one of the configured subnets.", "请使用不同的主机地址及可通过已配置子网访问的网关。", "異なるホストアドレスと、設定したサブネットから到達可能なゲートウェイを指定してください。"},
    [S_NETWORK_CONNECTION_LOST] = {"Connection lost during restart. Use the link below to reconnect, or reload this page to check whether the settings were saved.", "重启时连接断开。请使用下方链接重新连接，或重新加载此页检查设置是否已保存。", "再起動中に接続が切れました。下のリンクで再接続するか、このページを再読み込みして設定が保存されたか確認してください。"},
    [S_NETWORK_SITL] = {"SITL uses the host network; address and gateway settings are saved but do not change the host network.", "SITL 使用主机网络；地址和网关设置会保存，但不会更改主机网络。", "SITL はホストのネットワークを使用します。アドレスとゲートウェイの設定は保存されますが、ホストのネットワークは変更しません。"},
    [S_NETWORK_RECONNECT] = {"Network changes apply on camera-app restart. If the primary address changes, reconnect here after restarting:", "网络更改在相机应用重启后生效。主地址更改后，请重新连接：", "ネットワーク設定はカメラアプリの再起動後に反映されます。プライマリアドレスを変更した場合、再起動後はこちらに接続してください："},
    [S_P_NETWORK_INTERFACE] = {"Network interface", "网络接口", "ネットワークインターフェース"},
    [S_H_NETWORK_INTERFACE] = {"Interface used for camera addresses and the default gateway, independent of SupportProxy.", "用于相机地址和默认网关的接口，独立于 SupportProxy。", "カメラのアドレスとデフォルトゲートウェイのインターフェース。SupportProxy とは独立しています。"},
    [S_P_NETWORK_ADDRESS] = {"Secondary IPv4 address/prefix", "附加 IPv4 地址/前缀", "追加 IPv4 アドレス/プレフィックス"},
    [S_H_NETWORK_ADDRESS] = {"Optional second address, for example 192.168.20.25/24. Leave blank to remove the secondary address previously set here.", "可选次地址，例如 192.168.20.25/24。留空删除先前在此设置的次地址。", "任意のセカンダリアドレス（例：192.168.20.25/24）。空欄にすると以前ここで設定したアドレスを削除します。"},
    [S_P_NETWORK_GATEWAY] = {"Default gateway", "默认网关", "デフォルトゲートウェイ"},
    [S_H_NETWORK_GATEWAY] = {"Optional IPv4 gateway, independent of SupportProxy. Leave blank to remove the gateway previously set here.", "可选 IPv4 网关，独立于 SupportProxy。留空删除先前在此设置的网关。", "任意の IPv4 ゲートウェイ。SupportProxy とは独立しています。空欄にすると以前ここで設定したゲートウェイを削除します。"},
    [S_STATUS_FIRMWARE_VERSION] = {"Firmware version", "固件版本", "ファームウェアバージョン"},
    [S_STATUS_CAMERA_APP] = {"Camera app", "相机应用", "カメラアプリ"},
    [S_STATUS_PID_RSS] = {" (PID %ld, RSS %ld KiB)", "（PID %ld，RSS %ld KiB）", "（PID %ld、RSS %ld KiB）"},
    [S_STATUS_WEB_SERVICE] = {"Web service", "Web 服务", "Web サービス"},
    [S_STATUS_WEB_PID_RSS] = {"PID %ld, RSS %ld KiB", "PID %ld，RSS %ld KiB", "PID %ld、RSS %ld KiB"},
    [S_STATUS_TIME] = {"Time", "时间", "時刻"},
    [S_STATUS_SYNC] = {"Sync", "同步", "同期"},
    [S_STATUS_UPTIME] = {"Uptime", "运行时间", "稼働時間"},
    [S_STATUS_UPTIME_FORMAT] = {"%u d %02u:%02u:%02u", "%u 天 %02u:%02u:%02u", "%u 日 %02u:%02u:%02u"},
    [S_STATUS_CPU] = {"CPU", "CPU", "CPU"},
    [S_STATUS_CPU_BUSY] = {"%u%% busy across %u cores (load average %.*s is inflated by the media driver's kernel threads)", "占用 %u%%，共 %u 个核心（负载平均值 %.*s 因媒体驱动的内核线程而偏高）", "使用率 %u%%、%u コア（ロードアベレージ %.*s はメディアドライバーのカーネルスレッドにより高めに出ます）"},
    [S_STATUS_LOAD] = {"Load", "负载", "負荷"},
    [S_STATUS_SOC_TEMPERATURE] = {"SoC temperature", "SoC 温度", "SoC 温度"},
#if APCAM_TARGET == APCAM_TARGET_Z1_MINI
    [S_STATUS_SOC_AVERAGE] = {"%s%d.%d &deg;C", "%s%d.%d &deg;C", "%s%d.%d &deg;C"},
#else
    [S_STATUS_SOC_AVERAGE] = {"%s%d.%d &deg;C (three-sensor average)", "%s%d.%d &deg;C（三个传感器的平均值）", "%s%d.%d &deg;C（3 センサーの平均）"},
#endif
    [S_STATUS_MEMORY] = {"Memory", "内存", "メモリ"},
    [S_STATUS_MEMORY_VALUE] = {"%ld MiB available / %ld MiB", "可用 %ld MiB / 共 %ld MiB", "空き %ld MiB / 合計 %ld MiB"},
    [S_STATUS_IPV4] = {"IPv4", "IPv4", "IPv4"},
    [S_STORAGE_TMPFS] = {"Temporary files (RAM)", "临时文件（内存）", "一時ファイル（RAM）"},
    [S_STORAGE_ROOTFS] = {"Rootfs", "根文件系统", "ルートファイルシステム"},
    [S_STORAGE_APPLICATION] = {"Application", "应用分区", "アプリ領域"},
    [S_STORAGE_SETTINGS] = {"Settings", "设置分区", "設定領域"},
    [S_STORAGE_MICROSD] = {"microSD", "microSD 卡", "microSD"},
    [S_STATUS_REFRESH] = {"Refresh status", "刷新状态", "ステータスを更新"},
    [S_STATUS_ACTIONS] = {"Actions", "操作", "操作"},
    [S_STATUS_RESTART] = {"Restart camera app", "重新启动相机应用", "カメラアプリを再起動"},
    [S_STATUS_UPGRADE_BUTTON] = {"Upgrade Firmware&hellip;", "升级固件&hellip;", "ファームウェアを更新&hellip;"},
    [S_STATUS_UPGRADE_HELP] = {"Select a <code>%s</code> package. ", "请选择 <code>%s</code> 升级包。", "<code>%s</code> パッケージを選択してください。"},
    [S_STATUS_UPGRADE_SYNC_MT11] = {"A complete upload is synced as <code>.bin.tmp</code>, atomically renamed to <code>.bin</code>, and synced again before the updater can discover it.", "上传完成后先以 <code>.bin.tmp</code> 同步写入，再原子地重命名为 <code>.bin</code> 并再次同步，之后升级程序才会发现它。", "アップロードが完了すると <code>.bin.tmp</code> として同期し、<code>.bin</code> にアトミックにリネームして再度同期してから、アップデーターが検出できるようになります。"},
    [S_STATUS_UPGRADE_SYNC_A8] = {"A complete upload is synced to the microSD card as <code>%s</code>; U-Boot installs it on the next reboot.", "上传完成后会以 <code>%s</code> 同步写入 microSD 卡；U-Boot 会在下次重启时安装。", "アップロードが完了すると <code>%s</code> として microSD カードに同期され、次回の再起動時に U-Boot がインストールします。"},
    [S_STATUS_UPGRADE_INSTALL_Z1] = {"The package is verified, installed into the application partition and the camera reboots. Settings and the web password are kept.", "升级包经校验后安装到应用分区，随后相机重启。设置和网页密码将被保留。", "パッケージは検証後にアプリ領域へインストールされ、カメラが再起動します。設定とウェブパスワードは保持されます。"},
    [S_STATUS_REBOOT_CONFIRM] = {"I confirm this camera should reboot", "我确认要重启此相机", "このカメラを再起動することを確認しました"},
    [S_STATUS_REBOOT_BUTTON] = {"Reboot camera", "重启相机", "カメラを再起動"},
    [S_STATUS_AUTH_NOTE] = {"Authentication user: <code>admin</code>. The password is read from <code>%s</code> for every request. This service is HTTP, not HTTPS; keep it on the isolated camera network.", "认证用户：<code>admin</code>。每次请求都会从 <code>%s</code> 读取密码。本服务使用 HTTP 而非 HTTPS，请仅在隔离的相机网络中使用。", "認証ユーザー: <code>admin</code>。パスワードはリクエストごとに <code>%s</code> から読み込まれます。このサービスは HTTPS ではなく HTTP です。隔離されたカメラ用ネットワーク内でのみ使用してください。"},
    [S_TIME_SYNCED] = {"Camera time synchronized with browser", "相机时间已与浏览器同步", "カメラの時刻をブラウザと同期しました"},
    [S_REBOOT_UNCONFIRMED] = {"Reboot confirmation was not checked", "未勾选重启确认", "再起動の確認にチェックが入っていません"},
    [S_TITLE_REBOOTING] = {"%s rebooting", "%s 正在重启", "%s 再起動中"},
    [S_REBOOTING_HEADING] = {"Camera rebooting", "相机正在重启", "カメラを再起動しています"},
    [S_REBOOTING_TEXT] = {"Reconnect in about one minute.", "请在约一分钟后重新连接。", "約 1 分後に再接続してください。"},
    [S_CSRF_RELOAD] = {"Invalid or expired form token; reload the page", "表单令牌无效或已过期，请刷新页面", "フォームトークンが無効または期限切れです。ページを再読み込みしてください"},
    [S_CSRF_INVALID] = {"Invalid or expired form token", "表单令牌无效或已过期", "フォームトークンが無効または期限切れです"},
    [S_UNKNOWN_POST] = {"Unknown action", "未知的操作", "不明な操作です"},
    [S_SENSORS_SUBTITLE_MT11] = {"Live range and temperature telemetry with still capture", "实时距离与温度遥测，以及拍照", "距離・温度のライブ計測と静止画撮影"},
    [S_SENSORS_SUBTITLE_A8] = {"Still capture and recent photos", "拍照与最近的照片", "静止画撮影と最近の写真"},
    [S_SENSORS_LIVE] = {"Live sensors", "实时传感器数据", "センサーの現在値"},
    [S_SENSORS_LIDAR] = {"LiDAR range", "激光测距", "LiDAR 距離"},
    [S_LOADING] = {"Loading&hellip;", "加载中&hellip;", "読み込み中&hellip;"},
    [S_ENABLE] = {"Enable", "启用", "有効化"},
    [S_DISABLE] = {"Disable", "禁用", "無効化"},
    [S_SENSORS_MIN] = {"Minimum temperature", "最低温度", "最低温度"},
    [S_SENSORS_MAX] = {"Maximum temperature", "最高温度", "最高温度"},
    [S_SENSORS_CPU] = {"CPU temperature", "CPU 温度", "CPU 温度"},
    [S_SENSORS_UPDATING] = {"Updating twice per second.", "每秒更新两次。", "毎秒 2 回更新します。"},
    [S_SENSORS_LASER_NOTICE] = {"Enabling LiDAR turns on the camera's laser. Keep people clear of its path. A zero range means there is no valid return.", "启用激光测距会打开相机的激光器，请确保光路上没有人员。距离为零表示没有有效回波。", "LiDAR を有効にするとカメラのレーザーが点灯します。光路に人が入らないようにしてください。距離 0 は有効な反射がないことを示します。"},
    [S_SENSORS_SHUTTER] = {"Shutter", "快门", "シャッター"},
    [S_SENSORS_SHUTTER_TEXT] = {"Capture a still using the running camera application and save it to the microSD card.", "使用当前运行的相机应用拍摄一张照片并保存到 microSD 卡。", "動作中のカメラアプリで静止画を撮影し、microSD カードに保存します。"},
    [S_SENSORS_CAPTURE] = {"Capture photo", "拍照", "撮影"},
    [S_SENSORS_SCOPE_NOTE] = {"The ArduPilot camera app's <strong>Photo capture scope</strong> parameter controls which lenses are saved. New packages default to All lenses.", "ArduPilot 相机应用的<strong>拍照范围</strong>参数决定保存哪些镜头的图像。新版升级包默认为“全部镜头”。", "ArduPilot カメラアプリの<strong>静止画の撮影範囲</strong>パラメータで、どのレンズの画像を保存するかが決まります。新しいパッケージの既定値は「すべてのレンズ」です。"},
    [S_SENSORS_RECENT] = {"Recent photos", "最近的照片", "最近の写真"},
    [S_SENSORS_NO_PHOTOS] = {"No JPEG photos were found beneath <code>%s</code>.", "在 <code>%s</code> 下未找到 JPEG 照片。", "<code>%s</code> 配下に JPEG 写真が見つかりませんでした。"},
    [S_SENSORS_BROWSE] = {"Browse all captures", "浏览全部拍摄文件", "撮影ファイルをすべて表示"},
    [S_PHOTO_CAPTURED] = {"Photo captured and saved to the microSD card", "照片已拍摄并保存到 microSD 卡", "写真を撮影し、microSD カードに保存しました"},
    [S_E_LIDAR_ACTION] = {"Unknown LiDAR action", "未知的激光测距操作", "不明な LiDAR 操作です"},
    [S_LIDAR_ENABLED] = {"LiDAR enabled", "激光测距已启用", "LiDAR を有効にしました"},
    [S_LIDAR_DISABLED] = {"LiDAR disabled", "激光测距已禁用", "LiDAR を無効にしました"},
    [S_JS_UNAVAILABLE] = {"Unavailable", "不可用", "取得不可"},
    [S_JS_TEMPERATURE_AT] = {" °C at (", " °C，位于 (", " °C、座標 ("},
    [S_JS_NO_RETURN] = {"No valid return", "无有效回波", "有効な反射なし"},
    [S_JS_UPDATED] = {"Updated ", "更新于 ", "更新時刻: "},
    [S_JS_TWICE_PER_SECOND] = {" · twice per second", " · 每秒两次", " · 毎秒 2 回"},
    [S_JS_SENSOR_FAILED] = {"Sensor update failed: ", "传感器更新失败：", "センサーの更新に失敗しました: "},
    [S_JS_UNKNOWN_ERROR] = {"unknown error", "未知错误", "不明なエラー"},
    [S_JS_WAITING_RANGE] = {"Waiting for range…", "等待测距数据…", "距離の取得待ち…"},
    [S_JS_LIDAR_CONTROL_FAILED] = {"LiDAR control failed: ", "激光测距控制失败：", "LiDAR の制御に失敗しました: "},
    [S_TITLE_LIVE] = {"%s live video", "%s 实时视频", "%s ライブ映像"},
    [S_LIVE_HEADING] = {"Live video", "实时视频", "ライブ映像"},
    [S_LIVE_SUBTITLE] = {"Native H.264 preview from the running ArduPilot camera app", "来自运行中的 ArduPilot 相机应用的原生 H.264 预览", "動作中の ArduPilot カメラアプリからのネイティブ H.264 プレビュー"},
    [S_LIVE_STREAM] = {"Stream", "视频流", "ストリーム"},
    [S_LIVE_MAIN] = {"Main / video1", "主码流 / video1", "メイン / video1"},
    [S_LIVE_SECONDARY] = {"Secondary / video2", "子码流 / video2", "サブ / video2"},
    [S_LIVE_STARTING] = {"Starting live stream&hellip;", "正在启动实时视频流&hellip;", "ライブストリームを開始しています&hellip;"},
    [S_LIVE_HELP] = {"This uses the camera's encoded H.264 frames directly; no video proxy or transcoder is installed. Set the selected stream codec to H.264 if it is unavailable.", "此功能直接使用相机编码的 H.264 帧，未安装视频代理或转码器。若无法播放，请将所选视频流的编码格式设为 H.264。", "カメラがエンコードした H.264 フレームをそのまま使用します。映像プロキシやトランスコーダーはインストールされていません。表示できない場合は、選択したストリームのコーデックを H.264 に設定してください。"},
    [S_LIVE_PTZ] = {"Pan, tilt and zoom", "水平转动、俯仰与变焦", "パン・チルト・ズーム"},
    [S_LIVE_ENABLE_MANUAL] = {"Enable manual gimbal control", "启用手动云台控制", "ジンバルの手動操作を有効にする"},
    [S_LIVE_MANUAL_NOTICE] = {"Manual control temporarily blocks other gimbal commands and pauses ROI tracking. It clears on camera app restart or reboot, and expires if this page disconnects. Direction buttons send bounded 180 ms pulses and always issue a stop.", "手动控制会暂时阻止其他云台命令并暂停 ROI 跟踪。重启相机应用或相机后将清除，页面断开连接后会过期。方向按钮发送限定为 180 ms 的脉冲，并且始终会随后发送停止命令。", "手動操作中は他のジンバルコマンドと ROI 追跡を一時停止します。アプリやカメラの再起動、ページの切断で解除されます。方向ボタンは 180 ms に制限したパルスを送り、必ず停止コマンドを送信します。"},
    [S_LIVE_CENTRE] = {"Centre", "回中", "センター"},
    [S_LIVE_RATE] = {"Command rate", "控制速率", "操作速度"},
    [S_LIVE_ZOOM] = {"Zoom", "变焦", "ズーム"},
    [S_LIVE_NO_COMMANDS] = {"No manual commands sent.", "尚未发送手动命令。", "手動コマンドは送信されていません。"},
    [S_LIVE_ATTITUDE] = {"Gimbal attitude", "云台姿态", "ジンバル姿勢"},
    [S_LIVE_ATTITUDE_UNAVAILABLE] = {"Gimbal attitude unavailable", "云台姿态不可用", "ジンバル姿勢を取得できません"},
    [S_LIVE_YAW] = {"Yaw", "偏航", "ヨー"},
    [S_LIVE_ROLL] = {"Roll", "横滚", "ロール"},
    [S_LIVE_PITCH] = {"Pitch", "俯仰", "ピッチ"},
    [S_LIVE_YAW_RATE] = {"Yaw rate", "偏航速率", "ヨー速度"},
    [S_LIVE_ROLL_RATE] = {"Roll rate", "横滚速率", "ロール速度"},
    [S_LIVE_PITCH_RATE] = {"Pitch rate", "俯仰速率", "ピッチ速度"},
    [S_LIVE_WAITING_GIMBAL] = {"Waiting for gimbal&hellip;", "等待云台&hellip;", "ジンバルの応答待ち&hellip;"},
    [S_COMMAND_SENT] = {"Command sent", "命令已发送", "コマンドを送信しました"},
    [S_JS_CONNECTING] = {"Connecting…", "正在连接…", "接続中…"},
    [S_JS_RETRYING] = {" · retrying", " · 正在重试", " · 再試行中"},
    [S_JS_LIVE_PREFIX] = {"Live · ", "实时 · ", "ライブ · "},
    [S_JS_STREAM_ENDED] = {"Camera stream ended", "相机视频流已结束", "カメラのストリームが終了しました"},
    [S_JS_LIVE_UNAVAILABLE] = {"Live video unavailable: ", "实时视频不可用：", "ライブ映像を表示できません: "},
    [S_JS_ERROR_ABORTED] = {"aborted", "已中止", "中断"},
    [S_JS_ERROR_NETWORK] = {"network", "网络错误", "ネットワーク"},
    [S_JS_ERROR_DECODE] = {"decode", "解码错误", "デコード"},
    [S_JS_ERROR_UNSUPPORTED] = {"unsupported", "不支持", "非対応"},
    [S_JS_ERROR_CODE] = {"error ", "错误 ", "エラー "},
    [S_JS_ENABLE_FIRST] = {"Enable manual gimbal control first.", "请先启用手动云台控制。", "先にジンバルの手動操作を有効にしてください。"},
    [S_JS_CONTROL_FAILED] = {"Control failed: ", "控制失败：", "操作に失敗しました: "},
    [S_JS_MANUAL_ENABLED] = {"Manual control enabled.", "手动控制已启用。", "手動操作を有効にしました。"},
    [S_JS_MANUAL_DISABLED] = {"Manual control disabled.", "手动控制已禁用。", "手動操作を無効にしました。"},
    [S_JS_ATTITUDE_LABEL] = {"Gimbal roll %s degrees, pitch %s degrees, yaw %s degrees", "云台横滚 %s 度，俯仰 %s 度，偏航 %s 度", "ジンバルのロール %s 度、ピッチ %s 度、ヨー %s 度"},
    [S_JS_LIVE_ATTITUDE] = {"Live attitude · 4 Hz", "实时姿态 · 4 Hz", "姿勢のリアルタイム表示 · 4 Hz"},
    [S_JS_ATTITUDE_FAILED] = {"Attitude update failed: ", "姿态更新失败：", "姿勢の更新に失敗しました: "},
    [S_TITLE_USERS] = {"%s users", "%s 用户", "%s ユーザー"},
    [S_USERS_HEADING] = {"Users", "用户", "ユーザー"},
    [S_USERS_SUBTITLE_MT11] = {"Administrative credentials and SSH access", "管理凭据与 SSH 访问", "管理者の認証情報と SSH アクセス"},
    [S_USERS_SUBTITLE_A8] = {"Administrative credentials", "管理凭据", "管理者の認証情報"},
    [S_USERS_PASSWORD_HEADING] = {"Admin password", "管理员密码", "管理者パスワード"},
    [S_USERS_PASSWORD_TEXT] = {"The username remains <code>admin</code>. The new password applies to the next login and to the next HTTP Basic request; existing browser sessions stay logged in.", "用户名保持为 <code>admin</code>。新密码对下一次登录和下一次 HTTP Basic 请求生效；已登录的浏览器会话保持登录状态。", "ユーザー名は <code>admin</code> のままです。新しいパスワードは次回のログインと次の HTTP Basic リクエストから適用され、ログイン済みのブラウザセッションはそのまま維持されます。"},
    [S_USERS_NEW_PASSWORD] = {"New password", "新密码", "新しいパスワード"},
    [S_USERS_CONFIRM_PASSWORD] = {"Confirm password", "确认密码", "パスワードの確認"},
    [S_USERS_CHANGE_BUTTON] = {"Change admin password", "修改管理员密码", "管理者パスワードを変更"},
    [S_USERS_HTTP_NOTICE] = {"This service runs over HTTP without TLS. Change credentials only on the isolated management network.", "本服务通过不带 TLS 的 HTTP 运行。请仅在隔离的管理网络中修改凭据。", "このサービスは TLS なしの HTTP で動作します。認証情報の変更は隔離された管理ネットワーク内でのみ行ってください。"},
    [S_USERS_ADD_KEYS] = {"Add SSH public keys", "添加 SSH 公钥", "SSH 公開鍵を追加"},
    [S_USERS_ADD_KEYS_BUTTON] = {"Add public-key files&hellip;", "添加公钥文件&hellip;", "公開鍵ファイルを追加&hellip;"},
    [S_USERS_ADD_KEYS_HELP] = {"Select one or more <code>.pub</code> files. The whole batch is validated before anything changes. Key options such as forced commands are intentionally rejected. Changes update persistent and live Dropbear files.", "选择一个或多个 <code>.pub</code> 文件。整批文件会在做出任何更改前先行验证。强制命令等密钥选项会被有意拒绝。更改会同时更新 Dropbear 的持久文件和运行时文件。", "<code>.pub</code> ファイルを 1 つ以上選択してください。変更前に一括で検証されます。強制コマンドなどの鍵オプションは意図的に拒否されます。変更は Dropbear の永続ファイルと実行時ファイルの両方に反映されます。"},
    [S_USERS_AUTHORIZED] = {"Authorized SSH keys", "已授权的 SSH 密钥", "登録済みの SSH 鍵"},
    [S_USERS_KEYS_UNREADABLE] = {"Unable to read the authorized-key file.", "无法读取已授权密钥文件。", "authorized_keys ファイルを読み取れません。"},
    [S_USERS_KEY_TYPE] = {"Type", "类型", "種類"},
    [S_USERS_KEY] = {"Key", "密钥", "鍵"},
    [S_USERS_KEY_COMMENT] = {"Comment", "备注", "コメント"},
    [S_USERS_KEY_ACTION] = {"Action", "操作", "操作"},
    [S_USERS_KEY_NO_COMMENT] = {"none", "无", "なし"},
    [S_USERS_KEY_CONFIRM] = {"confirm", "确认", "確認"},
    [S_USERS_KEY_REMOVE] = {"Remove", "删除", "削除"},
    [S_USERS_NO_KEYS] = {"No supported public keys are currently authorized.", "当前没有已授权的受支持公钥。", "現在、対応形式の公開鍵は登録されていません。"},
    [S_USERS_UNMANAGED_ONE] = {"1 unrecognized authorized_keys line preserved but not editable here.", "有 1 行无法识别的 authorized_keys 条目已保留，但无法在此编辑。", "認識できない authorized_keys の行が 1 行あります。保持されますが、ここでは編集できません。"},
    [S_USERS_UNMANAGED_MANY] = {"%zu unrecognized authorized_keys lines preserved but not editable here.", "有 %zu 行无法识别的 authorized_keys 条目已保留，但无法在此编辑。", "認識できない authorized_keys の行が %zu 行あります。保持されますが、ここでは編集できません。"},
    [S_USERS_LAST_KEY_NOTICE] = {"The last usable SSH key cannot be removed. Add and test a replacement before removing old keys. Existing SSH sessions are not closed.", "最后一个可用的 SSH 密钥无法删除。删除旧密钥前，请先添加并测试替代密钥。现有 SSH 会话不会被关闭。", "最後の有効な SSH 鍵は削除できません。古い鍵を削除する前に、代わりの鍵を追加して動作を確認してください。既存の SSH セッションは切断されません。"},
    [S_PASSWORD_CHANGED] = {"Admin password changed; use it for the next login", "管理员密码已修改，下次登录时请使用新密码", "管理者パスワードを変更しました。次回のログインから新しいパスワードを使ってください"},
    [S_KEY_ADDED_ONE] = {"1 SSH public key added", "已添加 1 个 SSH 公钥", "SSH 公開鍵を 1 件追加しました"},
    [S_KEYS_ADDED_MANY] = {"%zu SSH public keys added", "已添加 %zu 个 SSH 公钥", "SSH 公開鍵を %zu 件追加しました"},
    [S_KEY_REMOVED] = {"SSH public key removed", "SSH 公钥已删除", "SSH 公開鍵を削除しました"},
    [S_JS_SELECT_PUB] = {"Select only .pub public-key files.", "只能选择 .pub 公钥文件。", ".pub の公開鍵ファイルのみ選択してください。"},
    [S_JS_READING_KEYS] = {"Reading public-key files…", "正在读取公钥文件…", "公開鍵ファイルを読み込んでいます…"},
    [S_JS_FILES_EMPTY] = {"The selected files are empty.", "所选文件为空。", "選択したファイルは空です。"},
    [S_JS_KEYS_TOO_LARGE] = {"The selected public keys exceed 64 KiB.", "所选公钥超过 64 KiB。", "選択した公開鍵が 64 KiB を超えています。"},
    [S_JS_UPLOADING_KEYS] = {"Uploading public keys…", "正在上传公钥…", "公開鍵をアップロードしています…"},
    [S_JS_FILES_UNREADABLE] = {"Unable to read the selected files.", "无法读取所选文件。", "選択したファイルを読み取れません。"},
    [S_TITLE_APP_PARAMETERS] = {"Parameters", "参数", "パラメータ"},
    [S_APP_PARAMETERS_SUBTITLE] = {"Configure the camera, networking and video", "配置相机、网络和视频", "カメラ、ネットワーク、映像の設定"},
    [S_APP_PARAMETERS_NOTICE] = {"Use <strong>Save</strong> to apply live settings automatically. Video-format changes briefly reconnect streams and wait until recording stops. Settings marked as requiring restart use <strong>Save and restart</strong>.", "<strong>保存</strong>后自动应用实时设置。视频格式更改会短暂重连视频，并等待录像停止。标记需要重启的设置请使用<strong>保存并重新启动</strong>。", "<strong>保存</strong>で設定を自動反映します。映像形式の変更は録画停止後にストリームを再接続します。再起動が必要と表示された設定は<strong>保存して再起動</strong>を使ってください。"},
    [S_PARAMS_SYSTEM] = {"System", "系统", "システム"},
    [S_PARAMS_NETWORK] = {"Network", "网络", "ネットワーク"},
    [S_PARAMS_VIDEO] = {"Video", "视频", "映像"},
    [S_PARAMS_CATEGORIES] = {"Parameter categories", "参数分类", "パラメータ分類"},
    [S_PARAMS_SAVE_ALL] = {"Save applies changes from all tabs.", "保存将应用所有选项卡中的更改。", "保存はすべてのタブの変更を反映します。"},
    [S_PARAMS_SAVE] = {"Save parameters", "保存参数", "パラメータを保存"},
    [S_PARAMS_SAVE_RESTART] = {"Save and restart camera app", "保存并重新启动相机应用", "保存してカメラアプリを再起動"},
    [S_PARAMS_SAVED_RESTARTED] = {"Parameters saved and %s restarted", "参数已保存，%s 已重新启动", "パラメータを保存し、%s を再起動しました"},
    [S_PARAMS_SAVED] = {"Parameters saved; waiting for %s to apply live settings", "参数已保存；重新启动 %s 后生效", "パラメータを保存しました。反映するには %s を再起動してください"},
    [S_E_CONFIG_UNREADABLE] = {"Unable to read config or out of memory", "无法读取配置或内存不足", "設定を読み取れないか、メモリが不足しています"},
    [S_TITLE_RAW] = {"%s raw config", "%s 原始配置", "%s 設定ファイル"},
    [S_RAW_HEADING] = {"Raw %s", "原始 %s", "%s の直接編集"},
    [S_RAW_SUBTITLE] = {"Advanced editor for the running %s", "面向当前运行的 %s 的高级编辑器", "動作中の %s 向けの高度なエディター"},
    [S_RAW_SAVE] = {"Save config", "保存配置", "設定を保存"},
    [S_RAW_HELP] = {"Saves are syntax-checked and atomic. Editing <code>%s</code>; the previous file is kept at <code>%s</code>. The Parameters page performs stronger per-value validation.", "保存前会进行语法检查，且保存是原子操作。当前编辑 <code>%s</code>；旧文件保留在 <code>%s</code>。“参数”页面会对每个值进行更严格的校验。", "保存時に構文チェックを行い、アトミックに書き込みます。編集対象は <code>%s</code> で、以前のファイルは <code>%s</code> に保持されます。「パラメータ」ページでは値ごとにより厳密な検証が行われます。"},
    [S_CONFIG_SAVED_RESTARTED] = {"Config saved and %s restarted", "配置已保存，%s 已重新启动", "設定を保存し、%s を再起動しました"},
    [S_CONFIG_SAVED] = {"Config saved", "配置已保存", "設定を保存しました"},
    [S_TITLE_FILES] = {"%s files", "%s 文件", "%s ファイル"},
    [S_FILES_HEADING] = {"Filesystem", "文件系统", "ファイルシステム"},
    [S_FILES_SUBTITLE] = {"Authenticated browsing and downloads; deletion is restricted to %s", "经认证的浏览与下载；删除仅限于 %s", "認証済みの閲覧とダウンロード。削除は %s 配下に限られます"},
    [S_FILES_PATH] = {"Path", "路径", "パス"},
    [S_FILES_OPEN] = {"Open path", "打开路径", "パスを開く"},
    [S_FILES_NAME] = {"Name", "名称", "名前"},
    [S_FILES_TYPE] = {"Type", "类型", "種類"},
    [S_FILES_SIZE] = {"Size", "大小", "サイズ"},
    [S_FILES_MODIFIED] = {"Modified", "修改时间", "更新日時"},
    [S_FILES_MODE] = {"Mode", "权限", "モード"},
    [S_FILES_ACTIONS] = {"Actions", "操作", "操作"},
    [S_FILES_DIRECTORY] = {"directory", "目录", "ディレクトリ"},
    [S_FILES_LINK] = {" (link)", "（链接）", "（リンク）"},
    [S_FILES_DOWNLOAD] = {"Download", "下载", "ダウンロード"},
    [S_FILES_DELETE] = {"Delete", "删除", "削除"},
    [S_FILES_TRUNCATED] = {"Only the first 5000 entries are shown.", "仅显示前 5000 个条目。", "先頭の 5000 件のみ表示しています。"},
    [S_FILES_LEGEND] = {"Type letters: d directory, f regular file, l symbolic link, c character device, b block device, p FIFO, s socket. Files outside %s can be viewed or downloaded but never deleted through this service.", "类型字母：d 目录，f 普通文件，l 符号链接，c 字符设备，b 块设备，p FIFO，s 套接字。%s 之外的文件可以查看或下载，但不能通过本服务删除。", "種類の記号: d ディレクトリ、f 通常ファイル、l シンボリックリンク、c キャラクターデバイス、b ブロックデバイス、p FIFO、s ソケット。%s の外にあるファイルは表示・ダウンロードはできますが、このサービスからは削除できません。"},
    [S_TITLE_VIEWER] = {"%s file viewer", "%s 文件查看器", "%s ファイルビューアー"},
    [S_VIEWER_HEADING] = {"File viewer", "文件查看器", "ファイルビューアー"},
    [S_VIEWER_BACK] = {"Back to directory", "返回目录", "ディレクトリに戻る"},
    [S_VIEWER_IMAGE_ALT] = {"Image preview", "图片预览", "画像プレビュー"},
    [S_VIEWER_NO_VIDEO] = {"This browser cannot play the video; use Download.", "此浏览器无法播放该视频，请使用“下载”。", "このブラウザでは動画を再生できません。「ダウンロード」を使用してください。"},
    [S_VIEWER_NO_PREVIEW] = {"No inline preview is available for this file type. Use Download.", "此文件类型没有内嵌预览，请使用“下载”。", "このファイル形式はインラインプレビューに対応していません。「ダウンロード」を使用してください。"},
    [S_TITLE_DELETE] = {"Confirm deletion", "确认删除", "削除の確認"},
    [S_DELETE_TEXT] = {"This permanently deletes ", "这将永久删除 ", "次の対象を完全に削除します: "},
    [S_DELETE_RECURSIVE] = {" and all files and directories beneath it", " 及其下的所有文件和目录", "（配下のすべてのファイルとディレクトリを含む）"},
    [S_DELETE_NO_UNDO] = {". This operation cannot be undone.", "。此操作无法撤销。", "。この操作は元に戻せません。"},
    [S_DELETE_CONFIRM] = {"I understand this deletion is permanent", "我了解此删除操作不可恢复", "この削除が元に戻せないことを理解しました"},
    [S_DELETE_BUTTON] = {"Delete permanently", "永久删除", "完全に削除"},
    [S_TITLE_DEBUG] = {"%s debug", "%s 调试", "%s デバッグ"},
    [S_DEBUG_HEADING] = {"Debug", "调试", "デバッグ"},
    [S_DEBUG_SUBTITLE] = {"Camera application output; refreshes every 3 seconds", "相机应用输出；每 3 秒刷新一次", "カメラアプリの出力。3 秒ごとに更新します"},
    [S_DEBUG_PLAIN_TEXT] = {"Plain text", "纯文本", "プレーンテキスト"},
    [S_DEBUG_EMPTY_MT11] = {"No captured output yet. Restart the camera application to begin capture.", "尚未捕获到输出。重新启动相机应用以开始捕获。", "まだ出力を取得していません。取得を開始するにはカメラアプリを再起動してください。"},
    [S_DEBUG_EMPTY_A8] = {"No camera application output yet.", "尚无相机应用日志输出。", "カメラアプリのログ出力はまだありません。"},
    [S_DEBUG_FOOTER] = {"Showing the newest %zu bytes. The RAM log rotates at %u KiB and retains one previous segment.", "显示最新的 %zu 字节。内存日志在 %u KiB 时轮转，并保留上一段。", "最新の %zu バイトを表示しています。RAM 上のログは %u KiB でローテーションし、直前のログファイルを 1 世代保持します。"},
    [S_E_LOG_UNREADABLE] = {"Unable to read log", "无法读取日志", "ログを読み取れません"},
    [S_E_FW_SIZE] = {"Firmware size must be between 1 byte and 128 MiB", "固件大小必须在 1 字节至 128 MiB 之间", "ファームウェアのサイズは 1 バイト〜128 MiB である必要があります"},
    [S_E_FW_CONTENT_TYPE] = {"Firmware upload must use application/octet-stream", "固件上传必须使用 application/octet-stream", "ファームウェアのアップロードは application/octet-stream で行う必要があります"},
    [S_E_FW_NAME] = {"Filename must match %s and contain only safe ASCII characters", "文件名必须符合 %s 且只包含安全的 ASCII 字符", "ファイル名は %s の形式で、安全な ASCII 文字のみを含む必要があります"},
    [S_E_FW_NAME_LONG] = {"Firmware filename is too long", "固件文件名过长", "ファームウェアのファイル名が長すぎます"},
    [S_E_FW_LOCK] = {"Cannot lock firmware uploads: %s", "无法锁定固件上传：%s", "ファームウェアのアップロードをロックできません: %s"},
    [S_E_FW_MICROSD] = {"Cannot access mounted microSD: %s", "无法访问已挂载的 microSD 卡：%s", "マウントされた microSD にアクセスできません: %s"},
    [S_E_FW_SPACE] = {"Not enough free space on the microSD card", "microSD 卡剩余空间不足", "microSD カードの空き容量が不足しています"},
    [S_E_FW_INSPECT] = {"Cannot inspect %s: %s", "无法检查 %s：%s", "%s を確認できません: %s"},
    [S_E_FW_EXISTS] = {"Another %s firmware package already exists on the card", "卡上已存在另一个 %s 固件升级包", "カード上に別の %s ファームウェアパッケージがすでに存在します"},
    [S_E_FW_NAME_EXISTS] = {"A firmware file with that name already exists on the card", "卡上已存在同名的固件文件", "同じ名前のファームウェアファイルがカード上にすでに存在します"},
    [S_E_FW_TEMP_EXISTS] = {"A temporary upload with that name already exists on the card", "卡上已存在同名的临时上传文件", "同じ名前の一時アップロードファイルがカード上にすでに存在します"},
    [S_E_FW_CREATE] = {"Cannot create %s/%s: %s", "无法创建 %s/%s：%s", "%s/%s を作成できません: %s"},
    [S_E_FW_PUBLISH] = {"Cannot publish %s/%s: %s", "无法发布 %s/%s：%s", "%s/%s を公開できません: %s"},
    [S_E_FW_APPEARED] = {"A firmware file with that name appeared during upload", "上传期间出现了同名的固件文件", "アップロード中に同じ名前のファームウェアファイルが現れました"},
    [S_E_FW_PUBLISH_SAFE] = {"Cannot safely publish %s/%s: %s", "无法安全地发布 %s/%s：%s", "%s/%s を安全に公開できません: %s"},
    [S_E_FW_DIR_SYNC] = {"Firmware was renamed but directory sync failed: %s", "固件已重命名，但目录同步失败：%s", "ファームウェアのリネームは完了しましたが、ディレクトリの同期に失敗しました: %s"},
    [S_FW_UPLOADED_MT11] = {"Firmware %.200s uploaded and synced to the microSD card. The updater checks about every 5 seconds and should start soon; do not interrupt power.", "固件 %.200s 已上传并同步到 microSD 卡。升级程序约每 5 秒检查一次，应很快开始；请勿断电。", "ファームウェア %.200s をアップロードし、microSD カードに同期しました。アップデーターは約 5 秒ごとに確認するため、まもなく開始されます。電源を切らないでください。"},
    [S_FW_UPLOADED_A8] = {"Firmware %.200s uploaded and synced to the card as %s. Reboot the camera to install it; do not interrupt power while it installs.", "固件 %.200s 已上传并以 %s 同步到卡上。重启相机以安装；安装期间请勿断电。", "ファームウェア %.200s をアップロードし、%s としてカードに同期しました。インストールするにはカメラを再起動してください。インストール中は電源を切らないでください。"},
    [S_FW_INSTALLED_Z1] = {"Firmware %.200s installed. The camera is rebooting; do not interrupt power.", "固件 %.200s 已安装。相机正在重启，请勿断电。", "ファームウェア %.200s をインストールしました。カメラが再起動中です。電源を切らないでください。"},
    [S_E_FW_PACKAGE] = {"Invalid firmware package: %s", "固件升级包无效：%s", "ファームウェアパッケージが無効です: %s"},
    [S_E_FW_INSTALL] = {"Firmware installation failed: %s", "固件安装失败：%s", "ファームウェアのインストールに失敗しました: %s"},
    [S_E_FW_TMP] = {"Cannot store the upload in %s: %s", "无法将上传内容保存到 %s：%s", "%s にアップロードを保存できません: %s"},
    [S_E_FW_TMP_SPACE] = {"Not enough free space in %s", "%s 剩余空间不足", "%s の空き容量が不足しています"},
    [S_E_FW_FAILED] = {"Firmware upload failed after %zu bytes: %s", "固件上传在 %zu 字节后失败：%s", "ファームウェアのアップロードが %zu バイトで失敗しました: %s"},
    [S_JS_FW_TIMEOUT] = {"Timed out after 60 seconds waiting for the camera. Check its power and network connection before retrying.", "等待相机超过 60 秒已超时。重试前请检查其电源和网络连接。", "カメラの応答を 60 秒待ちましたがタイムアウトしました。再試行の前に電源とネットワーク接続を確認してください。"},
    [S_JS_FW_TIMEOUT_ALERT] = {"The camera did not return within 60 seconds. Check its power and network connection.", "相机在 60 秒内未恢复。请检查其电源和网络连接。", "カメラが 60 秒以内に復帰しませんでした。電源とネットワーク接続を確認してください。"},
    [S_JS_FW_BACK] = {"Camera restarted and is back online.", "相机已重启并恢复在线。", "カメラが再起動し、オンラインに復帰しました。"},
    [S_JS_FW_BACK_LOGIN] = {"The camera is responding again; log in to continue.", "相机已有响应，请重新登录以继续。", "カメラから応答がありました。続けるには再度ログインしてください。"},
    [S_JS_FW_BACK_ALERT] = {"The camera is back online after the firmware upgrade.", "固件升级后相机已恢复在线。", "ファームウェア更新後、カメラがオンラインに復帰しました。"},
    [S_JS_FW_REBOOTING] = {"Camera is rebooting; waiting for it to return…", "相机正在重启，等待其恢复…", "カメラが再起動中です。復帰を待っています…"},
    [S_JS_FW_WAITING_UPDATER] = {"Waiting for the updater to reboot the camera…", "等待升级程序重启相机…", "アップデーターによるカメラの再起動を待っています…"},
    [S_JS_FW_NAME] = {"Filename must match %s using ASCII letters, digits, dot, underscore or hyphen.", "文件名必须符合 %s，且只能使用 ASCII 字母、数字、点、下划线或连字符。", "ファイル名は %s の形式で、ASCII の英字・数字・ドット・アンダースコア・ハイフンのみ使用できます。"},
    [S_JS_FW_SIZE] = {"Firmware size must be between 1 byte and 128 MiB.", "固件大小必须在 1 字节至 128 MiB 之间。", "ファームウェアのサイズは 1 バイト〜128 MiB である必要があります。"},
    [S_JS_FW_CONFIRM] = {"Upload %s and start the automatic firmware upgrade? Do not interrupt camera power.", "上传 %s 并开始自动固件升级？请勿中断相机电源。", "%s をアップロードして自動ファームウェア更新を開始しますか？カメラの電源を切らないでください。"},
    [S_JS_FW_CONFIRM_A8] = {"Upload %s? Reboot the camera afterwards to install it, and do not interrupt power while it installs.", "上传 %s？上传完成后需重启相机以安装固件，安装期间请勿断电。", "%s をアップロードしますか？アップロード後、インストールするにはカメラを再起動してください。インストール中は電源を切らないでください。"},
    [S_JS_FW_CONFIRM_Z1] = {"Upload and install %s? The camera reboots when installation completes; do not interrupt power.", "上传并安装 %s？安装完成后相机将重启，请勿断电。", "%s をアップロードしてインストールしますか？インストール完了後にカメラが再起動します。電源を切らないでください。"},
    [S_JS_FW_INSTALLED] = {"Firmware installed. Waiting for the camera to reboot…", "固件已安装。等待相机重启…", "ファームウェアをインストールしました。カメラの再起動を待っています…"},
    [S_JS_FW_WRITING] = {"Writing %s…", "正在写入 %s…", "%s を書き込んでいます…"},
    [S_JS_FW_HTTP] = {"Upload returned HTTP ", "上传返回 HTTP ", "アップロードの応答: HTTP "},
    [S_JS_FW_UPLOADED] = {"Firmware uploaded. Waiting for the updater to reboot the camera…", "固件已上传。等待升级程序重启相机…", "ファームウェアをアップロードしました。アップデーターによるカメラの再起動を待っています…"},
    [S_JS_FW_CLOSED] = {"Connection closed during upload. Check camera status before retrying.", "上传期间连接已关闭。重试前请检查相机状态。", "アップロード中に接続が閉じられました。再試行の前にカメラの状態を確認してください。"},
    [S_E_BODY_TOO_LARGE] = {"Request body too large", "请求正文过大", "リクエスト本文が大きすぎます"},
    [S_E_HEADERS_TOO_LARGE] = {"Headers too large", "请求头过大", "ヘッダーが大きすぎます"},
    [S_E_MALFORMED] = {"Malformed HTTP request", "HTTP 请求格式错误", "不正な HTTP リクエストです"},
    [S_E_LIVE_UNAVAILABLE] = {"Native live video is not available", "原生实时视频不可用", "ネイティブライブ映像を利用できません"},
    [S_E_LIVE_H264] = {"This stream must be configured for H.264", "此视频流必须配置为 H.264", "このストリームは H.264 に設定する必要があります"},
};
/* ---- end of user-visible strings ---- */

/* each request runs in its own forked worker, so this is per request */
static enum language current_language = LANG_EN;

static const char *T(enum string_id id)
{
    const char *text = strings[id][current_language];

    return text != NULL ? text : strings[id][LANG_EN];
}

static bool language_from_code(const char *code, size_t length,
                               enum language *result)
{
    for (size_t i = 0; i < LANG_COUNT; i++) {
        if (strlen(languages[i].code) == length &&
            strncasecmp(languages[i].code, code, length) == 0) {
            *result = (enum language)i;
            return true;
        }
    }
    return false;
}

/* end of the current comma-separated header element, commas inside
 * quoted parameter values included */
static const char *header_element_end(const char *p, const char *end)
{
    bool quoted = false;

    for (; p < end; p++) {
        if (*p == '"') quoted = !quoted;
        else if (*p == '\\' && quoted && p + 1 < end) p++;
        else if (*p == ',' && !quoted) break;
    }
    return p;
}

/* The q value of a header element's parameters (RFC 9110 qvalue), 1 when
 * absent. False for malformed parameters: missing ";" separators, names
 * that are not tokens, unterminated quoted values. With weight_only, q is
 * the only parameter allowed (Accept-Language). */
static bool header_element_quality(const char *params, const char *end,
                                   bool weight_only, double *quality)
{
    const char *p = params;

    *quality = 1.0;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    while (p < end) {
        const char *name;
        const char *value;
        const char *value_end;
        if (*p != ';') return false;
        p++;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        name = p;
        while (p < end && (isalnum((unsigned char)*p) ||
                           strchr("!#$%&'*+-.^_`|~", *p) != NULL)) p++;
        if (p == name || p == end || *p != '=') return false;
        value = p + 1;
        if (value < end && *value == '"') {
            value_end = value + 1;
            while (value_end < end && *value_end != '"') {
                if (*value_end == '\\' && value_end + 1 < end) value_end++;
                value_end++;
            }
            if (value_end == end) return false;
            value_end++;
        } else {
            value_end = value;
            while (value_end < end && (isalnum((unsigned char)*value_end) ||
                                       strchr("!#$%&'*+-.^_`|~", *value_end) != NULL)) {
                value_end++;
            }
            if (value_end == value) return false;
        }
        if (p - name == 1 && (*name == 'q' || *name == 'Q')) {
            size_t length = (size_t)(value_end - value);
            double result;
            double scale = 1.0;
            if (length > 5 || (*value != '0' && *value != '1')) return false;
            result = *value - '0';
            if (length > 1) {
                if (value[1] != '.') return false;
                for (size_t i = 2; i < length; i++) {
                    if (!isdigit((unsigned char)value[i])) return false;
                    scale /= 10.0;
                    result += (value[i] - '0') * scale;
                }
            }
            if (result > 1.0) return false;
            *quality = result;
        } else if (weight_only) {
            return false;
        }
        p = value_end;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
    }
    return true;
}

/* Accept-Language: the supported primary subtag with the highest q value,
 * earlier entries winning ties; q=0 and malformed ranges are ignored */
static bool language_from_accept(const char *header, size_t header_len,
                                 enum language *result)
{
    const char *p = header;
    const char *end = header + header_len;
    double best = 0.0;
    bool found = false;

    while (p < end) {
        const char *item_end = header_element_end(p, end);
        const char *tag_end;
        const char *primary_end;
        double quality;
        enum language candidate;
        bool valid = true;

        while (p < item_end && (*p == ' ' || *p == '\t')) p++;
        /* language-range: alpha{1,8} ("-" alnum{1,8})*, or "*" */
        tag_end = p;
        while (tag_end < item_end && isalpha((unsigned char)*tag_end)) tag_end++;
        primary_end = tag_end;
        if (tag_end == p || tag_end - p > 8) valid = false;
        while (valid && tag_end < item_end && *tag_end == '-') {
            const char *sub = ++tag_end;
            while (tag_end < item_end && isalnum((unsigned char)*tag_end)) tag_end++;
            if (tag_end == sub || tag_end - sub > 8) valid = false;
        }
        if (valid && !header_element_quality(tag_end, item_end, true, &quality)) {
            valid = false;
        }
        if (valid && quality > best &&
            language_from_code(p, (size_t)(primary_end - p), &candidate)) {
            best = quality;
            *result = candidate;
            found = true;
        }
        p = item_end + (item_end < end ? 1 : 0);
    }
    return found;
}

struct request {
    char method[12];
    char path[4096];
    char query[4096];
    char *storage;
    size_t header_len;
    char *body;
    size_t body_len;
    size_t content_length;
    bool streaming_body;
};

struct option {
    const char *value;
    enum string_id label;
};

enum parameter_kind {
    PARAM_BOOLEAN,
    PARAM_INTEGER,
    PARAM_FLOAT,
    PARAM_IPV4,
    PARAM_ENUM,
    PARAM_LUT,
    PARAM_TEXT,
    PARAM_PASSWORD
};

struct parameter {
    const char *form_name;
    const char *section;
    const char *key;
    enum string_id label;
    enum string_id help;
    enum parameter_kind kind;
    double minimum;
    double maximum;
    double step;
    const struct option *options;
    size_t option_count;
};

struct ini_update {
    const char *section;
    char key[48];
    char value[160];
    bool found;
};


static const struct option photo_scope_options[] = {
    {"thermal", S_OPT_SCOPE_THERMAL},
    {"all", S_OPT_SCOPE_ALL},
};

static const struct option replacement_boolean_options[] = {
    {"false", S_OPT_DISABLED}, {"true", S_OPT_ENABLED},
};
static const struct option autorecord_options[] = {
    {"false", S_OPT_DISABLED}, {"true", S_OPT_ENABLED},
    {"while_armed", S_OPT_WHILE_ARMED},
};
static const struct option orientation_options[] = {
    {"auto", S_OPT_ORIENT_AUTO},
    {"upright", S_OPT_ORIENT_UPRIGHT}, {"inverted", S_OPT_ORIENT_INVERTED},
};
static const struct option uart_protocol_options[] = {
    {"none", S_OPT_UART_NONE},
#if APCAM_HAVE_EXTERNAL_UART
    {"siyi", S_OPT_UART_SIYI}, {"mavlink", S_OPT_UART_MAVLINK},
#endif
};
static const struct option main_resolution_options[] = {
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_720P
    {"1280x720", S_OPT_RES_720},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_1080P
    {"1920x1080", S_OPT_RES_1080},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_1440P
    {"2560x1440", S_OPT_RES_1440},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_2160P
    {"3840x2160", S_OPT_RES_4K},
#endif
};
static const struct option sub_resolution_options[] = {
#if APCAM_SUB_RESOLUTIONS & APCAM_RES_MASK_720P
    {"1280x720", S_OPT_RES_720},
#endif
#if APCAM_SUB_RESOLUTIONS & APCAM_RES_MASK_1080P
    {"1920x1080", S_OPT_RES_1080},
#endif
#if APCAM_SUB_RESOLUTIONS & APCAM_RES_MASK_1440P
    {"2560x1440", S_OPT_RES_1440},
#endif
#if APCAM_SUB_RESOLUTIONS & APCAM_RES_MASK_2160P
    {"3840x2160", S_OPT_RES_4K},
#endif
};
static const struct option recording_resolution_options[] = {
#if APCAM_RECORDING_RESOLUTIONS & APCAM_RES_MASK_720P
    {"1280x720", S_OPT_RES_720},
#endif
#if APCAM_RECORDING_RESOLUTIONS & APCAM_RES_MASK_1080P
    {"1920x1080", S_OPT_RES_1080},
#endif
#if APCAM_RECORDING_RESOLUTIONS & APCAM_RES_MASK_1440P
    {"2560x1440", S_OPT_RES_1440},
#endif
#if APCAM_RECORDING_RESOLUTIONS & APCAM_RES_MASK_2160P
    {"3840x2160", S_OPT_RES_4K},
#endif
};
static const struct option replacement_codec_options[] = {
    { "h264", S_OPT_CODEC_H264 },
#if APCAM_STREAM_CODECS & 2
    { "h265", S_OPT_CODEC_H265 },
#endif
};
static const struct option palette_options[] = {
    {"white_hot", S_OPT_PAL_WHITE_HOT}, {"sepia", S_OPT_PAL_SEPIA},
    {"ironbow", S_OPT_PAL_IRONBOW}, {"rainbow", S_OPT_PAL_RAINBOW},
    {"night", S_OPT_PAL_NIGHT}, {"aurora", S_OPT_PAL_AURORA},
    {"red_hot", S_OPT_PAL_RED_HOT}, {"jungle", S_OPT_PAL_JUNGLE},
    {"medical", S_OPT_PAL_MEDICAL}, {"black_hot", S_OPT_PAL_BLACK_HOT},
    {"glory_hot", S_OPT_PAL_GLORY_HOT},
};
static const struct option replacement_iso_options[] = {
    {"auto", S_OPT_AUTO}, {"100", S_OPT_ISO_100}, {"200", S_OPT_ISO_200},
    {"400", S_OPT_ISO_400}, {"800", S_OPT_ISO_800}, {"1600", S_OPT_ISO_1600},
    {"3200", S_OPT_ISO_3200},
};
static const struct option replacement_shutter_options[] = {
    {"auto", S_OPT_AUTO}, {"1/30", S_OPT_SHUTTER_30}, {"1/50", S_OPT_SHUTTER_50},
    {"1/100", S_OPT_SHUTTER_100}, {"1/250", S_OPT_SHUTTER_250},
    {"1/500", S_OPT_SHUTTER_500}, {"1/750", S_OPT_SHUTTER_750},
    {"1/1000", S_OPT_SHUTTER_1000}, {"1/2000", S_OPT_SHUTTER_2000},
};
static const struct option replacement_metering_options[] = {
    {"average", S_OPT_METER_AVERAGE}, {"center", S_OPT_METER_CENTER},
    {"spot", S_OPT_METER_SPOT_CENTER},
};
static const struct option replacement_wb_options[] = {
    {"auto", S_OPT_AUTO}, {"daylight", S_OPT_WB_DAYLIGHT}, {"cloudy", S_OPT_WB_CLOUDY},
    {"fluorescent", S_OPT_WB_FLUORESCENT}, {"incandescent", S_OPT_WB_INCANDESCENT},
};


static const struct option camera_component_options[] = {
    {"100", S_OPT_CAMERA_COMP1},
    {"101", S_OPT_CAMERA_COMP2},
    {"102", S_OPT_CAMERA_COMP3},
    {"103", S_OPT_CAMERA_COMP4},
    {"104", S_OPT_CAMERA_COMP5},
    {"105", S_OPT_CAMERA_COMP6},
};

static const struct option tracking_options[] = {{"angle", S_OPT_TRACK_ANGLE}, {"rate", S_OPT_TRACK_RATE}};

static const struct parameter replacement_parameters[] = {
    {"timezone", "general", "timezone", S_P_TIMEZONE, S_H_TIMEZONE,
     PARAM_TEXT, 1, 127, 0, NULL, 0},
    {"photo_scope", "capture", "photo_scope", S_P_PHOTO_SCOPE, S_H_PHOTO_SCOPE,
     PARAM_ENUM, 0, 0, 0, photo_scope_options, 2},
    {"orientation", "mount", "orientation", S_P_ORIENTATION, S_H_ORIENTATION,
     PARAM_ENUM, 0, 0, 0, orientation_options, 3},
    {"uart_protocol", "uart", "protocol", S_P_UART_PROTOCOL,
     TARGET_TEXT(S_H_UART_PROTOCOL_MT11, S_H_UART_PROTOCOL_A8),
     PARAM_ENUM, 0, 0, 0, uart_protocol_options, 3},
    {"mavlink_system_id", "mavlink", "system_id", S_P_MAVLINK_SYSID, S_H_MAVLINK_SYSID,
     PARAM_INTEGER, 0, 255, 1, NULL, 0},
    {"mavlink_camera_component_id", "mavlink", "camera_component_id", S_P_MAVLINK_CAMERA_COMPID, S_H_MAVLINK_CAMERA_COMPID,
     PARAM_ENUM, 0, 0, 0, camera_component_options, 6},
    {"mavlink_tcp_port", "mavlink", "tcp_port", S_P_MAVLINK_TCP, S_H_MAVLINK_TCP,
     PARAM_INTEGER, 0, 65535, 1, NULL, 0},
    {"mavlink_udp_port", "mavlink", "udp_port", S_P_MAVLINK_UDP, S_H_MAVLINK_UDP,
     PARAM_INTEGER, 0, 65535, 1, NULL, 0},
    {"position_targeting", "mavlink", "position_targeting",
     S_P_POSITION_TARGETING, S_H_POSITION_TARGETING,
     PARAM_ENUM, 0, 0, 0, replacement_boolean_options, 2},
    {"log_disarmed", "logging", "disarmed", S_P_LOG_DISARMED, S_H_LOG_DISARMED,
     PARAM_ENUM, 0, 0, 0, replacement_boolean_options, 2},
    {"tracking_method", "mavlink", "tracking_method", S_P_TRACK_METHOD, S_H_TRACK_METHOD,
     PARAM_ENUM, 0, 0, 0, tracking_options, 2},
    {"thermal_palette", "thermal", "palette", S_P_THERMAL_PALETTE, S_H_THERMAL_PALETTE,
     PARAM_ENUM, 0, 0, 0, palette_options,
     sizeof(palette_options) / sizeof(palette_options[0])},
    {"autorecord", "recording", "autorecord", S_P_AUTORECORD, S_H_AUTORECORD_APP,
     PARAM_ENUM, 0, 0, 0, autorecord_options, 3},
    {"recording_resolution", "recording", "resolution", S_P_RECORDING_RESOLUTION,
     TARGET_TEXT(S_H_RECORDING_RESOLUTION_MT11, S_H_RECORDING_RESOLUTION_A8),
     PARAM_ENUM, 0, 0, 0, recording_resolution_options,
     sizeof(recording_resolution_options) / sizeof(recording_resolution_options[0])},
    {"main_resolution", "stream.main", "resolution", S_P_MAIN_RESOLUTION,
     TARGET_TEXT(S_H_MAIN_RESOLUTION_MT11, S_H_MAIN_RESOLUTION_A8),
     PARAM_ENUM, 0, 0, 0, main_resolution_options, sizeof(main_resolution_options) / sizeof(main_resolution_options[0])},
    {"main_codec", "stream.main", "codec", S_P_MAIN_CODEC, S_H_MAIN_CODEC,
     PARAM_ENUM, 0, 0, 0, replacement_codec_options, sizeof(replacement_codec_options) / sizeof(replacement_codec_options[0])},
    {"sub_resolution", "stream.sub", "resolution", S_P_SUB_RESOLUTION,
     TARGET_TEXT(S_H_SUB_RESOLUTION_MT11, S_H_SUB_RESOLUTION_A8),
     PARAM_ENUM, 0, 0, 0, sub_resolution_options, sizeof(sub_resolution_options) / sizeof(sub_resolution_options[0])},
    {"sub_codec", "stream.sub", "codec", S_P_SUB_CODEC, S_H_SUB_CODEC,
     PARAM_ENUM, 0, 0, 0, replacement_codec_options, sizeof(replacement_codec_options) / sizeof(replacement_codec_options[0])},
    {"brightness", "image", "brightness", S_P_BRIGHTNESS,
     TARGET_TEXT(S_H_BRIGHTNESS_MT11, S_H_CARDV_BRIGHTNESS),
     PARAM_INTEGER, 0, 100, 1, NULL, 0},
    {"saturation", "image", "saturation", S_P_SATURATION,
     TARGET_TEXT(S_H_SATURATION_MT11, S_H_CARDV_SATURATION),
     PARAM_INTEGER, 0, 100, 1, NULL, 0},
    {"contrast", "image", "contrast", S_P_CONTRAST,
     TARGET_TEXT(S_H_CONTRAST_MT11, S_H_CARDV_CONTRAST),
     PARAM_INTEGER, 0, 100, 1, NULL, 0},
    {"exposure_compensation", "image", "exposure_compensation",
     S_P_EXPOSURE_COMP, S_H_EXPOSURE_COMP_APP,
     PARAM_INTEGER, -10, 10, 1, NULL, 0},
    {"iso", "image", "iso", S_P_ISO, S_H_ISO_APP,
     PARAM_ENUM, 0, 0, 0, replacement_iso_options, 7},
    {"shutter", "image", "shutter", S_P_SHUTTER, S_H_SHUTTER_APP,
     PARAM_ENUM, 0, 0, 0, replacement_shutter_options, 9},
    {"metering", "image", "metering", S_P_METERING, S_H_METERING_APP,
     PARAM_ENUM, 0, 0, 0, replacement_metering_options, 3},
    {"white_balance", "image", "white_balance", S_P_WHITE_BALANCE, S_H_WB_APP,
     PARAM_ENUM, 0, 0, 0, replacement_wb_options, 5},
    {"proxy_enabled", "support_proxy", "enabled", S_P_PROXY_ENABLED, S_H_PROXY_ENABLED,
     PARAM_ENUM, 0, 0, 1, replacement_boolean_options, 2},
    {"proxy_host", "support_proxy", "host", S_P_PROXY_HOST, S_H_PROXY_HOST,
     PARAM_TEXT, 0, 127, 1, NULL, 0},
    {"proxy_mavlink_port", "support_proxy", "mavlink_port", S_P_PROXY_MAVLINK_PORT, S_H_PROXY_MAVLINK_PORT,
     PARAM_INTEGER, 0, 65535, 1, NULL, 0},
    {"proxy_signing", "support_proxy", "signing", S_P_PROXY_SIGNING, S_H_PROXY_SIGNING,
     PARAM_ENUM, 0, 0, 1, replacement_boolean_options, 2},
    {"proxy_signing_passphrase", "support_proxy", "signing_passphrase", S_P_PROXY_SIGNING_PASSPHRASE, S_H_PROXY_SIGNING_PASSPHRASE,
     PARAM_PASSWORD, 0, 127, 1, NULL, 0},
    {"proxy_signing_link_id", "support_proxy", "signing_link_id", S_P_PROXY_SIGNING_LINK_ID, S_H_PROXY_SIGNING_LINK_ID,
     PARAM_INTEGER, 0, 255, 1, NULL, 0},
    {"proxy_video1_port", "support_proxy", "video1_port", S_P_PROXY_VIDEO1_PORT, S_H_PROXY_VIDEO1_PORT,
     PARAM_INTEGER, 0, 65535, 1, NULL, 0},
    {"proxy_video1_name", "support_proxy", "video1_name", S_P_PROXY_VIDEO1_NAME, S_H_PROXY_VIDEO1_NAME,
     PARAM_TEXT, 0, 63, 1, NULL, 0},
    {"proxy_video2_port", "support_proxy", "video2_port", S_P_PROXY_VIDEO2_PORT, S_H_PROXY_VIDEO2_PORT,
     PARAM_INTEGER, 0, 65535, 1, NULL, 0},
    {"proxy_video2_name", "support_proxy", "video2_name", S_P_PROXY_VIDEO2_NAME, S_H_PROXY_VIDEO2_NAME,
     PARAM_TEXT, 0, 63, 1, NULL, 0},
    {"proxy_publish_password", "support_proxy", "publish_password", S_P_PROXY_PUBLISH_PASSWORD, S_H_PROXY_PUBLISH_PASSWORD,
     PARAM_PASSWORD, 0, 127, 1, NULL, 0},
    {"network_interface", "network", "interface", S_P_NETWORK_INTERFACE, S_H_NETWORK_INTERFACE,
     PARAM_TEXT, 1, 15, 1, NULL, 0},
    {"network_primary_address", "network", "primary_address", S_P_NETWORK_PRIMARY, S_H_NETWORK_PRIMARY,
     PARAM_TEXT, 0, 31, 1, NULL, 0},
    {"network_secondary_address", "network", "secondary_address", S_P_NETWORK_ADDRESS, S_H_NETWORK_ADDRESS,
     PARAM_TEXT, 0, 31, 1, NULL, 0},
    {"network_gateway", "network", "gateway", S_P_NETWORK_GATEWAY, S_H_NETWORK_GATEWAY,
     PARAM_TEXT, 0, 15, 1, NULL, 0},
};

enum parameter_tab { TAB_SYSTEM, TAB_NETWORK, TAB_VIDEO, TAB_PROXY, TAB_COUNT };
static const struct {
    const char *name;
    enum string_id label;
} parameter_tabs[TAB_COUNT] = {
    {"system", S_PARAMS_SYSTEM}, {"network", S_PARAMS_NETWORK},
    {"video", S_PARAMS_VIDEO}, {"supportproxy", S_PARAMS_PROXY},
};

static enum parameter_tab parameter_tab(const struct parameter *p)
{
    if (!strcmp(p->section, "network") ||
        !strcmp(p->form_name, "mavlink_tcp_port") || !strcmp(p->form_name, "mavlink_udp_port"))
        return TAB_NETWORK;
    if (!strcmp(p->section, "support_proxy")) return TAB_PROXY;
    if (!strcmp(p->section, "capture") || !strcmp(p->section, "recording") ||
        !strncmp(p->section, "stream.", 7) || !strcmp(p->section, "image") ||
        !strcmp(p->section, "thermal")) return TAB_VIDEO;
    return TAB_SYSTEM;
}

static const char *replacement_defaults[] = {
    APCAM_DEFAULT_TIMEZONE, APCAM_DEFAULT_PHOTO_SCOPE == 0 ? "thermal" : "all",
    APCAM_DEFAULT_ORIENTATION == 0 ? "auto" : APCAM_DEFAULT_ORIENTATION == 1 ? "upright" : "inverted",
    "none", APCAM_STRING_VALUE(APCAM_DEFAULT_SYSTEM_ID), "100", "14550", "14550",
    APCAM_DEFAULT_POSITION_TARGETING ? "true" : "false", "false", "angle", "white_hot", "false",
    APCAM_RESOLUTION_NAME(APCAM_DEFAULT_RECORDING_RESOLUTION),
    APCAM_RESOLUTION_NAME(APCAM_DEFAULT_MAIN_RESOLUTION), "h264",
    APCAM_RESOLUTION_NAME(APCAM_DEFAULT_SUB_RESOLUTION), "h264", "50", "50", "50", "0",
    "auto", "auto", "average", "auto",
    "false", "", "10001", "false", "", "1", "0", "video1", "0", "video2", "", "eth0", "", "", "",
};


static void log_message(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static void log_message(const char *fmt, ...)
{
    char timestamp[64];
    time_t now = time(NULL);
    struct tm tm_now;
    va_list ap;

    localtime_r(&now, &tm_now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_now);
    fprintf(stderr, "[%s] ", timestamp);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void sb_init(struct string_buffer *sb)
{
    memset(sb, 0, sizeof(*sb));
}

static bool sb_reserve(struct string_buffer *sb, size_t extra)
{
    size_t needed;
    size_t new_cap;
    char *new_data;

    if (extra > SIZE_MAX - sb->len - 1) {
        return false;
    }
    needed = sb->len + extra + 1;
    if (needed <= sb->cap) {
        return true;
    }
    new_cap = sb->cap ? sb->cap : 4096;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }
    new_data = realloc(sb->data, new_cap);
    if (new_data == NULL) {
        return false;
    }
    sb->data = new_data;
    sb->cap = new_cap;
    return true;
}

static bool sb_append_n(struct string_buffer *sb, const char *text, size_t len)
{
    if (!sb_reserve(sb, len)) {
        return false;
    }
    memcpy(sb->data + sb->len, text, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
    return true;
}

static bool sb_append(struct string_buffer *sb, const char *text)
{
    return sb_append_n(sb, text, strlen(text));
}

static bool sb_vappendf(struct string_buffer *sb, const char *fmt, va_list ap)
{
    va_list copy;
    int length;

    va_copy(copy, ap);
    length = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (length < 0 || !sb_reserve(sb, (size_t)length)) return false;
    vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap);
    sb->len += (size_t)length;
    return true;
}

static bool sb_appendf(struct string_buffer *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static bool sb_appendf(struct string_buffer *sb, const char *fmt, ...)
{
    va_list ap;
    bool ok;

    va_start(ap, fmt);
    ok = sb_vappendf(sb, fmt, ap);
    va_end(ap);
    return ok;
}

/* length of text without a trailing incomplete UTF-8 sequence, so a
 * message clipped by a fixed buffer never ends in a broken character */
static size_t utf8_complete_length(const char *text, size_t length)
{
    size_t start = length;
    unsigned expected;

    while (start > 0 && length - start < 4 &&
           ((unsigned char)text[start - 1] & 0xc0U) == 0x80U) {
        start--;
    }
    if (start == 0) return length;
    unsigned char lead = (unsigned char)text[start - 1];
    if (lead < 0x80U) return length;
    expected = lead >= 0xf0U ? 4U : lead >= 0xe0U ? 3U : lead >= 0xc0U ? 2U : 1U;
    return length - start + 1 >= expected ? length : start - 1;
}

static bool sb_append_html(struct string_buffer *sb, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    while (*p != '\0') {
        switch (*p) {
        case '&':
            if (!sb_append(sb, "&amp;")) return false;
            break;
        case '<':
            if (!sb_append(sb, "&lt;")) return false;
            break;
        case '>':
            if (!sb_append(sb, "&gt;")) return false;
            break;
        case '"':
            if (!sb_append(sb, "&quot;")) return false;
            break;
        case '\'':
            if (!sb_append(sb, "&#39;")) return false;
            break;
        default:
            if (*p >= 0x20 || *p == '\n' || *p == '\r' || *p == '\t') {
                if (!sb_append_n(sb, (const char *)p, 1)) return false;
            }
            break;
        }
        p++;
    }
    return true;
}

#if WEB_HAVE_SSH_KEYS
static bool sb_append_html_n(struct string_buffer *sb, const char *text,
                             size_t length)
{
    char *copy = malloc(length + 1);
    bool ok;

    if (copy == NULL) return false;
    memcpy(copy, text, length);
    copy[length] = '\0';
    ok = sb_append_html(sb, copy);
    free(copy);
    return ok;
}
#endif

static bool sb_append_url(struct string_buffer *sb, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char *p = (const unsigned char *)text;

    while (*p != '\0') {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            if (!sb_append_n(sb, (const char *)p, 1)) return false;
        } else {
            char encoded[3] = {'%', hex[*p >> 4], hex[*p & 15]};
            if (!sb_append_n(sb, encoded, sizeof(encoded))) return false;
        }
        p++;
    }
    return true;
}

static bool sb_append_log_html(struct string_buffer *sb, const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        unsigned char value = (unsigned char)text[i];
        if (value == 0x1b && i + 1 < length && text[i + 1] == '[') {
            i += 2;
            while (i < length) {
                value = (unsigned char)text[i];
                if (value >= 0x40 && value <= 0x7e) break;
                i++;
            }
            continue;
        }
        if (value == '\r') continue;
        if (value == '&') {
            if (!sb_append(sb, "&amp;")) return false;
        } else if (value == '<') {
            if (!sb_append(sb, "&lt;")) return false;
        } else if (value == '>') {
            if (!sb_append(sb, "&gt;")) return false;
        } else if (value >= 0x20 || value == '\n' || value == '\t') {
            if (!sb_append_n(sb, (const char *)&text[i], 1)) return false;
        }
    }
    return true;
}

static bool send_all(int fd, const void *buffer, size_t length)
{
    const char *p = buffer;

    while (length > 0) {
        ssize_t sent = send(fd, p, length, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (sent == 0) return false;
        p += sent;
        length -= (size_t)sent;
    }
    return true;
}

static void send_text_error(int fd, int status, const char *reason,
                            const char *message, const char *extra_headers);
static void send_text_errorf(int fd, int status, const char *reason,
                             enum string_id id, ...);

static void send_response(int fd, int status, const char *reason,
                          const char *content_type, const char *body,
                          size_t body_len, const char *extra_headers)
{
    struct string_buffer header;

    sb_init(&header);
    if (sb_appendf(&header,
        "HTTP/1.1 %d %s\r\n"
        "Server: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; "
        "img-src 'self' https://firmware.ardupilot.org; media-src 'self'; "
        "script-src 'self'; connect-src 'self'; form-action 'self'; "
        "frame-ancestors 'none'\r\n"
        "%s\r\n",
        status, reason, SERVER_NAME, content_type, body_len,
        extra_headers ? extra_headers : "") &&
        send_all(fd, header.data, header.len) && body_len > 0) {
        (void)send_all(fd, body, body_len);
    }
    free(header.data);
}

#if APCAM_TARGET == APCAM_TARGET_ZR10
#ifndef __CYGWIN__
#include <sys/syscall.h>

/* The installed uClibc predates renameat2, but the 4.9 kernel supports it. */
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
static int renameat2(int olddir, const char *oldname, int newdir,
                     const char *newname, unsigned flags)
{
    return (int)syscall(SYS_renameat2, olddir, oldname, newdir, newname, flags);
}
#endif

#endif /* !__CYGWIN__ */

#endif

#if !WEB_INSTALLS_FIRMWARE
/* Publishing an uploaded file must never replace an existing file. */
static int publish_no_replace(int directory, const char *temporary, const char *published)
{
#ifdef __CYGWIN__
    if (linkat(directory, temporary, directory, published, 0) < 0) return -1;
    (void)unlinkat(directory, temporary, 0);
    return 0;
#else
    return renameat2(directory, temporary, directory, published, RENAME_NOREPLACE);
#endif
}
#endif /* !WEB_INSTALLS_FIRMWARE */

static unsigned live_video_port(void)
{
#ifdef MT11_WEB_TEST
    const char *text = getenv("MT11_WEB_LIVE_PORT");
    char *end;
    unsigned long value;

    if (text != NULL && *text != '\0') {
        errno = 0;
        value = strtoul(text, &end, 10);
        if (errno == 0 && *end == '\0' && value > 0U && value <= 65535U) {
            return (unsigned)value;
        }
    }
#endif
    return LIVE_VIDEO_PORT;
}

static void relay_live_video(int fd, unsigned stream)
{
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)live_video_port()),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
    uint8_t status = (uint8_t)stream;
    uint8_t buffer[64U * 1024U];
    int source = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    static const char header[] =
        "HTTP/1.1 200 OK\r\n"
        "Server: " SERVER_NAME "\r\n"
        "Content-Type: video/mp4\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Content-Security-Policy: default-src 'none'; frame-ancestors 'none'\r\n"
        "\r\n";

    if (source < 0 ||
        setsockopt(source, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) < 0 ||
        connect(source, (const struct sockaddr *)&address, sizeof(address)) < 0 ||
        send(source, &status, 1U, MSG_NOSIGNAL) != 1 ||
        recv(source, &status, 1U, MSG_WAITALL) != 1) {
        if (source >= 0) close(source);
        send_text_errorf(fd, 503, "Service Unavailable", S_E_LIVE_UNAVAILABLE);
        return;
    }
    if (status != 0U) {
        close(source);
        send_text_errorf(fd, 415, "Unsupported Media Type", S_E_LIVE_H264);
        return;
    }
    /* Deliver each frame's final chunk without waiting for a delayed ACK. */
    int no_delay = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
    if (!send_all(fd, header, sizeof(header) - 1U)) {
        close(source);
        return;
    }
    for (;;) {
        ssize_t got = recv(source, buffer, sizeof(buffer), 0);
        if (got > 0) {
            if (!send_all(fd, buffer, (size_t)got)) break;
        } else if (got < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    close(source);
}

static void send_text_error(int fd, int status, const char *reason,
                            const char *message, const char *extra_headers)
{
    send_response(fd, status, reason, "text/plain; charset=utf-8", message,
                  utf8_complete_length(message, strlen(message)), extra_headers);
}

/* plain-text reply built from a translated format, newline terminated */
static void send_text_errorf(int fd, int status, const char *reason,
                             enum string_id id, ...)
{
    struct string_buffer body;
    va_list ap;
    bool ok;

    sb_init(&body);
    va_start(ap, id);
    ok = sb_vappendf(&body, T(id), ap) && sb_append(&body, "\n");
    va_end(ap);
    send_text_error(fd, status, reason, ok ? body.data : "\n", NULL);
    free(body.data);
}

static void send_redirect_headers(int fd, const char *location,
                                  const char *extra_headers)
{
    struct string_buffer headers;

    sb_init(&headers);
    if (sb_appendf(&headers, "%sLocation: %s\r\n",
                   extra_headers ? extra_headers : "", location)) {
        send_response(fd, 303, "See Other", "text/plain; charset=utf-8",
                      "", 0, headers.data);
    } else {
        send_text_errorf(fd, 500, "Internal Server Error", S_OUT_OF_MEMORY);
    }
    free(headers.data);
}

static void send_redirect(int fd, const char *location)
{
    send_redirect_headers(fd, location, NULL);
}

static char *read_file(const char *path, size_t limit, size_t *length)
{
    int fd;
    struct stat st;
    char *buffer;
    size_t used = 0;

    *length = 0;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    if (fstat(fd, &st) < 0 || st.st_size < 0 || (uint64_t)st.st_size > limit) {
        close(fd);
        errno = EFBIG;
        return NULL;
    }
    buffer = malloc((size_t)st.st_size + 1);
    if (buffer == NULL) {
        close(fd);
        return NULL;
    }
    while (used < (size_t)st.st_size) {
        ssize_t got = read(fd, buffer + used, (size_t)st.st_size - used);
        if (got < 0) {
            if (errno == EINTR) continue;
            free(buffer);
            close(fd);
            return NULL;
        }
        if (got == 0) break;
        used += (size_t)got;
    }
    close(fd);
    buffer[used] = '\0';
    *length = used;
    return buffer;
}

static char *read_file_tail(const char *path, size_t limit, size_t *length)
{
    int fd;
    struct stat st;
    off_t start = 0;
    size_t wanted;
    size_t used = 0;
    char *buffer;

    *length = 0;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    if (fstat(fd, &st) < 0 || st.st_size < 0) {
        close(fd);
        return NULL;
    }
    if ((uint64_t)st.st_size > limit) start = st.st_size - (off_t)limit;
    wanted = (size_t)(st.st_size - start);
    if (lseek(fd, start, SEEK_SET) < 0) {
        close(fd);
        return NULL;
    }
    buffer = malloc(wanted + 1);
    if (buffer == NULL) {
        close(fd);
        return NULL;
    }
    while (used < wanted) {
        ssize_t got = read(fd, buffer + used, wanted - used);
        if (got < 0) {
            if (errno == EINTR) continue;
            free(buffer);
            close(fd);
            return NULL;
        }
        if (got == 0) break;
        used += (size_t)got;
    }
    close(fd);
    buffer[used] = '\0';
    *length = used;
    return buffer;
}

static char *read_app_log_paths(const char *path, const char *old_path,
                                size_t *length)
{
    size_t current_len = 0;
    size_t old_len = 0;
    char *current = read_file_tail(path, APP_LOG_DISPLAY_SIZE, &current_len);
    char *old = NULL;
    char *combined;

    if (current_len < APP_LOG_DISPLAY_SIZE) {
        old = read_file_tail(old_path,
                             APP_LOG_DISPLAY_SIZE - current_len, &old_len);
    }
    if (current == NULL && old == NULL) {
        *length = 0;
        return strdup("");
    }
    combined = malloc(old_len + current_len + 1);
    if (combined == NULL) {
        free(old);
        free(current);
        *length = 0;
        return NULL;
    }
    if (old_len > 0) memcpy(combined, old, old_len);
    if (current_len > 0) memcpy(combined + old_len, current, current_len);
    combined[old_len + current_len] = '\0';
    *length = old_len + current_len;
    free(old);
    free(current);
    return combined;
}

static bool random_token(char output[65])
{
    unsigned char bytes[32];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    size_t used = 0;

    if (fd < 0) return false;
    while (used < sizeof(bytes)) {
        ssize_t got = read(fd, bytes + used, sizeof(bytes) - used);
        if (got < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        if (got == 0) {
            close(fd);
            return false;
        }
        used += (size_t)got;
    }
    close(fd);
    for (size_t i = 0; i < sizeof(bytes); i++) {
        snprintf(output + i * 2, 3, "%02x", bytes[i]);
    }
    output[64] = '\0';
    return true;
}

/* trimmed value of the first header called name, or NULL when absent */
static const char *find_header_span(const struct request *request,
                                    const char *name, size_t *length)
{
    const char *p = request->storage;
    const char *end = request->storage + request->header_len;
    size_t name_len = strlen(name);

    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (line_end == NULL) line_end = end;
        if ((size_t)(line_end - p) > name_len + 1 &&
            strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
            const char *value_start = p + name_len + 1;
            const char *value_end;
            while (value_start < line_end && isspace((unsigned char)*value_start)) {
                value_start++;
            }
            value_end = line_end;
            while (value_end > value_start && isspace((unsigned char)value_end[-1])) {
                value_end--;
            }
            *length = (size_t)(value_end - value_start);
            return value_start;
        }
        p = line_end + (line_end < end ? 1 : 0);
    }
    return NULL;
}

static const char *find_header(const struct request *request, const char *name,
                               char *value, size_t value_size)
{
    size_t length;
    const char *start = find_header_span(request, name, &length);

    if (start == NULL || length >= value_size) return NULL;
    memcpy(value, start, length);
    value[length] = '\0';
    return value;
}

static bool parse_content_length(const char *headers, size_t header_len,
                                 size_t *content_length, bool *present)
{
    struct request temporary = {.storage = (char *)headers, .header_len = header_len};
    char value[64];
    char *end;
    unsigned long long parsed;

    *content_length = 0;
    *present = false;
    if (find_header(&temporary, "Content-Length", value, sizeof(value)) == NULL) {
        return true;
    }
    *present = true;
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || *value == '\0' || *end != '\0' || parsed > SIZE_MAX) {
        return false;
    }
    *content_length = (size_t)parsed;
    return true;
}

static int receive_request(int fd, struct request *request)
{
    size_t capacity = MAX_HEADER + 1;
    size_t used = 0;
    size_t header_len = 0;
    size_t content_length = 0;
    bool content_length_present = false;
    char *storage = calloc(1, capacity);

    if (storage == NULL) return 500;
    while (used < MAX_HEADER) {
        ssize_t got = recv(fd, storage + used, capacity - used - 1, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            free(storage);
            return RECEIVE_INCOMPLETE;
        }
        if (got == 0) break;
        used += (size_t)got;
        storage[used] = '\0';
        char *boundary = strstr(storage, "\r\n\r\n");
        if (boundary != NULL) {
            header_len = (size_t)(boundary + 4 - storage);
            if (header_len > MAX_HEADER) {
                free(storage);
                return 431;
            }
            break;
        }
    }
    if (header_len == 0) {
        free(storage);
        return used >= MAX_HEADER ? 431 : RECEIVE_INCOMPLETE;
    }
    if (sscanf(storage, "%11s %4095s", request->method, request->path) != 2) {
        free(storage);
        return 400;
    }
    char *query = strchr(request->path, '?');
    if (query != NULL) {
        snprintf(request->query, sizeof(request->query), "%s", query + 1);
        *query = '\0';
    }
    if (!parse_content_length(storage, header_len, &content_length,
                              &content_length_present)) {
        free(storage);
        return 400;
    }
    char transfer_encoding[64];
    if (find_header(&(struct request){.storage = storage, .header_len = header_len},
                    "Transfer-Encoding", transfer_encoding,
                    sizeof(transfer_encoding)) != NULL) {
        free(storage);
        return 400;
    }
    request->streaming_body = strcmp(request->method, "POST") == 0 &&
                              strcmp(request->path, "/upgrade") == 0;
    request->content_length = content_length;
    request->storage = storage;
    request->header_len = header_len;
    request->body = storage + header_len;
    request->body_len = used - header_len;

    if (request->body_len > content_length ||
        ((strcmp(request->method, "POST") == 0 ||
          strcmp(request->method, "PUT") == 0) && !content_length_present)) {
        free(storage);
        memset(request, 0, sizeof(*request));
        return 400;
    }
    if (request->streaming_body) return 0;
    if (content_length > MAX_BODY) {
        free(storage);
        memset(request, 0, sizeof(*request));
        return 413;
    }
    if (header_len + content_length + 1 > capacity) {
        char *expanded = realloc(storage, header_len + content_length + 1);
        if (expanded == NULL) {
            free(storage);
            memset(request, 0, sizeof(*request));
            return 500;
        }
        storage = expanded;
        capacity = header_len + content_length + 1;
        request->storage = storage;
        request->body = storage + header_len;
    }
    while (used < header_len + content_length) {
        ssize_t got = recv(fd, storage + used, capacity - used - 1, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            free(storage);
            memset(request, 0, sizeof(*request));
            return RECEIVE_INCOMPLETE;
        }
        if (got == 0) {
            free(storage);
            memset(request, 0, sizeof(*request));
            return RECEIVE_INCOMPLETE;
        }
        used += (size_t)got;
    }
    request->body_len = content_length;
    request->body[content_length] = '\0';
    return 0;
}

static int b64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool decode_base64(const char *input, unsigned char *output,
                          size_t output_size, size_t *output_len)
{
    unsigned accumulator = 0;
    unsigned bits = 0;
    size_t used = 0;

    for (const unsigned char *p = (const unsigned char *)input; *p != '\0'; p++) {
        int value;
        if (*p == '=') break;
        if (isspace(*p)) continue;
        value = b64_value(*p);
        if (value < 0) return false;
        accumulator = (accumulator << 6) | (unsigned)value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (used + 1 >= output_size) return false;
            output[used++] = (unsigned char)((accumulator >> bits) & 0xff);
        }
    }
    output[used] = '\0';
    *output_len = used;
    return true;
}

static bool constant_time_equal(const char *a, size_t a_len,
                                const char *b, size_t b_len)
{
    size_t max_len = a_len > b_len ? a_len : b_len;
    unsigned difference = (unsigned)(a_len ^ b_len);

    for (size_t i = 0; i < max_len; i++) {
        unsigned char av = i < a_len ? (unsigned char)a[i] : 0;
        unsigned char bv = i < b_len ? (unsigned char)b[i] : 0;
        difference |= av ^ bv;
    }
    return difference == 0;
}


static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* -1: password file missing or empty; 0: wrong; 1: matches admin */
static int check_password(const char *user, size_t user_len,
                          const char *candidate, size_t candidate_len)
{
    size_t password_len;
    char *password = read_file(PASSWORD_PATH, 512, &password_len);
    bool valid;

    if (password == NULL) return -1;
    while (password_len > 0 &&
           (password[password_len - 1] == '\n' || password[password_len - 1] == '\r')) {
        password[--password_len] = '\0';
    }
    if (password_len == 0) {
        free(password);
        return -1;
    }
    valid = user_len == 5 && memcmp(user, "admin", 5) == 0 &&
            constant_time_equal(candidate, candidate_len, password, password_len);
    memset(password, 0, password_len);
    free(password);
    return valid ? 1 : 0;
}

static int authenticate_basic(const struct request *request)
{
    char authorization[1024];
    unsigned char decoded[768];
    size_t decoded_len;
    char *colon;

    if (find_header(request, "Authorization", authorization,
                    sizeof(authorization)) == NULL ||
        strncasecmp(authorization, "Basic ", 6) != 0 ||
        !decode_base64(authorization + 6, decoded, sizeof(decoded), &decoded_len)) {
        return 0;
    }
    colon = memchr(decoded, ':', decoded_len);
    if (colon == NULL) return 0;
    return check_password((const char *)decoded, (size_t)(colon - (char *)decoded),
                          colon + 1,
                          decoded_len - (size_t)(colon + 1 - (char *)decoded));
}

static bool cookie_value(const struct request *request, const char *name,
                         char *value, size_t value_size)
{
    char cookies[4096];
    const char *p;
    size_t name_len = strlen(name);

    if (find_header(request, "Cookie", cookies, sizeof(cookies)) == NULL) {
        return false;
    }
    for (p = cookies; *p != '\0';) {
        const char *end = strchr(p, ';');
        const char *equals;
        size_t length;

        if (end == NULL) end = p + strlen(p);
        while (p < end && *p == ' ') p++;
        equals = memchr(p, '=', (size_t)(end - p));
        if (equals != NULL && (size_t)(equals - p) == name_len &&
            memcmp(p, name, name_len) == 0) {
            length = (size_t)(end - equals - 1);
            if (length >= value_size) return false;
            memcpy(value, equals + 1, length);
            value[length] = '\0';
            return true;
        }
        p = *end == ';' ? end + 1 : end;
    }
    return false;
}

/* 64 lower-case hex digits, the form random_token() produces */
static bool valid_token_text(const char *text)
{
    if (strlen(text) != 64) return false;
    for (size_t i = 0; i < 64; i++) {
        if (hex_value(text[i]) < 0 || isupper((unsigned char)text[i])) return false;
    }
    return true;
}

static uint64_t monotonic_seconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0U;
    return (uint64_t)now.tv_sec;
}

/* Login sessions are shared between the forked HTTP workers through a
 * small file of "token expiry" lines, read and rewritten in place under
 * flock on the file itself. Expiry uses CLOCK_MONOTONIC so a clock change
 * made from the Status page cannot log everyone out; the file is in RAM,
 * so a reboot drops all sessions anyway. */
struct session_entry {
    char token[65];
    uint64_t expiry;
};

/* the file must be the server's own regular, private file: a file another
 * local user planted at the predictable path must not supply sessions */
static int open_session_file(int operation)
{
    struct stat st;
    int fd = open(SESSION_PATH, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);

    if (fd < 0) return -1;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077) != 0 || st.st_nlink != 1 ||
        flock(fd, operation) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* The first line names the boot the monotonic expiries belong to, so a
 * file that survives a reboot (SITL, host tests) is not trusted. procfs
 * reports a zero size, so this reads until EOF; no sessions are issued or
 * accepted without a boot id. */
static bool read_boot_id(char output[64])
{
#ifdef WEB_PORTABLE_SITL
    /* Cygwin has no Linux boot_id file. A random web-server incarnation is
     * inherited by request workers and invalidates persisted sessions on
     * every server restart, including after a Windows reboot. */
    memcpy(output, session_epoch, 32);
    output[32] = '\0';
    return output[0] != '\0';
#else
    int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    size_t used = 0;

    output[0] = '\0';
    if (fd < 0) return false;
    while (used < 63) {
        ssize_t got = read(fd, output + used, 63 - used);
        if (got < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        if (got == 0) break;
        used += (size_t)got;
    }
    close(fd);
    output[used] = '\0';
    /* the kernel writes 36 characters and a newline; anything else is junk */
    if (used == 63 || used < 33 || output[used - 1] != '\n') return false;
    output[used - 1] = '\0';
    for (const char *p = output; *p != '\0'; p++) {
        if (hex_value(*p) < 0 && *p != '-') return false;
    }
    return true;
#endif
}

/* false on a read error; entries then must not be rewritten */
static bool load_sessions(int fd, struct session_entry *entries, size_t capacity,
                          size_t *count)
{
    char contents[MAX_SESSION_FILE + 1];
    char boot_id[64];
    char expected[80];
    struct stat st;
    size_t used = 0;
    uint64_t now = monotonic_seconds();
    char *line;
    char *next;

    *count = 0;
    if (fstat(fd, &st) < 0 || st.st_size > (off_t)MAX_SESSION_FILE) return false;
    while (used < MAX_SESSION_FILE) {
        ssize_t got = pread(fd, contents + used, MAX_SESSION_FILE - used, (off_t)used);
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (got == 0) break;
        used += (size_t)got;
    }
    contents[used] = '\0';
    if (!read_boot_id(boot_id)) return false;
    snprintf(expected, sizeof(expected), "boot %s", boot_id);
    line = contents;
    next = strchr(line, '\n');
    if (next == NULL) return true;
    *next++ = '\0';
    if (strcmp(line, expected) != 0) return true;
    for (line = next; line != NULL && *line != '\0' && *count < capacity; line = next) {
        char *space;
        char *end;
        unsigned long long expiry;

        next = strchr(line, '\n');
        if (next == NULL) break;
        *next++ = '\0';
        space = strchr(line, ' ');
        if (space == NULL) continue;
        *space = '\0';
        if (!valid_token_text(line) || !isdigit((unsigned char)space[1])) continue;
        errno = 0;
        expiry = strtoull(space + 1, &end, 10);
        if (errno != 0 || *end != '\0' || expiry <= now) continue;
        memcpy(entries[*count].token, line, sizeof(entries[*count].token));
        entries[*count].expiry = expiry;
        (*count)++;
    }
    return true;
}

static bool store_sessions(int fd, const struct session_entry *entries, size_t count)
{
    char contents[MAX_SESSION_FILE + 1];
    char boot_id[64];
    size_t used;
    int length;

    if (!read_boot_id(boot_id)) return false;
    length = snprintf(contents, sizeof(contents), "boot %s\n", boot_id);
    if (length <= 0 || (size_t)length >= sizeof(contents)) return false;
    used = (size_t)length;
    for (size_t i = 0; i < count; i++) {
        length = snprintf(contents + used, sizeof(contents) - used, "%s %llu\n",
                          entries[i].token, (unsigned long long)entries[i].expiry);
        if (length <= 0 || (size_t)length >= sizeof(contents) - used) return false;
        used += (size_t)length;
    }
    if (ftruncate(fd, 0) < 0) return false;
    for (size_t offset = 0; offset < used;) {
        ssize_t written = pwrite(fd, contents + offset, used - offset, (off_t)offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        offset += (size_t)written;
    }
    return fsync(fd) == 0;
}

static bool create_session(char token[65])
{
    struct session_entry entries[MAX_SESSIONS];
    size_t count;
    int fd;
    bool ok;

    if (!random_token(token)) return false;
    fd = open_session_file(LOCK_EX);
    if (fd < 0) return false;
    if (!load_sessions(fd, entries, MAX_SESSIONS, &count)) {
        close(fd);
        return false;
    }
    if (count == MAX_SESSIONS) {
        /* entries are in login order; drop the oldest */
        memmove(entries, entries + 1, (count - 1) * sizeof(entries[0]));
        count--;
    }
    memcpy(entries[count].token, token, 65);
    entries[count].expiry = monotonic_seconds() + SESSION_LIFETIME_SECONDS;
    count++;
    ok = store_sessions(fd, entries, count);
    close(fd);
    return ok;
}

static bool session_valid(const char *token)
{
    struct session_entry entries[MAX_SESSIONS];
    size_t count;
    int fd;
    bool found = false;

    if (!valid_token_text(token)) return false;
    fd = open_session_file(LOCK_SH);
    if (fd < 0) return false;
    if (!load_sessions(fd, entries, MAX_SESSIONS, &count)) count = 0;
    close(fd);
    for (size_t i = 0; i < count; i++) {
        if (constant_time_equal(entries[i].token, 64, token, 64)) found = true;
    }
    return found;
}

/* true once the token is certainly gone from the file */
static bool destroy_session(const char *token)
{
    struct session_entry entries[MAX_SESSIONS];
    size_t count;
    size_t kept = 0;
    int fd;
    bool ok;

    if (!valid_token_text(token)) return true;
    fd = open_session_file(LOCK_EX);
    if (fd < 0) return false;
    if (!load_sessions(fd, entries, MAX_SESSIONS, &count)) {
        close(fd);
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].token, token) != 0) entries[kept++] = entries[i];
    }
    ok = kept == count || store_sessions(fd, entries, kept);
    close(fd);
    return ok;
}

/* -1: no usable password file; 0: unauthenticated; 1: HTTP Basic;
 * 2: session cookie, copied to session_token. An Authorization header is
 * always judged on its own so scripted clients never fall back to a
 * cookie. */
static int authenticate(const struct request *request, char session_token[65])
{
    size_t length;
    size_t password_len;
    char *password = read_file(PASSWORD_PATH, 512, &password_len);

    session_token[0] = '\0';
    if (password == NULL) return -1;
    while (password_len > 0 &&
           (password[password_len - 1] == '\n' || password[password_len - 1] == '\r')) {
        password_len--;
    }
    free(password);
    if (password_len == 0) return -1;
    if (find_header_span(request, "Authorization", &length) != NULL) {
        return authenticate_basic(request) == 1 ? 1 : 0;
    }
    if (cookie_value(request, "session", session_token, 65) &&
        session_valid(session_token)) {
        return 2;
    }
    session_token[0] = '\0';
    return 0;
}

static char *url_decode(const char *input, size_t length, size_t *decoded_len)
{
    char *output = malloc(length + 1);
    size_t used = 0;

    if (output == NULL) return NULL;
    for (size_t i = 0; i < length; i++) {
        unsigned char value;
        if (input[i] == '+') {
            value = ' ';
        } else if (input[i] == '%' && i + 2 < length) {
            int high = hex_value(input[i + 1]);
            int low = hex_value(input[i + 2]);
            if (high < 0 || low < 0) {
                free(output);
                return NULL;
            }
            value = (unsigned char)((high << 4) | low);
            i += 2;
        } else {
            value = (unsigned char)input[i];
        }
        if (value == '\0') {
            free(output);
            return NULL;
        }
        output[used++] = (char)value;
    }
    output[used] = '\0';
    *decoded_len = used;
    return output;
}

static char *encoded_value(const char *source, size_t source_length,
                           const char *key, size_t *value_len)
{
    const char *p = source;
    const char *end = source + source_length;
    size_t key_len = strlen(key);

    while (p < end) {
        const char *pair_end = memchr(p, '&', (size_t)(end - p));
        const char *equals;
        if (pair_end == NULL) pair_end = end;
        equals = memchr(p, '=', (size_t)(pair_end - p));
        if (equals != NULL && (size_t)(equals - p) == key_len &&
            memcmp(p, key, key_len) == 0) {
            return url_decode(equals + 1, (size_t)(pair_end - equals - 1), value_len);
        }
        p = pair_end + (pair_end < end ? 1 : 0);
    }
    return NULL;
}

static char *form_value(const struct request *request, const char *key,
                        size_t *value_len)
{
    return encoded_value(request->body, request->body_len, key, value_len);
}

static char *query_value(const struct request *request, const char *key,
                         size_t *value_len)
{
    return encoded_value(request->query, strlen(request->query), key, value_len);
}

static bool valid_csrf(const struct request *request)
{
    size_t length;
    char *token = form_value(request, "csrf", &length);
    bool valid = token != NULL &&
                 constant_time_equal(token, length, csrf_token, strlen(csrf_token));
    free(token);
    return valid;
}

static bool valid_csrf_header(const struct request *request)
{
    char token[128];

    return find_header(request, "X-CSRF-Token", token, sizeof(token)) != NULL &&
           constant_time_equal(token, strlen(token),
                               csrf_token, strlen(csrf_token));
}

static void normalize_newlines(char *text, size_t *length)
{
    size_t source = 0;
    size_t dest = 0;

    while (source < *length) {
        if (text[source] == '\r') {
            if (source + 1 < *length && text[source + 1] == '\n') source++;
            text[dest++] = '\n';
        } else {
            text[dest++] = text[source];
        }
        source++;
    }
    text[dest] = '\0';
    *length = dest;
}

static bool validate_ini(const char *config, size_t length,
                         char *error, size_t error_size)
{
    size_t offset = 0;
    unsigned line_number = 0;
    unsigned sections = 0;
    unsigned assignments = 0;

    if (length == 0 || length > MAX_CONFIG) {
        snprintf(error, error_size, T(S_E_CONFIG_SIZE), MAX_CONFIG);
        return false;
    }
    while (offset < length) {
        size_t line_start = offset;
        size_t line_end;
        size_t start;
        size_t end;
        line_number++;
        while (offset < length && config[offset] != '\n') offset++;
        line_end = offset;
        if (offset < length) offset++;
        start = line_start;
        end = line_end;
        while (start < end && isspace((unsigned char)config[start])) start++;
        while (end > start && isspace((unsigned char)config[end - 1])) end--;
        if (start == end || config[start] == '#' || config[start] == ';') continue;
        if (config[start] == '[') {
            const char *close = memchr(config + start + 1, ']', end - start - 1);
            if (close == NULL) {
                snprintf(error, error_size, T(S_E_INI_UNTERMINATED_SECTION), line_number);
                return false;
            }
            for (const char *p = close + 1; p < config + end; p++) {
                if (!isspace((unsigned char)*p)) {
                    snprintf(error, error_size, T(S_E_INI_TEXT_AFTER_SECTION), line_number);
                    return false;
                }
            }
            sections++;
            continue;
        }
        const char *equals = memchr(config + start, '=', end - start);
        if (equals == NULL) {
            snprintf(error, error_size, T(S_E_INI_NOT_ASSIGNMENT), line_number);
            return false;
        }
        const char *key_end = equals;
        while (key_end > config + start && isspace((unsigned char)key_end[-1])) key_end--;
        if (key_end == config + start) {
            snprintf(error, error_size, T(S_E_INI_EMPTY_KEY), line_number);
            return false;
        }
        assignments++;
    }
    if (sections == 0 || assignments == 0) {
        snprintf(error, error_size, "%s", T(S_E_INI_NO_SECTION));
        return false;
    }
    return true;
}

static void trim_span(const char **start, const char **end)
{
    while (*start < *end && isspace((unsigned char)**start)) (*start)++;
    while (*end > *start && isspace((unsigned char)(*end)[-1])) (*end)--;
}

static bool span_equal(const char *start, const char *end, const char *text)
{
    size_t length = (size_t)(end - start);
    return strlen(text) == length && memcmp(start, text, length) == 0;
}

static bool ini_get_value(const char *config, const char *wanted_section,
                          const char *wanted_key, char *output, size_t output_size)
{
    const char *p = config;
    char section[80] = "";

    while (*p != '\0') {
        const char *line_end = strchr(p, '\n');
        const char *start = p;
        const char *end;
        if (line_end == NULL) line_end = p + strlen(p);
        end = line_end;
        trim_span(&start, &end);
        if (start < end && *start == '[') {
            const char *close = memchr(start + 1, ']', (size_t)(end - start - 1));
            if (close != NULL) {
                const char *name_start = start + 1;
                const char *name_end = close;
                size_t length;
                trim_span(&name_start, &name_end);
                length = (size_t)(name_end - name_start);
                if (length >= sizeof(section)) length = sizeof(section) - 1;
                memcpy(section, name_start, length);
                section[length] = '\0';
            }
        } else if (start < end && *start != '#' && *start != ';' &&
                   strcmp(section, wanted_section) == 0) {
            const char *equals = memchr(start, '=', (size_t)(end - start));
            if (equals != NULL) {
                const char *key_start = start;
                const char *key_end = equals;
                trim_span(&key_start, &key_end);
                if (span_equal(key_start, key_end, wanted_key)) {
                    const char *value_start = equals + 1;
                    const char *value_end = end;
                    size_t length;
                    trim_span(&value_start, &value_end);
                    if (value_end > value_start + 1 && *value_start == '"' &&
                        value_end[-1] == '"') {
                        value_start++;
                        value_end--;
                        if (strcmp(section, "support_proxy") != 0)
                            trim_span(&value_start, &value_end);
                    }
                    length = (size_t)(value_end - value_start);
                    if (length >= output_size) return false;
                    memcpy(output, value_start, length);
                    output[length] = '\0';
                    return true;
                }
            }
        }
        p = *line_end == '\n' ? line_end + 1 : line_end;
    }
    return false;
}

static struct ini_update *find_update(struct ini_update *updates, size_t count,
                                      const char *section,
                                      const char *key_start, const char *key_end)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(updates[i].section, section) == 0 &&
            span_equal(key_start, key_end, updates[i].key)) return &updates[i];
    }
    return NULL;
}

static char *ini_apply_updates(const char *config, struct ini_update *updates,
                               size_t count, bool add_missing,
                               size_t *new_length,
                               char *error, size_t error_size)
{
    struct string_buffer output;
    const char *p = config;
    char section[80] = "";

    for (size_t i = 0; i < count; i++) updates[i].found = false;
    sb_init(&output);
    while (*p != '\0') {
        const char *line_end = strchr(p, '\n');
        const char *start = p;
        const char *end;
        struct ini_update *update = NULL;
        if (line_end == NULL) line_end = p + strlen(p);
        end = line_end;
        trim_span(&start, &end);
        if (start < end && *start == '[') {
            const char *close = memchr(start + 1, ']', (size_t)(end - start - 1));
            if (close != NULL) {
                const char *name_start = start + 1;
                const char *name_end = close;
                size_t length;
                trim_span(&name_start, &name_end);
                length = (size_t)(name_end - name_start);
                if (length >= sizeof(section)) length = sizeof(section) - 1;
                memcpy(section, name_start, length);
                section[length] = '\0';
            }
        } else if (start < end && *start != '#' && *start != ';') {
            const char *equals = memchr(start, '=', (size_t)(end - start));
            if (equals != NULL) {
                const char *key_start = start;
                const char *key_end = equals;
                trim_span(&key_start, &key_end);
                update = find_update(updates, count, section, key_start, key_end);
                if (update != NULL && update->found) {
                    snprintf(error, error_size, T(S_E_INI_DUPLICATE_KEY),
                             section, update->key);
                    free(output.data);
                    return NULL;
                }
                if (update != NULL) {
                    const char *value_start = equals + 1;
                    const char *value_end = end;
                    trim_span(&value_start, &value_end);
                    if (value_end > value_start + 1 && *value_start == '"' &&
                        value_end[-1] == '"') {
                        value_start++;
                        value_end--;
                        if (strcmp(section, "support_proxy") != 0)
                            trim_span(&value_start, &value_end);
                    }
                    if (span_equal(value_start, value_end, update->value)) {
                        update->found = true;
                        update = NULL;
                    } else {
                        if (!sb_append_n(&output, p, (size_t)(equals + 1 - p)) ||
                            !sb_appendf(&output, " \"%s\"", update->value)) {
                            free(output.data);
                            return NULL;
                        }
                        update->found = true;
                    }
                }
            }
        }
        if (update == NULL && !sb_append_n(&output, p, (size_t)(line_end - p))) {
            free(output.data);
            return NULL;
        }
        if (*line_end == '\n' && !sb_append_n(&output, "\n", 1)) {
            free(output.data);
            return NULL;
        }
        p = *line_end == '\n' ? line_end + 1 : line_end;
    }
    for (size_t i = 0; i < count; i++) {
        if (!updates[i].found && !add_missing) {
            snprintf(error, error_size, T(S_E_INI_MISSING_KEY),
                     updates[i].section, updates[i].key);
            free(output.data);
            return NULL;
        }
    }
    if (add_missing) {
        for (size_t i = 0; i < count; i++) {
            if (updates[i].found) continue;
            if (output.len != 0U && output.data[output.len - 1U] != '\n' &&
                !sb_append_n(&output, "\n", 1U)) goto allocation_failed;
            if (output.len != 0U && !sb_append_n(&output, "\n", 1U)) {
                goto allocation_failed;
            }
            if (!sb_appendf(&output, "[%s]\n", updates[i].section)) {
                goto allocation_failed;
            }
            for (size_t j = i; j < count; j++) {
                if (updates[j].found ||
                    strcmp(updates[j].section, updates[i].section) != 0) {
                    continue;
                }
                if (!sb_appendf(&output, "%s = \"%s\"\n",
                                updates[j].key, updates[j].value)) {
                    goto allocation_failed;
                }
                updates[j].found = true;
            }
        }
    }
    if (output.len > MAX_CONFIG) {
        snprintf(error, error_size, "%s", T(S_E_CONFIG_TOO_LARGE));
        free(output.data);
        return NULL;
    }
    *new_length = output.len;
    return output.data;

allocation_failed:
    snprintf(error, error_size, "%s", T(S_E_CONFIG_ALLOC));
    free(output.data);
    return NULL;
}

static bool parse_long_strict(const char *text, long minimum, long maximum,
                              long *result)
{
    char *end;
    long value;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' ||
        value < minimum || value > maximum) return false;
    *result = value;
    return true;
}

static bool parse_double_strict(const char *text, double minimum, double maximum,
                                double *result)
{
    char *end;
    double value;
    errno = 0;
    value = strtod(text, &end);
    if (errno != 0 || *text == '\0' || *end != '\0' || !isfinite(value) ||
        value < minimum || value > maximum) return false;
    *result = value;
    return true;
}

static bool valid_netmask(const char *text)
{
    struct in_addr address;
    uint32_t mask;
    uint32_t inverse;
    if (inet_pton(AF_INET, text, &address) != 1) return false;
    mask = ntohl(address.s_addr);
    inverse = ~mask;
    return (inverse & (inverse + 1U)) == 0;
}

static bool option_contains(const struct parameter *parameter, const char *value)
{
    for (size_t i = 0; i < parameter->option_count; i++) {
        if (strcmp(parameter->options[i].value, value) == 0) return true;
    }
    return false;
}

static bool valid_lut_value(const char *text)
{
    const char *p = text;
    for (unsigned item = 0; item < 3; item++) {
        char *end;
        long value;
        errno = 0;
        value = strtol(p, &end, 10);
        if (errno != 0 || end == p || value < INT32_MIN || value > INT32_MAX) return false;
        if (item < 2) {
            if (*end != ',') return false;
            p = end + 1;
        } else if (*end != '\0') {
            return false;
        }
    }
    return true;
}

static bool valid_text_parameter(const struct parameter *parameter,
                                 const char *text)
{
    size_t length = strlen(text);

    if (length < (size_t)parameter->minimum ||
        length > (size_t)parameter->maximum) return false;
    if (!strcmp(parameter->section, "network")) {
        struct in_addr address;
        unsigned prefix;
        if (!strcmp(parameter->key, "interface"))
            return length && strspn(text, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == length;
        if (!length) return true;
        if (!strcmp(parameter->key, "gateway")) return apcam_ipv4_host(text, &address);
        return apcam_ipv4_prefix(text, &address, &prefix);
    }
    if (strcmp(parameter->section, "support_proxy") == 0) {
        for (size_t i = 0; i < length; i++) {
            unsigned char c = (unsigned char)text[i];
            if (c < 32 || c > 126 || c == '"') return false;
            if (strcmp(parameter->key, "host") == 0 &&
                !isalnum(c) && c != '.' && c != '-' && c != '_') return false;
        }
        return true;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (iscntrl(c) || isspace(c)) return false;
    }
    return true;
}

static bool append_update(struct ini_update *updates, size_t capacity, size_t *count,
                          const char *section, const char *key, const char *value,
                          char *error, size_t error_size)
{
    if (*count >= capacity || strlen(value) >= sizeof(updates[0].value)) {
        snprintf(error, error_size, "%s", T(S_E_TOO_MANY_PARAMETERS));
        return false;
    }
    updates[*count].section = section;
    snprintf(updates[*count].key, sizeof(updates[*count].key), "%s", key);
    snprintf(updates[*count].value, sizeof(updates[*count].value), "%s", value);
    updates[*count].found = false;
    (*count)++;
    return true;
}

/* parameters that do not exist on this camera stay out of the form */
static bool parameter_shown(const struct parameter *parameter)
{
    if (!APCAM_HAVE_IMAGE_CONTROLS && !strcmp(parameter->section, "image")) return false;
    if (!APCAM_HAVE_EXTERNAL_UART && !strcmp(parameter->section, "uart")) return false;
    if (!APCAM_HAVE_THERMAL && (!strcmp(parameter->section, "thermal") ||
        !strcmp(parameter->key, "photo_scope"))) return false;
    return true;
}

static bool collect_described_parameters(const struct request *request,
                                         const struct parameter *parameters,
                                         size_t parameter_count,
                                         struct ini_update *updates,
                                         size_t capacity, size_t *count,
                                         char *error, size_t error_size)
{
    for (size_t i = 0; i < parameter_count; i++) {
        const struct parameter *parameter = &parameters[i];
        size_t value_length = 0;
        char *value;

        if (!parameter_shown(parameter)) continue;
        value = form_value(request, parameter->form_name, &value_length);
        bool valid = value != NULL && value_length < sizeof(updates[0].value);
        char normalized[160] = "";
        long integer_value;
        double float_value;
        struct in_addr ipv4;

        if (valid) {
            switch (parameter->kind) {
            case PARAM_BOOLEAN:
            case PARAM_ENUM:
                valid = option_contains(parameter, value);
                if (valid) snprintf(normalized, sizeof(normalized), "%s", value);
                break;
            case PARAM_INTEGER:
                valid = parse_long_strict(value, (long)parameter->minimum,
                                          (long)parameter->maximum, &integer_value);
                if (valid) snprintf(normalized, sizeof(normalized), "%ld", integer_value);
                break;
            case PARAM_FLOAT:
                valid = parse_double_strict(value, parameter->minimum,
                                            parameter->maximum, &float_value);
                if (valid) snprintf(normalized, sizeof(normalized), "%s", value);
                break;
            case PARAM_IPV4:
                valid = inet_pton(AF_INET, value, &ipv4) == 1;
                if (valid && strcmp(parameter->key, "netmask") == 0) valid = valid_netmask(value);
                if (valid) snprintf(normalized, sizeof(normalized), "%s", value);
                break;
            case PARAM_LUT:
                valid = valid_lut_value(value);
                if (valid) snprintf(normalized, sizeof(normalized), "%s", value);
                break;
            case PARAM_TEXT:
            case PARAM_PASSWORD:
                valid = valid_text_parameter(parameter, value);
                if (valid) snprintf(normalized, sizeof(normalized), "%s", value);
                break;
            }
        }
        if (!valid) {
            snprintf(error, error_size, T(S_E_INVALID_VALUE), T(parameter->label));
            if (!strcmp(parameter->form_name, "network_primary_address") || !strcmp(parameter->form_name, "network_secondary_address"))
                snprintf(error, error_size, "%s: %s", T(parameter->label), T(parameter->help));
            log_message("parameter validation failed: %s", error);
            free(value);
            return false;
        }
        if (!append_update(updates, capacity, count, parameter->section,
                           parameter->key, normalized, error, error_size)) {
            free(value);
            return false;
        }
        free(value);
    }
    return true;
}



static bool collect_replacement_parameter_updates(
    const struct request *request, struct ini_update *updates, size_t capacity,
    size_t *update_count, char *error, size_t error_size)
{
    size_t count = 0;

    if (!collect_described_parameters(
            request, replacement_parameters,
            sizeof(replacement_parameters) / sizeof(replacement_parameters[0]),
            updates, capacity, &count, error, error_size)) return false;
    const char *primary = "", *secondary = "", *gateway = "";
    for (size_t i = 0; i < count; i++) {
        if (strcmp(updates[i].section, "network")) continue;
        if (!strcmp(updates[i].key, "primary_address")) primary = updates[i].value;
        if (!strcmp(updates[i].key, "secondary_address")) secondary = updates[i].value;
        if (!strcmp(updates[i].key, "gateway")) gateway = updates[i].value;
    }
    if (!apcam_network_valid(primary, secondary, gateway)) {
        snprintf(error, error_size, "%s", T(S_E_NETWORK));
        return false;
    }
    const char *enabled = "false", *host = "", *signing = "false", *passphrase = "";
    const char *video1_port = "0", *video2_port = "0", *video1_name = "", *video2_name = "";
    for (size_t i = 0; i < count; i++) {
        if (strcmp(updates[i].section, "support_proxy") != 0) continue;
#define SUPPORT_VALUE(key_name, destination) \
        if (strcmp(updates[i].key, key_name) == 0) destination = updates[i].value
        SUPPORT_VALUE("enabled", enabled);
        SUPPORT_VALUE("host", host);
        SUPPORT_VALUE("signing", signing);
        SUPPORT_VALUE("signing_passphrase", passphrase);
        SUPPORT_VALUE("video1_port", video1_port);
        SUPPORT_VALUE("video2_port", video2_port);
        SUPPORT_VALUE("video1_name", video1_name);
        SUPPORT_VALUE("video2_name", video2_name);
#undef SUPPORT_VALUE
    }
    if (strcmp(enabled, "true") == 0) {
        enum string_id invalid = S_COUNT;
        if (!*host) invalid = S_P_PROXY_HOST;
        else if (strcmp(signing, "true") == 0 && !*passphrase)
            invalid = S_P_PROXY_SIGNING_PASSPHRASE;
        else if (strcmp(video1_port, "0") != 0 && !*video1_name)
            invalid = S_P_PROXY_VIDEO1_NAME;
        else if (strcmp(video2_port, "0") != 0 && !*video2_name)
            invalid = S_P_PROXY_VIDEO2_NAME;
        else if (strcmp(video1_port, "0") != 0 && strcmp(video1_port, video2_port) == 0)
            invalid = S_P_PROXY_VIDEO2_PORT;
        if (invalid != S_COUNT) {
            snprintf(error, error_size, T(S_E_INVALID_VALUE), T(invalid));
            log_message("parameter validation failed: %s", error);
            return false;
        }
    }
    *update_count = count;
    return true;
}

static void free_parameter_updates(struct ini_update *updates, size_t count)
{
    (void)updates;
    (void)count;
}

static bool write_all(int fd, const void *buffer, size_t length)
{
    const char *p = buffer;
    while (length > 0) {
        ssize_t written = write(fd, p, length);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        p += written;
        length -= (size_t)written;
    }
    return true;
}

static bool sync_parent_directory(const char *path)
{
    char parent[PATH_MAX];
    char *slash;
    int fd;
    bool ok;

    if (snprintf(parent, sizeof(parent), "%s", path) >= (int)sizeof(parent)) {
        errno = ENAMETOOLONG;
        return false;
    }
    slash = strrchr(parent, '/');
    if (slash == NULL) {
        snprintf(parent, sizeof(parent), ".");
    } else if (slash == parent) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

static bool save_atomic_file(const char *path, const char *contents,
                             size_t length, mode_t mode,
                             char *error, size_t error_size)
{
    char temporary[PATH_MAX];
    int fd = -1;
    bool renamed = false;
    bool ok = false;

    if (snprintf(temporary, sizeof(temporary), "%s.web.new.%ld",
                 path, (long)getpid()) >= (int)sizeof(temporary)) {
        snprintf(error, error_size, "%s", T(S_E_PATH_TOO_LONG));
        return false;
    }
    (void)unlink(temporary);
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
              mode);
    if (fd < 0 || fchmod(fd, mode) < 0 ||
        !write_all(fd, contents, length) || fsync(fd) < 0) {
        snprintf(error, error_size, T(S_E_CANNOT_WRITE), path, strerror(errno));
        goto done;
    }
    if (close(fd) < 0) {
        fd = -1;
        snprintf(error, error_size, T(S_E_CANNOT_CLOSE), path, strerror(errno));
        goto done;
    }
    fd = -1;
    if (rename(temporary, path) < 0) {
        snprintf(error, error_size, T(S_E_CANNOT_INSTALL), path, strerror(errno));
        goto done;
    }
    renamed = true;
    if (!sync_parent_directory(path)) {
        snprintf(error, error_size, T(S_E_CANNOT_SYNC), path, strerror(errno));
        goto done;
    }
    ok = true;
done:
    if (fd >= 0) close(fd);
    if (!renamed) (void)unlink(temporary);
    return ok;
}

static int lock_user_files(char *error, size_t error_size)
{
    int fd = open(USER_LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0600);

    if (fd < 0 || flock(fd, LOCK_EX) < 0) {
        int saved_errno = errno;
        if (fd >= 0) close(fd);
        snprintf(error, error_size, T(S_E_LOCK_USERS), strerror(saved_errno));
        return -1;
    }
    return fd;
}

#if WEB_HAVE_SSH_KEYS
struct public_key_view {
    const char *type;
    size_t type_len;
    const char *blob;
    size_t blob_len;
    const char *comment;
    size_t comment_len;
};

struct public_key_line {
    struct public_key_view key;
    size_t start;
    size_t end;
};

static bool span_equals(const char *text, size_t length, const char *expected)
{
    return length == strlen(expected) && memcmp(text, expected, length) == 0;
}

static bool supported_public_key_type(const char *type, size_t length)
{
    static const char *const types[] = {
        "ssh-ed25519", "ssh-rsa",
        "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521",
        "sk-ssh-ed25519@openssh.com",
        "sk-ecdsa-sha2-nistp256@openssh.com",
    };

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        if (span_equals(type, length, types[i])) return true;
    }
    return false;
}

static bool parse_public_key(const char *text, size_t length,
                             struct public_key_view *key)
{
    size_t start = 0;
    size_t end = length;
    size_t offset;
    bool padding = false;

    while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r')) end--;
    if (start == end) return false;
    key->type = text + start;
    offset = start;
    while (offset < end && text[offset] != ' ' && text[offset] != '\t') offset++;
    key->type_len = offset - start;
    if (!supported_public_key_type(key->type, key->type_len)) return false;
    while (offset < end && (text[offset] == ' ' || text[offset] == '\t')) offset++;
    key->blob = text + offset;
    while (offset < end && text[offset] != ' ' && text[offset] != '\t') {
        unsigned char value = (unsigned char)text[offset];
        if (value == '=') {
            padding = true;
        } else if (padding || !(isalnum(value) || value == '+' || value == '/')) {
            return false;
        }
        offset++;
    }
    key->blob_len = (size_t)((text + offset) - key->blob);
    if (key->blob_len < 16 || key->blob_len > 16384) return false;
    size_t padding_count = 0;
    while (padding_count < key->blob_len &&
           key->blob[key->blob_len - padding_count - 1] == '=') padding_count++;
    if (padding_count > 2 || key->blob_len % 4 != 0) return false;
    while (offset < end && (text[offset] == ' ' || text[offset] == '\t')) offset++;
    key->comment = text + offset;
    key->comment_len = end - offset;
    for (size_t i = 0; i < key->comment_len; i++) {
        unsigned char value = (unsigned char)key->comment[i];
        if (value < 0x20 || value == 0x7f) return false;
    }
    return true;
}

static bool public_key_identity_equal(const struct public_key_view *a,
                                      const struct public_key_view *b)
{
    return a->type_len == b->type_len && a->blob_len == b->blob_len &&
           memcmp(a->type, b->type, a->type_len) == 0 &&
           memcmp(a->blob, b->blob, a->blob_len) == 0;
}

static bool install_authorized_keys(const char *contents, size_t length,
                                    char *error, size_t error_size)
{
    if (!save_atomic_file(AUTHORIZED_KEYS_PATH, contents, length, 0600,
                          error, error_size)) return false;
    if (mkdir(RUNTIME_AUTHORIZED_KEYS_DIR, 0700) < 0 && errno != EEXIST) {
        snprintf(error, error_size, T(S_E_KEYS_RUNTIME_DIR), strerror(errno));
        return false;
    }
    if (chmod(RUNTIME_AUTHORIZED_KEYS_DIR, 0700) < 0 ||
        !save_atomic_file(RUNTIME_AUTHORIZED_KEYS_PATH, contents, length, 0600,
                          error, error_size)) {
        snprintf(error, error_size, T(S_E_KEYS_RUNTIME_ACTIVATION), strerror(errno));
        return false;
    }
    return true;
}
#endif

static bool change_admin_password(const struct request *request,
                                  char *error, size_t error_size)
{
    size_t password_len = 0;
    size_t confirmation_len = 0;
    char *password = form_value(request, "password", &password_len);
    char *confirmation = form_value(request, "confirmation", &confirmation_len);
    char saved[130];
    int lock_fd = -1;
    bool ok = false;

    if (password == NULL || confirmation == NULL || password_len < 8 ||
        password_len > 128) {
        snprintf(error, error_size, "%s", T(S_E_PASSWORD_LENGTH));
        goto done;
    }
    for (size_t i = 0; i < password_len; i++) {
        unsigned char value = (unsigned char)password[i];
        if (value < 0x20 || value == 0x7f) {
            snprintf(error, error_size, "%s", T(S_E_PASSWORD_CONTROL));
            goto done;
        }
    }
    if (!constant_time_equal(password, password_len,
                             confirmation, confirmation_len)) {
        snprintf(error, error_size, "%s", T(S_E_PASSWORD_MISMATCH));
        goto done;
    }
    memcpy(saved, password, password_len);
    saved[password_len] = '\n';
    lock_fd = lock_user_files(error, error_size);
    if (lock_fd < 0) goto done;
    ok = save_atomic_file(PASSWORD_PATH, saved, password_len + 1, 0600,
                          error, error_size);
done:
    if (lock_fd >= 0) close(lock_fd);
    if (password != NULL) memset(password, 0, password_len);
    if (confirmation != NULL) memset(confirmation, 0, confirmation_len);
    memset(saved, 0, sizeof(saved));
    free(password);
    free(confirmation);
    return ok;
}

static bool sync_time_from_browser(const struct request *request,
                                   char *error, size_t error_size)
{
    const uint64_t earliest_ms = UINT64_C(1577836800000); /* 2020-01-01 */
    const uint64_t latest_ms = UINT64_C(4102444800000);   /* 2100-01-01 */
    size_t length = 0;
    char *value = form_value(request, "time_ms", &length);
    char *end = NULL;
    unsigned long long milliseconds;
    bool ok = false;

    errno = 0;
    milliseconds = value != NULL ? strtoull(value, &end, 10) : 0U;
    if (value == NULL || length == 0U || length > 16U || errno != 0 ||
        end != value + length || milliseconds < earliest_ms ||
        milliseconds >= latest_ms) {
        snprintf(error, error_size, "%s", T(S_E_BROWSER_TIME));
        goto done;
    }
#ifdef MT11_WEB_TEST
    {
        char recorded[64];
        int recorded_length = snprintf(recorded, sizeof(recorded), "%llu\n",
                                       milliseconds);
        if (recorded_length <= 0 || (size_t)recorded_length >= sizeof(recorded) ||
            !save_atomic_file(TIME_SYNC_TEST_PATH, recorded,
                              (size_t)recorded_length, 0600,
                              error, error_size)) {
            goto done;
        }
    }
#else
    struct timespec requested = {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
    };
    if (clock_settime(CLOCK_REALTIME, &requested) < 0) {
        snprintf(error, error_size, T(S_E_SET_TIME), strerror(errno));
        goto done;
    }
#endif
    ok = true;
done:
    free(value);
    return ok;
}

#if WEB_HAVE_SSH_KEYS
static char *read_authorized_keys(size_t *length)
{
    char *keys = read_file(AUTHORIZED_KEYS_PATH, MAX_AUTHORIZED_KEYS, length);

    if (keys == NULL && errno == ENOENT) {
        keys = strdup("");
        if (keys != NULL) *length = 0;
    }
    return keys;
}

static bool add_authorized_keys(const struct request *request,
                                size_t *added_count,
                                char *error, size_t error_size)
{
    size_t input_len = 0;
    char *input = form_value(request, "public_key", &input_len);
    struct public_key_line *candidates = NULL;
    char *existing = NULL;
    char *updated = NULL;
    size_t existing_len = 0;
    size_t candidate_count = 0;
    size_t max_candidates = 1;
    size_t updated_len;
    int lock_fd = -1;
    bool ok = false;

    *added_count = 0;
    if (input == NULL || input_len == 0) {
        snprintf(error, error_size, "%s", T(S_E_KEY_REQUIRED));
        goto done;
    }
    normalize_newlines(input, &input_len);
    if (input_len > MAX_AUTHORIZED_KEYS) {
        snprintf(error, error_size, T(S_E_KEYS_TOO_LARGE), MAX_AUTHORIZED_KEYS);
        goto done;
    }
    for (size_t i = 0; i < input_len; i++) {
        if (input[i] == '\n') max_candidates++;
    }
    candidates = calloc(max_candidates, sizeof(*candidates));
    if (candidates == NULL) {
        snprintf(error, error_size, "%s", T(S_OUT_OF_MEMORY));
        goto done;
    }
    for (size_t offset = 0; offset < input_len;) {
        size_t line_end = offset;
        size_t start;
        size_t end;

        while (line_end < input_len && input[line_end] != '\n') line_end++;
        start = offset;
        end = line_end;
        while (start < end && (input[start] == ' ' || input[start] == '\t')) start++;
        while (end > start && (input[end - 1] == ' ' || input[end - 1] == '\t')) end--;
        if (start < end) {
            struct public_key_line *candidate = &candidates[candidate_count];
            if (!parse_public_key(input + start, end - start, &candidate->key)) {
                snprintf(error, error_size, T(S_E_KEY_MALFORMED), candidate_count + 1);
                goto done;
            }
            candidate->start = start;
            candidate->end = end;
            candidate_count++;
        }
        offset = line_end + (line_end < input_len ? 1 : 0);
    }
    if (candidate_count == 0) {
        snprintf(error, error_size, "%s", T(S_E_KEY_REQUIRED));
        goto done;
    }
    for (size_t i = 0; i < candidate_count; i++) {
        for (size_t prior = 0; prior < i; prior++) {
            if (public_key_identity_equal(&candidates[i].key,
                                          &candidates[prior].key)) {
                snprintf(error, error_size, T(S_E_KEY_DUPLICATE_LINE), i + 1);
                goto done;
            }
        }
    }
    lock_fd = lock_user_files(error, error_size);
    if (lock_fd < 0) goto done;
    existing = read_authorized_keys(&existing_len);
    if (existing == NULL) {
        snprintf(error, error_size, T(S_E_KEYS_READ), strerror(errno));
        goto done;
    }
    for (size_t i = 0; i < candidate_count; i++) {
        for (size_t offset = 0; offset < existing_len;) {
            size_t line_end = offset;
            struct public_key_view current;
            while (line_end < existing_len && existing[line_end] != '\n') line_end++;
            if (parse_public_key(existing + offset, line_end - offset, &current) &&
                public_key_identity_equal(&candidates[i].key, &current)) {
                snprintf(error, error_size, T(S_E_KEY_ALREADY), i + 1);
                goto done;
            }
            offset = line_end + (line_end < existing_len ? 1 : 0);
        }
    }
    updated_len = existing_len +
                  (existing_len > 0 && existing[existing_len - 1] != '\n' ? 1 : 0);
    for (size_t i = 0; i < candidate_count; i++) {
        size_t line_len = candidates[i].end - candidates[i].start;
        if (updated_len > MAX_AUTHORIZED_KEYS - line_len - 1) {
            snprintf(error, error_size, T(S_E_KEY_FILE_TOO_LARGE), MAX_AUTHORIZED_KEYS);
            goto done;
        }
        updated_len += line_len + 1;
    }
    updated = malloc(updated_len + 1);
    if (updated == NULL) {
        snprintf(error, error_size, "%s", T(S_OUT_OF_MEMORY));
        goto done;
    }
    memcpy(updated, existing, existing_len);
    size_t used = existing_len;
    if (used > 0 && updated[used - 1] != '\n') updated[used++] = '\n';
    for (size_t i = 0; i < candidate_count; i++) {
        size_t line_len = candidates[i].end - candidates[i].start;
        memcpy(updated + used, input + candidates[i].start, line_len);
        used += line_len;
        updated[used++] = '\n';
    }
    updated[used] = '\0';
    ok = install_authorized_keys(updated, used, error, error_size);
    if (ok) *added_count = candidate_count;
done:
    if (lock_fd >= 0) close(lock_fd);
    free(candidates);
    free(updated);
    free(existing);
    free(input);
    return ok;
}

static bool remove_authorized_key(const struct request *request,
                                  char *error, size_t error_size)
{
    size_t requested_len = 0;
    size_t confirmation_len = 0;
    char *requested = form_value(request, "public_key", &requested_len);
    char *confirmation = form_value(request, "confirm", &confirmation_len);
    struct public_key_view wanted;
    char *existing = NULL;
    char *updated = NULL;
    size_t existing_len = 0;
    size_t used = 0;
    int lock_fd = -1;
    bool found = false;
    size_t remaining_supported = 0;
    bool ok = false;

    if (confirmation == NULL || confirmation_len != 3 ||
        memcmp(confirmation, "yes", 3) != 0) {
        snprintf(error, error_size, "%s", T(S_E_KEY_REMOVE_UNCONFIRMED));
        goto done;
    }
    if (requested == NULL || !parse_public_key(requested, requested_len, &wanted)) {
        snprintf(error, error_size, "%s", T(S_E_KEY_SELECTION));
        goto done;
    }
    lock_fd = lock_user_files(error, error_size);
    if (lock_fd < 0) goto done;
    existing = read_authorized_keys(&existing_len);
    if (existing == NULL) {
        snprintf(error, error_size, T(S_E_KEYS_READ), strerror(errno));
        goto done;
    }
    updated = malloc(existing_len + 1);
    if (updated == NULL) {
        snprintf(error, error_size, "%s", T(S_OUT_OF_MEMORY));
        goto done;
    }
    for (size_t offset = 0; offset < existing_len;) {
        size_t line_end = offset;
        size_t next;
        struct public_key_view current;
        while (line_end < existing_len && existing[line_end] != '\n') line_end++;
        next = line_end + (line_end < existing_len ? 1 : 0);
        if (!found && parse_public_key(existing + offset, line_end - offset, &current) &&
            public_key_identity_equal(&wanted, &current)) {
            found = true;
        } else {
            if (parse_public_key(existing + offset, line_end - offset, &current)) {
                remaining_supported++;
            }
            memcpy(updated + used, existing + offset, next - offset);
            used += next - offset;
        }
        offset = next;
    }
    if (!found) {
        snprintf(error, error_size, "%s", T(S_E_KEY_ABSENT));
        goto done;
    }
    if (remaining_supported == 0) {
        snprintf(error, error_size, "%s", T(S_E_KEY_LAST));
        goto done;
    }
    updated[used] = '\0';
    ok = install_authorized_keys(updated, used, error, error_size);
done:
    if (lock_fd >= 0) close(lock_fd);
    free(updated);
    free(existing);
    free(requested);
    free(confirmation);
    return ok;
}
#endif

#if (APCAM_TARGET == APCAM_TARGET_MT11) || (defined(MT11_WEB_SITL) && !defined(WEB_SUPERVISED_TEST))
static bool line_has_timestamp(const char *line, size_t length)
{
    return length >= 6U && line[0] == '[' &&
           isdigit((unsigned char)line[1]) &&
           isdigit((unsigned char)line[2]) &&
           isdigit((unsigned char)line[3]) &&
           isdigit((unsigned char)line[4]) && line[5] == '-';
}

static size_t format_log_timestamp(char output[32])
{
    struct timespec now;
    struct tm local;
    int length;

    if (clock_gettime(CLOCK_REALTIME, &now) < 0 ||
        localtime_r(&now.tv_sec, &local) == NULL) return 0;
    length = snprintf(output, 32, "[%04d-%02d-%02d %02d:%02d:%02d.%03ld] ",
                      local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                      local.tm_hour, local.tm_min, local.tm_sec,
                      now.tv_nsec / 1000000L);
    return length > 0 && length < 32 ? (size_t)length : 0;
}

static int capture_app_log(const char *path, const char *old_path,
                           bool timestamp_missing)
{
    char *line = NULL;
    size_t capacity = 0;
    int output = -1;
    off_t output_size = 0;
    FILE *input = fdopen(dup(STDIN_FILENO), "r");

    if (input == NULL) return 1;

    for (;;) {
        char timestamp[32];
        size_t timestamp_length = 0;
        ssize_t got = getline(&line, &capacity, input);
        if (got < 0) break;
        if (timestamp_missing && !line_has_timestamp(line, (size_t)got)) {
            timestamp_length = format_log_timestamp(timestamp);
        }
        if (output < 0) {
            struct stat st;
            output = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
            if (output < 0) goto fail;
            if (fstat(output, &st) == 0) output_size = st.st_size;
            if ((uint64_t)output_size > APP_LOG_ROTATE_SIZE) {
                close(output);
                (void)unlink(old_path);
                output = open(path,
                              O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
                if (output < 0) goto fail;
                output_size = 0;
            }
        }
        if ((uint64_t)output_size + timestamp_length + (size_t)got >
            APP_LOG_ROTATE_SIZE) {
            close(output);
            (void)rename(path, old_path);
            output = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
            if (output < 0) goto fail;
            output_size = 0;
        }
        if ((timestamp_length != 0U &&
             !write_all(output, timestamp, timestamp_length)) ||
            !write_all(output, line, (size_t)got)) goto fail;
        output_size += (off_t)(timestamp_length + (size_t)got);
    }
    int input_error = ferror(input);
    free(line);
    fclose(input);
    if (output >= 0) close(output);
    return input_error ? 1 : 0;
fail:
    free(line);
    fclose(input);
    if (output >= 0) close(output);
    return 1;
}
#endif

static bool copy_to_temp(const char *source, const char *temp_path,
                         mode_t mode, uid_t uid, gid_t gid)
{
    int source_fd = -1;
    int dest_fd = -1;
    char buffer[8192];
    bool ok = false;

    source_fd = open(source, O_RDONLY | O_CLOEXEC);
    if (source_fd < 0) goto done;
    dest_fd = open(temp_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (dest_fd < 0) goto done;
    if (fchown(dest_fd, uid, gid) < 0) goto done;
    for (;;) {
        ssize_t got = read(source_fd, buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR) continue;
            goto done;
        }
        if (got == 0) break;
        if (!write_all(dest_fd, buffer, (size_t)got)) goto done;
    }
    if (fsync(dest_fd) < 0) goto done;
    ok = true;
done:
    if (source_fd >= 0) close(source_fd);
    if (dest_fd >= 0) close(dest_fd);
    if (!ok) unlink(temp_path);
    return ok;
}

static bool save_config(const char *config_path, const char *backup_path,
                        const char *config, size_t length,
                        char *error, size_t error_size)
{
    struct stat st;
    char config_temp[PATH_MAX];
    char backup_temp[PATH_MAX];
    int fd = -1;
    int dir_fd = -1;
    bool ok = false;

    if (!validate_ini(config, length, error, error_size)) return false;
    if (stat(config_path, &st) < 0) {
        snprintf(error, error_size, T(S_E_CANNOT_STAT), config_path, strerror(errno));
        return false;
    }
    if (snprintf(config_temp, sizeof(config_temp), "%s.web.new.%ld",
                 config_path, (long)getpid()) >= (int)sizeof(config_temp) ||
        snprintf(backup_temp, sizeof(backup_temp), "%s.web.bak.new.%ld",
                 config_path, (long)getpid()) >= (int)sizeof(backup_temp)) {
        snprintf(error, error_size, "%s", T(S_E_CONFIG_PATH_LONG));
        return false;
    }
    unlink(config_temp);
    unlink(backup_temp);

    if (!copy_to_temp(config_path, backup_temp, st.st_mode & 0777, st.st_uid, st.st_gid) ||
        rename(backup_temp, backup_path) < 0) {
        snprintf(error, error_size, T(S_E_CONFIG_BACKUP), strerror(errno));
        goto done;
    }
    fd = open(config_temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 0777);
    if (fd < 0 || fchown(fd, st.st_uid, st.st_gid) < 0 ||
        !write_all(fd, config, length) || fsync(fd) < 0) {
        snprintf(error, error_size, T(S_E_CONFIG_WRITE), strerror(errno));
        goto done;
    }
    close(fd);
    fd = -1;
    if (rename(config_temp, config_path) < 0) {
        snprintf(error, error_size, T(S_E_CONFIG_INSTALL), strerror(errno));
        goto done;
    }
    dir_fd = open(APP_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd >= 0) (void)fsync(dir_fd);
    ok = true;
done:
    if (fd >= 0) close(fd);
    if (dir_fd >= 0) close(dir_fd);
    unlink(config_temp);
    unlink(backup_temp);
    return ok;
}

enum camera_kind {
    CAMERA_NONE = 0,
    CAMERA_REPLACEMENT,
    CAMERA_ANY,
};

static const char *config_path(enum camera_kind kind)
{
    (void)kind;
    return REPLACEMENT_CONFIG_PATH;
}

static const char *config_backup_path(enum camera_kind kind)
{
    (void)kind;
    return REPLACEMENT_CONFIG_BACKUP_PATH;
}

#if (APCAM_TARGET == APCAM_TARGET_MT11) || (defined(MT11_WEB_SITL) && !defined(WEB_SUPERVISED_TEST))
static const char *camera_path(enum camera_kind kind)
{
    (void)kind;
    return REPLACEMENT_CAMERA_PATH;
}
#endif

static const char *camera_label(enum camera_kind kind)
{
    if (kind == CAMERA_REPLACEMENT) return T(S_APP_REPLACEMENT);
    return T(S_APP_GENERIC);
}


static enum camera_kind pid_camera_kind(pid_t pid)
{
    char proc_path[64];
    char executable[PATH_MAX];
    ssize_t length;

    snprintf(proc_path, sizeof(proc_path), "/proc/%ld/exe", (long)pid);
    length = readlink(proc_path, executable, sizeof(executable) - 1);
    if (length < 0) return CAMERA_NONE;
    executable[length] = '\0';
#ifdef WEB_PORTABLE_SITL
    /* Cygwin may omit .exe and use another spelling of the Windows path.
     * Match file identity and the runtime, so one launcher cannot restart a
     * second instance using the same installed camera executable. */
    struct stat running, expected;
    if (stat(executable, &running) < 0 || stat(REPLACEMENT_CAMERA_PATH, &expected) < 0 ||
        running.st_dev != expected.st_dev || running.st_ino != expected.st_ino)
        return CAMERA_NONE;
    snprintf(proc_path, sizeof(proc_path), "/proc/%ld/environ", (long)pid);
    FILE *environment = fopen(proc_path, "rb");
    if (!environment) return CAMERA_NONE;
    bool belongs = false;
    char *entry = NULL;
    size_t capacity = 0;
    while (getdelim(&entry, &capacity, '\0', environment) > 0) {
        if (strncmp(entry, "CAMERA_APP_CONFIG=", 18) == 0 &&
            strcmp(entry + 18, REPLACEMENT_CONFIG_PATH) == 0) belongs = true;
    }
    free(entry);
    fclose(environment);
    return belongs ? CAMERA_REPLACEMENT : CAMERA_NONE;
#endif
    static const char deleted_suffix[] = " (deleted)";
    size_t suffix_length = sizeof(deleted_suffix) - 1U;
    if ((size_t)length > suffix_length &&
        strcmp(executable + length - (ssize_t)suffix_length, deleted_suffix) == 0) {
        executable[length - (ssize_t)suffix_length] = '\0';
    }

#if APCAM_TARGET == APCAM_TARGET_ZR10
    /* The initial SD test leaves the original executable at its stock path. */

    char resolved[PATH_MAX];
    if (realpath(REPLACEMENT_CAMERA_PATH, resolved) != NULL &&
        strcmp(executable, resolved) == 0) return CAMERA_REPLACEMENT;
#endif
    if (strcmp(executable, REPLACEMENT_CAMERA_PATH) == 0) return CAMERA_REPLACEMENT;
    return CAMERA_NONE;
}


static size_t camera_pids_kind(enum camera_kind wanted, pid_t *pids, size_t max_pids)
{
    DIR *proc = opendir("/proc");
    const struct dirent *entry;
    size_t count = 0;

    if (proc == NULL) return 0;
    while ((entry = readdir(proc)) != NULL) {
        char *end;
        long value;
        if (!isdigit((unsigned char)entry->d_name[0])) continue;
        errno = 0;
        value = strtol(entry->d_name, &end, 10);
        if (errno != 0 || *end != '\0' || value <= 1 || value > INT_MAX) continue;
        enum camera_kind found = pid_camera_kind((pid_t)value);
        if (found != CAMERA_NONE && (wanted == CAMERA_ANY || wanted == found)) {
            if (count < max_pids) pids[count] = (pid_t)value;
            count++;
        }
    }
    closedir(proc);
    return count;
}

static size_t camera_pids(pid_t *pids, size_t max_pids)
{
    return camera_pids_kind(CAMERA_ANY, pids, max_pids);
}


static enum camera_kind current_camera_kind(void)
{
    pid_t pid;
    if (camera_pids_kind(CAMERA_REPLACEMENT, &pid, 1) != 0) return CAMERA_REPLACEMENT;

    return CAMERA_NONE;
}

/* The ZR10 bundle only edits replacement settings, including during fallback. */
static enum camera_kind configuration_camera_kind(void)
{
    return CAMERA_REPLACEMENT;
}

static uint16_t siyi_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0;

    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000U) != 0U
                      ? (uint16_t)((crc << 1) ^ 0x1021U)
                      : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static bool vendor_siyi_send(int fd, uint16_t sequence, uint8_t opcode,
                             const uint8_t *payload, uint16_t payload_length)
{
    uint8_t packet[32];
    size_t packet_length = 10U + payload_length;
    uint16_t crc;

    if (packet_length > sizeof(packet)) return false;
    packet[0] = 0x55U;
    packet[1] = 0x66U;
    packet[2] = 1U;
    packet[3] = (uint8_t)payload_length;
    packet[4] = (uint8_t)(payload_length >> 8);
    packet[5] = (uint8_t)sequence;
    packet[6] = (uint8_t)(sequence >> 8);
    packet[7] = opcode;
    if (payload_length != 0U) memcpy(packet + 8U, payload, payload_length);
    crc = siyi_crc16(packet, packet_length - 2U);
    packet[packet_length - 2U] = (uint8_t)crc;
    packet[packet_length - 1U] = (uint8_t)(crc >> 8);
    return send(fd, packet, packet_length, 0) == (ssize_t)packet_length;
}

static bool parse_siyi_reply(const uint8_t *response, size_t response_size,
                             uint8_t opcode, uint8_t *reply,
                             size_t reply_size, size_t *reply_length)
{
    for (size_t offset = 0; offset + 10U <= response_size;) {
        uint16_t response_payload;
        size_t response_length;
        uint16_t received_crc;

        if (response[offset] != 0x55U || response[offset + 1U] != 0x66U) {
            break;
        }
        response_payload = (uint16_t)response[offset + 3U] |
                           (uint16_t)response[offset + 4U] << 8;
        response_length = 10U + response_payload;
        if (offset + response_length > response_size) break;
        received_crc = (uint16_t)response[offset + response_length - 2U] |
                       (uint16_t)response[offset + response_length - 1U] << 8;
        if (response[offset + 7U] == opcode &&
            received_crc == siyi_crc16(response + offset,
                                       response_length - 2U)) {
            if (response_payload > reply_size) return false;
            if (response_payload != 0U && reply != NULL) {
                memcpy(reply, response + offset + 8U, response_payload);
            }
            *reply_length = response_payload;
            return true;
        }
        offset += response_length;
    }
    return false;
}

static bool vendor_siyi_receive(int fd, uint8_t opcode, uint8_t *reply,
                                size_t reply_size, size_t *reply_length)
{
    uint8_t response[2048];

    for (unsigned attempt = 0; attempt < 8U; attempt++) {
        ssize_t got = recv(fd, response, sizeof(response), 0);
        if (got < 0) {
            if (errno == EINTR) {
                attempt--;
                continue;
            }
            return false;
        }
        if (parse_siyi_reply(response, (size_t)got, opcode, reply, reply_size,
                             reply_length)) return true;
    }
    return false;
}

#if APCAM_HAVE_SIYI
static bool vendor_siyi_exchange(int fd, uint16_t sequence, uint8_t opcode,
                                 const uint8_t *payload, uint16_t payload_length,
                                 uint8_t *reply, size_t reply_size,
                                 size_t *reply_length)
{
    return vendor_siyi_send(fd, sequence, opcode, payload, payload_length) &&
           vendor_siyi_receive(fd, opcode, reply, reply_size, reply_length);
}
#endif


static unsigned camera_api_port(void)
{
#ifdef MT11_WEB_TEST
#if APCAM_HAVE_XFROBOT
    const char *text = getenv("CAMERA_APP_MAVLINK_TCP_PORT");
#else
    const char *text = getenv("MT11_WEB_CAMERA_PORT");
#endif
    char *end;
    unsigned long value;

    if (text != NULL && *text != '\0') {
        errno = 0;
        value = strtoul(text, &end, 10);
        if (errno == 0 && *end == '\0' && value > 0U && value <= 65535U) {
            return (unsigned)value;
        }
    }
#endif
#if APCAM_WEB_CONTROL_MAVLINK
    size_t size = 0;
    char *config = read_file(REPLACEMENT_CONFIG_PATH, MAX_CONFIG, &size);
    char port_text[32] = "";
    if (config) {
        bool present = ini_get_value(config, "mavlink", "tcp_port", port_text, sizeof(port_text));
        free(config);
        if (present) {
            char *end; unsigned long value = strtoul(port_text, &end, 10);
            if (*port_text && !*end && value <= 65535) return (unsigned)value;
        }
    }
#endif
    return CAMERA_API_PORT;
}

static int open_camera_api_socket(const struct timeval *timeout)
{
    struct sockaddr_in camera = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)camera_api_port()),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, timeout,
                             sizeof(*timeout)) < 0 ||
        connect(fd, (const struct sockaddr *)&camera, sizeof(camera)) < 0) {
        int saved_errno = errno;
        if (fd >= 0) close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

static bool send_live_control(const struct request *request, char *error,
                              size_t error_size, char acquired[33])
{
    size_t n=0;
    char *action=form_value(request,"action",&n);
    char *value=form_value(request,"value",&n);
    char *lease=form_value(request,"lease",&n);
    struct apcam_manual_packet packet={.magic=APCAM_MANUAL_MAGIC};
    bool ok=false;
    int fd=-1;
    acquired[0]='\0';
    const char *actions[]={"acquire","renew","release","left","right","up","down","center","zoom"};
    for (unsigned i=0; action && i<sizeof(actions)/sizeof(actions[0]); i++)
        if (!strcmp(action,actions[i])) packet.action=i+1;
    if (!packet.action) { snprintf(error,error_size,"%s",T(S_E_UNKNOWN_ACTION)); goto done; }
    bool pulse=packet.action>=APCAM_MANUAL_LEFT && packet.action<=APCAM_MANUAL_DOWN;
    if (pulse || packet.action==APCAM_MANUAL_ZOOM) {
        if (!value) { snprintf(error,error_size,"%s",T(pulse ? S_E_MISSING_RATE : S_E_MISSING_ZOOM)); goto done; }
        char *end;
        errno=0; packet.value=strtof(value,&end);
        if (errno || end==value || *end || !isfinite(packet.value) ||
            packet.value<(pulse ? 5 : 1) || packet.value>(pulse ? 60 : APCAM_ZOOM_CONTROL_MAX)) {
            snprintf(error,error_size,"%s",T(pulse ? S_E_RATE_RANGE : S_E_ZOOM_RANGE)); goto done;
        }
    }
    if (packet.action!=APCAM_MANUAL_ACQUIRE) {
        if (!lease || strlen(lease)!=32 || strspn(lease,"0123456789abcdef")!=32) {
            snprintf(error,error_size,"%s",T(S_JS_ENABLE_FIRST)); goto done;
        }
        for (unsigned i=0;i<16;i++) {
            char hex[3]={lease[2*i],lease[2*i+1],0};
            packet.token[i]=(uint8_t)strtoul(hex,NULL,16);
        }
    }
    size_t size=0;
    char *ready=read_file(CAMERA_READY_PATH,4096,&size);
    unsigned port=0;
    if (ready) {
        const char *line=strstr(ready,"\nmanual_port=");
        if (line) { char *end; unsigned long v=strtoul(line+13,&end,10);
            if ((*end=='\n' || !*end) && v>0 && v<=65535) port=(unsigned)v; }
        free(ready);
    }
    if (!port) { snprintf(error,error_size,"%s",T(S_E_CAMERA_NOT_RUNNING)); goto done; }
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK),.sin_port=htons(port)};
    const struct timeval timeout={.tv_sec=1};
    fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);
    if (fd<0 || setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))<0 ||
        connect(fd,(struct sockaddr *)&address,sizeof(address))<0 ||
        send(fd,&packet,sizeof(packet),0)!=(ssize_t)sizeof(packet)) {
        snprintf(error,error_size,T(S_E_CAMERA_API),strerror(errno)); goto done;
    }
    struct apcam_manual_packet reply;
    ssize_t received=recv(fd,&reply,sizeof(reply),0);
    if (received!=sizeof(reply) || reply.magic!=packet.magic || reply.action!=packet.action) {
        snprintf(error,error_size,"%s",T(S_E_CAMERA_NOT_RUNNING)); goto done;
    }
    if (reply.result) {
        snprintf(error,error_size,"%s",reply.result==-EBUSY ? "Manual gimbal control is in use by another live view." :
                 reply.result==-EACCES ? "Manual control expired or the camera app restarted. Enable it again." :
                 "Camera could not apply manual control.");
        goto done;
    }
    if (packet.action==APCAM_MANUAL_ACQUIRE)
        for (unsigned i=0;i<16;i++) snprintf(acquired+2*i,3,"%02x",reply.token[i]);
    ok=true;
done:
    if (fd>=0) close(fd);
    free(action); free(value); free(lease);
    return ok;
}

#if WEB_HAVE_THERMAL
struct camera_sensor_values {
    bool have_lidar;
    uint16_t lidar_decimetres;
    bool have_thermal;
    uint16_t maximum_centi_c;
    uint16_t minimum_centi_c;
    uint16_t maximum_x;
    uint16_t maximum_y;
    uint16_t minimum_x;
    uint16_t minimum_y;
};
#endif

#if APCAM_HAVE_SIYI
static uint16_t get_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8U);
}
#endif


#if APCAM_HAVE_SIYI
static int16_t get_i16_le(const uint8_t *data)
{
    uint16_t raw = get_u16_le(data);

    return (int16_t)(raw <= INT16_MAX ? (int32_t)raw : (int32_t)raw - 65536);
}
#endif



struct camera_attitude_values {
    bool have_attitude;
    int16_t yaw_tenths;
    int16_t pitch_tenths;
    int16_t roll_tenths;
    int16_t yaw_rate_tenths;
    int16_t pitch_rate_tenths;
    int16_t roll_rate_tenths;
};

static void read_camera_attitude(struct camera_attitude_values *values)
{
#if APCAM_WEB_CONTROL_MAVLINK
    memset(values, 0, sizeof(*values));
    float roll, pitch, yaw;
    if (z1_web_attitude(camera_api_port(), &roll, &pitch, &yaw)) {
        values->roll_tenths = (int16_t)(roll * 572.9577951f);
        values->pitch_tenths = (int16_t)(pitch * 572.9577951f);
        values->yaw_tenths = (int16_t)(yaw * 572.9577951f);
        values->have_attitude = true;
    }
#else
    const struct timeval timeout = {.tv_sec = 0, .tv_usec = 350000};
    uint8_t reply[16];
    size_t reply_length = 0U;
    int fd;

    memset(values, 0, sizeof(*values));
    fd = open_camera_api_socket(&timeout);
    if (fd < 0) return;
    if (vendor_siyi_exchange(fd, 0x7501U, 0x0dU, NULL, 0U, reply,
                             sizeof(reply), &reply_length) &&
        reply_length >= 12U) {
        values->yaw_tenths = get_i16_le(reply + 0U);
        values->pitch_tenths = get_i16_le(reply + 2U);
        values->roll_tenths = get_i16_le(reply + 4U);
        values->yaw_rate_tenths = get_i16_le(reply + 6U);
        values->pitch_rate_tenths = get_i16_le(reply + 8U);
        values->roll_rate_tenths = get_i16_le(reply + 10U);
        uint8_t status[16];
        size_t status_length = 0;
        bool inverted = false;
        if (vendor_siyi_exchange(fd, 0x7502U, 0x0aU, NULL, 0U,
                                status, sizeof(status), &status_length) && status_length >= 6U) {
            inverted = status[5] == 2U;
        }
        float pose[3] = {values->roll_tenths / 10.0f, values->pitch_tenths / 10.0f, values->yaw_tenths / 10.0f};
        float rates[3] = {values->roll_rate_tenths / 10.0f, values->pitch_rate_tenths / 10.0f, values->yaw_rate_tenths / 10.0f};
        apcam_transform(&apcam_feedback[inverted], pose, pose, false);
        apcam_transform(&apcam_feedback[inverted], rates, rates, true);
        values->roll_tenths = (int16_t)(pose[0] * 10.0f);
        values->pitch_tenths = (int16_t)(pose[1] * 10.0f);
        values->yaw_tenths = (int16_t)(pose[2] * 10.0f);
        values->roll_rate_tenths = (int16_t)(rates[0] * 10.0f);
        values->pitch_rate_tenths = (int16_t)(rates[1] * 10.0f);
        values->yaw_rate_tenths = (int16_t)(rates[2] * 10.0f);
        values->have_attitude = true;
    }
    close(fd);
#endif
}

#if WEB_HAVE_THERMAL
static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0U;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static void read_camera_sensors(struct camera_sensor_values *values)
{
    const struct timeval timeout = {.tv_sec = 1, .tv_usec = 200000};
    const uint8_t one_shot[] = {1U};
    struct pollfd polls[2] = {{.fd = -1, .events = POLLIN},
                              {.fd = -1, .events = POLLIN}};
    uint8_t datagram[2048];
    uint8_t reply[32];
    uint64_t deadline;
    unsigned pending = 0U;

    memset(values, 0, sizeof(*values));
    polls[0].fd = open_camera_api_socket(&timeout);
    polls[1].fd = open_camera_api_socket(&timeout);
    if (polls[0].fd >= 0 &&
        vendor_siyi_send(polls[0].fd, 0x7301U, 0x15U, NULL, 0U)) {
        pending++;
    } else if (polls[0].fd >= 0) {
        close(polls[0].fd);
        polls[0].fd = -1;
    }
    if (polls[1].fd >= 0 &&
        vendor_siyi_send(polls[1].fd, 0x7302U, 0x14U, one_shot,
                         sizeof(one_shot))) {
        pending++;
    } else if (polls[1].fd >= 0) {
        close(polls[1].fd);
        polls[1].fd = -1;
    }
    /* The MCU publishes fresh range samples at 2 Hz. A request arriving just
     * after that boundary can legitimately take nearly a second when another
     * aircraft client is also polling, so allow overlapping HTTP polls to
     * pipeline under a bounded deadline. */
    deadline = monotonic_milliseconds() + 1200U;
    while (pending != 0U) {
        uint64_t now = monotonic_milliseconds();
        int remaining = now < deadline ? (int)(deadline - now) : 0;
        int ready;

        if (remaining == 0) break;
        ready = poll(polls, 2, remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        for (unsigned index = 0; index < 2U; index++) {
            size_t reply_length = 0U;
            uint8_t opcode = index == 0U ? 0x15U : 0x14U;
            ssize_t got;

            if (polls[index].fd < 0 ||
                (polls[index].revents & (POLLIN | POLLERR | POLLHUP)) == 0) {
                continue;
            }
            got = recv(polls[index].fd, datagram, sizeof(datagram),
                       MSG_DONTWAIT);
            if (got > 0 && parse_siyi_reply(datagram, (size_t)got, opcode,
                                            reply, sizeof(reply),
                                            &reply_length)) {
                if (index == 0U && reply_length >= 2U) {
                    values->lidar_decimetres = get_u16_le(reply);
                    values->have_lidar = true;
                } else if (index == 1U && reply_length >= 12U) {
                    values->maximum_centi_c = get_u16_le(reply + 0U);
                    values->minimum_centi_c = get_u16_le(reply + 2U);
                    values->maximum_x = get_u16_le(reply + 4U);
                    values->maximum_y = get_u16_le(reply + 6U);
                    values->minimum_x = get_u16_le(reply + 8U);
                    values->minimum_y = get_u16_le(reply + 10U);
                    values->have_thermal = true;
                }
                close(polls[index].fd);
                polls[index].fd = -1;
                pending--;
            } else if (got == 0 ||
                       (got < 0 && errno != EINTR && errno != EAGAIN &&
                        errno != EWOULDBLOCK)) {
                close(polls[index].fd);
                polls[index].fd = -1;
                pending--;
            }
        }
    }
    for (unsigned index = 0; index < 2U; index++) {
        if (polls[index].fd >= 0) close(polls[index].fd);
    }
}

static bool set_lidar_enabled(bool enabled, char *error, size_t error_size)
{
    const struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    const struct timespec retry = {.tv_sec = 0, .tv_nsec = 20000000L};
    const uint8_t payload[] = {enabled ? 1U : 0U};
    bool sent = false;
    int fd;

#ifndef MT11_WEB_TEST
    if (current_camera_kind() == CAMERA_NONE) {
        snprintf(error, error_size, "%s", T(S_E_CAMERA_NOT_RUNNING));
        return false;
    }
#endif
    fd = open_camera_api_socket(&timeout);
    if (fd < 0) {
        snprintf(error, error_size, T(S_E_CAMERA_API), strerror(errno));
        return false;
    }
    /* The MT11 does not acknowledge 0x32. Repeat the idempotent command so a
     * dropped UART frame is unlikely to leave the laser in the opposite state,
     * especially when disabling it. */
    for (unsigned attempt = 0; attempt < 3U; attempt++) {
        if (vendor_siyi_send(fd, (uint16_t)(0x7320U + attempt), 0x32U,
                             payload, sizeof(payload))) {
            sent = true;
        }
        if (attempt != 2U) (void)nanosleep(&retry, NULL);
    }
    if (!sent) {
        snprintf(error, error_size, T(S_E_SEND_LIDAR), strerror(errno));
    }
    close(fd);
    return sent;
}
#endif

static bool trigger_camera_photo(char *error, size_t error_size)
{
    const struct timeval timeout = {.tv_sec = 10, .tv_usec = 0};
    const uint8_t capture[] = {0U};
    uint8_t feedback[16];
    size_t feedback_length = 0U;
    int fd;

#ifndef MT11_WEB_TEST
    if (current_camera_kind() == CAMERA_NONE) {
        snprintf(error, error_size, "%s", T(S_E_CAMERA_NOT_RUNNING));
        return false;
    }
#endif
    fd = open_camera_api_socket(&timeout);
    if (fd < 0) {
        snprintf(error, error_size, T(S_E_CAMERA_API), strerror(errno));
        return false;
    }
    if (!vendor_siyi_send(fd, 0x7310U, 0x0cU, capture, sizeof(capture))) {
        snprintf(error, error_size, T(S_E_SEND_SHUTTER), strerror(errno));
        close(fd);
        return false;
    }
    if (!vendor_siyi_receive(fd, 0x0bU, feedback, sizeof(feedback),
                             &feedback_length)) {
        snprintf(error, error_size, "%s", T(S_E_SHUTTER_UNCONFIRMED));
        close(fd);
        return false;
    }
    close(fd);
    if (feedback_length < 1U || feedback[0] != 0U) {
        snprintf(error, error_size, "%s", T(S_E_CAPTURE_FAILED));
        return false;
    }
    return true;
}


static char *read_current_app_log(size_t *length)
{
    return read_app_log_paths(REPLACEMENT_LOG_PATH, REPLACEMENT_LOG_OLD_PATH, length);
}

static int lock_restart(char *error, size_t error_size)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/camera-app.restart.lock", RUNTIME_DIR) >= (int)sizeof(path)) {
        snprintf(error, error_size, "%s", T(S_E_APP_SELECTION_PATH));
        return -1;
    }
    int lock = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);

    if (lock < 0 || flock(lock, LOCK_EX) < 0) {
        snprintf(error, error_size, T(S_E_SWITCH_LOCK), strerror(errno));
        if (lock >= 0) close(lock);
        return -1;
    }
    return lock;
}

#if (APCAM_TARGET == APCAM_TARGET_MT11) || (defined(MT11_WEB_SITL) && !defined(WEB_SUPERVISED_TEST))
static pid_t start_camera(enum camera_kind kind)
{
    int output_pipe[2];
    pid_t logger;
    pid_t camera;

    if (pipe2(output_pipe, O_CLOEXEC) < 0) return -1;
    logger = fork();
    if (logger < 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        return -1;
    }
    if (logger == 0) {
        int console;
        if (listen_fd >= 0) close(listen_fd);
        close(output_pipe[1]);
        if (dup2(output_pipe[0], STDIN_FILENO) < 0) _exit(127);
        if (output_pipe[0] != STDIN_FILENO) close(output_pipe[0]);
        console = open("/dev/console", O_WRONLY | O_NOCTTY);
        if (console >= 0) {
            (void)dup2(console, STDERR_FILENO);
            if (console > STDERR_FILENO) close(console);
        }
        signal(SIGCHLD, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        execl(WEB_PATH, WEB_PATH, "--capture-app-log",
              "replacement",
              (char *)NULL);
        _exit(127);
    }

    camera = fork();
    if (camera < 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        (void)kill(logger, SIGTERM);
        return -1;
    }
    if (camera == 0) {
        int null_fd;
        if (listen_fd >= 0) close(listen_fd);
        close(output_pipe[0]);
        if (dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(output_pipe[1], STDERR_FILENO) < 0) _exit(127);
        if (output_pipe[1] > STDERR_FILENO) close(output_pipe[1]);
        null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDIN_FILENO);
            if (null_fd > STDERR_FILENO) close(null_fd);
        }
        signal(SIGCHLD, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        (void)setsid();
        if (chdir(APP_DIR) < 0) _exit(127);
        if (setenv("LD_LIBRARY_PATH", APP_LIB_DIR, 1) < 0 ||
            setenv("TMP", RUNTIME_DIR, 1) < 0) _exit(127);
        execl(camera_path(kind), camera_path(kind), (char *)NULL);
        _exit(127);
    }
    close(output_pipe[0]);
    close(output_pipe[1]);
    return camera;
}
#endif

static bool restarted_config_ok(char *error, size_t error_size);

#if (APCAM_TARGET == APCAM_TARGET_MT11) || (defined(MT11_WEB_SITL) && !defined(WEB_SUPERVISED_TEST))
static bool stop_camera(char *error, size_t error_size)
{
    pid_t pids[16];
    size_t count = camera_pids(pids, sizeof(pids) / sizeof(pids[0]));

    if (count > sizeof(pids) / sizeof(pids[0])) count = sizeof(pids) / sizeof(pids[0]);
    for (size_t i = 0; i < count; i++) (void)kill(pids[i], SIGTERM);
    for (unsigned attempt = 0; attempt < 150 && camera_pids(pids, 1) > 0; attempt++) {
        usleep(100000);
    }
    count = camera_pids(pids, sizeof(pids) / sizeof(pids[0]));
    if (count != 0) {
        snprintf(error, error_size, "%s", T(S_E_APP_STOP));
        return false;
    }
    return true;
}
#endif

#if (APCAM_TARGET == APCAM_TARGET_MT11) || (defined(MT11_WEB_SITL) && !defined(WEB_SUPERVISED_TEST))
static bool restart_camera_direct(char *error, size_t error_size)
{
    if (!stop_camera(error, error_size)) return false;
    (void)unlink(CAMERA_READY_PATH);
    pid_t child = start_camera(CAMERA_REPLACEMENT);
    if (child < 0) {
        snprintf(error, error_size, T(S_E_CANNOT_START), REPLACEMENT_CAMERA_PATH);
        return false;
    }
    for (unsigned attempt = 0; attempt < 200U; attempt++) {
        usleep(100000);
        if (kill(child, 0) < 0 && errno == ESRCH) break;
        if (access(CAMERA_READY_PATH, R_OK) == 0) return true;
    }
    (void)kill(child, SIGTERM);
    snprintf(error, error_size, T(S_E_NOT_READY), REPLACEMENT_CAMERA_PATH);
    return false;
}
static bool restart_camera(char *error, size_t error_size)
{
    int lock = lock_restart(error, error_size);
    if (lock < 0) return false;
    bool ok = restart_camera_direct(error, error_size);
    if (ok) ok = restarted_config_ok(error, error_size);
    close(lock);
    return ok;
}

#else
/* The supervisor observes a request generation and starts a fresh camera app. */
static unsigned long read_generation(const char *path)
{
    size_t length = 0;
    char *contents = read_file(path, 64, &length);
    unsigned long generation = 0;

    if (contents != NULL) {
        generation = strtoul(contents, NULL, 10);
        free(contents);
    }
    return generation;
}

static bool bump_request(unsigned long *generation, char *error,
                         size_t error_size)
{
    char temp_path[PATH_MAX];
    char contents[32];
    int written;
    int fd;

    *generation = read_generation(APP_REQUEST_PATH) + 1U;
    snprintf(temp_path, sizeof(temp_path), "%s.new.%ld", APP_REQUEST_PATH,
             (long)getpid());
    written = snprintf(contents, sizeof(contents), "%lu\n", *generation);
    fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0 || !write_all(fd, contents, (size_t)written) || close(fd) < 0 ||
        rename(temp_path, APP_REQUEST_PATH) < 0) {
        snprintf(error, error_size, T(S_E_REQUEST_RECORD), strerror(errno));
        if (fd >= 0) close(fd);
        (void)unlink(temp_path);
        return false;
    }
    return true;
}

/* Serialize restart generations across HTTP workers. */
static bool publish_request(enum camera_kind kind, unsigned long *generation,
                            char *error, size_t error_size)
{
    int lock = open(APP_REQUEST_LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    bool ok;

    if (lock < 0 || flock(lock, LOCK_EX) < 0) {
        snprintf(error, error_size, T(S_E_REQUEST_LOCK), strerror(errno));
        if (lock >= 0) close(lock);
        return false;
    }
    (void)kind;
    ok = bump_request(generation, error, error_size);
    close(lock);
    return ok;
}

static bool pid_listed(pid_t pid, const pid_t *pids, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (pids[i] == pid) return true;
    }
    return false;
}

/* pid from the "pid=N" line of the ready file camera-app writes */
static pid_t ready_pid(void)
{
    size_t length = 0;
    char *contents = read_file(CAMERA_READY_PATH, 4096, &length);
    long pid = 0;

    if (contents == NULL) return 0;
    for (char *line = contents; line != NULL && *line != '\0';) {
        char *next = strchr(line, '\n');
        if (next != NULL) *next++ = '\0';
        if (strncmp(line, "pid=", 4) == 0) {
            char *end;
            errno = 0;
            pid = strtol(line + 4, &end, 10);
            if (errno != 0 || *end != '\0' || pid <= 1 || pid > INT_MAX) pid = 0;
            break;
        }
        line = next;
    }
    free(contents);
    return (pid_t)pid;
}

static void stop_pids(const pid_t *pids, size_t count)
{
    size_t remaining;

    for (size_t i = 0; i < count; i++) (void)kill(pids[i], SIGTERM);
    for (unsigned attempt = 0; attempt < 150U; attempt++) {
        remaining = 0;
        for (size_t i = 0; i < count; i++) {
            if (kill(pids[i], 0) == 0) remaining++;
        }
        if (remaining == 0) return;
        usleep(100000);
    }
    /* Bound teardown even if the application is unresponsive. */
    for (size_t i = 0; i < count; i++) (void)kill(pids[i], SIGKILL);
    for (unsigned attempt = 0; attempt < 50U; attempt++) {
        remaining = 0;
        for (size_t i = 0; i < count; i++) {
            if (kill(pids[i], 0) == 0) remaining++;
        }
        if (remaining == 0) return;
        usleep(100000);
    }
}

static bool wait_for_camera(enum camera_kind kind, const pid_t *old_pids,
                            size_t old_count, char *error, size_t error_size)
{
    for (unsigned attempt = 0; attempt < 900U; attempt++) {
        usleep(100000);
        pid_t pid = ready_pid();
        if (pid > 0 && !pid_listed(pid, old_pids, old_count) &&
            pid_camera_kind(pid) == CAMERA_REPLACEMENT) return true;
    }
    snprintf(error, error_size, T(S_E_APP_NOT_UP), camera_label(kind));
    return false;
}

/* One restart at a time across HTTP workers. */


static bool restart_camera_locked(enum camera_kind kind, char *error,
                                 size_t error_size)
{
    pid_t old_pids[32];
    size_t old_count;
    unsigned long generation;

    old_count = camera_pids(old_pids, 16);
    if (old_count > 16U) old_count = 16U;
    if (!publish_request(kind, &generation, error, error_size)) return false;
    /* An instance the launcher started for an older request, possibly after
     * the capture above, is stale as well. The launcher records the request
     * it is starting for before it forks, so an instance seen here while
     * that record is older than this request both before and after the
     * scan cannot be the one started for it. */
    if (read_generation(APP_STARTED_PATH) < generation) {
        pid_t now_pids[16];
        size_t now_count = camera_pids(now_pids, 16);
        if (now_count > 16U) now_count = 16U;
        if (read_generation(APP_STARTED_PATH) < generation) {
            for (size_t i = 0; i < now_count; i++) {
                if (!pid_listed(now_pids[i], old_pids, old_count)) {
                    old_pids[old_count++] = now_pids[i];
                }
            }
        }
    }
    stop_pids(old_pids, old_count);
    return wait_for_camera(kind, old_pids, old_count, error, error_size);
}






static bool restart_camera(char *error, size_t error_size)
{
    enum camera_kind kind;
    int lock = lock_restart(error, error_size);
    bool ok;

    if (lock < 0) return false;
    /* Serialize concurrent restart requests. */
    kind = CAMERA_REPLACEMENT;
    ok = restart_camera_locked(kind, error, error_size);
    if (ok) ok = restarted_config_ok(error, error_size);
    close(lock);
    return ok;
}
#endif

static long process_rss_kib(pid_t pid)
{
    char path[64];
    FILE *file;
    char line[256];
    long result = -1;

    snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
    file = fopen(path, "r");
    if (file == NULL) return -1;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (sscanf(line, "VmRSS: %ld kB", &result) == 1) break;
    }
    fclose(file);
    return result;
}

static bool read_memory(long *total_kib, long *available_kib)
{
    FILE *file = fopen("/proc/meminfo", "r");
    char line[256];

    *total_kib = -1;
    *available_kib = -1;
    if (file == NULL) return false;
    while (fgets(line, sizeof(line), file) != NULL) {
        (void)sscanf(line, "MemTotal: %ld kB", total_kib);
        (void)sscanf(line, "MemAvailable: %ld kB", available_kib);
    }
    fclose(file);
    return *total_kib >= 0 && *available_kib >= 0;
}

static bool is_mounted(const char *mountpoint)
{
    FILE *file = fopen("/proc/mounts", "r");
    char device[256];
    char path[256];
    char type[64];
    bool mounted = false;

    if (file == NULL) return false;
    while (fscanf(file, "%255s %255s %63s %*s %*d %*d", device, path, type) == 3) {
        if (strcmp(path, mountpoint) == 0) {
            mounted = true;
            break;
        }
    }
    fclose(file);
    return mounted;
}

static void format_bytes(uint64_t bytes, char output[32])
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = (double)bytes;
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        unit++;
    }
    snprintf(output, 32, value >= 10.0 || unit == 0 ? "%.0f %s" : "%.1f %s",
             value, units[unit]);
}

static void append_filesystem_status(struct string_buffer *page,
                                     const char *label, const char *path)
{
    struct statvfs fs;
    char used[32];
    char total[32];
    unsigned percent = 0;
    bool mounted = strcmp(path, MEDIA_ROOT) != 0 || is_mounted(path);

    snprintf(used, sizeof(used), "%s", T(S_UNAVAILABLE));
    snprintf(total, sizeof(total), "%s", T(S_UNAVAILABLE));

    if (mounted && statvfs(path, &fs) == 0 && fs.f_blocks > 0) {
        uint64_t total_bytes = (uint64_t)fs.f_blocks * fs.f_frsize;
        uint64_t free_bytes = (uint64_t)fs.f_bavail * fs.f_frsize;
        uint64_t used_bytes = total_bytes - free_bytes;
        format_bytes(used_bytes, used);
        format_bytes(total_bytes, total);
        percent = (unsigned)((used_bytes * 100U) / total_bytes);
    }
    sb_appendf(page, "<tr><th>%s</th><td>%s%s / %s (%u%%)</td></tr>",
               label, mounted ? "" : T(S_NOT_MOUNTED), used, total, percent);
}

static void ipv4_addresses(char *output, size_t output_size)
{
    struct ifaddrs *addresses;
    struct ifaddrs *item;
    size_t used = 0;

    output[0] = '\0';
    if (getifaddrs(&addresses) != 0) return;
    for (item = addresses; item != NULL; item = item->ifa_next) {
        char address[INET_ADDRSTRLEN];
        int length;
        if (item->ifa_addr == NULL || item->ifa_addr->sa_family != AF_INET ||
            strcmp(item->ifa_name, "lo") == 0) continue;
        if (inet_ntop(AF_INET, &((struct sockaddr_in *)item->ifa_addr)->sin_addr,
                      address, sizeof(address)) == NULL) continue;
        length = snprintf(output + used, output_size - used, "%s%s=%s",
                          used ? ", " : "", item->ifa_name, address);
        if (length < 0 || (size_t)length >= output_size - used) break;
        used += (size_t)length;
    }
    freeifaddrs(addresses);
}

static const char *file_extension(const char *path)
{
    const char *base = strrchr(path, '/');
    const char *dot;
    if (base == NULL) base = path; else base++;
    dot = strrchr(base, '.');
    return dot != NULL ? dot + 1 : "";
}

static const char *file_mime_type(const char *path)
{
    const char *ext = file_extension(path);
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, "png") == 0) return "image/png";
    if (strcasecmp(ext, "gif") == 0) return "image/gif";
    if (strcasecmp(ext, "webp") == 0) return "image/webp";
    if (strcasecmp(ext, "bmp") == 0) return "image/bmp";
    if (strcasecmp(ext, "mp4") == 0 || strcasecmp(ext, "m4v") == 0) return "video/mp4";
    if (strcasecmp(ext, "webm") == 0) return "video/webm";
    if (strcasecmp(ext, "mov") == 0) return "video/quicktime";
    if (strcasecmp(ext, "mkv") == 0) return "video/x-matroska";
    if (strcasecmp(ext, "avi") == 0) return "video/x-msvideo";
    if (strcasecmp(ext, "txt") == 0 || strcasecmp(ext, "log") == 0 ||
        strcasecmp(ext, "ini") == 0 || strcasecmp(ext, "md") == 0) {
        return "text/plain; charset=utf-8";
    }
    if (strcasecmp(ext, "json") == 0) return "application/json";
    if (strcasecmp(ext, "pdf") == 0) return "application/pdf";
    return "application/octet-stream";
}

static bool mime_is_image(const char *mime)
{
    return strncmp(mime, "image/", 6) == 0;
}

static bool mime_is_video(const char *mime)
{
    return strncmp(mime, "video/", 6) == 0;
}

static bool path_is_below(const char *root, const char *path)
{
    size_t length = strlen(root);
    return strncmp(root, path, length) == 0 && path[length] == '/';
}

static void path_parent(const char *path, char output[PATH_MAX])
{
    char *slash;
    snprintf(output, PATH_MAX, "%s", path);
    while (strlen(output) > 1 && output[strlen(output) - 1] == '/') {
        output[strlen(output) - 1] = '\0';
    }
    slash = strrchr(output, '/');
    if (slash == output) output[1] = '\0';
    else if (slash != NULL) *slash = '\0';
    else snprintf(output, PATH_MAX, "/");
}

static bool canonical_existing_path(const char *path, char resolved[PATH_MAX],
                                    char *error, size_t error_size)
{
    if (path == NULL || path[0] != '/') {
        snprintf(error, error_size, "%s", T(S_E_PATH_ABSOLUTE));
        return false;
    }
    if (realpath(path, resolved) == NULL) {
        snprintf(error, error_size, T(S_E_PATH_RESOLVE), strerror(errno));
        return false;
    }
    return true;
}

static bool path_can_be_deleted(const char *path)
{
    char root[PATH_MAX];
    char resolved[PATH_MAX];
    char parent[PATH_MAX];
    struct stat st;

    if (path == NULL || path[0] != '/' || realpath(MEDIA_ROOT, root) == NULL ||
        lstat(path, &st) < 0) return false;
    if (S_ISLNK(st.st_mode)) {
        path_parent(path, parent);
        if (realpath(parent, resolved) == NULL) return false;
        return strcmp(resolved, root) == 0 || path_is_below(root, resolved);
    }
    if (realpath(path, resolved) == NULL) return false;
    return path_is_below(root, resolved);
}

static int remove_tree_item(const char *path, const struct stat *st,
                            int type, struct FTW *ftw)
{
    (void)st;
    (void)type;
    (void)ftw;
    return remove(path);
}

static bool delete_media_path(const char *path, char *deleted_path,
                              size_t deleted_size, char *error, size_t error_size)
{
    char root[PATH_MAX];
    char resolved[PATH_MAX];
    char parent[PATH_MAX];
    char safe_target[PATH_MAX];
    const char *base;
    struct stat st;
    int result;

    if (path == NULL || path[0] != '/' || realpath(MEDIA_ROOT, root) == NULL ||
        lstat(path, &st) < 0) {
        snprintf(error, error_size, "%s", T(S_E_TARGET_MISSING));
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        path_parent(path, parent);
        base = strrchr(path, '/');
        if (base == NULL || base[1] == '\0' || realpath(parent, resolved) == NULL ||
            !(strcmp(resolved, root) == 0 || path_is_below(root, resolved))) {
            snprintf(error, error_size, T(S_E_DELETE_BENEATH), MEDIA_ROOT);
            return false;
        }
        if (snprintf(safe_target, sizeof(safe_target), "%s/%s", resolved, base + 1) >=
            (int)sizeof(safe_target)) {
            snprintf(error, error_size, "%s", T(S_E_PATH_TOO_LONG));
            return false;
        }
        if (lstat(safe_target, &st) < 0 || !S_ISLNK(st.st_mode)) {
            snprintf(error, error_size, "%s", T(S_E_TARGET_CHANGED));
            return false;
        }
        result = unlink(safe_target);
        snprintf(deleted_path, deleted_size, "%s", safe_target);
    } else {
        if (realpath(path, resolved) == NULL || !path_is_below(root, resolved)) {
            snprintf(error, error_size, T(S_E_DELETE_BENEATH), MEDIA_ROOT);
            return false;
        }
        if (lstat(resolved, &st) < 0) {
            snprintf(error, error_size, "%s", T(S_E_TARGET_CHANGED));
            return false;
        }
        if (S_ISDIR(st.st_mode)) {
            result = nftw(resolved, remove_tree_item, 32,
                          FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
        } else {
            result = unlink(resolved);
        }
        snprintf(deleted_path, deleted_size, "%s", resolved);
    }
    if (result != 0) {
        snprintf(error, error_size, T(S_E_DELETE_FAILED), strerror(errno));
        return false;
    }
    return true;
}

static void append_path_query(struct string_buffer *page, const char *route,
                              const char *path)
{
    sb_append(page, route);
    sb_append_url(page, path);
}

static void format_file_time(time_t value, char output[64])
{
    struct tm local;
    localtime_r(&value, &local);
    strftime(output, 64, "%Y-%m-%d %H:%M:%S", &local);
}

static char file_type_char(mode_t mode)
{
    if (S_ISDIR(mode)) return 'd';
    if (S_ISLNK(mode)) return 'l';
    if (S_ISREG(mode)) return 'f';
    if (S_ISCHR(mode)) return 'c';
    if (S_ISBLK(mode)) return 'b';
    if (S_ISFIFO(mode)) return 'p';
    if (S_ISSOCK(mode)) return 's';
    return '?';
}

static const char *page_style =
    ".parameter-tabs:not([hidden]){display:flex;flex-wrap:wrap;gap:6px;margin:18px 0 12px}"
    ".parameter-tabs button{margin:0;border:2px solid var(--line);background:var(--card);color:var(--accent)}"
    ".parameter-tabs button[aria-selected=true]{border-color:var(--accent);background:var(--accent);color:var(--bg)}"
    ".parameter-tabs button:focus-visible{outline:3px solid var(--accent);outline-offset:3px}"
    ".parameter-tabs button.has-error{border-color:var(--danger)}"
    ".parameter-panel{margin-bottom:12px}.parameter-panel[hidden]{display:none}.parameter-panel h2{margin-top:0}.parameter-panel .help code{overflow-wrap:anywhere}"
        ":root{color-scheme:light dark;--bg:#f4f7fa;--card:#fff;--text:#17212b;"
        "--muted:#607080;--line:#d7e0e8;--accent:#1769aa;--danger:#b42318}"
        "@media(prefers-color-scheme:dark){:root{--bg:#10161d;--card:#18222d;"
        "--text:#e8edf2;--muted:#a7b3bf;--line:#344352;--accent:#77bdf2;--danger:#ff8a80}}"
        "*{box-sizing:border-box}body{display:flex;flex-direction:column;max-width:1050px;margin:auto;padding:24px;"
        "font:15px/1.5 system-ui,sans-serif;color:var(--text);background:var(--bg)}"
        "h1{margin-bottom:4px}h2{margin-top:28px}.muted{color:var(--muted)}"
        ".brand-logo{order:-1;align-self:flex-start;display:inline-flex;align-items:center;width:min(280px,70vw);padding:7px 10px;"
        "border-radius:8px;background:#263746}.brand-logo img{display:block;width:100%;height:auto}"
        "nav{display:flex;flex-wrap:wrap;align-items:center;gap:8px;margin:18px 0}nav a,.nav-link{padding:8px 12px;border:1px solid var(--line);"
        "border-radius:6px;color:var(--accent);text-decoration:none;background:var(--card)}"
        ".nav-form{display:inline}.lang-form{margin-left:auto}.nav-link{margin:0;font:inherit;font-weight:normal;cursor:pointer}"
        "nav select{width:auto;padding:7px 8px;margin:0 4px 0 0}.login-card{max-width:460px}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:16px}"
        ".card{padding:18px;border:1px solid var(--line);border-radius:10px;background:var(--card)}"
        "table{width:100%;border-collapse:collapse}th,td{padding:7px;border-bottom:1px solid var(--line);text-align:left}"
        "textarea{width:100%;min-height:520px;padding:12px;border:1px solid var(--line);border-radius:7px;"
        "background:var(--card);color:var(--text);font:13px/1.45 ui-monospace,monospace;tab-size:4}"
        "input[type=text],input[type=password],input[type=number],select{width:100%;padding:8px;border:1px solid var(--line);"
        "border-radius:6px;background:var(--card);color:var(--text)}"
        ".field [aria-invalid=true]{border:2px solid var(--danger)}"
        ".field-error{color:var(--danger);font-size:13px;margin-top:5px}"
        ".key-input{min-height:110px}.upload-status{display:block;margin-top:8px;white-space:pre-wrap}"
        ".fields{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:16px}"
        ".field label{display:block;font-weight:650;margin-bottom:5px}.help{color:var(--muted);font-size:13px;margin-top:5px}"
        ".checks{display:flex;flex-wrap:wrap;gap:16px}.checks label{white-space:nowrap}"
        "details.card{margin-top:16px}summary{font-size:1.25em;font-weight:650;cursor:pointer}"
        ".actions{position:sticky;bottom:0;padding:10px 0;background:var(--bg)}"
        ".preview{display:block;max-width:100%;max-height:75vh;margin:16px auto;background:#000}"
        ".pathbox{display:flex;gap:8px}.pathbox input{flex:1}.file-actions{white-space:nowrap}"
        ".delete-link{color:var(--danger)}"
        ".sensor-value{font-size:1.35em;font-weight:700}.sensor-control{display:flex;align-items:center;gap:10px}"
        ".sensor-control .compact{flex:none}.gallery{display:grid;"
        "grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:16px}.photo{margin:0}"
        ".photo img{display:block;width:100%;height:180px;object-fit:contain;background:#000;border-radius:6px}"
        ".photo figcaption{margin-top:6px;overflow-wrap:anywhere}"
        ".inline-form{display:inline}.compact{margin:0 0 0 8px;padding:5px 10px}"
        ".live-video{display:block;width:100%;max-height:70vh;background:#000;border-radius:8px}"
        ".ptz-layout{display:grid;grid-template-columns:minmax(260px,1fr) minmax(220px,300px);gap:24px;align-items:center}"
        ".ptz-pad{display:grid;grid-template-columns:repeat(3,74px);justify-content:center;gap:6px}"
        ".ptz-pad button{margin:0;touch-action:none;user-select:none}.zoom-row{display:flex;align-items:center;gap:12px}"
        ".zoom-row input{flex:1}"
        ".attitude-panel{text-align:center}.attitude-panel h3{margin:0 0 10px}.attitude-dial{position:relative;"
        "width:184px;height:184px;margin:auto;overflow:hidden;border:5px solid #607080;border-radius:50%;background:#151b20;box-shadow:inset 0 0 0 2px #111}"
        ".attitude-world{position:absolute;left:-50%;top:-50%;width:200%;height:200%;"
        "background:linear-gradient(to bottom,#4195ce 0 49.5%,#f4f4f4 49.5% 50.5%,#8b613d 50.5% 100%);"
        "transform:translateY(0) rotate(0);transition:transform .16s linear;will-change:transform}"
        ".attitude-reticle{position:absolute;left:50%;top:50%;width:74%;height:2px;transform:translate(-50%,-50%);background:#ffd34d;box-shadow:0 0 2px #222}"
        ".attitude-reticle:before{content:'';position:absolute;left:50%;top:50%;width:11px;height:11px;transform:translate(-50%,-50%);border:2px solid #ffd34d;border-radius:50%;background:transparent}"
        ".attitude-yaw{display:flex;justify-content:center;align-items:center;gap:9px;margin:10px 0 6px}.attitude-heading{display:inline-block;font-size:22px;line-height:1;color:var(--accent);transition:transform .16s linear}"
        ".attitude-values{display:grid;grid-template-columns:repeat(3,1fr);gap:5px}.attitude-values span{font-size:12px;color:var(--muted)}.attitude-values output{display:block;font-size:16px;font-weight:700;color:var(--text)}"
        ".attitude-rates{margin-top:7px;padding-top:7px;border-top:1px solid var(--line)}.attitude-rates output{font-size:14px}"
        "button{margin:8px 8px 0 0;padding:9px 14px;border:0;border-radius:6px;color:white;"
        "background:var(--accent);font-weight:650;cursor:pointer}.danger{background:var(--danger)}"
        ".notice{padding:12px;border-left:5px solid var(--accent);background:var(--card)}"
        ".error{border-color:var(--danger)}code{background:var(--bg);padding:2px 4px;border-radius:3px}"
        "@media(max-width:650px){body{padding:12px}textarea{min-height:420px}.ptz-layout{grid-template-columns:1fr}.attitude-panel{margin-top:8px}}";

static void append_head(struct string_buffer *page, const char *title,
                        const char *style, const char *extra_head)
{
    sb_appendf(page, "<!doctype html><html lang=%s><head><meta charset=utf-8>"
                     "<meta name=viewport content=\"width=device-width,initial-scale=1\">%s<title>",
               languages[current_language].html_lang,
               extra_head != NULL ? extra_head : "");
    sb_append_html(page, title);
    sb_append(page, "</title><link rel=icon href=/favicon.ico sizes=\"16x16 32x32 48x48\">"
                    "<link rel=icon href=/favicon.svg type=\"image/svg+xml\" sizes=any><style>");
    sb_append(page, style);
    sb_append(page, "</style></head><body>");
}

static void append_language_select(struct string_buffer *page, const char *id)
{
    sb_appendf(page, "<select name=lang id=%s aria-label=\"", id);
    sb_append_html(page, T(S_LANGUAGE));
    sb_append(page, "\">");
    for (size_t i = 0; i < LANG_COUNT; i++) {
        sb_appendf(page, "<option value=%s%s>", languages[i].code,
                   i == (size_t)current_language ? " selected" : "");
        sb_append_html(page, strings[S_LANGUAGE_NAME][i]);
        sb_append(page, "</option>");
    }
    sb_append(page, "</select>");
}

static void append_logo(struct string_buffer *page, const char *href)
{
    char label[160];

    snprintf(label, sizeof(label), T(S_NAV_TOP), PRODUCT_NAME);
    sb_appendf(page, "<a class=brand-logo href=\"%s\" aria-label=\"", href);
    sb_append_html(page, label);
    sb_append(page, "\"><img alt=\"ArduPilot\" referrerpolicy=no-referrer src=\""
                    ARDUPILOT_LOGO_URL "\"></a>");
}

/* route and path_argument name the current page so the language form can
 * return to it */
static void append_nav(struct string_buffer *page, const char *route,
                       const char *path_argument)
{
    append_logo(page, "/#top");
    sb_appendf(page, "<nav><a href=\"/\">%s</a><a href=\"/parameters\">%s</a>"
                     "<a href=\"/raw\">%s</a><a href=\"/users\">%s</a><a href=\"/files?path=",
               T(S_NAV_STATUS), T(S_NAV_PARAMETERS), T(S_NAV_RAW), T(S_NAV_USERS));
    sb_append_url(page, MEDIA_ROOT);
    sb_appendf(page, "\">%s</a><a href=\"/live\">%s</a><a href=\"/sensors\">%s</a>"
                     "<a href=\"/log\">%s</a>",
               T(S_NAV_FILES), T(S_NAV_LIVE), T(S_NAV_SENSORS), T(S_NAV_DEBUG));
    sb_appendf(page, "<form class=\"nav-form lang-form\" method=post action=/language>"
                     "<input type=hidden name=csrf value=\"%s\">"
                     "<input type=hidden name=next value=\"", csrf_token);
    sb_append_html(page, route);
    if (path_argument != NULL) {
        sb_append(page, "?path=");
        sb_append_url(page, path_argument);
    }
    sb_append(page, "\">");
    append_language_select(page, "lang-nav");
    sb_appendf(page, "<button class=\"nav-link lang-apply\" type=submit>%s</button></form>"
                     "<form class=nav-form method=post action=/logout>"
                     "<input type=hidden name=csrf value=\"%s\">"
                     "<button class=nav-link type=submit>%s</button></form></nav>"
                     "<script src=/language.js defer></script>",
               T(S_APPLY), csrf_token, T(S_NAV_LOGOUT));
}

/* Match the app acknowledgement to both the saved bytes and running PID. */
static bool live_config_notice(const char *config, size_t length, char *message,
                                size_t capacity, bool *is_error)
{
    char path[sizeof(CAMERA_READY_PATH) + 8], state[16];
    snprintf(path, sizeof(path), "%s.config", CAMERA_READY_PATH);
    size_t size;
    char *status = read_file(path, 1024, &size);
    char *ready = read_file(CAMERA_READY_PATH, 512, &size);
    bool found = false;
    unsigned long long hash;
    long pid, running;
    char *pid_line = ready ? strstr(ready, "\npid=") : NULL;
    char *text = status ? strchr(status, '\n') : NULL;
    if (status && text && pid_line &&
        sscanf(status, "%llx %ld %15s", &hash, &pid, state) == 3 &&
        sscanf(pid_line, "\npid=%ld", &running) == 1 && pid > 1 && pid == running &&
        kill((pid_t)pid, 0) == 0 &&
        hash == apcam_config_hash(config, length, APCAM_CONFIG_HASH_INITIAL)) {
        text++;
        text[strcspn(text, "\r\n")] = 0;
        snprintf(message, capacity, "Parameters saved. %s", text);
        *is_error = strcmp(state, "error") == 0;
        found = true;
    }
    free(status);
    free(ready);
    return found;
}

static bool wait_live_config(const char *config, size_t length, char *message, size_t capacity, bool require_ack)
{
    bool is_error = false;
    for (unsigned attempt = 0; attempt < 30; attempt++) {
        if (live_config_notice(config, length, message, capacity, &is_error)) return is_error;
        if (access(CAMERA_READY_PATH, R_OK) != 0) break;
        usleep(100000);
    }
    snprintf(message, capacity, "%s", require_ack ?
        "Camera restarted, but configuration was not acknowledged. Check the camera app log." :
        "Parameters saved. Live settings await camera app acknowledgement; start the app if it is stopped.");
    return require_ack;
}

/* Read acknowledgement from the new PID, not the previous app instance.
 * A running app is deliberately retained on failure so the user can recover. */
static bool restarted_config_ok(char *error, size_t error_size)
{
    size_t length;
    char *config = read_file(config_path(CAMERA_REPLACEMENT), MAX_CONFIG, &length);
    if (!config) {
        snprintf(error, error_size, "Camera restarted, but configuration status could not be checked: %s", strerror(errno));
        return false;
    }
    bool failed = wait_live_config(config, length, error, error_size, true);
    free(config);
    return !failed;
}

static void append_notice(struct string_buffer *page, const char *message, bool is_error)
{
    char *copy;

    if (message == NULL) return;
    copy = strndup(message, utf8_complete_length(message, strlen(message)));
    if (copy == NULL) return;
    sb_appendf(page, "<p class=\"notice%s\">", is_error ? " error" : "");
    sb_append_html(page, copy);
    sb_append(page, "</p>");
    free(copy);
}

static void append_parameter_field(struct string_buffer *page, const char *config,
                                   const struct parameter *parameter,
                                   const struct request *submitted)
{
    char value[160] = "";
    bool present = ini_get_value(config, parameter->section, parameter->key,
                                 value, sizeof(value));

    if (!present && !strcmp(parameter->section, "network")) {
        char enabled[16] = "";
        ini_get_value(config, "support_proxy", "enabled", enabled, sizeof(enabled));
        if (!strcmp(enabled, "true") || !strcmp(enabled, "1") || !strcmp(enabled, "yes")) {
            const char *legacy = !strcmp(parameter->key, "interface") ? "network_interface" :
                !strcmp(parameter->key, "secondary_address") ? "network_address" :
                !strcmp(parameter->key, "gateway") ? "network_gateway" : NULL;
            if (legacy) present = ini_get_value(config, "support_proxy", legacy, value, sizeof(value));
        }
    }

    if (!present) {
        size_t replacement_count = sizeof(replacement_parameters) /
                                   sizeof(replacement_parameters[0]);
        _Static_assert(sizeof(replacement_defaults) /
                           sizeof(replacement_defaults[0]) ==
                           sizeof(replacement_parameters) /
                           sizeof(replacement_parameters[0]),
                       "replacement defaults must match parameters");
        for (size_t i = 0; i < replacement_count; i++) {
            if (parameter == &replacement_parameters[i]) {
                snprintf(value, sizeof(value), "%s", replacement_defaults[i]);
                present = true;
                break;
            }
        }
    }

    // Keep rejected submissions editable instead of replacing them with the
    // saved defaults. Render directly with HTML escaping, never via INI text.
    size_t submitted_length = 0;
    char *submitted_value = submitted != NULL
        ? form_value(submitted, parameter->form_name, &submitted_length) : NULL;
    const char *display_value = submitted_value != NULL ? submitted_value : value;
    if (submitted_value != NULL) present = true;

    sb_append(page, "<div class=field><label for=\"");
    sb_append_html(page, parameter->form_name);
    sb_append(page, "\">");
    sb_append_html(page, T(parameter->label));
    sb_append(page, "</label>");
    if (parameter->kind == PARAM_ENUM || parameter->kind == PARAM_BOOLEAN) {
        sb_append(page, "<select required name=\"");
        sb_append_html(page, parameter->form_name);
        sb_append(page, "\" id=\"");
        sb_append_html(page, parameter->form_name);
        sb_append(page, "\">");
        if (!present || !option_contains(parameter, display_value)) {
            sb_append(page, "<option selected disabled value=\"\">");
            sb_append_html(page, T(S_UNKNOWN_CURRENT_VALUE));
            sb_append(page, "</option>");
        }
        for (size_t i = 0; i < parameter->option_count; i++) {
            sb_append(page, "<option value=\"");
            sb_append_html(page, parameter->options[i].value);
            sb_appendf(page, "\"%s>", present && strcmp(display_value, parameter->options[i].value) == 0 ? " selected" : "");
            sb_append_html(page, T(parameter->options[i].label));
            sb_append(page, "</option>");
        }
        sb_append(page, "</select>");
    } else {
        bool optional = (parameter->kind == PARAM_TEXT || parameter->kind == PARAM_PASSWORD) &&
                        parameter->minimum == 0;
        sb_append(page, optional ? "<input name=\"" : "<input required name=\"");
        sb_append_html(page, parameter->form_name);
        sb_append(page, "\" id=\"");
        sb_append_html(page, parameter->form_name);
        if (parameter->kind == PARAM_IPV4) {
            sb_append(page, "\" type=text inputmode=numeric value=\"");
        } else if (parameter->kind == PARAM_TEXT || parameter->kind == PARAM_PASSWORD) {
            sb_appendf(page, "\" type=%s autocomplete=off minlength=\"%.0f\" maxlength=\"%.0f\"",
                       parameter->kind == PARAM_PASSWORD ? "password" : "text",
                       parameter->minimum, parameter->maximum);
            if (!strcmp(parameter->form_name, "network_primary_address") || !strcmp(parameter->form_name, "network_secondary_address")) {
                sb_append(page, " pattern=\"[0-9]{1,3}(\\.[0-9]{1,3}){3}/([1-9]|[12][0-9]|3[0-2])\" title=\"");
                sb_append_html(page, T(parameter->help));
                sb_append(page, "\"");
            }
            sb_append(page, " value=\"");
        } else {
            sb_appendf(page, "\" type=number min=\"%.8g\" max=\"%.8g\" step=\"%.8g\" value=\"",
                       parameter->minimum, parameter->maximum, parameter->step);
        }
        if (present) sb_append_html(page, display_value);
        sb_append(page, "\">");
    }
    free(submitted_value);
    sb_append(page, "<div class=help>");
    sb_append_html(page, T(parameter->help));
    if (strcmp(parameter->section, "support_proxy") == 0 ||
        strcmp(parameter->section, "network") == 0 ||
        strcmp(parameter->form_name, "orientation") == 0 ||
        strcmp(parameter->form_name, "uart_protocol") == 0 ||
        strcmp(parameter->form_name, "mavlink_system_id") == 0 ||
        strcmp(parameter->form_name, "mavlink_camera_component_id") == 0 ||
        strcmp(parameter->form_name, "mavlink_tcp_port") == 0 ||
        strcmp(parameter->form_name, "mavlink_udp_port") == 0) {
        sb_append(page, " ");
        sb_append_html(page, T(S_RESTART_REQUIRED));
    }
    sb_appendf(page, " <code>[%s] %s</code></div></div>", parameter->section, parameter->key);
}


#if WEB_HAVE_SOC_TEMPERATURE
static bool read_soc_temperature(int *millidegrees)
{
#if APCAM_TARGET == APCAM_TARGET_Z1_MINI
    FILE *file = fopen(SOC_TEMPERATURE_PATH, "r");
    if (!file) return false;
    char text[64], extra;
    long value;
    bool ok = fgets(text, sizeof(text), file) != NULL &&
              sscanf(text, "%ld %c", &value, &extra) == 1 &&
              value >= -40000 && value <= 150000;
    fclose(file);
    if (ok) *millidegrees = (int)value;
    return ok;
#else
    int fd = -1;
    void *mapping = MAP_FAILED;
    int64_t total = 0;
    bool ok = false;

    fd = open(SOC_TSENSOR_DEVICE, O_RDONLY | O_CLOEXEC | O_SYNC);
    if (fd < 0) goto done;
    mapping = mmap(NULL, SOC_TSENSOR_MAP_SIZE, PROT_READ, MAP_SHARED, fd,
                   (off_t)SOC_TSENSOR_MAP_OFFSET);
    if (mapping == MAP_FAILED) goto done;

    /* SS928V100 boot SDK get_temperature(): three 10-bit on-die channels,
     * T = ((raw - 146) / 718) * 165 C - 40 C. */
    for (size_t channel = 0; channel < SOC_TSENSOR_COUNT; channel++) {
        size_t offset = SOC_TSENSOR_STATUS0 + channel * SOC_TSENSOR_STRIDE;
        uint32_t raw = *(volatile const uint32_t *)((const uint8_t *)mapping + offset);
        raw &= 0x3ffU;
        if (raw < 100U || raw > 900U) goto done;
        total += ((int64_t)raw - 146) * 165000 / 718 - 40000;
    }
    *millidegrees = (int)(total / SOC_TSENSOR_COUNT);
    ok = true;

done:
    if (mapping != MAP_FAILED) munmap(mapping, SOC_TSENSOR_MAP_SIZE);
    if (fd >= 0) close(fd);
    return ok;
#endif
}
#endif


#if APCAM_TARGET == APCAM_TARGET_A8
/* CPU busy percentage over a short /proc/stat sample; the load average is
 * inflated by the media driver's uninterruptible kernel threads */
static bool read_cpu_busy(unsigned *percent, unsigned *cpus)
{
    unsigned long long sample[2][2];

    for (unsigned i = 0; i < 2U; i++) {
        FILE *file = fopen("/proc/stat", "r");
        unsigned long long user, nice, system, idle, iowait, irq, softirq;
        char line[256];
        if (file == NULL) return false;
        if (fgets(line, sizeof(line), file) == NULL ||
            sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu", &user, &nice,
                   &system, &idle, &iowait, &irq, &softirq) != 7) {
            fclose(file);
            return false;
        }
        *cpus = 0;
        while (fgets(line, sizeof(line), file) != NULL) {
            if (strncmp(line, "cpu", 3) == 0 && isdigit((unsigned char)line[3])) {
                (*cpus)++;
            }
        }
        fclose(file);
        sample[i][0] = user + nice + system + irq + softirq;
        sample[i][1] = sample[i][0] + idle + iowait;
        if (i == 0U) usleep(200000);
    }
    if (sample[1][1] <= sample[0][1]) return false;
    *percent = (unsigned)((sample[1][0] - sample[0][0]) * 100U /
                          (sample[1][1] - sample[0][1]));
    return true;
}
#endif

static char *render_page(const char *message, bool message_is_error, size_t *page_len)
{
    struct string_buffer page;
    char title[96];
    char now_text[64];
    char uptime_text[64];
    char load_text[128];
    char addresses[512];
    time_t now = time(NULL);
    struct tm tm_now;
    double uptime = 0.0;
    FILE *file;
    long mem_total = -1;
    long mem_available = -1;
    pid_t pids[8];
    size_t pid_count = camera_pids(pids, sizeof(pids) / sizeof(pids[0]));
    enum camera_kind active_kind = current_camera_kind();
    long camera_rss = pid_count > 0 ? process_rss_kib(pids[0]) : -1;
    long web_rss = process_rss_kib(getpid());
#if WEB_HAVE_SOC_TEMPERATURE
    int soc_temperature = 0;
    bool have_soc_temperature = read_soc_temperature(&soc_temperature);
#endif

    snprintf(uptime_text, sizeof(uptime_text), "%s", T(S_UNAVAILABLE));
    snprintf(load_text, sizeof(load_text), "%s", T(S_UNAVAILABLE));
    localtime_r(&now, &tm_now);
    strftime(now_text, sizeof(now_text), "%Y-%m-%d %H:%M:%S %Z", &tm_now);
    file = fopen("/proc/uptime", "r");
    if (file != NULL) {
        if (fscanf(file, "%lf", &uptime) == 1) {
            snprintf(uptime_text, sizeof(uptime_text), T(S_STATUS_UPTIME_FORMAT),
                     (unsigned)(uptime / 86400), (unsigned)(uptime / 3600) % 24,
                     (unsigned)(uptime / 60) % 60, (unsigned)uptime % 60);
        }
        fclose(file);
    }
    file = fopen("/proc/loadavg", "r");
    if (file != NULL) {
        if (fgets(load_text, sizeof(load_text), file) != NULL) {
            load_text[strcspn(load_text, "\r\n")] = '\0';
        }
        fclose(file);
    }
    (void)read_memory(&mem_total, &mem_available);
    ipv4_addresses(addresses, sizeof(addresses));

    sb_init(&page);
    snprintf(title, sizeof(title), T(S_TITLE_CONTROL), PRODUCT_NAME);
    append_head(&page, title, page_style, NULL);
    sb_append(&page, "<header id=top><h1>");
    sb_append_html(&page, title);
    sb_appendf(&page, "</h1><div class=muted>%s</div></header>", T(S_STATUS_SUBTITLE));
    append_nav(&page, "/", NULL);
    append_notice(&page, message, message_is_error);
    sb_appendf(&page, "<div class=grid><section class=card><h2>%s</h2><table>",
               T(S_STATUS_HEADING));
    sb_appendf(&page, "<tr><th>%s</th><td id=firmware-version>", T(S_STATUS_FIRMWARE_VERSION));
    sb_append_html(&page, CA_FIRMWARE_VERSION);
    sb_append(&page, " (<span id=firmware-git-hash>");
    sb_append_html(&page, CA_FIRMWARE_GIT_HASH);
    sb_append(&page, "</span>)</td></tr>");
    sb_appendf(&page, "<tr><th>%s</th><td>%s", T(S_STATUS_CAMERA_APP),
               pid_count ? camera_label(active_kind) : T(S_APP_STOPPED));
    if (pid_count) sb_appendf(&page, T(S_STATUS_PID_RSS), (long)pids[0], camera_rss);
    sb_append(&page, "</td></tr>");
    sb_appendf(&page, "<tr><th>%s</th><td>", T(S_STATUS_WEB_SERVICE));
    sb_appendf(&page, T(S_STATUS_WEB_PID_RSS), (long)getpid(), web_rss);
    sb_appendf(&page, "</td></tr><tr><th>%s</th><td>%s"
                      "<form id=time-sync class=inline-form method=post action=/time/sync>"
                      "<input type=hidden name=csrf value=\"%s\">"
                      "<input id=browser-time-ms type=hidden name=time_ms>"
                      "<button id=sync-time class=compact type=submit>%s</button></form>"
                      "</td></tr>", T(S_STATUS_TIME), now_text, csrf_token,
               T(S_STATUS_SYNC));
    sb_appendf(&page, "<tr><th>%s</th><td>%s</td></tr>", T(S_STATUS_UPTIME), uptime_text);
#if APCAM_TARGET == APCAM_TARGET_A8
    {
        unsigned busy = 0, cpus = 0;
        sb_appendf(&page, "<tr><th>%s</th><td>", T(S_STATUS_CPU));
        if (read_cpu_busy(&busy, &cpus)) {
            sb_appendf(&page, T(S_STATUS_CPU_BUSY), busy, cpus,
                       (int)strcspn(load_text, " "), load_text);
        } else {
            sb_append(&page, T(S_UNAVAILABLE));
        }
        sb_append(&page, "</td></tr>");
    }
#else
    sb_appendf(&page, "<tr><th>%s</th><td>%s</td></tr>", T(S_STATUS_LOAD), load_text);
#endif
#if WEB_HAVE_SOC_TEMPERATURE
    sb_appendf(&page, "<tr><th>%s</th><td>", T(S_STATUS_SOC_TEMPERATURE));
    if (have_soc_temperature) {
        int absolute = soc_temperature < 0 ? -soc_temperature : soc_temperature;
        sb_appendf(&page, T(S_STATUS_SOC_AVERAGE),
                   soc_temperature < 0 ? "-" : "", absolute / 1000,
                   (absolute % 1000) / 100);
    } else {
        sb_append(&page, T(S_UNAVAILABLE));
    }
    sb_append(&page, "</td></tr>");
#endif
    sb_appendf(&page, "<tr><th>%s</th><td>", T(S_STATUS_MEMORY));
    sb_appendf(&page, T(S_STATUS_MEMORY_VALUE), mem_available / 1024, mem_total / 1024);
    sb_appendf(&page, "</td></tr><tr><th>%s</th><td>%s</td></tr>", T(S_STATUS_IPV4),
               addresses[0] ? addresses : T(S_UNAVAILABLE));
#if APCAM_TARGET == APCAM_TARGET_A8
    /* the root is an initramfs without size accounting; the RAM store that
     * matters is the tmpfs holding the logs */
    append_filesystem_status(&page, T(S_STORAGE_TMPFS), RUNTIME_DIR);
#else
    append_filesystem_status(&page, T(S_STORAGE_ROOTFS), "/");
#endif
    append_filesystem_status(&page, T(S_STORAGE_APPLICATION), APP_STORAGE_PATH);
#ifdef SETTINGS_STORAGE_PATH
    append_filesystem_status(&page, T(S_STORAGE_SETTINGS), SETTINGS_STORAGE_PATH);
#endif
    append_filesystem_status(&page, T(S_STORAGE_MICROSD), MEDIA_ROOT);
    sb_appendf(&page, "</table><script src=/status.js defer></script>"
                      "<p><a href=\"/\">%s</a></p></section>"
                      "<section class=card><h2>%s</h2>"
                      "<form method=post action=/restart><input type=hidden name=csrf value=\"%s\">"
                      "<button type=submit>", T(S_STATUS_REFRESH), T(S_STATUS_ACTIONS),
               csrf_token);
    sb_append(&page, T(S_STATUS_RESTART));
    sb_append(&page, "</button></form>");
    sb_appendf(&page, "<form id=firmware-upload method=post action=/upgrade>"
                      "<input id=firmware type=file accept=" FIRMWARE_SUFFIX " hidden>"
                      "<button id=select-firmware type=button>%s</button>"
                      "<progress id=firmware-progress value=0 max=100 hidden></progress>"
                      "<output id=firmware-status class=upload-status></output></form>"
                      "<script src=/upgrade.js data-csrf=\"%s\" defer></script>"
                      "<p class=muted>", T(S_STATUS_UPGRADE_BUTTON), csrf_token);
    sb_appendf(&page, T(S_STATUS_UPGRADE_HELP), FIRMWARE_NAME_PATTERNS);
#if WEB_INSTALLS_FIRMWARE
    sb_append(&page, T(S_STATUS_UPGRADE_INSTALL_Z1));
#elif APCAM_TARGET == APCAM_TARGET_MT11
    sb_append(&page, T(S_STATUS_UPGRADE_SYNC_MT11));
#else
    sb_appendf(&page, T(S_STATUS_UPGRADE_SYNC_A8), FIRMWARE_INSTALL_NAME);
#endif
    sb_append(&page, "</p>");
    sb_appendf(&page, "<form method=post action=/reboot><input type=hidden name=csrf value=\"%s\">"
                      "<label><input type=checkbox name=confirm value=yes required> %s</label><br>"
                      "<button class=danger type=submit>%s</button></form><p class=muted>",
               csrf_token, T(S_STATUS_REBOOT_CONFIRM), T(S_STATUS_REBOOT_BUTTON));
    sb_appendf(&page, T(S_STATUS_AUTH_NOTE), PASSWORD_PATH);
    sb_append(&page, "</p></section></div></body></html>");
    *page_len = page.len;
    return page.data;
}

#define RECENT_PHOTO_LIMIT 12U

struct recent_photo {
    char path[PATH_MAX];
    struct timespec modified;
    off_t size;
};

struct recent_photo_list {
    struct recent_photo items[RECENT_PHOTO_LIMIT];
    size_t count;
};

static struct recent_photo_list *photo_collection;

static bool photo_is_newer(const struct stat *candidate,
                           const struct recent_photo *existing)
{
    return candidate->st_mtim.tv_sec > existing->modified.tv_sec ||
           (candidate->st_mtim.tv_sec == existing->modified.tv_sec &&
            candidate->st_mtim.tv_nsec > existing->modified.tv_nsec);
}

static int collect_recent_photo(const char *path, const struct stat *st,
                                int type, struct FTW *walk)
{
    size_t position;

    (void)walk;
    if (photo_collection == NULL || type != FTW_F || st == NULL ||
        !mime_is_image(file_mime_type(path)) || strlen(path) >= PATH_MAX) {
        return 0;
    }
    position = photo_collection->count;
    if (position > RECENT_PHOTO_LIMIT) position = RECENT_PHOTO_LIMIT;
    while (position > 0U &&
           photo_is_newer(st, &photo_collection->items[position - 1U])) {
        position--;
    }
    if (position >= RECENT_PHOTO_LIMIT) return 0;
    if (photo_collection->count < RECENT_PHOTO_LIMIT) {
        photo_collection->count++;
    }
    for (size_t index = photo_collection->count - 1U; index > position;
         index--) {
        photo_collection->items[index] = photo_collection->items[index - 1U];
    }
    snprintf(photo_collection->items[position].path,
             sizeof(photo_collection->items[position].path), "%s", path);
    photo_collection->items[position].modified = st->st_mtim;
    photo_collection->items[position].size = st->st_size;
    return 0;
}

static void find_recent_photos(struct recent_photo_list *photos)
{
    memset(photos, 0, sizeof(*photos));
    photo_collection = photos;
    (void)nftw(CAPTURE_ROOT, collect_recent_photo, 16, FTW_PHYS | FTW_MOUNT);
    photo_collection = NULL;
}

static char *render_sensors_page(const char *message, bool message_is_error,
                                 size_t *page_len)
{
    struct string_buffer page;
    struct recent_photo_list photos;
    char title[96];

    find_recent_photos(&photos);
    sb_init(&page);
    snprintf(title, sizeof(title), T(S_TITLE_SENSORS), PRODUCT_NAME);
    append_head(&page, title, page_style, NULL);
    sb_appendf(&page, "<header id=top><h1>%s</h1><div class=muted>%s</div></header>",
               T(S_NAV_SENSORS),
               T(TARGET_TEXT(S_SENSORS_SUBTITLE_MT11, S_SENSORS_SUBTITLE_A8)));
    append_nav(&page, "/sensors", NULL);
    append_notice(&page, message, message_is_error);
    sb_append(&page, "<div class=grid>");
#if WEB_HAVE_THERMAL
    sb_appendf(&page, "<section class=card><h2>%s</h2><table>"
                      "<tr><th>%s</th><td><div class=sensor-control>"
                      "<span id=lidar class=sensor-value>%s</span>"
                      "<button id=lidar-toggle class=compact type=button disabled>%s</button>"
                      "</div></td></tr>"
                      "<tr><th>%s</th><td id=thermal-min class=sensor-value>%s</td></tr>"
                      "<tr><th>%s</th><td id=thermal-max class=sensor-value>%s</td></tr>"
                      "<tr><th>%s</th><td id=cpu-temperature class=sensor-value>%s</td></tr>"
                      "</table><p id=sensor-status class=muted>%s</p>"
                      "<p class=notice>%s</p></section>",
               T(S_SENSORS_LIVE), T(S_SENSORS_LIDAR), T(S_LOADING), T(S_ENABLE),
               T(S_SENSORS_MIN), T(S_LOADING), T(S_SENSORS_MAX), T(S_LOADING),
               T(S_SENSORS_CPU), T(S_LOADING), T(S_SENSORS_UPDATING),
               T(S_SENSORS_LASER_NOTICE));
#endif
    sb_appendf(&page, "<section class=card><h2>%s</h2><p>%s</p>"
                      "<form method=post action=/sensors/capture><input type=hidden name=csrf value=\"%s\">"
                      "<button type=submit>%s</button></form>",
               T(S_SENSORS_SHUTTER), T(S_SENSORS_SHUTTER_TEXT), csrf_token,
               T(S_SENSORS_CAPTURE));
#if WEB_HAVE_THERMAL
    sb_appendf(&page, "<p class=muted>%s</p>", T(S_SENSORS_SCOPE_NOTE));
#endif
    sb_appendf(&page, "</section></div><section class=card><h2>%s</h2>", T(S_SENSORS_RECENT));
    if (photos.count == 0U) {
        sb_append(&page, "<p>");
        sb_appendf(&page, T(S_SENSORS_NO_PHOTOS), CAPTURE_ROOT);
        sb_append(&page, "</p>");
    } else {
        sb_append(&page, "<div class=gallery>");
        for (size_t index = 0; index < photos.count; index++) {
            const struct recent_photo *photo = &photos.items[index];
            char time_text[64];
            char size_text[32];
            const char *name = strrchr(photo->path, '/');
            format_file_time(photo->modified.tv_sec, time_text);
            format_bytes((uint64_t)photo->size, size_text);
            if (name == NULL) name = photo->path;
            else name++;
            sb_append(&page, "<figure class=photo><a href=\"");
            append_path_query(&page, "/view?path=", photo->path);
            sb_append(&page, "\"><img loading=lazy alt=\"");
            sb_append_html(&page, name);
            sb_append(&page, "\" src=\"");
            append_path_query(&page, "/file?path=", photo->path);
            sb_append(&page, "\"></a><figcaption><strong>");
            sb_append_html(&page, name);
            sb_append(&page, "</strong><br><span class=muted>");
            sb_append_html(&page, time_text);
            sb_append(&page, " &middot; ");
            sb_append_html(&page, size_text);
            sb_append(&page, "</span></figcaption></figure>");
        }
        sb_append(&page, "</div>");
    }
    sb_append(&page, "<p><a href=\"/files?path=");
    sb_append_url(&page, CAPTURE_ROOT);
    sb_appendf(&page, "\">%s</a></p></section>", T(S_SENSORS_BROWSE));
#if WEB_HAVE_THERMAL
    sb_appendf(&page, "<script src=/sensors.js data-csrf=\"%s\" defer></script>", csrf_token);
#endif
    sb_append(&page, "</body></html>");
    *page_len = page.len;
    return page.data;
}

static char *render_live_page(size_t *page_len)
{
    struct string_buffer page;
    char title[96];

    sb_init(&page);
    snprintf(title, sizeof(title), T(S_TITLE_LIVE), PRODUCT_NAME);
    append_head(&page, title, page_style, NULL);
    sb_appendf(&page, "<header id=top><h1>%s</h1><div class=muted>%s</div></header>",
               T(S_LIVE_HEADING), T(S_LIVE_SUBTITLE));
    append_nav(&page, "/live", NULL);
    sb_appendf(&page, "<section class=card><div class=field><label for=live-stream>%s</label>"
                      "<select id=live-stream><option value=0>%s</option>"
                      "<option value=1>%s</option></select></div>"
                      "<p id=live-status class=muted>%s</p>"
                      "<video id=live-video class=live-video muted autoplay playsinline controls></video>"
                      "<p class=help>%s</p>"
                      "</section><section class=card><h2>%s</h2>"
                      "<label><input id=manual-enable type=checkbox> %s</label>"
                      "<p class=notice>%s</p>",
               T(S_LIVE_STREAM), T(S_LIVE_MAIN), T(S_LIVE_SECONDARY), T(S_LIVE_STARTING),
               T(S_LIVE_HELP), T(S_LIVE_PTZ), T(S_LIVE_ENABLE_MANUAL), T(S_LIVE_MANUAL_NOTICE));
    sb_appendf(&page, "<div class=ptz-layout><div><div class=ptz-pad><span></span><button type=button data-direction=up>&uarr;</button><span></span>"
                      "<button type=button data-direction=left>&larr;</button>"
                      "<button type=button id=live-center>%s</button>"
                      "<button type=button data-direction=right>&rarr;</button>"
                      "<span></span><button type=button data-direction=down>&darr;</button><span></span></div>"
                      "<div class=zoom-row><label for=live-rate>%s</label>"
                      "<input id=live-rate type=range min=5 max=60 step=1 value=30 disabled>"
                      "<output id=live-rate-value>30&deg;/s</output></div>"
                      "<div class=zoom-row><label for=live-zoom>%s</label>"
                      "<input id=live-zoom type=range min=1 max=%g step=.1 value=1 disabled>"
                      "<output id=live-zoom-value>1.0x</output></div>"
                      "<p id=control-status class=muted>%s</p></div>"
                      "<aside class=attitude-panel aria-labelledby=attitude-title><h3 id=attitude-title>%s</h3>"
                      "<div id=attitude-dial class=attitude-dial role=img aria-label=\"%s\">"
                      "<div id=attitude-world class=attitude-world></div><div class=attitude-reticle></div></div>"
                      "<div class=attitude-yaw><span>%s</span><span id=attitude-heading class=attitude-heading aria-hidden=true>&uarr;</span></div>"
                      "<div class=attitude-values><span>%s<output id=attitude-roll>&mdash;</output></span>"
                      "<span>%s<output id=attitude-pitch>&mdash;</output></span>"
                      "<span>%s<output id=attitude-yaw>&mdash;</output></span></div>"
                      "<div class=\"attitude-values attitude-rates\"><span>%s<output id=attitude-roll-rate>&mdash;</output></span>"
                      "<span>%s<output id=attitude-pitch-rate>&mdash;</output></span>"
                      "<span>%s<output id=attitude-yaw-rate>&mdash;</output></span></div>"
                      "<p id=attitude-status class=muted>%s</p></aside></div></section>"
                      "<script src=/live.js data-csrf=\"%s\" defer></script></body></html>",
               T(S_LIVE_CENTRE), T(S_LIVE_RATE), T(S_LIVE_ZOOM), (double)APCAM_ZOOM_CONTROL_MAX,
               T(S_LIVE_NO_COMMANDS), T(S_LIVE_ATTITUDE),
               T(S_LIVE_ATTITUDE_UNAVAILABLE), T(S_LIVE_YAW), T(S_LIVE_ROLL), T(S_LIVE_PITCH),
               T(S_LIVE_YAW), T(S_LIVE_ROLL_RATE), T(S_LIVE_PITCH_RATE),
               T(S_LIVE_YAW_RATE), T(S_LIVE_WAITING_GIMBAL), csrf_token);
#if !APCAM_HAVE_ZOOM
    sb_append(&page, "<style>.zoom-row:has(#live-zoom){display:none}</style>");
#endif
#if !APCAM_HAVE_GIMBAL_RATES
    sb_append(&page, "<style>.attitude-rates{display:none}</style>");
#endif
    *page_len = page.len;
    return page.data;
}

static char *render_users_page(const char *message, bool message_is_error,
                               size_t *page_len)
{
    struct string_buffer page;
    char title[96];
#if WEB_HAVE_SSH_KEYS
    size_t keys_len = 0;
    char *keys = read_authorized_keys(&keys_len);
    size_t key_count = 0;
    size_t unmanaged_count = 0;
#endif

    sb_init(&page);
    snprintf(title, sizeof(title), T(S_TITLE_USERS), PRODUCT_NAME);
    append_head(&page, title, page_style, NULL);
    sb_appendf(&page, "<header id=top><h1>%s</h1><div class=muted>%s</div></header>",
               T(S_USERS_HEADING),
               T(TARGET_TEXT(S_USERS_SUBTITLE_MT11, S_USERS_SUBTITLE_A8)));
    append_nav(&page, "/users", NULL);
    append_notice(&page, message, message_is_error);
    sb_appendf(&page, "<div class=grid><section class=card><h2>%s</h2><p>%s</p>"
                      "<form method=post action=/users/password><input type=hidden name=csrf value=\"%s\">"
                      "<div class=field><label for=password>%s</label>"
                      "<input id=password name=password type=password "
                      "autocomplete=new-password required></div>"
                      "<div class=field><label for=confirmation>%s</label>"
                      "<input id=confirmation name=confirmation type=password "
                      "autocomplete=new-password required></div>"
                      "<button type=submit>%s</button></form>"
                      "<p class=notice>%s</p></section>",
               T(S_USERS_PASSWORD_HEADING), T(S_USERS_PASSWORD_TEXT), csrf_token,
               T(S_USERS_NEW_PASSWORD), T(S_USERS_CONFIRM_PASSWORD),
               T(S_USERS_CHANGE_BUTTON), T(S_USERS_HTTP_NOTICE));
#if WEB_HAVE_SSH_KEYS
    sb_appendf(&page, "<section class=card><h2>%s</h2>"
                      "<form id=ssh-key-upload method=post action=/users/keys/add>"
                      "<input type=hidden name=csrf value=\"%s\">"
                      "<input id=public-key-files type=file accept=.pub multiple hidden>"
                      "<input id=public-key-data type=hidden name=public_key>"
                      "<button id=select-public-key-files type=button>%s</button>"
                      "<output id=public-key-status class=upload-status></output></form>"
                      "<script src=/users.js defer></script>"
                      "<p class=muted>%s</p>"
                      "</section></div><section class=card><h2>%s</h2>",
               T(S_USERS_ADD_KEYS), csrf_token, T(S_USERS_ADD_KEYS_BUTTON),
               T(S_USERS_ADD_KEYS_HELP), T(S_USERS_AUTHORIZED));
    if (keys == NULL) {
        sb_appendf(&page, "<p class=\"notice error\">%s</p>", T(S_USERS_KEYS_UNREADABLE));
    } else {
        sb_appendf(&page, "<table><thead><tr><th>%s</th><th>%s</th><th>%s</th><th>%s</th>"
                          "</tr></thead><tbody>", T(S_USERS_KEY_TYPE), T(S_USERS_KEY),
                   T(S_USERS_KEY_COMMENT), T(S_USERS_KEY_ACTION));
        for (size_t offset = 0; offset < keys_len;) {
            size_t line_end = offset;
            struct public_key_view key;
            while (line_end < keys_len && keys[line_end] != '\n') line_end++;
            if (parse_public_key(keys + offset, line_end - offset, &key)) {
                size_t preview = key.blob_len < 24 ? key.blob_len : 24;
                key_count++;
                sb_append(&page, "<tr><td><code>");
                sb_append_html_n(&page, key.type, key.type_len);
                sb_append(&page, "</code></td><td><code>");
                sb_append_html_n(&page, key.blob, preview);
                if (preview < key.blob_len) sb_append(&page, "&hellip;");
                sb_append(&page, "</code></td><td>");
                if (key.comment_len > 0) sb_append_html_n(&page, key.comment, key.comment_len);
                else sb_appendf(&page, "<span class=muted>%s</span>", T(S_USERS_KEY_NO_COMMENT));
                sb_appendf(&page, "</td><td><form method=post action=/users/keys/remove>"
                                  "<input type=hidden name=csrf value=\"%s\">"
                                  "<input type=hidden name=public_key value=\"", csrf_token);
                sb_append_html_n(&page, key.type, key.type_len);
                sb_append(&page, " ");
                sb_append_html_n(&page, key.blob, key.blob_len);
                sb_appendf(&page, "\"><label><input type=checkbox name=confirm value=yes required> "
                                  "%s</label><button class=danger type=submit>%s</button>"
                                  "</form></td></tr>", T(S_USERS_KEY_CONFIRM), T(S_USERS_KEY_REMOVE));
            } else {
                size_t start = offset;
                while (start < line_end && isspace((unsigned char)keys[start])) start++;
                if (start < line_end && keys[start] != '#') unmanaged_count++;
            }
            offset = line_end + (line_end < keys_len ? 1 : 0);
        }
        sb_append(&page, "</tbody></table>");
        if (key_count == 0) sb_appendf(&page, "<p>%s</p>", T(S_USERS_NO_KEYS));
        if (unmanaged_count == 1) {
            sb_appendf(&page, "<p class=\"notice error\">%s</p>", T(S_USERS_UNMANAGED_ONE));
        } else if (unmanaged_count > 1) {
            sb_append(&page, "<p class=\"notice error\">");
            sb_appendf(&page, T(S_USERS_UNMANAGED_MANY), unmanaged_count);
            sb_append(&page, "</p>");
        }
    }
    sb_appendf(&page, "<p class=notice>%s</p></section>", T(S_USERS_LAST_KEY_NOTICE));
    free(keys);
#else
    sb_append(&page, "</div>");
#endif
    sb_append(&page, "</body></html>");
    *page_len = page.len;
    return page.data;
}

static char *render_parameter_page(const char *message, bool message_is_error,
                                   const struct request *submitted, size_t *page_len)
{
    struct string_buffer page;
    enum camera_kind kind = configuration_camera_kind();
    size_t config_len = 0;
    char *config;

    if (kind == CAMERA_NONE) kind = DEFAULT_CAMERA_KIND;
    config = read_file(config_path(kind), MAX_CONFIG, &config_len);

    if (config == NULL) config = strdup("");
    if (config == NULL) return NULL;

    sb_init(&page);
    if (kind == CAMERA_REPLACEMENT) {
        append_head(&page, T(S_TITLE_APP_PARAMETERS), page_style, NULL);
        sb_appendf(&page, "<header><h1>%s</h1><div class=muted>", T(S_TITLE_APP_PARAMETERS));
        sb_append_html(&page, T(S_APP_PARAMETERS_SUBTITLE));
        sb_append(&page, "</div></header>");
        append_nav(&page, "/parameters", NULL);
        char applied[512];
        if (message == NULL && live_config_notice(config, config_len, applied, sizeof(applied), &message_is_error))
            message = applied;
        append_notice(&page, message, message_is_error);
        sb_appendf(&page, "<p class=notice>%s</p>"
                          "<form method=post action=/parameters><input type=hidden name=csrf value=\"%s\">"
                          "<div id=parameter-tabs class=parameter-tabs role=tablist aria-label=\"%s\" hidden>",
                   T(S_APP_PARAMETERS_NOTICE), csrf_token, T(S_PARAMS_CATEGORIES));
        for (unsigned tab = 0; tab < TAB_COUNT; tab++) {
            sb_appendf(&page, "<button type=button role=tab id=tab-%s aria-controls=parameters-%s "
                             "aria-selected=false tabindex=-1>%s</button>",
                       parameter_tabs[tab].name, parameter_tabs[tab].name, T(parameter_tabs[tab].label));
        }
        sb_append(&page, "</div>");
        for (unsigned tab = 0; tab < TAB_COUNT; tab++) {
            sb_appendf(&page, "<section class=\"card parameter-panel\" id=parameters-%s "
                             "aria-labelledby=tab-%s><h2>%s</h2><div class=fields>",
                       parameter_tabs[tab].name, parameter_tabs[tab].name, T(parameter_tabs[tab].label));
            for (size_t i = 0; i < sizeof(replacement_parameters) / sizeof(replacement_parameters[0]); i++) {
                const struct parameter *parameter = &replacement_parameters[i];
                if (parameter_shown(parameter) && parameter_tab(parameter) == tab)
                    append_parameter_field(&page, config, parameter, submitted);
            }
            sb_append(&page, "</div>");
            if (tab == TAB_NETWORK) {
#ifdef MT11_WEB_SITL
                sb_appendf(&page, "<p class=help>%s</p><p id=network-reconnect data-sitl=true hidden>", T(S_NETWORK_SITL));
#else
                sb_append(&page, "<p class=help id=network-reconnect hidden>");
#endif
                sb_appendf(&page, "%s <a id=network-link></a></p>", T(S_NETWORK_RECONNECT));
            }
            sb_append(&page, "</section>");
        }
        sb_appendf(&page, "<div class=actions><div class=help>%s</div>"
                          "<button type=submit name=action value=save>%s</button>"
                          "<button type=submit name=action value=save_restart>%s</button>"
                          "</div></form><script src=/parameters.js defer></script></body></html>",
                   T(S_PARAMS_SAVE_ALL), T(S_PARAMS_SAVE), T(S_PARAMS_SAVE_RESTART));
        free(config);
        *page_len = page.len;
        return page.data;
    }
    return NULL;
}

static char *render_raw_page(const char *message, bool message_is_error, size_t *page_len)
{
    struct string_buffer page;
    enum camera_kind kind = configuration_camera_kind();
    size_t config_len = 0;
    const char *path;
    const char *backup;
    char *config;
    char title[96];

    if (kind == CAMERA_NONE) kind = DEFAULT_CAMERA_KIND;
    path = config_path(kind);
    backup = config_backup_path(kind);
    config = read_file(path, MAX_CONFIG, &config_len);
    (void)config_len;
    if (config == NULL) config = strdup("");
    if (config == NULL) return NULL;
    sb_init(&page);
    snprintf(title, sizeof(title), T(S_TITLE_RAW), PRODUCT_NAME);
    append_head(&page, title, page_style, NULL);
    sb_append(&page, "<header><h1>");
    sb_appendf(&page, T(S_RAW_HEADING),
               kind == CAMERA_REPLACEMENT ? "camera.ini" : "config.ini");
    sb_append(&page, "</h1><div class=muted>");
    sb_appendf(&page, T(S_RAW_SUBTITLE), camera_label(kind));
    sb_append(&page, "</div></header>");
    append_nav(&page, "/raw", NULL);
    append_notice(&page, message, message_is_error);
    sb_appendf(&page, "<section class=card><form method=post action=/config>"
                      "<input type=hidden name=csrf value=\"%s\">"
                      "<textarea name=config spellcheck=false>", csrf_token);
    sb_append_html(&page, config);
    sb_appendf(&page, "</textarea><br><button type=submit name=action value=save>%s</button>"
                      "<button type=submit name=action value=save_restart>%s</button>"
                      "</form><p class=muted>", T(S_RAW_SAVE), T(S_PARAMS_SAVE_RESTART));
    sb_appendf(&page, T(S_RAW_HELP), path, backup);
    sb_append(&page, "</p></section></body></html>");
    free(config);
    *page_len = page.len;
    return page.data;
}

static const char files_script[] =
    "(()=>{"
        "const table=document.getElementById('files-table'),body=table.tBodies[0],"
        "buttons=[...table.querySelectorAll('.file-sort')],"
        "rows=[...body.rows].filter(r=>!r.dataset.parent),"
        "collator=new Intl.Collator(undefined,{numeric:true,sensitivity:'base'});"
        "let column=0,direction=1;"
        "try{const s=JSON.parse(sessionStorage.getItem('camera.files.sort'));"
        "if(Array.isArray(s)&&Number.isInteger(s[0])&&s[0]>=0&&s[0]<5&&Math.abs(s[1])===1)"
        "[column,direction]=s;}catch(e){}"
        "function sort(){rows.sort((a,b)=>{"
        "const folders=Number(b.dataset.directory)-Number(a.dataset.directory);"
        "if(folders)return folders;"
        "const x=a.cells[column],y=b.cells[column];"
        "const order=column>=2?Number(x.dataset.sort)-Number(y.dataset.sort):"
        "collator.compare(x.textContent,y.textContent);"
        "return direction*order||collator.compare(a.cells[0].textContent,b.cells[0].textContent);});"
        "const fragment=document.createDocumentFragment();rows.forEach(r=>fragment.append(r));body.append(fragment);"
        "buttons.forEach((b,i)=>b.parentElement.setAttribute('aria-sort',i===column?"
        "(direction===1?'ascending':'descending'):'none'));"
        "try{sessionStorage.setItem('camera.files.sort',JSON.stringify([column,direction]));}catch(e){}}"
        "buttons.forEach((b,i)=>b.addEventListener('click',()=>{direction=i===column?-direction:1;column=i;sort();}));"
        "sort();})();";

static char *render_files_page(const char *requested_path, const char *message,
                               bool message_is_error, size_t *page_len,
                               char *error, size_t error_size)
{
    struct string_buffer page;
    char resolved[PATH_MAX];
    char parent[PATH_MAX];
    char title[96];
    DIR *directory;
    const struct dirent *entry;
    unsigned shown = 0;

    if (!canonical_existing_path(requested_path != NULL ? requested_path : MEDIA_ROOT,
                                 resolved, error, error_size)) return NULL;
    directory = opendir(resolved);
    if (directory == NULL) {
        snprintf(error, error_size, T(S_E_OPEN_DIRECTORY), strerror(errno));
        return NULL;
    }
    path_parent(resolved, parent);
    sb_init(&page);
    snprintf(title, sizeof(title), T(S_TITLE_FILES), PRODUCT_NAME);
    append_head(&page, title, page_style, NULL);
    sb_appendf(&page, "<header><h1>%s</h1><div class=muted>", T(S_FILES_HEADING));
    sb_appendf(&page, T(S_FILES_SUBTITLE), MEDIA_ROOT);
    sb_append(&page, "</div></header>");
    append_nav(&page, "/files", resolved);
    append_notice(&page, message, message_is_error);
    sb_appendf(&page, "<section class=card><form class=pathbox method=get action=/files>"
                      "<input aria-label=\"%s\" required type=text name=path value=\"",
               T(S_FILES_PATH));
    sb_append_html(&page, resolved);
    sb_appendf(&page, "\"><button type=submit>%s</button></form>"
                      "<p><a href=\"/files?path=%%2F\">/</a> &middot; "
                      "<a href=\"/files?path=", T(S_FILES_OPEN));
    sb_append_url(&page, APP_DIR);
    sb_append(&page, "\">");
    sb_append_html(&page, APP_DIR);
    sb_append(&page, "</a> &middot; <a href=\"/files?path=");
    sb_append_url(&page, MEDIA_ROOT);
    sb_append(&page, "\">");
    sb_append_html(&page, MEDIA_ROOT);
    sb_append(&page, "</a></p></section>"
                    "<section class=card><h2>");
    sb_append_html(&page, resolved);
    sb_append(&page, "</h2><style>#files-table .file-sort{background:none;border:0;"
                     "color:inherit;font:inherit;font-weight:bold;padding:0;cursor:pointer}"
                     "#files-table .file-sort:focus-visible{outline:2px solid currentColor}"
                     "#files-table th[aria-sort=ascending] button:after{content:' \u25b2'}"
                     "#files-table th[aria-sort=descending] button:after{content:' \u25bc'}"
                     "</style><table id=files-table><thead><tr>");
    const unsigned labels[] = {S_FILES_NAME, S_FILES_TYPE, S_FILES_SIZE,
                                   S_FILES_MODIFIED, S_FILES_MODE};
    for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        sb_appendf(&page, "<th scope=col aria-sort=none><button type=button class=file-sort "
                          "data-column=%u>%s</button></th>", i, T(labels[i]));
    }
    sb_appendf(&page, "<th scope=col>%s</th></tr></thead><tbody>", T(S_FILES_ACTIONS));
    if (strcmp(resolved, "/") != 0) {
        sb_append(&page, "<tr data-parent=1><td><a href=\"");
        append_path_query(&page, "/files?path=", parent);
        sb_appendf(&page, "\">../</a></td><td>%s</td><td>-</td><td>-</td><td>-</td><td></td></tr>",
                   T(S_FILES_DIRECTORY));
    }
    while ((entry = readdir(directory)) != NULL && shown < 5000) {
        char child[PATH_MAX];
        char time_text[64];
        char size_text[32] = "-";
        struct stat lst;
        struct stat target;
        bool target_is_dir;
        const char *route;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        int child_length = strcmp(resolved, "/") == 0 ?
            snprintf(child, sizeof(child), "/%s", entry->d_name) :
            snprintf(child, sizeof(child), "%s/%s", resolved, entry->d_name);
        if (child_length < 0 || child_length >= (int)sizeof(child) || lstat(child, &lst) < 0) continue;
        target = lst;
        if (S_ISLNK(lst.st_mode)) (void)stat(child, &target);
        target_is_dir = S_ISDIR(target.st_mode);
        route = target_is_dir ? "/files?path=" : "/view?path=";
        format_file_time(lst.st_mtime, time_text);
        if (S_ISREG(lst.st_mode)) format_bytes((uint64_t)lst.st_size, size_text);
        sb_appendf(&page, "<tr data-directory=%u><td><a href=\"", target_is_dir ? 1U : 0U);
        append_path_query(&page, route, child);
        sb_append(&page, "\">");
        sb_append_html(&page, entry->d_name);
        if (target_is_dir) sb_append(&page, "/");
        sb_appendf(&page, "</a></td><td>%c%s</td><td data-sort=\"%lld\">%s</td>"
                          "<td data-sort=\"%lld\">%s</td><td data-sort=%u>%04o</td><td class=file-actions>",
                   file_type_char(lst.st_mode), S_ISLNK(lst.st_mode) ? T(S_FILES_LINK) : "",
                   S_ISREG(lst.st_mode) ? (long long)lst.st_size : -1LL, size_text,
                   (long long)lst.st_mtime, time_text,
                   (unsigned)(lst.st_mode & 07777), (unsigned)(lst.st_mode & 07777));
        if (S_ISREG(target.st_mode)) {
            sb_append(&page, "<a href=\"");
            append_path_query(&page, "/file?path=", child);
            sb_appendf(&page, "&amp;download=1\">%s</a>", T(S_FILES_DOWNLOAD));
        }
        if (path_can_be_deleted(child)) {
            sb_append(&page, " &middot; <a class=delete-link href=\"");
            append_path_query(&page, "/delete-confirm?path=", child);
            sb_appendf(&page, "\">%s</a>", T(S_FILES_DELETE));
        }
        sb_append(&page, "</td></tr>");
        shown++;
    }
    closedir(directory);
    sb_append(&page, "</tbody></table>");
    if (shown == 5000) sb_appendf(&page, "<p class=notice>%s</p>", T(S_FILES_TRUNCATED));
    sb_append(&page, "<p class=muted>");
    sb_appendf(&page, T(S_FILES_LEGEND), MEDIA_ROOT);
    sb_append(&page, "</p></section><script src=/files.js defer></script></body></html>");
    *page_len = page.len;
    return page.data;
}

static char *render_view_page(const char *requested_path, size_t *page_len,
                              char *error, size_t error_size)
{
    struct string_buffer page;
    char resolved[PATH_MAX];
    char parent[PATH_MAX];
    char size_text[32];
    char title[96];
    struct stat st;
    const char *mime;

    if (!canonical_existing_path(requested_path, resolved, error, error_size) ||
        stat(resolved, &st) < 0 || !S_ISREG(st.st_mode)) {
        snprintf(error, error_size, "%s", T(S_E_VIEW_REGULAR));
        return NULL;
    }
    mime = file_mime_type(resolved);
    path_parent(resolved, parent);
    format_bytes((uint64_t)st.st_size, size_text);
    sb_init(&page);
    snprintf(title, sizeof(title), T(S_TITLE_VIEWER), PRODUCT_NAME);
    append_head(&page, title, page_style, NULL);
    sb_appendf(&page, "<header><h1>%s</h1><div class=muted>", T(S_VIEWER_HEADING));
    sb_append_html(&page, resolved);
    sb_append(&page, "</div></header>");
    append_nav(&page, "/view", resolved);
    sb_append(&page, "<section class=card><p><a href=\"");
    append_path_query(&page, "/files?path=", parent);
    sb_appendf(&page, "\">%s</a> &middot; <a href=\"", T(S_VIEWER_BACK));
    append_path_query(&page, "/file?path=", resolved);
    sb_appendf(&page, "&amp;download=1\">%s</a>", T(S_FILES_DOWNLOAD));
    if (path_can_be_deleted(resolved)) {
        sb_append(&page, " &middot; <a class=delete-link href=\"");
        append_path_query(&page, "/delete-confirm?path=", resolved);
        sb_appendf(&page, "\">%s</a>", T(S_FILES_DELETE));
    }
    sb_appendf(&page, "</p><p class=muted>%s; %s</p>", mime, size_text);
    if (mime_is_image(mime)) {
        sb_appendf(&page, "<img class=preview alt=\"%s\" src=\"", T(S_VIEWER_IMAGE_ALT));
        append_path_query(&page, "/file?path=", resolved);
        sb_append(&page, "\">");
    } else if (mime_is_video(mime)) {
        sb_append(&page, "<video class=preview controls preload=metadata src=\"");
        append_path_query(&page, "/file?path=", resolved);
        sb_appendf(&page, "\">%s</video>", T(S_VIEWER_NO_VIDEO));
    } else {
        sb_appendf(&page, "<p>%s</p>", T(S_VIEWER_NO_PREVIEW));
    }
    sb_append(&page, "</section></body></html>");
    *page_len = page.len;
    return page.data;
}

static char *render_delete_page(const char *requested_path, size_t *page_len,
                                char *error, size_t error_size)
{
    struct string_buffer page;
    char resolved[PATH_MAX];
    struct stat st;

    if (requested_path == NULL || lstat(requested_path, &st) < 0 ||
        !path_can_be_deleted(requested_path)) {
        snprintf(error, error_size, T(S_E_DELETE_EXISTING), MEDIA_ROOT);
        return NULL;
    }
    if (S_ISLNK(st.st_mode)) snprintf(resolved, sizeof(resolved), "%s", requested_path);
    else if (realpath(requested_path, resolved) == NULL) {
        snprintf(error, error_size, "%s", T(S_E_DELETE_RESOLVE));
        return NULL;
    }
    sb_init(&page);
    append_head(&page, T(S_TITLE_DELETE), page_style, NULL);
    sb_appendf(&page, "<header><h1>%s</h1></header>", T(S_TITLE_DELETE));
    append_nav(&page, "/delete-confirm", resolved);
    sb_appendf(&page, "<section class=card><p class=\"notice error\">%s", T(S_DELETE_TEXT));
    sb_append_html(&page, resolved);
    if (S_ISDIR(st.st_mode)) sb_append(&page, T(S_DELETE_RECURSIVE));
    sb_appendf(&page, "%s</p><form method=post action=/delete>"
                      "<input type=hidden name=csrf value=\"%s\">"
                      "<input type=hidden name=path value=\"", T(S_DELETE_NO_UNDO), csrf_token);
    sb_append_html(&page, resolved);
    sb_appendf(&page, "\"><label><input required type=checkbox name=confirm value=yes> "
                      "%s</label><br>"
                      "<button class=danger type=submit>%s</button></form></section></body></html>",
               T(S_DELETE_CONFIRM), T(S_DELETE_BUTTON));
    *page_len = page.len;
    return page.data;
}

static const char *log_page_style =
    ":root{color-scheme:light dark;--bg:#f4f7fa;--card:#fff;--text:#17212b;"
    "--muted:#607080;--line:#d7e0e8;--accent:#1769aa}"
    "@media(prefers-color-scheme:dark){:root{--bg:#10161d;--card:#18222d;"
    "--text:#e8edf2;--muted:#a7b3bf;--line:#344352;--accent:#77bdf2}}"
    "*{box-sizing:border-box}body{display:flex;flex-direction:column;margin:0;padding:20px;font:15px/1.5 system-ui,sans-serif;"
    "color:var(--text);background:var(--bg)}h1{margin-bottom:4px}.muted{color:var(--muted)}"
    ".brand-logo{order:-1;align-self:flex-start;display:inline-flex;align-items:center;width:min(280px,70vw);padding:7px 10px;"
    "border-radius:8px;background:#263746}.brand-logo img{display:block;width:100%;height:auto}"
    "nav{display:flex;flex-wrap:wrap;align-items:center;gap:8px;margin:18px 0}nav a,.nav-link{padding:8px 12px;border:1px solid var(--line);"
    "border-radius:6px;color:var(--accent);text-decoration:none;background:var(--card)}"
    ".nav-form{display:inline}.lang-form{margin-left:auto}.nav-link{margin:0;font:inherit;font-weight:normal;cursor:pointer}"
    "nav select{width:auto;padding:7px 8px;margin:0 4px 0 0;border:1px solid var(--line);border-radius:6px;background:var(--card);color:var(--text)}"
    "pre{margin:0;padding:14px;border:1px solid var(--line);border-radius:8px;"
    "background:var(--card);white-space:pre-wrap;overflow-wrap:anywhere;"
    "font:12px/1.4 ui-monospace,monospace;min-height:70vh}";

/* the captured application output itself is shown as-is in every language */
static char *render_log_page(size_t *page_len)
{
    struct string_buffer page;
    size_t log_len = 0;
    char *log = read_current_app_log(&log_len);
    char title[96];

    if (log == NULL) return NULL;
    sb_init(&page);
    snprintf(title, sizeof(title), T(S_TITLE_DEBUG), PRODUCT_NAME);
    append_head(&page, title, log_page_style, "<meta http-equiv=refresh content=3>");
    sb_appendf(&page, "<header id=top><h1>%s</h1><div class=muted>%s</div></header>",
               T(S_DEBUG_HEADING), T(S_DEBUG_SUBTITLE));
    append_nav(&page, "/log", NULL);
    sb_appendf(&page, "<p><a href=\"/log.txt\">%s</a></p><pre>", T(S_DEBUG_PLAIN_TEXT));
    if (log_len == 0) {
        sb_append(&page, T(TARGET_TEXT(S_DEBUG_EMPTY_MT11, S_DEBUG_EMPTY_A8)));
    } else {
        sb_append_log_html(&page, log, log_len);
    }
    sb_append(&page, "</pre><p class=muted>");
    sb_appendf(&page, T(S_DEBUG_FOOTER), log_len, APP_LOG_ROTATE_SIZE / 1024U);
    sb_append(&page, "</p></body></html>");
    free(log);
    *page_len = page.len;
    return page.data;
}

static char *render_login_page(const char *message, bool message_is_error,
                               size_t *page_len)
{
    struct string_buffer page;
    char title[96];

    sb_init(&page);
    snprintf(title, sizeof(title), T(S_TITLE_LOGIN), PRODUCT_NAME);
    append_head(&page, title, page_style, NULL);
    sb_append(&page, "<header id=top><h1>");
    sb_append_html(&page, title);
    sb_appendf(&page, "</h1><div class=muted>%s</div></header>", T(S_LOGIN_SUBTITLE));
    append_logo(&page, "/");
    append_notice(&page, message, message_is_error);
    sb_appendf(&page, "<section class=\"card login-card\"><form method=post action=/login>"
                      "<input type=hidden name=token value=\"%s\">"
                      "<div class=field><label for=username>%s</label>"
                      "<input id=username name=username type=text value=admin autocomplete=username required></div>"
                      "<div class=field><label for=password>%s</label>"
                      "<input id=password name=password type=password autocomplete=current-password required autofocus></div>"
                      "<div class=field><label for=lang-login>%s</label>",
               login_token, T(S_LOGIN_USERNAME), T(S_LOGIN_PASSWORD), T(S_LANGUAGE));
    append_language_select(&page, "lang-login");
    sb_appendf(&page, "</div><button type=submit>%s</button></form><p class=help>", T(S_LOGIN_BUTTON));
    sb_appendf(&page, T(S_LOGIN_FIRST_TIME), PASSWORD_PATH);
    sb_appendf(&page, "</p><p class=muted>%s</p></section>"
                      "<script src=/language.js defer></script></body></html>",
               T(S_LOGIN_SCRIPTED));
    *page_len = page.len;
    return page.data;
}

static void send_login_page(int fd, int status, const char *message, bool is_error,
                            const char *extra_headers)
{
    size_t length;
    char *page = render_login_page(message, is_error, &length);

    if (page == NULL) {
        send_text_errorf(fd, 500, "Internal Server Error", S_OUT_OF_MEMORY);
        return;
    }
    send_response(fd, status,
                  status == 200 ? "OK" : status == 401 ? "Unauthorized" :
                  status == 403 ? "Forbidden" :
                  status == 500 ? "Internal Server Error" : "Bad Request",
                  "text/html; charset=utf-8", page, length, extra_headers);
    free(page);
}

/* Browsers send Origin on cross-site form posts; the login form carries no
 * cookie-bound token, so a foreign origin is refused here. Requests without
 * the header (scripts, older browsers) pass. */
static bool same_origin(const struct request *request)
{
    size_t origin_len;
    size_t host_len;
    const char *origin = find_header_span(request, "Origin", &origin_len);
    const char *host = find_header_span(request, "Host", &host_len);

    if (origin == NULL) return true;
    return host != NULL && origin_len == host_len + 7 &&
           strncasecmp(origin, "http://", 7) == 0 &&
           strncasecmp(origin + 7, host, host_len) == 0;
}

static void language_cookie(enum language lang, char *header, size_t header_size)
{
    snprintf(header, header_size,
             "Set-Cookie: lang=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=31536000\r\n",
             languages[lang].code);
}

static void handle_login(int fd, const struct request *request, const char *peer)
{
    size_t token_len = 0;
    size_t username_len = 0;
    size_t password_len = 0;
    size_t lang_len = 0;
    char *token;
    char *username;
    char *password;
    char *lang;
    char session[65];
    char headers[384];
    enum language selected;
    int result;

    if (strcmp(request->method, "GET") == 0 || strcmp(request->method, "HEAD") == 0) {
        /* ?lang= lets the form switch its own language before login */
        lang = query_value(request, "lang", &lang_len);
        headers[0] = '\0';
        if (lang != NULL && language_from_code(lang, lang_len, &selected)) {
            current_language = selected;
            language_cookie(selected, headers, sizeof(headers));
        }
        free(lang);
        send_login_page(fd, 200, NULL, false, headers[0] != '\0' ? headers : NULL);
        return;
    }
    if (strcmp(request->method, "POST") != 0) {
        send_text_errorf(fd, 404, "Not Found", S_NOT_FOUND);
        return;
    }
    token = form_value(request, "token", &token_len);
    username = form_value(request, "username", &username_len);
    password = form_value(request, "password", &password_len);
    lang = form_value(request, "lang", &lang_len);
    headers[0] = '\0';
    if (lang != NULL && language_from_code(lang, lang_len, &selected)) {
        current_language = selected;
        language_cookie(selected, headers, sizeof(headers));
    }
    if (!same_origin(request)) {
        log_message("cross-site login rejected from %s", peer);
        send_login_page(fd, 403, T(S_LOGIN_ORIGIN), true, NULL);
        goto done;
    }
    if (token == NULL || !constant_time_equal(token, token_len, login_token, 64)) {
        send_login_page(fd, 400, T(S_LOGIN_EXPIRED), true, NULL);
        goto done;
    }
    result = check_password(username != NULL ? username : "", username_len,
                            password != NULL ? password : "", password_len);
    if (result < 0) {
        send_text_errorf(fd, 503, "Service Unavailable", S_PASSWORD_FILE_MISSING,
                         PASSWORD_PATH);
        goto done;
    }
    if (result == 0) {
        log_message("login failed from %s", peer);
        /* slows password guessing without holding up other workers */
        usleep(500000);
        send_login_page(fd, 401, T(S_LOGIN_INCORRECT), true, NULL);
        goto done;
    }
    if (!create_session(session)) {
        log_message("cannot create login session for %s: %s", peer, strerror(errno));
        send_login_page(fd, 500, T(S_LOGIN_SESSION_FAILED), true, NULL);
        goto done;
    }
    log_message("admin logged in from %s", peer);
    snprintf(headers + strlen(headers), sizeof(headers) - strlen(headers),
             "Set-Cookie: session=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%u\r\n",
             session, SESSION_LIFETIME_SECONDS);
    send_redirect_headers(fd, "/", headers);
done:
    if (password != NULL) memset(password, 0, password_len);
    free(token);
    free(username);
    free(password);
    free(lang);
}

/* the language form returns to the page it was on; only a local,
 * URL-encoded path is accepted */
static bool safe_local_path(const char *path)
{
    size_t length = path != NULL ? strlen(path) : 0;

    if (length == 0 || length >= 2048 || path[0] != '/' ||
        path[1] == '/' || path[1] == '\\') return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)path[i];
        if (!(isalnum(c) || strchr("/?=&%._~-", c) != NULL)) return false;
    }
    return true;
}

static void select_language(const struct request *request)
{
    char code[16];
    size_t length;
    const char *accept;
    enum language lang;

    current_language = LANG_EN;
    if (cookie_value(request, "lang", code, sizeof(code)) &&
        language_from_code(code, strlen(code), &lang)) {
        current_language = lang;
        return;
    }
    accept = find_header_span(request, "Accept-Language", &length);
    if (accept != NULL && language_from_accept(accept, length, &lang)) {
        current_language = lang;
    }
}

/* an Accept header listing text/html with a non-zero q value, as a browser
 * navigation sends; fetch(), curl and video/img requests do not */
static bool wants_html(const struct request *request)
{
    size_t length;
    const char *accept = find_header_span(request, "Accept", &length);
    const char *p = accept;
    const char *end;

    if (accept == NULL) return false;
    end = accept + length;
    while (p < end) {
        const char *item_end = header_element_end(p, end);
        const char *type_end;
        double quality;
        while (p < item_end && (*p == ' ' || *p == '\t')) p++;
        type_end = p;
        while (type_end < item_end && *type_end != ';' && *type_end != ' ' &&
               *type_end != '\t') type_end++;
        if ((size_t)(type_end - p) == 9 && strncasecmp(p, "text/html", 9) == 0 &&
            header_element_quality(type_end, item_end, false, &quality) && quality > 0.0) {
            return true;
        }
        p = item_end + (item_end < end ? 1 : 0);
    }
    return false;
}

static void send_page(int fd, const char *message, bool is_error)
{
    size_t length;
    char *page = render_page(message, is_error, &length);
    if (page == NULL) {
        send_text_errorf(fd, 500, "Internal Server Error", S_OUT_OF_MEMORY);
        return;
    }
    send_response(fd, is_error ? 400 : 200, is_error ? "Bad Request" : "OK",
                  "text/html; charset=utf-8", page, length, NULL);
    free(page);
}

static void send_parameter_page(int fd, const char *message, bool is_error,
                                const struct request *submitted)
{
    size_t length;
    char *page = render_parameter_page(message, is_error, submitted, &length);
    if (page == NULL) {
        send_text_errorf(fd, 500, "Internal Server Error", S_E_CONFIG_UNREADABLE);
        return;
    }
    send_response(fd, is_error ? 400 : 200, is_error ? "Bad Request" : "OK",
                  "text/html; charset=utf-8", page, length, NULL);
    free(page);
}

static void send_raw_page(int fd, const char *message, bool is_error)
{
    size_t length;
    char *page = render_raw_page(message, is_error, &length);
    if (page == NULL) {
        send_text_errorf(fd, 500, "Internal Server Error", S_E_CONFIG_UNREADABLE);
        return;
    }
    send_response(fd, is_error ? 400 : 200, is_error ? "Bad Request" : "OK",
                  "text/html; charset=utf-8", page, length, NULL);
    free(page);
}

static void send_users_page(int fd, const char *message, bool is_error)
{
    size_t length;
    char *page = render_users_page(message, is_error, &length);

    if (page == NULL) {
        send_text_errorf(fd, 500, "Internal Server Error", S_OUT_OF_MEMORY);
        return;
    }
    send_response(fd, is_error ? 400 : 200, is_error ? "Bad Request" : "OK",
                  "text/html; charset=utf-8", page, length, NULL);
    free(page);
}

static void send_sensors_page(int fd, const char *message, bool is_error)
{
    size_t length;
    char *page = render_sensors_page(message, is_error, &length);

    if (page == NULL) {
        send_text_errorf(fd, 500, "Internal Server Error", S_OUT_OF_MEMORY);
        return;
    }
    send_response(fd, is_error ? 400 : 200, is_error ? "Bad Request" : "OK",
                  "text/html; charset=utf-8", page, length, NULL);
    free(page);
}

static void send_live_page(int fd)
{
    size_t length;
    char *page = render_live_page(&length);

    if (page == NULL) {
        send_text_errorf(fd, 500, "Internal Server Error", S_OUT_OF_MEMORY);
        return;
    }
    send_response(fd, 200, "OK", "text/html; charset=utf-8", page, length,
                  NULL);
    free(page);
}

#if WEB_HAVE_THERMAL
static void send_sensors_json(int fd)
{
    struct camera_sensor_values sensors;
    struct string_buffer json;
    int cpu_millidegrees = 0;
    bool have_cpu = read_soc_temperature(&cpu_millidegrees);

    read_camera_sensors(&sensors);
    sb_init(&json);
    sb_append(&json, "{");
    if (sensors.have_lidar) {
        sb_appendf(&json, "\"lidar_m\":%.1f",
                   sensors.lidar_decimetres / 10.0);
    } else {
        sb_append(&json, "\"lidar_m\":null");
    }
    if (sensors.have_lidar) {
        sb_append(&json, ",\"lidar_enabled\":true");
    } else if (sensors.have_thermal) {
        /* This MT11 firmware stops replying to 0x15 when the laser is off. A
         * simultaneous thermal reply proves that the camera API itself is
         * available, allowing the two cases to be distinguished. */
        sb_append(&json, ",\"lidar_enabled\":false");
    } else {
        sb_append(&json, ",\"lidar_enabled\":null");
    }
    if (sensors.have_thermal) {
        sb_appendf(&json,
                   ",\"minimum_c\":%.2f,\"minimum_x\":%u,\"minimum_y\":%u"
                   ",\"maximum_c\":%.2f,\"maximum_x\":%u,\"maximum_y\":%u",
                   sensors.minimum_centi_c / 100.0, sensors.minimum_x,
                   sensors.minimum_y, sensors.maximum_centi_c / 100.0,
                   sensors.maximum_x, sensors.maximum_y);
    } else {
        sb_append(&json,
                  ",\"minimum_c\":null,\"minimum_x\":null,\"minimum_y\":null"
                  ",\"maximum_c\":null,\"maximum_x\":null,\"maximum_y\":null");
    }
    if (have_cpu) {
        sb_appendf(&json, ",\"cpu_c\":%.1f", cpu_millidegrees / 1000.0);
    } else {
        sb_append(&json, ",\"cpu_c\":null");
    }
    sb_append(&json, "}\n");
    send_response(fd, 200, "OK", "application/json; charset=utf-8",
                  json.data, json.len, "Cache-Control: no-store\r\n");
    free(json.data);
}
#endif

static void send_live_attitude_json(int fd)
{
    struct camera_attitude_values attitude;
    struct string_buffer json;

    read_camera_attitude(&attitude);
    sb_init(&json);
    if (attitude.have_attitude) {
#if APCAM_TARGET == APCAM_TARGET_Z1_MINI
        sb_appendf(&json,
                   "{\"yaw_deg\":%.1f,\"pitch_deg\":%.1f,\"roll_deg\":%.1f,"
                   "\"yaw_rate_dps\":null,\"pitch_rate_dps\":null,\"roll_rate_dps\":null}\n",
                   attitude.yaw_tenths / 10.0, attitude.pitch_tenths / 10.0,
                   attitude.roll_tenths / 10.0);
#else
        sb_appendf(&json,
                   "{\"yaw_deg\":%.1f,\"pitch_deg\":%.1f,\"roll_deg\":%.1f,"
                   "\"yaw_rate_dps\":%.1f,\"pitch_rate_dps\":%.1f,"
                   "\"roll_rate_dps\":%.1f}\n",
                   attitude.yaw_tenths / 10.0,
                   attitude.pitch_tenths / 10.0,
                   attitude.roll_tenths / 10.0,
                   attitude.yaw_rate_tenths / 10.0,
                   attitude.pitch_rate_tenths / 10.0,
                   attitude.roll_rate_tenths / 10.0);
#endif
    } else {
        sb_append(&json,
                  "{\"yaw_deg\":null,\"pitch_deg\":null,\"roll_deg\":null,"
                  "\"yaw_rate_dps\":null,\"pitch_rate_dps\":null,"
                  "\"roll_rate_dps\":null}\n");
    }
    send_response(fd, 200, "OK", "application/json; charset=utf-8",
                  json.data, json.len, "Cache-Control: no-store\r\n");
    free(json.data);
}

/* JavaScript is served per request with its user-visible strings in a
 * leading "const L = {...}" object */
static bool sb_append_js(struct string_buffer *sb, const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        char escaped[8];
        if (*p == '\\' || *p == '\'') {
            escaped[0] = '\\';
            escaped[1] = (char)*p;
            if (!sb_append_n(sb, escaped, 2)) return false;
        } else if (*p < 0x20 || *p == '<' || *p == '>' || *p == '&') {
            snprintf(escaped, sizeof(escaped), "\\x%02x", *p);
            if (!sb_append(sb, escaped)) return false;
        } else if (!sb_append_n(sb, (const char *)p, 1)) {
            return false;
        }
    }
    return true;
}

struct js_string {
    const char *key;
    const char *text;
};

static void send_script(int fd, const struct js_string *items, size_t count,
                        const char *body)
{
    struct string_buffer script;

    sb_init(&script);
    sb_append(&script, "const L = {");
    for (size_t i = 0; i < count; i++) {
        sb_appendf(&script, "%s%s:'", i != 0 ? "," : "", items[i].key);
        sb_append_js(&script, items[i].text);
        sb_append(&script, "'");
    }
    sb_append(&script, "};\n");
    sb_append(&script, body);
    send_response(fd, 200, "OK", "application/javascript; charset=utf-8",
                  script.data, script.len, NULL);
    free(script.data);
}

static const char parameters_script[] =
    "(() => {\n"
    "  const form = document.querySelector('form[action=\"/parameters\"]');\n"
    "  if (!form) return;\n"
    "  const tablist = document.getElementById('parameter-tabs');\n"
    "  const tabs = [...tablist.querySelectorAll('[role=tab]')];\n"
    "  const panels = tabs.map(tab => document.getElementById(tab.getAttribute('aria-controls')));\n"
    "  function selectTab(tab, focus = false) {\n"
    "    if (!tabs.includes(tab)) tab = tabs[0];\n"
    "    tabs.forEach((item, i) => {\n"
    "      const selected = item === tab;\n"
    "      item.setAttribute('aria-selected', String(selected));\n"
    "      item.tabIndex = selected ? 0 : -1;\n"
    "      panels[i].hidden = !selected;\n"
    "    });\n"
    "    const name = tab.id.slice(4);\n"
    "    form.setAttribute('action', '/parameters#' + name);\n"
    "    try { history.replaceState(null, '', '#' + name); } catch (_) {}\n"
    "    if (focus) tab.focus();\n"
    "  }\n"
    "  function reveal(field) {\n"
    "    const panel = field.closest('.parameter-panel');\n"
    "    selectTab(tabs[panels.indexOf(panel)]);\n"
    "  }\n"
    "  tabs.forEach((tab, index) => {\n"
    "    tab.addEventListener('click', () => selectTab(tab));\n"
    "    tab.addEventListener('keydown', event => {\n"
    "      let next;\n"
    "      if (event.key === 'ArrowRight') next = (index + 1) % tabs.length;\n"
    "      else if (event.key === 'ArrowLeft') next = (index + tabs.length - 1) % tabs.length;\n"
    "      else if (event.key === 'Home') next = 0;\n"
    "      else if (event.key === 'End') next = tabs.length - 1;\n"
    "      else return;\n"
    "      event.preventDefault(); selectTab(tabs[next], true);\n"
    "    });\n"
    "  });\n"
    "  panels.forEach(panel => panel.setAttribute('role', 'tabpanel'));\n"
    "  tablist.hidden = false;\n"
    "  const fromHash = () => selectTab(tabs.find(tab => tab.id === 'tab-' + location.hash.slice(1)));\n"
    "  fromHash();\n"
    "  window.addEventListener('hashchange', fromHash);\n"
    "  const fields = [...form.querySelectorAll('.field input, .field select')];\n"
    "  const touched = new Set();\n"
    "  let attempted = Boolean(document.querySelector('.notice.error'));\n"
    "  const get = name => form.elements.namedItem(name);\n"
    "  const value = name => get(name)?.value || '';\n"
    "  for (const field of fields) {\n"
    "    const error = document.createElement('div');\n"
    "    error.id = field.id + '-error';\n"
    "    error.className = 'field-error';\n"
    "    error.hidden = true;\n"
    "    field.setAttribute('aria-describedby', error.id);\n"
    "    field.closest('.field').append(error);\n"
    "  }\n"
    "  const ipv4 = text => {\n"
    "    const parts = text.split('.');\n"
    "    return parts.length === 4 && parts.every(p => /^(0|[1-9][0-9]{0,2})$/.test(p) && Number(p) <= 255);\n"
    "  };\n"
    "  const number = text => text.split('.').reduce((v, n) => ((v << 8) | Number(n)) >>> 0, 0);\n"
    "  const host = text => ipv4(text) && Number(text.split('.')[0]) > 0 && Number(text.split('.')[0]) !== 127 && Number(text.split('.')[0]) < 224;\n"
    "  function invalid(field, message) {\n"
    "    if (field) field.setCustomValidity(message || L.invalid.replace('%s', field.closest('.field').querySelector('label').textContent));\n"
    "  }\n"
    "  function validate() {\n"
    "    for (const field of fields) {\n"
    "      field.setCustomValidity('');\n"
    "      if (field.name.startsWith('proxy_') && ['text', 'password'].includes(field.type)) {\n"
    "        if (/[^\\x20-\\x7e]|\"/.test(field.value) || field.value.length > field.maxLength) invalid(field);\n"
    "      }\n"
    "      if (field.name === 'timezone' && /[\\s\\x00-\\x1f\\x7f]/.test(field.value)) invalid(field);\n"
    "    }\n"
    "    for (const name of ['proxy_host', 'network_interface']) {\n"
    "      if (!/^[a-zA-Z0-9_.-]*$/.test(value(name))) invalid(get(name));\n"
    "    }\n"
    "    const parseAddress = text => {\n"
    "      const parts = text.split('/');\n"
    "      if (parts.length !== 2 || !host(parts[0]) || !/^([1-9]|[12][0-9]|3[0-2])$/.test(parts[1])) return null;\n"
    "      const ip = number(parts[0]), prefix = Number(parts[1]), mask = (0xffffffff << (32 - prefix)) >>> 0;\n"
    "      if (prefix < 31 && ((ip & ~mask) === 0 || (ip & ~mask) === (~mask >>> 0))) return null;\n"
    "      return {ip, mask, prefix};\n"
    "    };\n"
    "    for (const name of ['network_primary_address', 'network_secondary_address']) {\n"
    "      if (value(name) && !parseAddress(value(name))) invalid(get(name), L.address);\n"
    "    }\n"
    "    const primary = parseAddress(value('network_primary_address'));\n"
    "    const secondary = parseAddress(value('network_secondary_address'));\n"
    "    const gateway = value('network_gateway');\n"
    "    if (primary && secondary && primary.ip === secondary.ip) invalid(get('network_secondary_address'), L.network);\n"
    "    if (gateway) {\n"
    "      if (!host(gateway)) invalid(get('network_gateway'));\n"
    "      else {\n"
    "        const g = number(gateway);\n"
    "        const reachable = address => address && !((g ^ address.ip) & address.mask) &&\n"
    "          (address.prefix >= 31 || ((g & ~address.mask) !== 0 && (g & ~address.mask) !== (~address.mask >>> 0)));\n"
    "        if ((primary && g === primary.ip) || (secondary && g === secondary.ip) ||\n"
    "            (primary && !reachable(primary) && !reachable(secondary)))\n"
    "          invalid(get('network_gateway'), L.network);\n"
    "      }\n"
    "    }\n"
    "    const reconnect = document.getElementById('network-reconnect');\n"
    "    reconnect.hidden = !primary || reconnect.dataset.sitl === 'true';\n"
    "    if (primary) {\n"
    "      const link = document.getElementById('network-link');\n"
    "      const url = new URL(location.href);\n"
    "      url.hostname = value('network_primary_address').split('/')[0];\n"
    "      url.pathname = '/parameters'; url.search = ''; url.hash = 'network';\n"
    "      link.href = url.href; link.textContent = url.href;\n"
    "    }\n"
    "    if (value('proxy_enabled') === 'true') {\n"
    "      if (!value('proxy_host')) invalid(get('proxy_host'));\n"
    "      if (value('proxy_signing') === 'true' && !value('proxy_signing_passphrase')) invalid(get('proxy_signing_passphrase'));\n"
    "      for (const n of [1, 2]) {\n"
    "        if (Number(value('proxy_video' + n + '_port')) && !value('proxy_video' + n + '_name')) invalid(get('proxy_video' + n + '_name'));\n"
    "      }\n"
    "      if (Number(value('proxy_video1_port')) && Number(value('proxy_video1_port')) === Number(value('proxy_video2_port')))\n"
    "        invalid(get('proxy_video2_port'), L.ports);\n"
    "    }\n"
    "    for (const field of fields) {\n"
    "      const bad = !field.validity.valid && (attempted || touched.has(field));\n"
    "      const error = document.getElementById(field.id + '-error');\n"
    "      field.setAttribute('aria-invalid', String(bad));\n"
    "      error.hidden = !bad;\n"
    "      error.textContent = bad ? field.validationMessage : '';\n"
    "    }\n"
    "    tabs.forEach((tab, i) => tab.classList.toggle('has-error', Boolean(panels[i].querySelector('[aria-invalid=true]'))));\n"
    "    return fields.find(field => !field.validity.valid);\n"
    "  }\n"
    "  for (const name of ['input', 'change']) form.addEventListener(name, event => {\n"
    "    touched.add(event.target);\n"
    "    validate();\n"
    "  });\n"
    "  form.addEventListener('submit', event => {\n"
    "    attempted = true;\n"
    "    const bad = validate();\n"
    "    if (bad) {\n"
    "      event.preventDefault();\n"
    "      reveal(bad); bad.focus();\n"
    "      bad.reportValidity();\n"
    "      return;\n"
    "    }\n"
    "    const reconnect = document.getElementById('network-reconnect');\n"
    "    const link = document.getElementById('network-link');\n"
    "    if (event.submitter && event.submitter.value === 'save_restart' && !reconnect.hidden &&\n"
    "        new URL(link.href).hostname !== location.hostname) {\n"
    "      // Keep the reconnect link visible if removing our address breaks the response.\n"
    "      event.preventDefault();\n"
    "      reveal(get('network_primary_address'));\n"
    "      const body = new URLSearchParams(new FormData(form));\n"
    "      body.set('action', 'save_restart');\n"
    "      const buttons = [...form.querySelectorAll('button[type=submit]')];\n"
    "      buttons.forEach(button => { button.disabled = true; });\n"
    "      // Named submit buttons shadow form.action; read the HTML attribute.\n"
    "      const endpoint = form.getAttribute('action').split('#')[0];\n"
    "      fetch(endpoint, {method: 'POST', body, signal: AbortSignal.timeout(45000)})\n"
    "        .then(response => response.text()).then(page => {\n"
    "          document.open(); document.write(page); document.close();\n"
    "        }).catch(() => {\n"
    "          const notice = document.createElement('p');\n"
    "          notice.className = 'notice error'; notice.textContent = L.connectionLost;\n"
    "          reconnect.before(notice);\n"
    "          buttons.forEach(button => { button.disabled = false; });\n"
    "        });\n"
    "    }\n"
    "  });\n"
    "  // Run our checks before native submission so dependent fields are checked too.\n"
    "  form.noValidate = true;\n"
    "  const bad = validate();\n"
    "  if (attempted && bad) { reveal(bad); bad.focus(); }\n"
    "})();\n"
    ;

static void send_parameters_script(int fd)
{
    const struct js_string items[] = {
        {"invalid", T(S_E_INVALID_VALUE)},
        {"address", T(S_H_NETWORK_ADDRESS)},
        {"ports", T(S_E_PROXY_VIDEO_PORTS)},
        {"network", T(S_E_NETWORK)},
        {"connectionLost", T(S_NETWORK_CONNECTION_LOST)},
    };
    send_script(fd, items, sizeof(items) / sizeof(items[0]), parameters_script);
}

#if WEB_HAVE_THERMAL
static const char sensors_script[] =
    "(() => {\n"
    "  const script = document.currentScript;\n"
    "  const lidar = document.getElementById('lidar');\n"
    "  const lidarToggle = document.getElementById('lidar-toggle');\n"
    "  const minimum = document.getElementById('thermal-min');\n"
    "  const maximum = document.getElementById('thermal-max');\n"
    "  const cpu = document.getElementById('cpu-temperature');\n"
    "  const status = document.getElementById('sensor-status');\n"
    "  if (!script || !lidar || !lidarToggle || !minimum || !maximum || !cpu || !status) return;\n"
    "  let issued = 0;\n"
    "  let applied = 0;\n"
    "  const temperature = (value, x, y) => value === null ? L.unavailable : value.toFixed(2) + L.temperatureAt + x + ', ' + y + ')';\n"
    "  const refresh = async () => {\n"
    "    const requestId = ++issued;\n"
    "    try {\n"
    "      const response = await fetch('/sensors.json', {cache: 'no-store'});\n"
    "      if (!response.ok) throw new Error('HTTP ' + response.status);\n"
    "      const data = await response.json();\n"
    "      if (requestId < applied) return;\n"
    "      applied = requestId;\n"
    "      if (data.lidar_enabled === false) lidar.textContent = L.disabled;\n"
    "      else lidar.textContent = data.lidar_m === null ? L.unavailable : (data.lidar_m === 0 ? L.noReturn : data.lidar_m.toFixed(1) + ' m');\n"
    "      lidarToggle.textContent = data.lidar_enabled ? L.disable : L.enable;\n"
    "      lidarToggle.dataset.action = data.lidar_enabled ? 'disable' : 'enable';\n"
    "      lidarToggle.disabled = data.lidar_enabled === null;\n"
    "      minimum.textContent = temperature(data.minimum_c, data.minimum_x, data.minimum_y);\n"
    "      maximum.textContent = temperature(data.maximum_c, data.maximum_x, data.maximum_y);\n"
    "      cpu.textContent = data.cpu_c === null ? L.unavailable : data.cpu_c.toFixed(1) + ' °C';\n"
    "      status.textContent = L.updated + new Date().toLocaleTimeString() + L.twicePerSecond;\n"
    "    } catch (error) {\n"
    "      if (requestId < applied) return;\n"
    "      applied = requestId;\n"
    "      lidar.textContent = minimum.textContent = maximum.textContent = cpu.textContent = L.unavailable;\n"
    "      lidarToggle.disabled = true;\n"
    "      status.textContent = L.sensorFailed + (error && error.message ? error.message : L.unknownError);\n"
    "    }\n"
    "  };\n"
    "  lidarToggle.addEventListener('click', async () => {\n"
    "    const action = lidarToggle.dataset.action;\n"
    "    if (action !== 'enable' && action !== 'disable') return;\n"
    "    lidarToggle.disabled = true;\n"
    "    const body = new URLSearchParams({csrf: script.dataset.csrf, action});\n"
    "    try {\n"
    "      const response = await fetch('/sensors/lidar', {method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded'}, body});\n"
    "      if (!response.ok) throw new Error((await response.text()).trim() || ('HTTP ' + response.status));\n"
    "      lidar.textContent = action === 'enable' ? L.waitingRange : L.disabled;\n"
    "      lidarToggle.textContent = action === 'enable' ? L.disable : L.enable;\n"
    "      lidarToggle.dataset.action = action === 'enable' ? 'disable' : 'enable';\n"
    "      status.textContent = action === 'enable' ? L.lidarEnabled : L.lidarDisabled;\n"
    "    } catch (error) {\n"
    "      status.textContent = L.lidarControlFailed + (error && error.message ? error.message : L.unknownError);\n"
    "    } finally {\n"
    "      lidarToggle.disabled = false;\n"
    "      window.setTimeout(refresh, 100);\n"
    "    }\n"
    "  });\n"
    "  refresh();\n"
    "  window.setInterval(refresh, 500);\n"
    "})();\n";

static void send_sensors_script(int fd)
{
    const struct js_string items[] = {
        {"unavailable", T(S_JS_UNAVAILABLE)}, {"temperatureAt", T(S_JS_TEMPERATURE_AT)},
        {"disabled", T(S_OPT_DISABLED)}, {"noReturn", T(S_JS_NO_RETURN)},
        {"disable", T(S_DISABLE)}, {"enable", T(S_ENABLE)},
        {"updated", T(S_JS_UPDATED)}, {"twicePerSecond", T(S_JS_TWICE_PER_SECOND)},
        {"sensorFailed", T(S_JS_SENSOR_FAILED)}, {"unknownError", T(S_JS_UNKNOWN_ERROR)},
        {"waitingRange", T(S_JS_WAITING_RANGE)}, {"lidarEnabled", T(S_LIDAR_ENABLED)},
        {"lidarDisabled", T(S_LIDAR_DISABLED)},
        {"lidarControlFailed", T(S_JS_LIDAR_CONTROL_FAILED)},
    };

    send_script(fd, items, sizeof(items) / sizeof(items[0]), sensors_script);
}
#endif

static const char live_script[] =
    "(() => {\n"
    "  const script = document.currentScript;\n"
    "  const video = document.getElementById('live-video');\n"
    "  const stream = document.getElementById('live-stream');\n"
    "  const status = document.getElementById('live-status');\n"
    "  const enable = document.getElementById('manual-enable');\n"
    "  const controlStatus = document.getElementById('control-status');\n"
    "  const rate = document.getElementById('live-rate');\n"
    "  const rateValue = document.getElementById('live-rate-value');\n"
    "  const zoom = document.getElementById('live-zoom');\n"
    "  const zoomValue = document.getElementById('live-zoom-value');\n"
    "  const attitudeDial = document.getElementById('attitude-dial');\n"
    "  const attitudeWorld = document.getElementById('attitude-world');\n"
    "  const attitudeHeading = document.getElementById('attitude-heading');\n"
    "  const attitudeRoll = document.getElementById('attitude-roll');\n"
    "  const attitudePitch = document.getElementById('attitude-pitch');\n"
    "  const attitudeYaw = document.getElementById('attitude-yaw');\n"
    "  const attitudeRollRate = document.getElementById('attitude-roll-rate');\n"
    "  const attitudePitchRate = document.getElementById('attitude-pitch-rate');\n"
    "  const attitudeYawRate = document.getElementById('attitude-yaw-rate');\n"
    "  const attitudeStatus = document.getElementById('attitude-status');\n"
    "  if (!script || !video || !stream || !status || !enable || !controlStatus || !rate || !rateValue || !zoom || !zoomValue || !attitudeDial || !attitudeWorld || !attitudeHeading || !attitudeRoll || !attitudePitch || !attitudeYaw || !attitudeRollRate || !attitudePitchRate || !attitudeYawRate || !attitudeStatus) return;\n"
    "  let retry = null;\n"
    "  let startedAt = 0;\n"
    "  const start = () => {\n"
    "    if (retry !== null) { window.clearTimeout(retry); retry = null; }\n"
    "    startedAt = performance.now();\n"
    "    status.textContent = L.connecting;\n"
    "    video.src = '/live/video' + (Number(stream.value) + 1) + '.mp4?start=' + Date.now();\n"
    "    video.load(); video.play().catch(() => {});\n"
    "  };\n"
    "  const reconnect = message => {\n"
    "    status.textContent = message + L.retrying;\n"
    "    if (retry === null) retry = window.setTimeout(start, 1500);\n"
    "  };\n"
    "  video.addEventListener('loadedmetadata', () => { status.textContent = L.livePrefix + video.videoWidth + '×' + video.videoHeight; });\n"
    "  video.addEventListener('playing', () => { status.textContent = L.livePrefix + video.videoWidth + '×' + video.videoHeight; });\n"
    "  video.addEventListener('ended', () => reconnect(L.streamEnded));\n"
    "  video.addEventListener('error', () => {\n"
    "    const names = ['', L.errorAborted, L.errorNetwork, L.errorDecode, L.errorUnsupported];\n"
    "    const detail = video.error ? (names[video.error.code] || (L.errorCode + video.error.code)) + (video.error.message ? ': ' + video.error.message : '') : L.unknownError;\n"
    "    reconnect(L.liveUnavailable + detail);\n"
    "  });\n"
    "  // Native players can keep playing an old buffer after an underrun.\n"
    "  // Reopen at a fresh IDR: seeking a growing HTTP MP4 can fail or decode\n"
    "  // the old backlog again. Leave an intentional pause alone.\n"
    "  const catchUp = () => {\n"
    "    if (video.paused || video.seeking || video.readyState < 2 || !video.buffered.length) return;\n"
    "    if (performance.now() - startedAt < 5000) return; // let a new decoder settle\n"
    "    const last = video.buffered.length - 1;\n"
    "    const end = video.buffered.end(last);\n"
    "    if (end - video.currentTime > 1.5) start();\n"
    "  };\n"
    "  window.setInterval(catchUp, 250);\n"
    "  stream.addEventListener('change', start); start();\n"
    "  let lease = '', held = null, renewing = false, leaving = false;\n"
    "  const controls = [...document.querySelectorAll('[data-direction]'), document.getElementById('live-center'), rate, zoom];\n"
    "  const refreshControls = () => { controls.forEach(control => { control.disabled = !lease; }); };\n"
    "  enable.checked = false; refreshControls();\n"
    "  const controlRequest = async (action, token, value) => {\n"
    "    const body = new URLSearchParams({csrf: script.dataset.csrf, action, lease: token});\n"
    "    if (value !== undefined) body.set('value', value);\n"
    "    const controller = new AbortController();\n"
    "    const timeout = window.setTimeout(() => controller.abort(), 1500);\n"
    "    try {\n"
    "      const response = await fetch('/live/control', {method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded'}, body, signal: controller.signal});\n"
    "      const message = (await response.text()).trim();\n"
    "      if (!response.ok) throw new Error(message || ('HTTP ' + response.status));\n"
    "      return message;\n"
    "    } finally { window.clearTimeout(timeout); }\n"
    "  };\n"
    "  const dropControl = () => {\n"
    "    const previous = lease; lease = ''; held = null; enable.checked = false; refreshControls();\n"
    "    if (previous) fetch('/live/control', {method: 'POST', keepalive: true,\n"
    "      body: new URLSearchParams({csrf: script.dataset.csrf, action: 'release', lease: previous})}).catch(() => {});\n"
    "  };\n"
    "  enable.addEventListener('change', async () => {\n"
    "    enable.disabled = true;\n"
    "    try {\n"
    "      if (enable.checked) {\n"
    "        const token = await controlRequest('acquire', '');\n"
    "        if (!/^[0-9a-f]{32}$/.test(token)) throw new Error(L.unknownError);\n"
    "        lease = token;\n"
    "        if (leaving) dropControl();\n"
    "        else controlStatus.textContent = L.manualEnabled;\n"
    "      } else {\n"
    "        const previous = lease; lease = ''; held = null; refreshControls();\n"
    "        if (previous) await controlRequest('release', previous);\n"
    "        controlStatus.textContent = L.manualDisabled;\n"
    "      }\n"
    "    } catch (error) {\n"
    "      dropControl(); controlStatus.textContent = L.controlFailed + error.message;\n"
    "    } finally { enable.checked = !!lease; enable.disabled = false; refreshControls(); }\n"
    "  });\n"
    "  window.setInterval(async () => {\n"
    "    if (!lease || renewing) return;\n"
    "    const previous = lease; renewing = true;\n"
    "    try { await controlRequest('renew', previous); }\n"
    "    catch (error) { if (lease === previous) { dropControl(); controlStatus.textContent = L.controlFailed + error.message; } }\n"
    "    finally { renewing = false; }\n"
    "  }, 1000);\n"
    "  window.addEventListener('pagehide', () => { leaving = true; dropControl(); });\n"
    "  window.addEventListener('pageshow', () => { leaving = false; });\n"
    "  const sendControl = async (action, value) => {\n"
    "    if (!lease) { controlStatus.textContent = L.enableFirst; return false; }\n"
    "    const previous = lease;\n"
    "    try {\n"
    "      const message = await controlRequest(action, previous, value);\n"
    "      controlStatus.textContent = message || L.commandSent;\n"
    "      await new Promise(resolve => window.setTimeout(resolve, 200));\n"
    "      return lease === previous;\n"
    "    } catch (error) {\n"
    "      if (lease === previous) dropControl();\n"
    "      controlStatus.textContent = L.controlFailed + error.message; return false;\n"
    "    }\n"
    "  };\n"
    "  const release = () => { held = null; };\n"
    "  document.querySelectorAll('[data-direction]').forEach(button => button.addEventListener('pointerdown', async event => {\n"
    "    event.preventDefault(); if (!enable.checked || held) return; held = button.dataset.direction; button.setPointerCapture(event.pointerId);\n"
    "    const direction = held; while (held === direction && await sendControl(direction, Number(rate.value).toFixed(0))) {}\n"
    "  }));\n"
    "  window.addEventListener('pointerup', release); window.addEventListener('pointercancel', release); window.addEventListener('blur', release);\n"
    "  document.getElementById('live-center').addEventListener('click', () => sendControl('center'));\n"
    "  rate.addEventListener('input', () => { rateValue.textContent = Number(rate.value).toFixed(0) + '°/s'; });\n"
    "  zoom.addEventListener('input', () => { zoomValue.textContent = Number(zoom.value).toFixed(1) + 'x'; });\n"
    "  zoom.addEventListener('change', () => sendControl('zoom', Number(zoom.value).toFixed(1)));\n"
    "  let attitudePending = false;\n"
    "  const attitudeUnavailable = message => {\n"
    "    attitudeRoll.textContent = attitudePitch.textContent = attitudeYaw.textContent = '—';\n"
    "    attitudeRollRate.textContent = attitudePitchRate.textContent = attitudeYawRate.textContent = '—';\n"
    "    attitudeWorld.style.transform = 'translateY(0) rotate(0)';\n"
    "    attitudeHeading.style.transform = 'rotate(0)';\n"
    "    attitudeDial.setAttribute('aria-label', L.attitudeUnavailable);\n"
    "    attitudeStatus.textContent = message;\n"
    "  };\n"
    "  const refreshAttitude = async () => {\n"
    "    if (attitudePending) return; attitudePending = true;\n"
    "    try {\n"
    "      const response = await fetch('/live/attitude.json', {cache: 'no-store'});\n"
    "      if (!response.ok) throw new Error('HTTP ' + response.status);\n"
    "      const data = await response.json();\n"
    "      if (data.roll_deg === null || data.pitch_deg === null || data.yaw_deg === null) { attitudeUnavailable(L.attitudeUnavailable); return; }\n"
    "      const roll = Number(data.roll_deg), pitch = Number(data.pitch_deg), yaw = Number(data.yaw_deg);\n"
    "      attitudeRoll.textContent = roll.toFixed(1) + '°'; attitudePitch.textContent = pitch.toFixed(1) + '°'; attitudeYaw.textContent = yaw.toFixed(1) + '°';\n"
    "      attitudeRollRate.textContent = data.roll_rate_dps === null ? '—' : Number(data.roll_rate_dps).toFixed(1) + '°/s';\n"
    "      attitudePitchRate.textContent = data.pitch_rate_dps === null ? '—' : Number(data.pitch_rate_dps).toFixed(1) + '°/s';\n"
    "      attitudeYawRate.textContent = data.yaw_rate_dps === null ? '—' : Number(data.yaw_rate_dps).toFixed(1) + '°/s';\n"
    "      const pitchPixels = Math.max(-55, Math.min(55, pitch * 1.15));\n"
    "      attitudeWorld.style.transform = 'translateY(' + pitchPixels + 'px) rotate(' + (-roll) + 'deg)';\n"
    "      attitudeHeading.style.transform = 'rotate(' + yaw + 'deg)';\n"
    "      attitudeDial.setAttribute('aria-label', L.attitudeLabel.replace('%s', roll.toFixed(1)).replace('%s', pitch.toFixed(1)).replace('%s', yaw.toFixed(1)));\n"
    "      attitudeStatus.textContent = L.liveAttitude;\n"
    "    } catch (error) { attitudeUnavailable(L.attitudeFailed + (error && error.message ? error.message : L.unknownError)); }\n"
    "    finally { attitudePending = false; }\n"
    "  };\n"
    "  refreshAttitude(); window.setInterval(refreshAttitude, 250);\n"
    "})();\n";

static void send_live_script(int fd)
{
    const struct js_string items[] = {
        {"connecting", T(S_JS_CONNECTING)}, {"retrying", T(S_JS_RETRYING)},
        {"livePrefix", T(S_JS_LIVE_PREFIX)}, {"streamEnded", T(S_JS_STREAM_ENDED)},
        {"liveUnavailable", T(S_JS_LIVE_UNAVAILABLE)},
        {"errorAborted", T(S_JS_ERROR_ABORTED)}, {"errorNetwork", T(S_JS_ERROR_NETWORK)},
        {"errorDecode", T(S_JS_ERROR_DECODE)}, {"errorUnsupported", T(S_JS_ERROR_UNSUPPORTED)},
        {"errorCode", T(S_JS_ERROR_CODE)}, {"unknownError", T(S_JS_UNKNOWN_ERROR)},
        {"enableFirst", T(S_JS_ENABLE_FIRST)}, {"commandSent", T(S_COMMAND_SENT)},
        {"controlFailed", T(S_JS_CONTROL_FAILED)}, {"manualEnabled", T(S_JS_MANUAL_ENABLED)},
        {"manualDisabled", T(S_JS_MANUAL_DISABLED)},
        {"attitudeUnavailable", T(S_LIVE_ATTITUDE_UNAVAILABLE)},
        {"attitudeLabel", T(S_JS_ATTITUDE_LABEL)}, {"liveAttitude", T(S_JS_LIVE_ATTITUDE)},
        {"attitudeFailed", T(S_JS_ATTITUDE_FAILED)},
    };

    send_script(fd, items, sizeof(items) / sizeof(items[0]), live_script);
}

static const char status_script[] =
    "(() => {\n"
    "  const form = document.getElementById('time-sync');\n"
    "  const browserTime = document.getElementById('browser-time-ms');\n"
    "  if (!form || !browserTime) return;\n"
    "  form.addEventListener('submit', () => { browserTime.value = Date.now().toString(); });\n"
    "})();\n";

/* the nav and login language selectors apply on change; the fallback
 * button is hidden once the script runs */
static const char language_script[] =
    "(() => {\n"
    "  const nav = document.getElementById('lang-nav');\n"
    "  if (nav && nav.form) {\n"
    "    nav.form.querySelectorAll('.lang-apply').forEach(button => { button.hidden = true; });\n"
    "    nav.addEventListener('change', () => nav.form.submit());\n"
    "  }\n"
    "  const login = document.getElementById('lang-login');\n"
    "  if (login) login.addEventListener('change', () => { window.location.replace('/login?lang=' + encodeURIComponent(login.value)); });\n"
    "})();\n";

static const char upgrade_script[] =
    "(() => {\n"
    "  const script = document.currentScript;\n"
    "  const form = document.getElementById('firmware-upload');\n"
    "  if (!form || !script) return;\n"
    "  const input = document.getElementById('firmware');\n"
    "  const button = document.getElementById('select-firmware');\n"
    "  const progress = document.getElementById('firmware-progress');\n"
    "  const status = document.getElementById('firmware-status');\n"
    "  if (!input || !button || !progress || !status) return;\n"
    "  const waitTimeout = 60000;\n"
    "  const pollDelay = 1000;\n"
    "  const waitForRestart = () => {\n"
    "    const started = Date.now();\n"
    "    let disconnected = false;\n"
    "    const poll = () => {\n"
    "      const elapsed = Date.now() - started;\n"
    "      progress.value = Math.min(95, 50 + elapsed * 45 / waitTimeout);\n"
    "      if (elapsed >= waitTimeout) {\n"
    "        status.textContent = L.timeout;\n"
    "        alert(L.timeoutAlert);\n"
    "        return;\n"
    "      }\n"
    "      const check = new XMLHttpRequest();\n"
    "      check.open('GET', '/upgrade-status?t=' + Date.now());\n"
    "      check.timeout = 2000;\n"
    "      check.onload = () => {\n"
    "        if (check.status === 401) {\n"
    "          progress.hidden = true;\n"
    "          status.textContent = L.backLogin;\n"
    "          alert(L.backLogin);\n"
    "          window.location.replace('/login');\n"
    "          return;\n"
    "        }\n"
    "        if (check.status === 200 && /^[0-9a-f]{64}$/.test(check.responseText.trim()) && check.responseText.trim() !== script.dataset.csrf) {\n"
    "          progress.value = 100;\n"
    "          status.textContent = L.back;\n"
    "          alert(L.backAlert);\n"
    "          window.location.reload();\n"
    "          return;\n"
    "        }\n"
    "        status.textContent = disconnected ? L.rebooting : L.waitingUpdater;\n"
    "        setTimeout(poll, pollDelay);\n"
    "      };\n"
    "      check.onerror = check.ontimeout = () => {\n"
    "        disconnected = true;\n"
    "        status.textContent = L.rebooting;\n"
    "        setTimeout(poll, pollDelay);\n"
    "      };\n"
    "      check.send();\n"
    "    };\n"
    "    poll();\n"
    "  };\n"
    "  button.addEventListener('click', () => input.click());\n"
    "  input.addEventListener('change', () => {\n"
    "    const file = input.files && input.files[0];\n"
    "    if (!file) return;\n"
    "    const pattern = new RegExp('^' + L.prefix + '[A-Za-z0-9._-]+' + L.suffix.replace(/\\./g, '\\\\.') + '$');\n"
    "    if (!(pattern.test(file.name) || (L.installName && file.name === L.installName))) {\n"
    "      status.textContent = L.badName; return;\n"
    "    }\n"
    "    if (file.size < 1 || file.size > 134217728) {\n"
    "      status.textContent = L.badSize; return;\n"
    "    }\n"
    "    if (!confirm(L.confirmUpload.replace('%s', file.name))) return;\n"
    "    const request = new XMLHttpRequest();\n"
    "    request.open('POST', '/upgrade');\n"
    "    request.setRequestHeader('Content-Type', 'application/octet-stream');\n"
    "    request.setRequestHeader('X-CSRF-Token', script.dataset.csrf);\n"
    "    request.setRequestHeader('X-Firmware-Name', file.name);\n"
    "    request.upload.onprogress = event => {\n"
    "      if (event.lengthComputable) { progress.hidden = false; progress.value = event.loaded * 50 / event.total; }\n"
    "      status.textContent = L.writing.replace('%s', L.mediaRoot + '/' + file.name + '.tmp');\n"
    "    };\n"
    "    request.onload = () => {\n"
    "      status.textContent = request.responseText.trim() || (L.httpStatus + request.status);\n"
    "      if (request.status === 201 && " WEB_UPGRADE_REBOOTS_JS ") {\n"
    "        progress.value = 50;\n"
    "        status.textContent = L.uploaded;\n"
    "        waitForRestart();\n"
    "      } else if (request.status === 201) {\n"
    "        progress.value = 100;\n"
    "      } else { button.disabled = false; input.disabled = false; input.value = ''; }\n"
    "    };\n"
    "    request.onerror = () => {\n"
    "      status.textContent = L.closed;\n"
    "      button.disabled = false; input.disabled = false;\n"
    "    };\n"
    "    button.disabled = true; input.disabled = true; progress.hidden = false; progress.value = 0;\n"
    "    request.send(file);\n"
    "  });\n"
    "})();\n";

#if WEB_INSTALLS_FIRMWARE
#define UPGRADE_CONFIRM_STRING S_JS_FW_CONFIRM_Z1
#define UPGRADE_UPLOADED_STRING S_JS_FW_INSTALLED
#define UPGRADE_WRITE_ROOT RUNTIME_DIR
#else
#define UPGRADE_CONFIRM_STRING TARGET_TEXT(S_JS_FW_CONFIRM, S_JS_FW_CONFIRM_A8)
#define UPGRADE_UPLOADED_STRING S_JS_FW_UPLOADED
#define UPGRADE_WRITE_ROOT MEDIA_ROOT
#endif
#ifndef FIRMWARE_INSTALL_NAME
#define FIRMWARE_INSTALL_NAME_JS ""
#else
#define FIRMWARE_INSTALL_NAME_JS FIRMWARE_INSTALL_NAME
#endif

static void send_upgrade_script(int fd)
{
    char bad_name[256];
    const struct js_string items[] = {
        {"mediaRoot", UPGRADE_WRITE_ROOT},
        {"prefix", FIRMWARE_PREFIX}, {"suffix", FIRMWARE_SUFFIX},
        {"installName", FIRMWARE_INSTALL_NAME_JS},
        {"timeout", T(S_JS_FW_TIMEOUT)}, {"timeoutAlert", T(S_JS_FW_TIMEOUT_ALERT)},
        {"back", T(S_JS_FW_BACK)}, {"backAlert", T(S_JS_FW_BACK_ALERT)},
        {"backLogin", T(S_JS_FW_BACK_LOGIN)},
        {"rebooting", T(S_JS_FW_REBOOTING)}, {"waitingUpdater", T(S_JS_FW_WAITING_UPDATER)},
        {"badName", bad_name}, {"badSize", T(S_JS_FW_SIZE)},
        {"confirmUpload", T(UPGRADE_CONFIRM_STRING)},
        {"writing", T(S_JS_FW_WRITING)},
        {"httpStatus", T(S_JS_FW_HTTP)}, {"uploaded", T(UPGRADE_UPLOADED_STRING)},
        {"closed", T(S_JS_FW_CLOSED)},
    };

    snprintf(bad_name, sizeof(bad_name), T(S_JS_FW_NAME), FIRMWARE_NAME_PATTERNS);
    send_script(fd, items, sizeof(items) / sizeof(items[0]), upgrade_script);
}

#if WEB_HAVE_SSH_KEYS
static const char users_script[] =
    "(() => {\n"
    "  const form = document.getElementById('ssh-key-upload');\n"
    "  const input = document.getElementById('public-key-files');\n"
    "  const data = document.getElementById('public-key-data');\n"
    "  const select = document.getElementById('select-public-key-files');\n"
    "  const status = document.getElementById('public-key-status');\n"
    "  if (!form || !input || !data || !select || !status) return;\n"
    "  select.addEventListener('click', () => input.click());\n"
    "  input.addEventListener('change', async () => {\n"
    "    const files = Array.from(input.files || []);\n"
    "    if (!files.length) return;\n"
    "    if (files.some(file => !/\\.pub$/i.test(file.name))) {\n"
    "      status.textContent = L.selectPub; return;\n"
    "    }\n"
    "    select.disabled = true;\n"
    "    status.textContent = L.reading;\n"
    "    try {\n"
    "      const contents = await Promise.all(files.map(file => file.text()));\n"
    "      const combined = contents.map(text => text.trim()).filter(Boolean).join('\\n');\n"
    "      if (!combined) throw new Error(L.empty);\n"
    "      if (new TextEncoder().encode(combined).length > 65536) {\n"
    "        throw new Error(L.tooLarge);\n"
    "      }\n"
    "      data.value = combined;\n"
    "      status.textContent = L.uploading;\n"
    "      form.submit();\n"
    "    } catch (error) {\n"
    "      status.textContent = error && error.message ? error.message : L.unreadable;\n"
    "      select.disabled = false;\n"
    "    }\n"
    "  });\n"
    "})();\n";

static void send_users_script(int fd)
{
    const struct js_string items[] = {
        {"selectPub", T(S_JS_SELECT_PUB)}, {"reading", T(S_JS_READING_KEYS)},
        {"empty", T(S_JS_FILES_EMPTY)}, {"tooLarge", T(S_JS_KEYS_TOO_LARGE)},
        {"uploading", T(S_JS_UPLOADING_KEYS)}, {"unreadable", T(S_JS_FILES_UNREADABLE)},
    };

    send_script(fd, items, sizeof(items) / sizeof(items[0]), users_script);
}
#endif

static bool valid_firmware_name(const char *name)
{
#ifdef FIRMWARE_INSTALL_NAME
    if (strcmp(name, FIRMWARE_INSTALL_NAME) == 0) return true;
#endif
    size_t length = strlen(name);
    const char prefix[] = FIRMWARE_PREFIX;
    const char suffix[] = FIRMWARE_SUFFIX;

    if (length <= strlen(prefix) + strlen(suffix) || length > 200 ||
        strncmp(name, prefix, strlen(prefix)) != 0 ||
        strcmp(name + length - strlen(suffix), suffix) != 0) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char value = (unsigned char)name[i];
        if (!(isalnum(value) || value == '.' || value == '_' || value == '-')) return false;
    }
    return true;
}

static void sync_firmware_storage(void)
{
#ifndef MT11_WEB_TEST
    sync();
#endif
}

#if APCAM_TARGET == APCAM_TARGET_ZR10
#include "zr10_firmware.h"
#endif

/* Stream the request body to output; errno describes a failure. */
static bool receive_upload_body(int fd, const struct request *request, int output,
                                size_t *received)
{
    char expect[64];

    *received = 0;
    if (find_header(request, "Expect", expect, sizeof(expect)) != NULL &&
        strcasecmp(expect, "100-continue") == 0) {
        static const char continue_response[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (!send_all(fd, continue_response, sizeof(continue_response) - 1)) return false;
    }
    if (request->body_len > 0) {
        if (!write_all(output, request->body, request->body_len)) return false;
        *received = request->body_len;
    }
    while (*received < request->content_length) {
        char buffer[64 * 1024];
        size_t wanted = request->content_length - *received;
        if (wanted > sizeof(buffer)) wanted = sizeof(buffer);
        ssize_t got = recv(fd, buffer, wanted, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (got == 0) {
            errno = ECONNRESET;
            return false;
        }
        if (!write_all(output, buffer, (size_t)got)) return false;
        *received += (size_t)got;
    }
    return true;
}

#if WEB_INSTALLS_FIRMWARE
/* The .gcu overlay is a ZIP of the gcu/ap and gcu/ipc files. It is checked before
 * extraction, unpacked next to GCU_ROOT, verified against its SHA256SUMS and
 * manifest, then exchanged with the running installation before a reboot. */
#ifndef GCU_PACKAGE_MAX_EXTRACTED
#define GCU_PACKAGE_MAX_EXTRACTED (64U * 1024U * 1024U)
#endif
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE 2
#endif
static void schedule_reboot(void);

/* Atomically swap two paths; unsupported filesystems fail safely. */
static int exchange_paths(const char *a, const char *b)
{
#ifdef __CYGWIN__
#ifdef WEB_PORTABLE_SITL
    /* The simulator's Cygwin filesystem has no renameat2 exchange. Keep the
     * old tree recoverable while swapping this isolated temporary runtime. */
    char backup[PATH_MAX];
    if (snprintf(backup, sizeof(backup), "%s.exchange", b) >= (int)sizeof(backup)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (access(backup, F_OK) == 0) { errno = EEXIST; return -1; }
    if (errno != ENOENT) return -1;
    if (rename(b, backup) < 0) return -1;
    if (rename(a, b) < 0) {
        int saved = errno;
        (void)rename(backup, b);
        errno = saved;
        return -1;
    }
    if (rename(backup, a) < 0) {
        int saved = errno;
        (void)rename(b, a);
        (void)rename(backup, b);
        errno = saved;
        return -1;
    }
    return 0;
#else
    (void)a;
    (void)b;
    errno = ENOSYS;
    return -1;
#endif
#else
#ifdef MT11_WEB_TEST
    const char *failure_marker = getenv("CAMERA_GIMBAL_TEST_EXCHANGE_FAIL");
    if (failure_marker != NULL && access(failure_marker, F_OK) == 0) {
        errno = ENOSYS;
        return -1;
    }
#endif
#ifdef SYS_renameat2
    return (int)syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b, RENAME_EXCHANGE);
#else
    (void)a;
    (void)b;
    errno = ENOSYS;
    return -1;
#endif
#endif
}

static uint32_t zip_le32(const unsigned char *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static unsigned zip_le16(const unsigned char *p)
{
    return p[0] | (unsigned)p[1] << 8;
}

static bool gcu_entry_name_ok(const char *name, size_t length)
{
    static const char *const allowed[] = {
        "gcu/ap/SHA256SUMS", "gcu/ap/manifest.json", "gcu/ap/service.sh",
        "gcu/ap/camera-app", "gcu/ap/z1mini-web", "gcu/ap/ax-capture",
        "gcu/ap/camera.ini.default", "gcu/ap/web.pass.default", "gcu/ap/README.md",
        "gcu/ipc/run.sh", "gcu/ipc/camera_gcu.sh",
    };

    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strlen(allowed[i]) == length && memcmp(name, allowed[i], length) == 0) {
            return true;
        }
    }
    return false;
}

struct gcu_package_index {
    unsigned count;
    char names[256][256];
};

/* Check the ZIP central directory before anything is extracted. Returns a
 * problem description or NULL, and the total extracted size. */
static const char *check_gcu_package(int fd, size_t size, uint64_t *extracted,
                                     struct gcu_package_index *index)
{
    static const char *const required[] = {
        "gcu/ap/SHA256SUMS", "gcu/ap/manifest.json", "gcu/ap/service.sh",
        "gcu/ap/camera-app", "gcu/ap/z1mini-web", "gcu/ipc/run.sh", "gcu/ipc/camera_gcu.sh",
    };
    static char missing[64];
    bool found[sizeof(required) / sizeof(required[0])] = {false};
    static unsigned char tail[22 + 65535];
    size_t tail_length = size < sizeof(tail) ? size : sizeof(tail);
    unsigned char *directory;
    const char *problem = "not a ZIP archive";
    size_t eocd = tail_length;

    *extracted = 0;
    index->count = 0;
    if (size < 22 ||
        pread(fd, tail, tail_length, (off_t)(size - tail_length)) != (ssize_t)tail_length) {
        return problem;
    }
    for (size_t i = tail_length - 22;; i--) {
        if (zip_le32(tail + i) == 0x06054b50 && i + 22 + zip_le16(tail + i + 20) == tail_length) {
            eocd = i;
            break;
        }
        if (i == 0) break;
    }
    if (eocd == tail_length) return problem;
    const unsigned char *end = tail + eocd;
    unsigned entries = zip_le16(end + 10);
    uint32_t directory_size = zip_le32(end + 12);
    uint32_t directory_offset = zip_le32(end + 16);
    uint64_t eocd_position = size - tail_length + eocd;
    if (zip_le16(end + 4) != 0 || zip_le16(end + 6) != 0 || entries != zip_le16(end + 8)) {
        return "multi-part archive";
    }
    if (entries == 0xFFFF || directory_size == 0xFFFFFFFF || directory_offset == 0xFFFFFFFF) {
        return "ZIP64 archive";
    }
    if (entries == 0 || entries > 256) return "unexpected entry count";
    if ((uint64_t)directory_offset + directory_size > eocd_position) {
        return "central directory out of range";
    }
    /* Only our generated flat package format is accepted. Bound the extra
     * fields/comments too, so entries cannot force a multi-megabyte malloc. */
    uint64_t max_directory = (uint64_t)entries * (46U + 207U + 2048U);
    if (directory_size > max_directory) return "central directory too large";
    directory = malloc(directory_size);
    if (directory == NULL) return "out of memory";
    if (pread(fd, directory, directory_size, directory_offset) != (ssize_t)directory_size) {
        free(directory);
        return "cannot read central directory";
    }
    size_t position = 0;
    for (unsigned n = 0; n < entries; n++) {
        const unsigned char *header = directory + position;
        if (position + 46 > directory_size || zip_le32(header) != 0x02014b50) {
            problem = "corrupt central directory";
            goto fail;
        }
        unsigned method = zip_le16(header + 10);
        uint32_t uncompressed = zip_le32(header + 24);
        unsigned name_length = zip_le16(header + 28);
        unsigned extra_length = zip_le16(header + 30);
        unsigned comment_length = zip_le16(header + 32);
        if (extra_length > 1024 || comment_length > 1024) {
            problem = "oversized central directory entry";
            goto fail;
        }
        size_t entry_length = 46 + name_length + extra_length + comment_length;
        unsigned mode = zip_le32(header + 38) >> 16;
        const char *name = (const char *)header + 46;
        if (position + entry_length > directory_size) {
            problem = "corrupt central directory";
            goto fail;
        }
        if (!gcu_entry_name_ok(name, name_length)) {
            problem = "unexpected file in archive";
            goto fail;
        }
        for (unsigned previous = 0; previous < n; previous++) {
            if (strlen(index->names[previous]) == name_length &&
                memcmp(index->names[previous], name, name_length) == 0) {
                problem = "duplicate archive entry";
                goto fail;
            }
        }
        if (method != 0 && method != 8) {
            problem = "unsupported compression";
            goto fail;
        }
        if (zip_le16(header + 8) & 0x41) {
            problem = "encrypted entry";
            goto fail;
        }
        if ((mode & S_IFMT) != 0 && (mode & S_IFMT) != S_IFREG) {
            problem = "archive contains a non-regular file";
            goto fail;
        }
        if (uncompressed == 0xFFFFFFFF) {
            problem = "ZIP64 entry";
            goto fail;
        }
        *extracted += uncompressed;
        if (*extracted > GCU_PACKAGE_MAX_EXTRACTED) {
            problem = "package too large";
            goto fail;
        }
        if (name_length >= sizeof(index->names[0])) {
            problem = "file name too long";
            goto fail;
        }
        memcpy(index->names[n], name, name_length);
        index->names[n][name_length] = '\0';
        for (size_t r = 0; r < sizeof(required) / sizeof(required[0]); r++) {
            if (strlen(required[r]) == name_length && memcmp(required[r], name, name_length) == 0) {
                found[r] = true;
            }
        }
        position += entry_length;
    }
    index->count = entries;
    for (size_t r = 0; r < sizeof(required) / sizeof(required[0]); r++) {
        if (found[r]) continue;
        snprintf(missing, sizeof(missing), "missing %s", required[r]);
        problem = missing;
        goto fail;
    }
    free(directory);
    return NULL;
fail:
    free(directory);
    return problem;
}

/* Run a tool to completion with no terminal; the server ignores SIGCHLD. */
static int run_tool(const char *directory, char *const argv[], int client,
                    rlim_t max_file_size)
{
    struct sigaction previous;
    struct sigaction reap = {.sa_handler = SIG_DFL};
    pid_t child;
    int status;
    int result = -1;

    sigaction(SIGCHLD, &reap, &previous);
    child = fork();
    if (child == 0) {
        int null = open("/dev/null", O_RDWR);
        if (listen_fd >= 0) close(listen_fd);
        if (client >= 0) close(client);
        if (null >= 0) {
            dup2(null, 0);
            dup2(null, 1);
            dup2(null, 2);
            if (null > 2) close(null);
        }
        if (max_file_size != RLIM_INFINITY) {
            struct rlimit limit = {
                .rlim_cur = max_file_size,
                .rlim_max = max_file_size,
            };
            if (setrlimit(RLIMIT_FSIZE, &limit) < 0) _exit(126);
        }
        if (directory != NULL && chdir(directory) < 0) _exit(127);
        execvp(argv[0], argv);
        _exit(127);
    }
    if (child > 0) {
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) continue;
        result = WIFEXITED(status) ? WEXITSTATUS(status) :
                 (WIFSIGNALED(status) && WTERMSIG(status) == SIGXFSZ ? -2 : -1);
    }
    sigaction(SIGCHLD, &previous, NULL);
    return result;
}

static bool join_path(char *out, size_t size, const char *directory, const char *name)
{
    return snprintf(out, size, "%s/%s", directory, name) < (int)size;
}

static bool remove_path_tree(const char *path)
{
    struct stat st;

    if (lstat(path, &st) < 0) return errno == ENOENT;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;
    return nftw(path, remove_tree_item, 32, FTW_DEPTH | FTW_PHYS | FTW_MOUNT) == 0;
}

static int set_package_tree_permissions(const char *path, const struct stat *st,
                                        int type, struct FTW *walk)
{
    (void)type;
    (void)walk;
    if (S_ISDIR(st->st_mode)) return chmod(path, 0755);
    if (S_ISREG(st->st_mode)) return chmod(path, (st->st_mode & 0111) ? 0755 : 0644);
    errno = EINVAL;
    return -1;
}

/* Returns 0 when installed, 1 for a rejected package, 2 for an install failure. */
static int install_gcu_package(const char *package, const struct gcu_package_index *index,
                               uint64_t declared_extracted, int client,
                               char *error, size_t error_size)
{
    static const char *const executables[] = {
        "ap/service.sh", "ap/camera-app", "ap/z1mini-web", "ap/ax-capture",
        "ipc/run.sh", "ipc/camera_gcu.sh",
    };
    char stage[PATH_MAX];
    char staged[PATH_MAX];
    char path[PATH_MAX];
    const char *removal;
    struct stat st;
    size_t length = 0;
    char *manifest;
    bool needs_isp;

    if (snprintf(stage, sizeof(stage), "%s.new", GCU_ROOT) >= (int)sizeof(stage) ||
        snprintf(staged, sizeof(staged), "%s.new/gcu", GCU_ROOT) >= (int)sizeof(staged)) {
        snprintf(error, error_size, "path too long");
        return 2;
    }
    if (!remove_path_tree(stage) || mkdir(stage, 0755) < 0) {
        snprintf(error, error_size, "cannot prepare %.200s: %s", stage, strerror(errno));
        return 2;
    }
    uint64_t remaining = declared_extracted;
    for (unsigned i = 0; i < index->count; i++) {
        char *const unzip_argv[] = {"unzip", "-o", "-q", (char *)package,
                                    (char *)index->names[i], "-d", stage, NULL};
        int unzip_result = run_tool(NULL, unzip_argv, client, (rlim_t)remaining);
        if (unzip_result != 0) {
            if (unzip_result == -2) {
                snprintf(error, error_size, "extracted data exceeds package limit");
                return 1;
            }
            snprintf(error, error_size, "%s", unzip_result == 126 || unzip_result == 127 ?
                     "required unzip utility is unavailable" : "extraction failed");
            return unzip_result == 126 || unzip_result == 127 ? 2 : 1;
        }
        if (!join_path(path, sizeof(path), staged, index->names[i] + 4) ||
            stat(path, &st) < 0 || !S_ISREG(st.st_mode) ||
            (uint64_t)st.st_size > remaining) {
            snprintf(error, error_size, "invalid extracted file");
            return 1;
        }
        remaining -= (uint64_t)st.st_size;
    }
    if (nftw(staged, set_package_tree_permissions, 16, FTW_PHYS | FTW_MOUNT) < 0) {
        snprintf(error, error_size, "cannot set application permissions: %s", strerror(errno));
        return 2;
    }
    for (size_t i = 0; i < sizeof(executables) / sizeof(executables[0]); i++) {
        if (!join_path(path, sizeof(path), staged, executables[i]) || lstat(path, &st) < 0) continue;
        if (!S_ISREG(st.st_mode) || chmod(path, 0755) < 0) {
            snprintf(error, error_size, "cannot mark %.200s executable", executables[i]);
            return 2;
        }
    }
    char *const sums_argv[] = {"sha256sum", "-c", "SHA256SUMS", NULL};
    if (!join_path(path, sizeof(path), staged, "ap")) {
        snprintf(error, error_size, "invalid staged application path");
        return 2;
    }
    int sums_result = run_tool(path, sums_argv, client, RLIM_INFINITY);
    if (sums_result == 126 || sums_result == 127) {
        snprintf(error, error_size, "required sha256sum utility is unavailable");
        return 2;
    }
    if (sums_result != 0) {
        snprintf(error, error_size, "SHA256SUMS mismatch");
        return 1;
    }
    manifest = join_path(path, sizeof(path), staged, "ap/manifest.json") ?
               read_file(path, 65536, &length) : NULL;
    if (manifest == NULL || strstr(manifest, "\"target\": \"xfrobot-z1mini\"") == NULL) {
        free(manifest);
        snprintf(error, error_size, "manifest is not for the " PRODUCT_NAME);
        return 1;
    }
    needs_isp = strstr(manifest, "\"vendor_isp_required\": true") != NULL;
    free(manifest);
    if (needs_isp) {
        snprintf(error, error_size, "retained-ISP packages cannot be installed from the web UI");
        return 1;
    }
    sync_firmware_storage();
    if (lstat(GCU_ROOT, &st) < 0 && errno == ENOENT) {
        if (rename(staged, GCU_ROOT) < 0) {
            snprintf(error, error_size, "cannot install %.200s: %s", GCU_ROOT, strerror(errno));
            return 2;
        }
        removal = stage;
    } else if (exchange_paths(staged, GCU_ROOT) == 0) {
        removal = staged;
    } else {
        snprintf(error, error_size,
                 "cannot atomically exchange %.200s: %s; existing installation preserved",
                 GCU_ROOT, strerror(errno));
        return 2;
    }
    sync_firmware_storage();
    if (!remove_path_tree(removal)) {
        log_message("cannot remove previous application %s: %s", removal, strerror(errno));
    }
    (void)remove_path_tree(stage);
    sync_firmware_storage();
    return 0;
}

static void handle_firmware_install(int fd, const struct request *request, const char *peer)
{
    char filename[256];
    char content_type[128];
    char package[PATH_MAX];
    char error[512];
    struct statvfs space;
    uint64_t extracted = 0;
    struct gcu_package_index package_index;
    int upgrade_lock = -1;
    int output = -1;
    size_t received = 0;
    bool created = false;
    const char *problem;
    int result;

    if (!valid_csrf_header(request)) {
        log_message("CSRF check failed from %s for firmware upload", peer);
        send_text_errorf(fd, 403, "Forbidden", S_CSRF_RELOAD);
        return;
    }
    if (request->content_length == 0 || request->content_length > MAX_FIRMWARE_SIZE) {
        send_text_errorf(fd, 413, "Payload Too Large", S_E_FW_SIZE);
        return;
    }
    if (find_header(request, "Content-Type", content_type, sizeof(content_type)) == NULL ||
        strncasecmp(content_type, "application/octet-stream", 24) != 0 ||
        (content_type[24] != '\0' && content_type[24] != ';')) {
        send_text_errorf(fd, 415, "Unsupported Media Type", S_E_FW_CONTENT_TYPE);
        return;
    }
    if (find_header(request, "X-Firmware-Name", filename, sizeof(filename)) == NULL ||
        !valid_firmware_name(filename)) {
        send_text_errorf(fd, 400, "Bad Request", S_E_FW_NAME, FIRMWARE_NAME_PATTERNS);
        return;
    }
    upgrade_lock = open(UPGRADE_LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (upgrade_lock < 0 || flock(upgrade_lock, LOCK_EX) < 0) {
        send_text_errorf(fd, 503, "Service Unavailable", S_E_FW_LOCK, strerror(errno));
        goto done;
    }
    if (snprintf(package, sizeof(package), "%s/firmware-upload.XXXXXX", RUNTIME_DIR) >=
        (int)sizeof(package)) {
        send_text_errorf(fd, 500, "Internal Server Error", S_E_FW_TMP, RUNTIME_DIR, "path too long");
        goto done;
    }
    output = mkostemp(package, O_CLOEXEC);
    if (output < 0) {
        send_text_errorf(fd, 500, "Internal Server Error", S_E_FW_TMP, RUNTIME_DIR, strerror(errno));
        goto done;
    }
    created = true;
    if (fstatvfs(output, &space) == 0 &&
        (uint64_t)space.f_bavail * space.f_frsize < request->content_length + 1024U * 1024U) {
        send_text_errorf(fd, 507, "Insufficient Storage", S_E_FW_TMP_SPACE, RUNTIME_DIR);
        goto done;
    }
    if (!receive_upload_body(fd, request, output, &received) || fsync(output) < 0) goto receive_failed;
    problem = check_gcu_package(output, received, &extracted, &package_index);
    if (problem != NULL) {
        log_message("firmware %s from %s rejected: %s", filename, peer, problem);
        send_text_errorf(fd, 400, "Bad Request", S_E_FW_PACKAGE, problem);
        goto done;
    }
    if (statvfs(GCU_ROOT, &space) == 0 &&
        (uint64_t)space.f_bavail * space.f_frsize < extracted + 1024U * 1024U) {
        send_text_errorf(fd, 507, "Insufficient Storage", S_E_FW_TMP_SPACE, GCU_ROOT);
        goto done;
    }
    close(output);
    output = -1;
    result = install_gcu_package(package, &package_index, extracted, fd, error, sizeof(error));
    if (result != 0) {
        char stage[PATH_MAX];
        if (snprintf(stage, sizeof(stage), "%s.new", GCU_ROOT) < (int)sizeof(stage)) {
            (void)remove_path_tree(stage);
        }
        log_message("firmware %s from %s not installed: %s", filename, peer, error);
        if (result == 1) {
            send_text_errorf(fd, 400, "Bad Request", S_E_FW_PACKAGE, error);
        } else {
            send_text_errorf(fd, 500, "Internal Server Error", S_E_FW_INSTALL, error);
        }
        goto done;
    }
    log_message("firmware %s (%zu bytes) installed by %s; rebooting", filename, received, peer);
    send_text_errorf(fd, 201, "Created", S_FW_INSTALLED_Z1, filename);
    schedule_reboot();
    goto done;

receive_failed:
    log_message("firmware upload from %s failed after %zu bytes: %s", peer, received,
                strerror(errno));
    send_text_errorf(fd, 400, "Bad Request", S_E_FW_FAILED, received, strerror(errno));
done:
    if (output >= 0) close(output);
    if (created) (void)unlink(package);
    if (upgrade_lock >= 0) close(upgrade_lock);
}
#endif

static void handle_firmware_upload(int fd, const struct request *request,
                                   const char *peer)
{
#if WEB_INSTALLS_FIRMWARE
    handle_firmware_install(fd, request, peer);
#else
    char filename[256];
    char temporary[272];
    char content_type[128];
    struct statvfs space;
    struct stat st;
    int upgrade_lock = -1;
    int directory = -1;
    int output = -1;
    bool renamed = false;
    bool temporary_created = false;
    size_t received = 0;

    if (!valid_csrf_header(request)) {
        log_message("CSRF check failed from %s for firmware upload", peer);
        send_text_errorf(fd, 403, "Forbidden", S_CSRF_RELOAD);
        return;
    }
    if (request->content_length == 0 ||
        request->content_length > MAX_FIRMWARE_SIZE) {
        send_text_errorf(fd, 413, "Payload Too Large", S_E_FW_SIZE);
        return;
    }
    if (find_header(request, "Content-Type", content_type,
                    sizeof(content_type)) == NULL ||
        strncasecmp(content_type, "application/octet-stream", 24) != 0 ||
        (content_type[24] != '\0' && content_type[24] != ';')) {
        send_text_errorf(fd, 415, "Unsupported Media Type", S_E_FW_CONTENT_TYPE);
        return;
    }
    if (find_header(request, "X-Firmware-Name", filename, sizeof(filename)) == NULL ||
        !valid_firmware_name(filename)) {
        send_text_errorf(fd, 400, "Bad Request", S_E_FW_NAME, FIRMWARE_NAME_PATTERNS);
        return;
    }
#ifdef FIRMWARE_INSTALL_NAME
    const char *published = FIRMWARE_INSTALL_NAME;
#else
    const char *published = filename;
#endif
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", filename) >=
        (int)sizeof(temporary)) {
        send_text_errorf(fd, 400, "Bad Request", S_E_FW_NAME_LONG);
        return;
    }
    upgrade_lock = open(UPGRADE_LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (upgrade_lock < 0 || flock(upgrade_lock, LOCK_EX) < 0) {
        send_text_errorf(fd, 503, "Service Unavailable", S_E_FW_LOCK, strerror(errno));
        goto done;
    }
#if APCAM_TARGET == APCAM_TARGET_ZR10
#ifndef MT11_WEB_TEST
    if (!zr10_sd_mounted()) {
        send_text_error(fd, 503, "Service Unavailable", "Mount the microSD card at /mnt before updating.\n", NULL);
        goto done;
    }
#endif
    if (request->content_length != ZR10_FIRMWARE_SIZE) {
        send_text_error(fd, 400, "Bad Request", "Invalid ZR10 customer-partition firmware size.\n", NULL);
        goto done;
    }
#endif
    directory = open(MEDIA_ROOT, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0 || fstatvfs(directory, &space) < 0) {
        send_text_errorf(fd, 503, "Service Unavailable", S_E_FW_MICROSD, strerror(errno));
        goto done;
    }
    uint64_t available = (uint64_t)space.f_bavail * (uint64_t)space.f_frsize;
    if (available < (uint64_t)request->content_length + 1024U * 1024U) {
        send_text_errorf(fd, 507, "Insufficient Storage", S_E_FW_SPACE);
        goto done;
    }
    DIR *listing = fdopendir(dup(directory));
    if (listing == NULL) {
        send_text_errorf(fd, 500, "Internal Server Error", S_E_FW_INSPECT, MEDIA_ROOT,
                         strerror(errno));
        goto done;
    }
    bool trigger_exists = false;
    struct dirent *entry;
    while ((entry = readdir(listing)) != NULL) {
        if (valid_firmware_name(entry->d_name) ||
            strcmp(entry->d_name, published) == 0) {
            trigger_exists = true;
            break;
        }
    }
    closedir(listing);
    if (trigger_exists) {
        send_text_errorf(fd, 409, "Conflict", S_E_FW_EXISTS, PRODUCT_NAME);
        goto done;
    }
    if (fstatat(directory, published, &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
        send_text_errorf(fd, 409, "Conflict", S_E_FW_NAME_EXISTS);
        goto done;
    }
    if (fstatat(directory, temporary, &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
        send_text_errorf(fd, 409, "Conflict", S_E_FW_TEMP_EXISTS);
        goto done;
    }
    output = openat(directory, temporary,
                    O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0) {
        send_text_errorf(fd, 500, "Internal Server Error", S_E_FW_CREATE, MEDIA_ROOT,
                         temporary, strerror(errno));
        goto done;
    }
    temporary_created = true;
    if (!receive_upload_body(fd, request, output, &received)) goto receive_failed;
#if APCAM_TARGET == APCAM_TARGET_ZR10
    if (!zr10_valid_firmware(output)) {
        send_text_error(fd, 400, "Bad Request", "Invalid ZR10 firmware: header, partition payload or checksum mismatch.\n", NULL);
        goto done;
    }
#endif
    if (fsync(output) < 0) goto receive_failed;
    if (close(output) < 0) {
        output = -1;
        goto receive_failed;
    }
    output = -1;
    sync_firmware_storage();
    if (publish_no_replace(directory, temporary, published) < 0) {
        int rename_error = errno;
        if (rename_error != ENOSYS && rename_error != EINVAL &&
            rename_error != EOPNOTSUPP) {
            send_text_errorf(fd, rename_error == EEXIST ? 409 : 500,
                             rename_error == EEXIST ? "Conflict" : "Internal Server Error",
                             S_E_FW_PUBLISH, MEDIA_ROOT, published, strerror(rename_error));
            goto done;
        }
        if (fstatat(directory, published, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            send_text_errorf(fd, 409, "Conflict", S_E_FW_APPEARED);
            goto done;
        }
        if (errno != ENOENT ||
            renameat(directory, temporary, directory, published) < 0) {
            send_text_errorf(fd, 500, "Internal Server Error", S_E_FW_PUBLISH_SAFE,
                             MEDIA_ROOT, published, strerror(errno));
            goto done;
        }
    }
    renamed = true;
    if (fsync(directory) < 0) {
        send_text_errorf(fd, 500, "Internal Server Error", S_E_FW_DIR_SYNC, strerror(errno));
        goto done;
    }
    sync_firmware_storage();
    log_message("firmware %s (%zu bytes) uploaded and published as %s by %s",
                filename, received, published, peer);
#if APCAM_TARGET == APCAM_TARGET_MT11
    send_text_errorf(fd, 201, "Created", S_FW_UPLOADED_MT11, filename);
#else
    send_text_errorf(fd, 201, "Created", S_FW_UPLOADED_A8, filename, FIRMWARE_INSTALL_NAME);
#endif
    goto done;

receive_failed:
    log_message("firmware upload from %s failed after %zu bytes: %s", peer,
                received, strerror(errno));
    send_text_errorf(fd, 400, "Bad Request", S_E_FW_FAILED, received, strerror(errno));
done:
    if (output >= 0) close(output);
    if (directory >= 0 && temporary_created && !renamed) (void)unlinkat(directory, temporary, 0);
    if (directory >= 0) close(directory);
    if (upgrade_lock >= 0) close(upgrade_lock);
#endif
}

static void send_log_page(int fd)
{
    size_t length;
    char *page = render_log_page(&length);
    if (page == NULL) {
        send_text_errorf(fd, 500, "Internal Server Error", S_OUT_OF_MEMORY);
        return;
    }
    send_response(fd, 200, "OK", "text/html; charset=utf-8", page, length, NULL);
    free(page);
}

static void send_log_text(int fd)
{
    size_t length = 0;
    char *log = read_current_app_log(&length);
    if (log == NULL) {
        send_text_errorf(fd, 500, "Internal Server Error", S_E_LOG_UNREADABLE);
        return;
    }
    send_response(fd, 200, "OK", "text/plain; charset=utf-8", log, length, NULL);
    free(log);
}

static void send_files_page(int fd, const char *path, const char *message, bool is_error)
{
    char error[256];
    size_t length;
    char *page = render_files_page(path, message, is_error, &length, error, sizeof(error));
    if (page == NULL) {
        send_text_error(fd, 400, "Bad Request", error, NULL);
        return;
    }
    send_response(fd, is_error ? 400 : 200, is_error ? "Bad Request" : "OK",
                  "text/html; charset=utf-8", page, length, NULL);
    free(page);
}

static void send_view_page(int fd, const char *path)
{
    char error[256];
    size_t length;
    char *page = render_view_page(path, &length, error, sizeof(error));
    if (page == NULL) {
        send_text_error(fd, 400, "Bad Request", error, NULL);
        return;
    }
    send_response(fd, 200, "OK", "text/html; charset=utf-8", page, length, NULL);
    free(page);
}

static void send_delete_page(int fd, const char *path)
{
    char error[256];
    size_t length;
    char *page = render_delete_page(path, &length, error, sizeof(error));
    if (page == NULL) {
        send_text_error(fd, 403, "Forbidden", error, NULL);
        return;
    }
    send_response(fd, 200, "OK", "text/html; charset=utf-8", page, length, NULL);
    free(page);
}

enum range_result {
    RANGE_NONE,
    RANGE_VALID,
    RANGE_INVALID
};

static enum range_result parse_byte_range(const struct request *request, uint64_t size,
                                          uint64_t *start, uint64_t *end)
{
    char value[256];
    char *dash;
    char *parse_end;
    unsigned long long first;
    unsigned long long last;

    if (find_header(request, "Range", value, sizeof(value)) == NULL) return RANGE_NONE;
    if (strncmp(value, "bytes=", 6) != 0 || strchr(value + 6, ',') != NULL || size == 0) {
        return RANGE_INVALID;
    }
    dash = strchr(value + 6, '-');
    if (dash == NULL) return RANGE_INVALID;
    *dash = '\0';
    if (value[6] == '\0') {
        errno = 0;
        last = strtoull(dash + 1, &parse_end, 10);
        if (errno != 0 || dash[1] == '\0' || *parse_end != '\0' || last == 0) return RANGE_INVALID;
        if (last > size) last = size;
        *start = size - last;
        *end = size - 1;
        return RANGE_VALID;
    }
    errno = 0;
    first = strtoull(value + 6, &parse_end, 10);
    if (errno != 0 || *parse_end != '\0' || first >= size) return RANGE_INVALID;
    if (dash[1] == '\0') {
        last = size - 1;
    } else {
        errno = 0;
        last = strtoull(dash + 1, &parse_end, 10);
        if (errno != 0 || *parse_end != '\0' || last < first) return RANGE_INVALID;
        if (last >= size) last = size - 1;
    }
    *start = first;
    *end = last;
    return RANGE_VALID;
}

static void safe_download_name(const char *path, char output[256])
{
    const char *base = strrchr(path, '/');
    size_t used = 0;
    if (base == NULL) base = path; else base++;
    while (*base != '\0' && used + 1 < 256) {
        unsigned char value = (unsigned char)*base++;
        output[used++] = value >= 0x20 && value < 0x7f && value != '"' && value != '\\' ?
                         (char)value : '_';
    }
    if (used == 0) output[used++] = '_';
    output[used] = '\0';
}

static void send_file_response(int client, const struct request *request,
                               const char *requested_path, bool download)
{
    char resolved[PATH_MAX];
    char error[256];
    char filename[256];
    char header[4096];
    struct stat st;
    int file = -1;
    uint64_t start = 0;
    uint64_t end;
    uint64_t remaining;
    enum range_result range;
    bool head = strcmp(request->method, "HEAD") == 0;
    const char *mime;
    int header_length;

    if (!canonical_existing_path(requested_path, resolved, error, sizeof(error))) {
        send_text_error(client, 404, "Not Found", error, NULL);
        return;
    }
    file = open(resolved, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    /* camera-app holds an exclusive lock while appending each MP4 frame.
     * Snapshot its length between writes, then release the lock so recording
     * continues throughout even a slow download of this immutable prefix. */
    if (file < 0 || flock(file, LOCK_SH) < 0 ||
        fstat(file, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        if (file >= 0) close(file);
        send_text_errorf(client, 400, "Bad Request", S_E_DOWNLOAD_REGULAR);
        return;
    }
    (void)flock(file, LOCK_UN);
    end = (uint64_t)st.st_size == 0 ? 0 : (uint64_t)st.st_size - 1;
    range = parse_byte_range(request, (uint64_t)st.st_size, &start, &end);
    if (range == RANGE_INVALID) {
        header_length = snprintf(header, sizeof(header),
            "HTTP/1.1 416 Range Not Satisfiable\r\nServer: %s\r\n"
            "Content-Range: bytes */%" PRIu64 "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
            SERVER_NAME, (uint64_t)st.st_size);
        if (header_length > 0 && (size_t)header_length < sizeof(header))
            (void)send_all(client, header, (size_t)header_length);
        close(file);
        return;
    }
    remaining = (uint64_t)st.st_size == 0 ? 0 : end - start + 1;
    mime = file_mime_type(resolved);
    safe_download_name(resolved, filename);
    if (range == RANGE_VALID) {
        header_length = snprintf(header, sizeof(header),
            "HTTP/1.1 206 Partial Content\r\nServer: %s\r\nContent-Type: %s\r\n"
            "Content-Length: %" PRIu64 "\r\nContent-Range: bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64 "\r\n"
            "Accept-Ranges: bytes\r\nContent-Disposition: %s; filename=\"%s\"\r\n"
            "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
            SERVER_NAME, mime, remaining, start, end, (uint64_t)st.st_size,
            download ? "attachment" : "inline", filename);
    } else {
        header_length = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\nServer: %s\r\nContent-Type: %s\r\n"
            "Content-Length: %" PRIu64 "\r\nAccept-Ranges: bytes\r\n"
            "Content-Disposition: %s; filename=\"%s\"\r\nCache-Control: no-store\r\n"
            "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
            SERVER_NAME, mime, remaining, download ? "attachment" : "inline", filename);
    }
    if (header_length <= 0 || (size_t)header_length >= sizeof(header) ||
        !send_all(client, header, (size_t)header_length) || head) {
        close(file);
        return;
    }
    while (remaining > 0) {
        char buffer[64 * 1024];
        size_t wanted = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        ssize_t got = pread(file, buffer, wanted, (off_t)start);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0 || !send_all(client, buffer, (size_t)got)) break;
        start += (uint64_t)got;
        remaining -= (uint64_t)got;
    }
    close(file);
}

static void send_file_response_async(int client, const struct request *request,
                                     const char *path, bool download)
{
    pid_t worker = fork();
    if (worker < 0) {
        send_file_response(client, request, path, download);
        return;
    }
    if (worker == 0) {
        if (listen_fd >= 0) close(listen_fd);
        send_file_response(client, request, path, download);
        close(client);
        _exit(0);
    }
}

static void schedule_reboot(void)
{
#ifdef MT11_WEB_SITL
    log_message("SITL reboot request ignored");
#else
    pid_t child = fork();
    if (child != 0) return;
    if (listen_fd >= 0) close(listen_fd);
    sleep(2);
    sync();
    reboot(RB_AUTOBOOT);
    _exit(1);
#endif
}

static void handle_request(int fd, const char *peer)
{
    struct request request = {0};
    int receive_status = receive_request(fd, &request);
    char session_token[65];
    bool login_route;
    int auth;

    /* Browsers commonly leave speculative connections idle.  Closing an
     * incomplete connection quietly avoids presenting an unsolicited 400 as
     * the result of an unrelated navigation on another connection. */
    if (receive_status == RECEIVE_INCOMPLETE) return;
    if (receive_status != 0) {
        if (receive_status == 500) {
            send_text_errorf(fd, 500, "Internal Server Error", S_OUT_OF_MEMORY);
        } else if (receive_status == 413) {
            send_text_errorf(fd, 413, "Payload Too Large", S_E_BODY_TOO_LARGE);
        } else if (receive_status == 431) {
            send_text_errorf(fd, 431, "Request Header Fields Too Large", S_E_HEADERS_TOO_LARGE);
        } else {
            send_text_errorf(fd, 400, "Bad Request", S_E_MALFORMED);
        }
        return;
    }
    /* Branding is public so login pages can display their favicon too. */
    if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/favicon.svg") == 0) {
        send_response(fd, 200, "OK", "image/svg+xml", (const char *)favicon_svg, sizeof(favicon_svg), NULL);
        goto done;
    }
    if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/favicon.ico") == 0) {
        send_response(fd, 200, "OK", "image/vnd.microsoft.icon", (const char *)favicon_ico, sizeof(favicon_ico), NULL);
        goto done;
    }
    select_language(&request);
    auth = authenticate(&request, session_token);
    if (auth < 0) {
        send_text_errorf(fd, 503, "Service Unavailable", S_PASSWORD_FILE_MISSING,
                         PASSWORD_PATH);
        goto done;
    }
    login_route = strcmp(request.path, "/login") == 0;
    if (auth == 0) {
        size_t header_len;
        if (login_route) {
            handle_login(fd, &request, peer);
        } else if (strcmp(request.method, "GET") == 0 &&
                   strcmp(request.path, "/language.js") == 0) {
            send_response(fd, 200, "OK", "application/javascript; charset=utf-8",
                          language_script, sizeof(language_script) - 1U, NULL);
        } else if (find_header_span(&request, "Authorization", &header_len) == NULL &&
                   wants_html(&request)) {
            /* browsers get the login form; scripted clients keep Basic */
            send_redirect(fd, "/login");
        } else {
            char body[256];
            log_message("authentication failed from %s", peer);
            snprintf(body, sizeof(body), "%s\n", T(S_AUTH_REQUIRED));
            send_text_error(fd, 401, "Unauthorized", body,
                            "WWW-Authenticate: Basic realm=\"" PRODUCT_NAME " camera\", charset=\"UTF-8\"\r\n");
        }
        goto done;
    }
    if (login_route) {
        if (strcmp(request.method, "POST") == 0) handle_login(fd, &request, peer);
        else send_redirect(fd, "/");
    } else if (request.streaming_body) {
        handle_firmware_upload(fd, &request, peer);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/") == 0) {
        send_page(fd, NULL, false);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/parameters") == 0) {
        send_parameter_page(fd, NULL, false, NULL);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/files.js") == 0) {
        send_response(fd, 200, "OK", "application/javascript; charset=utf-8",
                      files_script, sizeof(files_script) - 1U, NULL);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/parameters.js") == 0) {
        send_parameters_script(fd);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/raw") == 0) {
        send_raw_page(fd, NULL, false);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/users") == 0) {
        send_users_page(fd, NULL, false);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/live") == 0) {
        send_live_page(fd);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/live.js") == 0) {
        send_live_script(fd);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/live/attitude.json") == 0) {
        send_live_attitude_json(fd);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/live/video1.mp4") == 0) {
        relay_live_video(fd, 0U);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/live/video2.mp4") == 0) {
        relay_live_video(fd, 1U);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/sensors") == 0) {
        send_sensors_page(fd, NULL, false);
#if WEB_HAVE_THERMAL
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/sensors.json") == 0) {
        send_sensors_json(fd);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/sensors.js") == 0) {
        send_sensors_script(fd);
#endif
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/status.js") == 0) {
        send_response(fd, 200, "OK", "application/javascript; charset=utf-8",
                      status_script, sizeof(status_script) - 1U, NULL);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/language.js") == 0) {
        send_response(fd, 200, "OK", "application/javascript; charset=utf-8",
                      language_script, sizeof(language_script) - 1U, NULL);
#if WEB_HAVE_SSH_KEYS
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/users.js") == 0) {
        send_users_script(fd);
#endif
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/upgrade.js") == 0) {
        send_upgrade_script(fd);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/upgrade-status") == 0) {
        send_response(fd, 200, "OK", "text/plain; charset=utf-8",
                      csrf_token, strlen(csrf_token), NULL);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/files") == 0) {
        size_t path_length = 0;
        char *path = query_value(&request, "path", &path_length);
        if (path == NULL) path = strdup(MEDIA_ROOT);
        if (path == NULL || path_length >= PATH_MAX) {
            send_text_errorf(fd, 400, "Bad Request", S_E_PATH_LONG_OR_INVALID);
        } else {
            send_files_page(fd, path, NULL, false);
        }
        free(path);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/view") == 0) {
        size_t path_length = 0;
        char *path = query_value(&request, "path", &path_length);
        if (path == NULL || path_length >= PATH_MAX) {
            send_text_errorf(fd, 400, "Bad Request", S_E_PATH_MISSING);
        } else {
            send_view_page(fd, path);
        }
        free(path);
    } else if ((strcmp(request.method, "GET") == 0 || strcmp(request.method, "HEAD") == 0) &&
               strcmp(request.path, "/file") == 0) {
        size_t path_length = 0;
        size_t download_length = 0;
        char *path = query_value(&request, "path", &path_length);
        char *download = query_value(&request, "download", &download_length);
        bool as_download = download != NULL && download_length == 1 && download[0] == '1';
        if (path == NULL || path_length >= PATH_MAX) {
            send_text_errorf(fd, 400, "Bad Request", S_E_PATH_MISSING);
        } else {
            send_file_response_async(fd, &request, path, as_download);
        }
        free(download);
        free(path);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/delete-confirm") == 0) {
        size_t path_length = 0;
        char *path = query_value(&request, "path", &path_length);
        if (path == NULL || path_length >= PATH_MAX) {
            send_text_errorf(fd, 400, "Bad Request", S_E_PATH_MISSING);
        } else {
            send_delete_page(fd, path);
        }
        free(path);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/log") == 0) {
        send_log_page(fd);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/log.txt") == 0) {
        send_log_text(fd);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/healthz") == 0) {
        char health[96];
        enum camera_kind kind = current_camera_kind();
        snprintf(health, sizeof(health), "camera_app=%s\ncamera_kind=%s\n",
                 kind != CAMERA_NONE ? "running" : "stopped",
                 kind == CAMERA_REPLACEMENT ? "replacement" :
                 "none");
        send_response(fd, 200, "OK", "text/plain; charset=utf-8",
                      health, strlen(health), NULL);
    } else if (strcmp(request.method, "POST") == 0) {
        char error[256];
        if (!valid_csrf(&request)) {
            log_message("CSRF check failed from %s for %s", peer, request.path);
            if (strcmp(request.path, "/live/control") == 0 ||
                strcmp(request.path, "/sensors/lidar") == 0) {
                send_text_errorf(fd, 403, "Forbidden", S_CSRF_INVALID);
            } else if (strcmp(request.path, "/sensors/capture") == 0) {
                send_sensors_page(fd, T(S_CSRF_RELOAD), true);
            } else {
                send_page(fd, T(S_CSRF_RELOAD), true);
            }
        } else if (strcmp(request.path, "/logout") == 0) {
            /* revoke whatever session the browser holds, also when this
             * request itself was authenticated by Basic credentials */
            char cookie_session[65];
            if (!cookie_value(&request, "session", cookie_session, sizeof(cookie_session))) {
                cookie_session[0] = '\0';
            }
            if (!destroy_session(cookie_session)) {
                log_message("cannot revoke login session for %s: %s", peer,
                            strerror(errno));
                send_page(fd, T(S_LOGOUT_FAILED), true);
            } else {
                log_message("admin logged out from %s", peer);
                send_redirect_headers(fd, "/login",
                                      "Set-Cookie: session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0\r\n");
            }
        } else if (strcmp(request.path, "/language") == 0) {
            size_t lang_length = 0;
            size_t next_length = 0;
            char *lang = form_value(&request, "lang", &lang_length);
            char *next = form_value(&request, "next", &next_length);
            enum language selected;
            char headers[160];
            if (lang == NULL || !language_from_code(lang, lang_length, &selected)) {
                send_page(fd, T(S_LANGUAGE_INVALID), true);
            } else {
                current_language = selected;
                language_cookie(selected, headers, sizeof(headers));
                send_redirect_headers(fd, safe_local_path(next) ? next : "/", headers);
            }
            free(lang);
            free(next);
        } else if (strcmp(request.path, "/live/control") == 0) {
            char acquired[33];
            if (send_live_control(&request, error, sizeof(error), acquired)) {
                send_text_error(fd,200,"OK",acquired[0] ? acquired : T(S_COMMAND_SENT),NULL);
            } else {
                log_message("live gimbal control failed for %s: %s", peer,
                            error);
                send_text_error(fd, 400, "Bad Request", error, NULL);
            }
#if WEB_HAVE_THERMAL
        } else if (strcmp(request.path, "/sensors/lidar") == 0) {
            size_t action_length = 0U;
            char *action = form_value(&request, "action", &action_length);
            bool enable = action != NULL && action_length == 6U &&
                          memcmp(action, "enable", 6U) == 0;
            bool disable = action != NULL && action_length == 7U &&
                           memcmp(action, "disable", 7U) == 0;

            if (!enable && !disable) {
                send_text_errorf(fd, 400, "Bad Request", S_E_LIDAR_ACTION);
            } else if (set_lidar_enabled(enable, error, sizeof(error))) {
                log_message("LiDAR %s requested from Sensors page by %s",
                            enable ? "enable" : "disable", peer);
                send_text_errorf(fd, 200, "OK", enable ? S_LIDAR_ENABLED : S_LIDAR_DISABLED);
            } else {
                log_message("Sensors-page LiDAR control failed for %s: %s",
                            peer, error);
                send_text_error(fd, 503, "Service Unavailable", error, NULL);
            }
            free(action);
#endif
        } else if (strcmp(request.path, "/sensors/capture") == 0) {
            if (trigger_camera_photo(error, sizeof(error))) {
                log_message("photo captured from Sensors page by %s", peer);
                send_sensors_page(fd, T(S_PHOTO_CAPTURED), false);
            } else {
                log_message("Sensors-page photo capture failed for %s: %s", peer, error);
                send_sensors_page(fd, error, true);
            }
        } else if (strcmp(request.path, "/time/sync") == 0) {
            if (sync_time_from_browser(&request, error, sizeof(error))) {
                log_message("camera time synchronized from browser by %s", peer);
                send_page(fd, T(S_TIME_SYNCED), false);
            } else {
                log_message("browser time synchronization failed for %s: %s",
                            peer, error);
                send_page(fd, error, true);
            }
        } else if (strcmp(request.path, "/users/password") == 0) {
            if (change_admin_password(&request, error, sizeof(error))) {
                log_message("admin password changed by %s", peer);
                send_users_page(fd, T(S_PASSWORD_CHANGED), false);
            } else {
                log_message("admin password change failed for %s: %s", peer, error);
                send_users_page(fd, error, true);
            }
#if WEB_HAVE_SSH_KEYS
        } else if (strcmp(request.path, "/users/keys/add") == 0) {
            size_t added_count = 0;
            if (add_authorized_keys(&request, &added_count, error, sizeof(error))) {
                if (added_count == 1) {
                    snprintf(error, sizeof(error), "%s", T(S_KEY_ADDED_ONE));
                } else {
                    snprintf(error, sizeof(error), T(S_KEYS_ADDED_MANY), added_count);
                }
                log_message("%zu SSH public key(s) added by %s", added_count, peer);
                send_users_page(fd, error, false);
            } else {
                log_message("SSH public-key addition failed for %s: %s", peer, error);
                send_users_page(fd, error, true);
            }
        } else if (strcmp(request.path, "/users/keys/remove") == 0) {
            if (remove_authorized_key(&request, error, sizeof(error))) {
                log_message("SSH public key removed by %s", peer);
                send_users_page(fd, T(S_KEY_REMOVED), false);
            } else {
                log_message("SSH public-key removal failed for %s: %s", peer, error);
                send_users_page(fd, error, true);
            }
#endif
        } else if (strcmp(request.path, "/delete") == 0) {
            size_t path_length = 0;
            size_t confirmation_length = 0;
            char *path = form_value(&request, "path", &path_length);
            char *confirmation = form_value(&request, "confirm", &confirmation_length);
            bool confirmed = confirmation != NULL && confirmation_length == 3 &&
                             memcmp(confirmation, "yes", 3) == 0;
            char deleted[PATH_MAX];
            char parent[PATH_MAX];
            if (!confirmed) {
                send_text_errorf(fd, 400, "Bad Request", S_E_DELETE_UNCONFIRMED);
            } else if (path == NULL || path_length >= PATH_MAX) {
                send_text_errorf(fd, 400, "Bad Request", S_E_DELETE_PATH);
            } else if (!delete_media_path(path, deleted, sizeof(deleted), error, sizeof(error))) {
                log_message("filesystem deletion rejected/failed for %s: %s", peer, error);
                send_text_error(fd, 403, "Forbidden", error, NULL);
            } else {
                path_parent(deleted, parent);
                log_message("filesystem path deleted by %s: %s", peer, deleted);
                send_files_page(fd, parent, T(S_PATH_DELETED), false);
            }
            free(confirmation);
            free(path);
        } else if (strcmp(request.path, "/restart") == 0) {
            if (restart_camera(error, sizeof(error))) {
                log_message("camera application restarted by %s", peer);
                send_redirect(fd, "/");
            } else {
                log_message("camera restart failed for %s: %s", peer, error);
                send_page(fd, error, true);
            }
        } else if (strcmp(request.path, "/parameters") == 0) {
            struct ini_update updates[80] = {0};
            enum camera_kind kind = configuration_camera_kind();
            size_t update_count = 0;
            size_t old_length = 0;
            size_t new_length = 0;
            size_t action_len = 0;
            const char *path;
            char *old_config;
            char *new_config = NULL;
            char *action = form_value(&request, "action", &action_len);
            bool restart = action != NULL && action_len == strlen("save_restart") &&
                           memcmp(action, "save_restart", action_len) == 0;
            if (kind == CAMERA_NONE) kind = DEFAULT_CAMERA_KIND;
            path = config_path(kind);
            old_config = read_file(path, MAX_CONFIG, &old_length);
            (void)old_length;
            if (old_config == NULL) {
                snprintf(error, sizeof(error), T(S_E_CANNOT_READ), path, strerror(errno));
                send_parameter_page(fd, error, true, &request);
            } else if (!collect_replacement_parameter_updates(
                                   &request, updates,
                                   sizeof(updates) / sizeof(updates[0]),
                                   &update_count, error, sizeof(error))) {
                send_parameter_page(fd, error, true, &request);
            } else if ((new_config = ini_apply_updates(old_config, updates, update_count,
                                                       kind == CAMERA_REPLACEMENT,
                                                       &new_length, error, sizeof(error))) == NULL) {
                send_parameter_page(fd, error, true, &request);
            } else if (!save_config(path, config_backup_path(kind), new_config,
                                    new_length, error, sizeof(error))) {
                log_message("parameter save failed for %s: %s", peer, error);
                send_parameter_page(fd, error, true, &request);
            } else if (restart && !restart_camera(error, sizeof(error))) {
                log_message("parameters saved but restart failed for %s: %s", peer, error);
                send_parameter_page(fd, error, true, &request);
            } else {
                log_message("parameters saved%s by %s", restart ? " and camera restarted" : "", peer);
                snprintf(error, sizeof(error),
                         T(restart ? S_PARAMS_SAVED_RESTARTED : S_PARAMS_SAVED),
                         camera_label(kind));
                bool apply_error = !restart && wait_live_config(new_config, new_length, error, sizeof(error), false);
                send_parameter_page(fd, error, apply_error, NULL);
            }
            free(action);
            free(new_config);
            free(old_config);
            free_parameter_updates(updates, update_count);
        } else if (strcmp(request.path, "/config") == 0) {
            enum camera_kind kind = configuration_camera_kind();
            size_t config_len = 0;
            size_t action_len = 0;
            char *config = form_value(&request, "config", &config_len);
            char *action = form_value(&request, "action", &action_len);
            bool restart = action != NULL && action_len == strlen("save_restart") &&
                           memcmp(action, "save_restart", action_len) == 0;
            if (kind == CAMERA_NONE) kind = DEFAULT_CAMERA_KIND;
            if (config == NULL) {
                send_raw_page(fd, T(S_E_NO_CONFIG_DATA), true);
            } else {
                normalize_newlines(config, &config_len);
                if (!save_config(config_path(kind), config_backup_path(kind),
                                 config, config_len, error, sizeof(error))) {
                    log_message("config save failed for %s: %s", peer, error);
                    send_raw_page(fd, error, true);
                } else if (restart && !restart_camera(error, sizeof(error))) {
                    log_message("config saved but restart failed for %s: %s", peer, error);
                    send_raw_page(fd, error, true);
                } else {
                    log_message("config saved%s by %s", restart ? " and camera restarted" : "", peer);
                    if (restart) {
                        snprintf(error, sizeof(error), T(S_CONFIG_SAVED_RESTARTED),
                                 camera_label(kind));
                    } else {
                        snprintf(error, sizeof(error), "%s", T(S_CONFIG_SAVED));
                    }
                    bool apply_error = !restart && wait_live_config(config, config_len, error, sizeof(error), false);
                    send_raw_page(fd, error, apply_error);
                }
            }
            free(config);
            free(action);
        } else if (strcmp(request.path, "/reboot") == 0) {
            size_t confirmation_len = 0;
            char *confirmation = form_value(&request, "confirm", &confirmation_len);
            bool confirmed = confirmation != NULL && confirmation_len == 3 &&
                             memcmp(confirmation, "yes", 3) == 0;
            free(confirmation);
            if (!confirmed) {
                send_page(fd, T(S_REBOOT_UNCONFIRMED), true);
            } else {
                struct string_buffer body;
                char title[96];
                log_message("camera reboot requested by %s", peer);
                snprintf(title, sizeof(title), T(S_TITLE_REBOOTING), PRODUCT_NAME);
                sb_init(&body);
                sb_appendf(&body, "<!doctype html><html lang=%s><meta charset=utf-8><title>",
                           languages[current_language].html_lang);
                sb_append_html(&body, title);
                sb_appendf(&body, "</title><h1>%s</h1><p>%s</p>", T(S_REBOOTING_HEADING),
                           T(S_REBOOTING_TEXT));
                send_response(fd, 200, "OK", "text/html; charset=utf-8",
                              body.data, body.len, NULL);
                free(body.data);
                schedule_reboot();
            }
        } else {
            send_text_errorf(fd, 404, "Not Found", S_UNKNOWN_POST);
        }
    } else {
        send_text_errorf(fd, 404, "Not Found", S_NOT_FOUND);
    }
done:
    free(request.storage);
}

static void handle_signal(int signal_number)
{
    (void)signal_number;
    if (listen_fd >= 0) close(listen_fd);
    listen_fd = -1;
}

static int create_listener(unsigned port)
{
    struct sockaddr_in address = {0};
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;

    if (fd < 0) return -1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
#ifdef WEB_PORTABLE_SITL
    if (portable_paths_init() < 0) { fprintf(stderr, "Invalid portable SITL paths\n"); return 2; }
#endif
    unsigned port = DEFAULT_PORT;

#if (APCAM_TARGET == APCAM_TARGET_MT11) || (defined(MT11_WEB_SITL) && !defined(WEB_SUPERVISED_TEST))
    if (argc == 2 && strcmp(argv[1], "--capture-app-log") == 0) {
        return capture_app_log(REPLACEMENT_LOG_PATH, REPLACEMENT_LOG_OLD_PATH, true);
    }
    if (argc == 3 && strcmp(argv[1], "--capture-app-log") == 0) {
        if (strcmp(argv[2], "replacement") == 0) {
            return capture_app_log(REPLACEMENT_LOG_PATH,
                                   REPLACEMENT_LOG_OLD_PATH, true);
        }
        fprintf(stderr, "Invalid log owner: %s\n", argv[2]);
        return 2;
    }
#endif
    if (argc == 3 && strcmp(argv[1], "-p") == 0) {
        char *end;
        unsigned long value = strtoul(argv[2], &end, 10);
        if (*argv[2] == '\0' || *end != '\0' || value < 1 || value > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[2]);
            return 2;
        }
        port = (unsigned)value;
    } else if (argc != 1) {
        fprintf(stderr,
                TARGET_TEXT("Usage: %s [-p port] | --capture-app-log [replacement]\n",
                            "Usage: %s [-p port]\n"),
                argv[0]);
        return 2;
    }
#ifdef MT11_WEB_TEST
    /* every id must have every language; a production build falls back
     * to English instead */
    for (size_t id = 0; id < S_COUNT; id++) {
        for (size_t lang = 0; lang < LANG_COUNT; lang++) {
            if (strings[id][lang] == NULL || strings[id][lang][0] == '\0') {
                fprintf(stderr, "string %zu has no text for %s\n", id,
                        languages[lang].code);
                return 3;
            }
        }
    }
#endif
    if (!random_token(csrf_token) || !random_token(login_token)) {
        perror("Cannot create CSRF token");
        return 1;
    }
#ifdef WEB_PORTABLE_SITL
    if (!random_token(session_epoch)) {
        perror("Cannot create session epoch");
        return 1;
    }
#endif
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    listen_fd = create_listener(port);
    if (listen_fd < 0) {
        perror("Cannot listen");
        return 1;
    }
#ifdef WEB_PORTABLE_SITL
    char pid_path[sizeof(RUNTIME_DIR) + sizeof("/web.pid")];
    snprintf(pid_path, sizeof(pid_path), "%s/web.pid", RUNTIME_DIR);
    FILE *pid_file = fopen(pid_path, "w");
    if (!pid_file) return 1;
    fprintf(pid_file, "%ld\n", (long)getpid());
    fclose(pid_file);
#endif
    log_message("%s listening on 0.0.0.0:%u", SERVER_NAME, port);
    while (listen_fd >= 0) {
        struct sockaddr_in peer_address;
        socklen_t peer_length = sizeof(peer_address);
        char peer[INET_ADDRSTRLEN] = "unknown";
        int client = accept4(listen_fd, (struct sockaddr *)&peer_address,
                             &peer_length, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) continue;
            if (listen_fd < 0 || errno == EBADF) break;
            log_message("accept failed: %s", strerror(errno));
            continue;
        }
        (void)inet_ntop(AF_INET, &peer_address.sin_addr, peer, sizeof(peer));
        struct timeval timeout = {.tv_sec = 10, .tv_usec = 0};
        (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        pid_t worker = fork();
        if (worker < 0) {
            log_message("cannot create HTTP worker: %s", strerror(errno));
            handle_request(client, peer);
            close(client);
            continue;
        }
        if (worker == 0) {
            close(listen_fd);
            listen_fd = -1;
            handle_request(client, peer);
            close(client);
            _exit(0);
        }
        close(client);
    }
    log_message("server stopped");
    return 0;
}
