#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "build/version.h"
#include "build/icons.h"
#include "../include/apcam/config_status.h"
#include "../include/apcam/network.h"
#include "../include/apcam/soc_temperature.h"

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
#if !defined(__CYGWIN__) && !defined(__APPLE__)
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
#ifdef __APPLE__
#include <libproc.h>
#endif

#ifdef ZT30_WEB_DEV
#include "zt30-dev-paths.h"
#elif defined(ZT30_WEB_NATIVE)
#include "zt30-paths.h"
#include "zt30-process.h"
#include "../include/apcam/zt30_services.h"
#endif

/* Shared web UI with target-specific paths, capabilities and update formats.
 * Z1-Mini uses the kernel SoC thermal sensor and MAVLink for web controls. */
#define APCAM_WEB_BUILD 1
#include "../include/apcam/target.h"
#include "../include/apcam/APC_Camera.h"
#include "../include/apcam/APC_Config.h"
#ifdef CAMERA_APP_ZT30_DEV
#include "../include/apcam/APC_ZT30_Config.h"
#endif
#include "../include/apcam/gimbal_transform.h"
#include "../include/apcam/manual_control.h"
#include "../include/apcam/survey_runtime.h"
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
/* Targets keeping media on the root filesystem set this to 0. */
#ifndef WEB_MEDIA_IS_MOUNT
#define WEB_MEDIA_IS_MOUNT 1
#endif
/* Targets updated outside the web UI set this to 0 to hide package upload. */
#ifndef WEB_HAVE_FIRMWARE_UPLOAD
#define WEB_HAVE_FIRMWARE_UPLOAD 1
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
#define CAMERA_API_PORT APCAM_DEFAULT_MAVLINK_PORT
#include "z1mini_mavlink.h"
#else
#define DEFAULT_PORT APCAM_WEB_PORT
#ifndef CAMERA_API_PORT
#define CAMERA_API_PORT 37260U
#endif
#endif
#define LIVE_VIDEO_PORT 8555U
#define ARDUPILOT_LOGO_URL \
    "https://firmware.ardupilot.org/Tools/Logos/ArduPilot-Cleaned-Transparent.png"
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

#include "APC_StringBuffer.h"
#include "APC_WebRoot.h"
#if APCAM_TARGET == APCAM_TARGET_Z1_MINI
#include "APC_RecoveryPage.h"
#endif
static APC_WebRoot webroot;

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

#include "APC_Translations.h"

/* each request runs in its own forked worker, so this is per request */
static enum language current_language = LANG_EN;

static const char *T(enum string_id id)
{
    const char *text = translations[id][current_language];

    return text != NULL ? text : translations[id][LANG_EN];
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

#include "APC_HTTPRequest.h"

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
    PARAM_TIMEZONE,
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

#if APCAM_HAVE_PHOTO_RESOLUTION
static const struct option photo_resolution_options[] = {
    {"video", S_OPT_PHOTO_RES_VIDEO},
    {APCAM_PHOTO_BINNED_NAME, S_OPT_PHOTO_RES_BINNED},
    {APCAM_PHOTO_FULL_NAME, S_OPT_PHOTO_RES_FULL},
};
#endif

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
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_2160P
    {"3840x2160", S_OPT_RES_4K},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_1440P
    {"2560x1440", S_OPT_RES_1440},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_1080P
    {"1920x1080", S_OPT_RES_1080},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_720P
    {"1280x720", S_OPT_RES_720},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_540P
    {"960x540", S_OPT_RES_540},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_480P
    {"854x480", S_OPT_RES_480},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_360P
    {"640x360", S_OPT_RES_360},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_240P
    {"426x240", S_OPT_RES_240},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_180P
    {"320x180", S_OPT_RES_180},
#endif
};
static const struct option sub_resolution_options[] = {
#if APCAM_SUB_RESOLUTIONS & APCAM_RES_MASK_2160P
    {"3840x2160", S_OPT_RES_4K},
#endif
#if APCAM_SUB_RESOLUTIONS & APCAM_RES_MASK_1440P
    {"2560x1440", S_OPT_RES_1440},
#endif
#if APCAM_SUB_RESOLUTIONS & APCAM_RES_MASK_1080P
    {"1920x1080", S_OPT_RES_1080},
#endif
#if APCAM_SUB_RESOLUTIONS & APCAM_RES_MASK_720P
    {"1280x720", S_OPT_RES_720},
#endif
};
static const struct option recording_resolution_options[] = {
#if APCAM_RECORDING_RESOLUTIONS & APCAM_RES_MASK_2160P
    {"3840x2160", S_OPT_RES_4K},
#endif
#if APCAM_RECORDING_RESOLUTIONS & APCAM_RES_MASK_1440P
    {"2560x1440", S_OPT_RES_1440},
#endif
#if APCAM_RECORDING_RESOLUTIONS & APCAM_RES_MASK_1080P
    {"1920x1080", S_OPT_RES_1080},
#endif
#if APCAM_RECORDING_RESOLUTIONS & APCAM_RES_MASK_720P
    {"1280x720", S_OPT_RES_720},
#endif
};
static const struct option replacement_codec_options[] = {
    { "h264", S_OPT_CODEC_H264 },
#if APCAM_STREAM_CODECS & 2
    { "h265", S_OPT_CODEC_H265 },
#endif
};
static const struct option telemetry_options[] = {
    {"apcg", S_OPT_META_APCG}, {"misb", S_OPT_META_MISB},
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

static const struct option survey_patterns[] = {
    {"both", S_SURV_BOTH}, {"left_right", S_SURV_LEFT_RIGHT}, {"fore_aft", S_SURV_FORE_AFT},
    {"fore_only", S_SURV_FORE_ONLY},
};
static const struct option survey_lenses[] = {
#if WEB_HAVE_THERMAL || defined(CAMERA_APP_ZT30_DEV)
    {"thermal", S_SURV_THERMAL},
#endif
    {"wide", S_SURV_WIDE},
#if APCAM_HAVE_ZOOM_LENS
    {"zoom", S_SURV_ZOOM},
#endif
};
/* Suggested gimbal links: Raspberry Pi serial ports, then the usual
 * flight-controller serial passthrough address. */
static const char *const UART_DEVICE_OVERRIDE_CHOICES[] = {
    "/dev/serial0", "/dev/ttyAMA0", "/dev/ttyAMA1", "/dev/ttyUSB0", "udp://192.168.144.15:15002",
};
static const struct parameter replacement_parameters[] = {
    {"timezone", "general", "timezone", S_P_TIMEZONE, S_H_TIMEZONE,
     PARAM_TIMEZONE, 1, 127, 0, NULL, 0},
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
#ifdef CAMERA_APP_ZT30_DEV
    {"thermal_hfov_cd", "thermal", "hfov_cd", S_ZT30_THERMAL_HFOV, S_ZT30_THERMAL_HFOV_HELP,
     PARAM_INTEGER, 0, 17900, 1, NULL, 0},
#endif
    {"thermal_palette", "thermal", "palette", S_P_THERMAL_PALETTE, S_H_THERMAL_PALETTE,
     PARAM_ENUM, 0, 0, 0, palette_options,
     sizeof(palette_options) / sizeof(palette_options[0])},
#if APCAM_HAVE_RAW_THERMAL
    {"raw_stream_fps", "thermal", "stream_fps", S_P_RAW_STREAM_FPS, S_H_RAW_STREAM_FPS,
     PARAM_INTEGER, 1, 25, 1, NULL, 0},
    {"raw_record_fps", "thermal", "record_fps", S_P_RAW_RECORD_FPS, S_H_RAW_RECORD_FPS,
     PARAM_INTEGER, 0, 25, 1, NULL, 0},
#endif
    {"main_resolution", "stream.main", "resolution", S_P_MAIN_RESOLUTION,
     TARGET_TEXT(S_H_MAIN_RESOLUTION_MT11, S_H_MAIN_RESOLUTION_A8),
     PARAM_ENUM, 0, 0, 0, main_resolution_options, sizeof(main_resolution_options) / sizeof(main_resolution_options[0])},
    {"video_telemetry", "video", "telemetry", S_P_VIDEO_TELEMETRY, S_H_VIDEO_TELEMETRY,
     PARAM_ENUM, 0, 0, 0, telemetry_options, sizeof(telemetry_options) / sizeof(telemetry_options[0])},
    {"main_codec", "stream.main", "codec", S_P_MAIN_CODEC, S_H_MAIN_CODEC,
     PARAM_ENUM, 0, 0, 0, replacement_codec_options, sizeof(replacement_codec_options) / sizeof(replacement_codec_options[0])},
#if APCAM_HAVE_VIDEO_BITRATE
    {"main_bitrate", "stream.main", "bitrate_kbps", S_P_MAIN_BITRATE, S_H_MAIN_BITRATE,
     PARAM_INTEGER, APCAM_MAIN_BITRATE_MIN_KBPS, APCAM_MAIN_BITRATE_MAX_KBPS, 1, NULL, 0},
#endif
#if APCAM_HAVE_VIDEO_FRAME_RATE
    {"main_frame_rate", "stream.main", "frame_rate", S_P_MAIN_FPS, S_H_MAIN_FPS,
     PARAM_INTEGER, APCAM_MAIN_FPS_MIN, APCAM_FRAME_RATE, 1, NULL, 0},
#endif
    {"sub_resolution", "stream.sub", "resolution", S_P_SUB_RESOLUTION,
     TARGET_TEXT(S_H_SUB_RESOLUTION_MT11, S_H_SUB_RESOLUTION_A8),
     PARAM_ENUM, 0, 0, 0, sub_resolution_options, sizeof(sub_resolution_options) / sizeof(sub_resolution_options[0])},
    {"sub_codec", "stream.sub", "codec", S_P_SUB_CODEC, S_H_SUB_CODEC,
     PARAM_ENUM, 0, 0, 0, replacement_codec_options, sizeof(replacement_codec_options) / sizeof(replacement_codec_options[0])},
    {"main_alias", "stream.main", "alias", S_P_MAIN_ALIAS, S_H_MAIN_ALIAS,
     PARAM_TEXT, 0, 63, 1, NULL, 0},
    {"sub_alias", "stream.sub", "alias", S_P_SUB_ALIAS, S_H_SUB_ALIAS,
     PARAM_TEXT, 0, 63, 1, NULL, 0},
    {"recording_resolution", "recording", "resolution", S_P_RECORDING_RESOLUTION,
     TARGET_TEXT(S_H_RECORDING_RESOLUTION_MT11, S_H_RECORDING_RESOLUTION_A8),
     PARAM_ENUM, 0, 0, 0, recording_resolution_options,
     sizeof(recording_resolution_options) / sizeof(recording_resolution_options[0])},
    {"autorecord", "recording", "autorecord", S_P_AUTORECORD, S_H_AUTORECORD_APP,
     PARAM_ENUM, 0, 0, 0, autorecord_options, 3},
#if APCAM_HAVE_PHOTO_RESOLUTION
    {"photo_resolution", "capture", "resolution", S_P_PHOTO_RESOLUTION, S_H_PHOTO_RESOLUTION,
     PARAM_ENUM, 0, 0, 0, photo_resolution_options, 3},
#endif
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
#if APCAM_HAVE_RAW_THERMAL
    {"proxy_video3_port", "support_proxy", "video3_port", S_P_PROXY_VIDEO3_PORT, S_H_PROXY_VIDEO3_PORT,
     PARAM_INTEGER, 0, 65535, 1, NULL, 0},
    {"proxy_video3_name", "support_proxy", "video3_name", S_P_PROXY_VIDEO3_NAME, S_H_PROXY_VIDEO3_NAME,
     PARAM_TEXT, 0, 63, 1, NULL, 0},
#endif
    {"proxy_publish_password", "support_proxy", "publish_password", S_P_PROXY_PUBLISH_PASSWORD, S_H_PROXY_PUBLISH_PASSWORD,
     PARAM_PASSWORD, 0, 127, 1, NULL, 0},
    {"osd_cross", "overlay", "cross", S_P_OSD_CROSS, S_H_OSD_CROSS,
     PARAM_ENUM, 0, 0, 0, replacement_boolean_options, 2},
    {"osd_recording", "overlay", "recording", S_P_OSD_RECORD, S_H_OSD_RECORD,
     PARAM_ENUM, 0, 0, 0, replacement_boolean_options, 2},
    {"osd_thermal_fov", "overlay", "thermal_fov", S_P_OSD_THERMAL_FOV, S_H_OSD_THERMAL_FOV,
     PARAM_ENUM, 0, 0, 0, replacement_boolean_options, 2},
    {"network_interface", "network", "interface", S_P_NETWORK_INTERFACE, S_H_NETWORK_INTERFACE,
     PARAM_TEXT, 1, 15, 1, NULL, 0},
    {"network_primary_address", "network", "primary_address", S_P_NETWORK_PRIMARY, S_H_NETWORK_PRIMARY,
     PARAM_TEXT, 0, 31, 1, NULL, 0},
    {"network_secondary_address", "network", "secondary_address", S_P_NETWORK_ADDRESS, S_H_NETWORK_ADDRESS,
     PARAM_TEXT, 0, 31, 1, NULL, 0},
    {"network_gateway", "network", "gateway", S_P_NETWORK_GATEWAY, S_H_NETWORK_GATEWAY,
     PARAM_TEXT, 0, 15, 1, NULL, 0},
    {"network_capture", "network", "capture", S_P_NETWORK_CAPTURE, S_H_NETWORK_CAPTURE,
     PARAM_ENUM, 0, 0, 0, replacement_boolean_options, 2},
#if !APCAM_HAVE_NETWORK_CONFIG
    {"network_video1_uri", "network", "video1_uri", S_P_NETWORK_VIDEO1_URI, S_H_NETWORK_VIDEO1_URI,
     PARAM_TEXT, 0, 159, 1, NULL, 0},
#endif
#if APCAM_HAVE_UART_DEVICE_OVERRIDE
    {"network_uart_device_override", "network", "uart_device_override", S_P_UART_DEVICE_OVERRIDE, S_H_UART_DEVICE_OVERRIDE,
     PARAM_TEXT, 0, 159, 1, NULL, 0},
#endif
    {"survey_pattern", "survey", "pattern", S_SURV_PATTERN, S_SURV_PATTERN_HELP,
     PARAM_ENUM, 0, 0, 0, survey_patterns, sizeof(survey_patterns)/sizeof(survey_patterns[0])},
    {"survey_lens", "survey", "lens", S_SURV_LENS, S_SURV_LENS_HELP,
     PARAM_ENUM, 0, 0, 0, survey_lenses, sizeof(survey_lenses)/sizeof(survey_lenses[0])},
    {"survey_overlap", "survey", "overlap", S_SURV_OVERLAP, S_SURV_OVERLAP_HELP, PARAM_INTEGER, 0, 90, 1, NULL, 0},
    {"survey_fore_pct", "survey", "fore_pct", S_SURV_FORE_PCT, S_SURV_FORE_PCT_HELP, PARAM_INTEGER, 10, 200, 1, NULL, 0},
    {"survey_aft_pct", "survey", "aft_pct", S_SURV_AFT_PCT, S_SURV_AFT_PCT_HELP, PARAM_INTEGER, 10, 200, 1, NULL, 0},
    {"survey_burst", "survey", "burst", S_SURV_BURST, S_SURV_BURST_HELP, PARAM_INTEGER, 1, 25, 1, NULL, 0},
    {"survey_burst_ms", "survey", "burst_ms", S_SURV_BURST_MS, S_SURV_BURST_MS_HELP, PARAM_INTEGER, 40, 2000, 1, NULL, 0},
    {"survey_dwell_ms", "survey", "dwell_ms", S_SURV_DWELL_MS, S_SURV_DWELL_MS_HELP, PARAM_INTEGER, 200, 10000, 1, NULL, 0},
    {"survey_settle_ms", "survey", "settle_ms", S_SURV_SETTLE_MS, S_SURV_SETTLE_MS_HELP, PARAM_INTEGER, 0, 2000, 1, NULL, 0},
    {"survey_error_cd", "survey", "error_cd", S_SURV_ERROR_CD, S_SURV_ERROR_CD_HELP, PARAM_INTEGER, 10, 1000, 1, NULL, 0},
    {"survey_rate_cd", "survey", "rate_cd", S_SURV_RATE_CD, S_SURV_RATE_CD_HELP, PARAM_INTEGER, 10, 2000, 1, NULL, 0},
    {"survey_terrain_ms", "survey", "terrain_ms", S_SURV_TERRAIN_MS, S_SURV_TERRAIN_MS_HELP, PARAM_INTEGER, 500, 10000, 1, NULL, 0},
    {"survey_terrain_m", "survey", "terrain_m", S_SURV_TERRAIN_M, S_SURV_TERRAIN_M_HELP, PARAM_INTEGER, 50, 2000, 1, NULL, 0},
    {"survey_terrain_tol", "survey", "terrain_tol", S_SURV_TERRAIN_TOL, S_SURV_TERRAIN_TOL_HELP, PARAM_INTEGER, 1, 100, 1, NULL, 0},
    {"survey_min_speed", "survey", "min_speed", S_SURV_MIN_SPEED, S_SURV_MIN_SPEED_HELP, PARAM_INTEGER, 1, 30, 1, NULL, 0},
    {"survey_turn_deg", "survey", "turn_deg", S_SURV_TURN_DEG, S_SURV_TURN_DEG_HELP, PARAM_INTEGER, 5, 45, 1, NULL, 0},
};

enum parameter_tab { TAB_SYSTEM, TAB_NETWORK, TAB_VIDEO, TAB_PROXY, TAB_SURVEY, TAB_COUNT };
static const struct {
    const char *name;
    enum string_id label;
} parameter_tabs[TAB_COUNT] = {
    {"system", S_PARAMS_SYSTEM}, {"network", S_PARAMS_NETWORK},
    {"video", S_PARAMS_VIDEO}, {"supportproxy", S_PARAMS_PROXY}, {"survey", S_PARAMS_SURVEY},
};

static enum parameter_tab parameter_tab(const struct parameter *p)
{
    if (!strcmp(p->section, "network") ||
        !strcmp(p->form_name, "mavlink_tcp_port") || !strcmp(p->form_name, "mavlink_udp_port"))
        return TAB_NETWORK;
    if (!strcmp(p->section, "survey")) return TAB_SURVEY;
    if (!strcmp(p->section, "support_proxy")) return TAB_PROXY;
    if (!strcmp(p->section, "video") || !strcmp(p->section, "capture") || !strcmp(p->section, "recording") ||
        !strncmp(p->section, "stream.", 7) || !strcmp(p->section, "image") ||
        !strcmp(p->section, "thermal") || !strcmp(p->section, "overlay")) return TAB_VIDEO;
    return TAB_SYSTEM;
}

static const char *replacement_defaults[] = {
    APCAM_DEFAULT_TIMEZONE, APCAM_DEFAULT_PHOTO_SCOPE == 0 ? "thermal" : "all",
    APCAM_DEFAULT_ORIENTATION == 0 ? "auto" : APCAM_DEFAULT_ORIENTATION == 1 ? "upright" : "inverted",
    "none", APCAM_STRING_VALUE(APCAM_DEFAULT_SYSTEM_ID), "100",
    APCAM_STRING_VALUE(APCAM_DEFAULT_MAVLINK_PORT), APCAM_STRING_VALUE(APCAM_DEFAULT_MAVLINK_PORT),
    APCAM_DEFAULT_POSITION_TARGETING ? "true" : "false", "false", "rate",
#ifdef CAMERA_APP_ZT30_DEV
    "2420",
#endif
    "white_hot",
#if APCAM_HAVE_RAW_THERMAL
    "5", "5",
#endif
    APCAM_RESOLUTION_NAME(APCAM_DEFAULT_MAIN_RESOLUTION), ca_video_telemetry_name(CA_DEFAULT_VIDEO_TELEMETRY), "h264",
#if APCAM_HAVE_VIDEO_BITRATE
    APCAM_STRING_VALUE(APCAM_DEFAULT_MAIN_BITRATE_KBPS),
#endif
#if APCAM_HAVE_VIDEO_FRAME_RATE
    APCAM_STRING_VALUE(APCAM_FRAME_RATE),
#endif
    APCAM_RESOLUTION_NAME(APCAM_DEFAULT_SUB_RESOLUTION), "h264",
    APCAM_DEFAULT_MAIN_ALIAS, APCAM_DEFAULT_SUB_ALIAS,
    APCAM_RESOLUTION_NAME(APCAM_DEFAULT_RECORDING_RESOLUTION), "false",
#if APCAM_HAVE_PHOTO_RESOLUTION
    "video",
#endif
    "50", "50", "50", "0",
    "auto", "auto", "average", "auto",
    "false", "", "10001", "false", "", "1", "0", "video1", "0", "video2",
#if APCAM_HAVE_RAW_THERMAL
    "0", "Raw Thermal (16-bit)",
#endif
    "", "false", "false", "false", "eth0", "", "", "", "false",
#if !APCAM_HAVE_NETWORK_CONFIG
    "",
#endif
#if APCAM_HAVE_UART_DEVICE_OVERRIDE
    "",
#endif
    "both", APCAM_HAVE_THERMAL ? "thermal" : "wide", "50", "50", "50", "1", "100", "1200", "200", "100", "100", "3000", "500", "25", "5", "15",
};


static void log_message(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static void log_message(const char *fmt, ...)
{
    char timestamp[64];
    time_t now = time(NULL);
    struct tm tm_now;
    va_list ap;

#ifdef ZT30_WEB_DEV
    /* Development services can run for hours in tmpfs; bound the regular log. */
    struct stat log_stat {};
    if (fstat(STDERR_FILENO, &log_stat) == 0 && S_ISREG(log_stat.st_mode) &&
        log_stat.st_size >= 1024 * 1024 && ftruncate(STDERR_FILENO, 0) == 0) {
        (void)lseek(STDERR_FILENO, 0, SEEK_SET);
    }
#endif
    localtime_r(&now, &tm_now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_now);
    fprintf(stderr, "[%s] ", timestamp);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
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

static bool send_all(int fd, const void *buffer, size_t length)
{
    const char *p = (const char*)(buffer);

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
                             int id, ...);

#include "APC_HTTPResponse.h"
static void send_response(int fd, int status, const char *reason,
                          const char *content_type, const char *body,
                          size_t body_len, const char *extra_headers)
{
    APC_HTTPResponse(fd, SERVER_NAME).send(status, reason, content_type, body, body_len, extra_headers);
}

#if APCAM_TARGET == APCAM_TARGET_ZR10
#if !defined(__CYGWIN__) && !defined(__APPLE__)
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
[[maybe_unused]] static int publish_no_replace(int directory, const char *temporary, const char *published)
{
#if defined(__CYGWIN__) || defined(__APPLE__)
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
#if APCAM_TARGET == APCAM_TARGET_Z1_MINI
    /* Z1 camera-app derives the private preview socket from its RTSP port. */
    const char *rtsp = getenv("CAMERA_APP_RTSP_PORT");
    if (rtsp != NULL && *rtsp != '\0') {
        char *last;
        errno = 0;
        unsigned long port = strtoul(rtsp, &last, 10);
        if (errno == 0 && *last == '\0' && port > 0 && port < 65535) {
            return (unsigned)port + 1U;
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
        .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) },
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

/* Plain-text translated reply. The final named varargs argument must use
 * its promoted type (int, rather than enum string_id) for va_start. */
static void send_text_errorf(int fd, int status, const char *reason,
                             int id, ...)
{
    APC_StringBuffer body;
    va_list ap;
    bool ok;

    body.reset();
    va_start(ap, id);
    ok = body.vappendf(T(static_cast<string_id>(id)), ap) && body.append("\n");
    va_end(ap);
    send_text_error(fd, status, reason, ok ? body.data() : "\n", NULL);
    body.reset();
}

static void send_redirect_headers(int fd, const char *location,
                                  const char *extra_headers)
{
    APC_StringBuffer headers;

    headers.reset();
    if (headers.appendf("%sLocation: %s\r\n",
                   extra_headers ? extra_headers : "", location)) {
        send_response(fd, 303, "See Other", "text/plain; charset=utf-8",
                      "", 0, headers.data());
    } else {
        send_text_errorf(fd, 500, "Internal Server Error", S_OUT_OF_MEMORY);
    }
    headers.reset();
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
    buffer = (char*)(malloc((size_t)st.st_size + 1));
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

/* Rotated log files, unless the camera app logs to a journal. */
#ifndef APP_LOG_COMMAND
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
    buffer = (char*)(malloc(wanted + 1));
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
    combined = (char*)(malloc(old_len + current_len + 1));
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
#endif

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

static const char *find_header_span(const APC_HTTPRequest *request, const char *name, size_t *length)
{
    return request->header_span(name, length);
}
static const char *find_header(const APC_HTTPRequest *request, const char *name, char *value, size_t size)
{
    return request->header(name, value, size);
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

static int authenticate_basic(const APC_HTTPRequest *request)
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
    colon = (char*)(memchr(decoded, ':', decoded_len));
    if (colon == NULL) return 0;
    return check_password((const char *)decoded, (size_t)(colon - (char *)decoded),
                          colon + 1,
                          decoded_len - (size_t)(colon + 1 - (char *)decoded));
}

static bool cookie_value(const APC_HTTPRequest *request, const char *name,
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
        equals = (const char*)(memchr(p, '=', (size_t)(end - p)));
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
static int authenticate(const APC_HTTPRequest *request, char session_token[65])
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
    char *output = (char*)(malloc(length + 1));
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
        const char *pair_end = (const char*)(memchr(p, '&', (size_t)(end - p)));
        const char *equals;
        if (pair_end == NULL) pair_end = end;
        equals = (const char*)(memchr(p, '=', (size_t)(pair_end - p)));
        if (equals != NULL && (size_t)(equals - p) == key_len &&
            memcmp(p, key, key_len) == 0) {
            return url_decode(equals + 1, (size_t)(pair_end - equals - 1), value_len);
        }
        p = pair_end + (pair_end < end ? 1 : 0);
    }
    return NULL;
}

static char *form_value(const APC_HTTPRequest *request, const char *key,
                        size_t *value_len)
{
    return encoded_value(request->body, request->body_len, key, value_len);
}

static char *query_value(const APC_HTTPRequest *request, const char *key,
                         size_t *value_len)
{
    return encoded_value(request->query, strlen(request->query), key, value_len);
}

static bool valid_csrf(const APC_HTTPRequest *request)
{
    size_t length;
    char *token = form_value(request, "csrf", &length);
    bool valid = token != NULL &&
                 constant_time_equal(token, length, csrf_token, strlen(csrf_token));
    free(token);
    return valid;
}

static bool valid_csrf_header(const APC_HTTPRequest *request)
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
            const char *close = (const char*)(memchr(config + start + 1, ']', end - start - 1));
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
        const char *equals = (const char*)(memchr(config + start, '=', end - start));
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
            const char *close = (const char*)(memchr(start + 1, ']', (size_t)(end - start - 1)));
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
            const char *equals = (const char*)(memchr(start, '=', (size_t)(end - start)));
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

// The camera reader only strips quotes wrapping the whole value, so quote just
// the values that would otherwise change when read back.
static bool ini_needs_quotes(const char *value)
{
    size_t length = strlen(value);
    return length == 0U || isspace((unsigned char)value[0]) ||
           isspace((unsigned char)value[length - 1U]) ||
           (length >= 2U && value[0] == '"' && value[length - 1U] == '"');
}

static char *ini_apply_updates(const char *config, struct ini_update *updates,
                               size_t count, bool add_missing,
                               size_t *new_length,
                               char *error, size_t error_size)
{
    APC_StringBuffer output;
    const char *p = config;
    char section[80] = "";

    for (size_t i = 0; i < count; i++) updates[i].found = false;
    output.reset();
    while (*p != '\0') {
        const char *line_end = strchr(p, '\n');
        const char *start = p;
        const char *end;
        struct ini_update *update = NULL;
        if (line_end == NULL) line_end = p + strlen(p);
        end = line_end;
        trim_span(&start, &end);
        if (start < end && *start == '[') {
            const char *close = (const char*)(memchr(start + 1, ']', (size_t)(end - start - 1)));
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
            const char *equals = (const char*)(memchr(start, '=', (size_t)(end - start)));
            if (equals != NULL) {
                const char *key_start = start;
                const char *key_end = equals;
                trim_span(&key_start, &key_end);
                update = find_update(updates, count, section, key_start, key_end);
                if (update != NULL && update->found) {
                    snprintf(error, error_size, T(S_E_INI_DUPLICATE_KEY),
                             section, update->key);
                    output.reset();
                    return NULL;
                }
                if (update != NULL) {
                    const char *value_start = equals + 1;
                    const char *value_end = end;
                    trim_span(&value_start, &value_end);
                    // keep the existing line's quoting style
                    const bool quoted = value_end > value_start + 1 &&
                                        *value_start == '"' && value_end[-1] == '"';
                    if (quoted) {
                        value_start++;
                        value_end--;
                        if (strcmp(section, "support_proxy") != 0)
                            trim_span(&value_start, &value_end);
                    }
                    if (span_equal(value_start, value_end, update->value)) {
                        update->found = true;
                        update = NULL;
                    } else {
                        if (!output.append_n(p, (size_t)(equals + 1 - p)) ||
                            !output.appendf(quoted || ini_needs_quotes(update->value)
                                                ? " \"%s\"" : " %s",
                                            update->value)) {
                            output.reset();
                            return NULL;
                        }
                        update->found = true;
                    }
                }
            }
        }
        if (update == NULL && !output.append_n(p, (size_t)(line_end - p))) {
            output.reset();
            return NULL;
        }
        if (*line_end == '\n' && !output.append_n("\n", 1)) {
            output.reset();
            return NULL;
        }
        p = *line_end == '\n' ? line_end + 1 : line_end;
    }
    for (size_t i = 0; i < count; i++) {
        if (!updates[i].found && !add_missing) {
            snprintf(error, error_size, T(S_E_INI_MISSING_KEY),
                     updates[i].section, updates[i].key);
            output.reset();
            return NULL;
        }
    }
    if (add_missing) {
        for (size_t i = 0; i < count; i++) {
            if (updates[i].found) continue;
            if (output.size() != 0U && output.data()[output.size() - 1U] != '\n' &&
                !output.append_n("\n", 1U)) goto allocation_failed;
            if (output.size() != 0U && !output.append_n("\n", 1U)) {
                goto allocation_failed;
            }
            if (!output.appendf("[%s]\n", updates[i].section)) {
                goto allocation_failed;
            }
            for (size_t j = i; j < count; j++) {
                if (updates[j].found ||
                    strcmp(updates[j].section, updates[i].section) != 0) {
                    continue;
                }
                if (!output.appendf(ini_needs_quotes(updates[j].value)
                                        ? "%s = \"%s\"\n" : "%s = %s\n",
                                    updates[j].key, updates[j].value)) {
                    goto allocation_failed;
                }
                updates[j].found = true;
            }
        }
    }
    if (output.size() > MAX_CONFIG) {
        snprintf(error, error_size, "%s", T(S_E_CONFIG_TOO_LARGE));
        output.reset();
        return NULL;
    }
    *new_length = output.size();
    return output.release();

allocation_failed:
    snprintf(error, error_size, "%s", T(S_E_CONFIG_ALLOC));
    output.reset();
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
        if (!strcmp(parameter->key, "video1_uri"))
            return apcam_video_uri_valid(text);
        if (!strcmp(parameter->key, "uart_device_override"))
            return apcam_uart_device_override_valid(text);
        return apcam_ipv4_prefix(text, &address, &prefix);
    }
    if (!strcmp(parameter->key, "alias")) /* one RTSP path segment */
        return strspn(text, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == length;
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
    if (APCAM_NUM_STREAMS < 2 && (!strcmp(parameter->section, "stream.sub") ||
        !strcmp(parameter->form_name, "proxy_video2_port") ||
        !strcmp(parameter->form_name, "proxy_video2_name"))) return false;
    if (!APCAM_HAVE_RECORDING_RESOLUTION && !strcmp(parameter->form_name, "recording_resolution")) return false;
#ifdef CAMERA_APP_ZT30_DEV
    const auto *field = APC_Config::find(parameter->section, parameter->key);
    if (!field || !APC_ZT30_Config::editable(*field)) return false;
#endif
#ifndef CAMERA_APP_ZT30_DEV
    if (!APCAM_HAVE_THERMAL && !strcmp(parameter->form_name, "osd_thermal_fov")) return false;
#endif
    if (!APCAM_HAVE_OVERLAY_RECORDING_SELECT && !strcmp(parameter->form_name, "osd_recording")) return false;
    if (!APCAM_HAVE_IMAGE_CONTROLS && !strcmp(parameter->section, "image")) return false;
    if (!APCAM_HAVE_EXTERNAL_UART && !strcmp(parameter->section, "uart")) return false;
    /* The host OS owns the interfaces; only capture, the video URI and the gimbal link apply. */
    if (!APCAM_HAVE_NETWORK_CONFIG && !strcmp(parameter->section, "network") &&
        strcmp(parameter->key, "capture") && strcmp(parameter->key, "video1_uri") &&
        strcmp(parameter->key, "uart_device_override")) return false;
    if (!APCAM_HAVE_THERMAL && !strcmp(parameter->section,"thermal") &&
        !(APCAM_HAVE_RAW_THERMAL && (!strcmp(parameter->key,"stream_fps") || !strcmp(parameter->key,"record_fps"))) &&
        !(APCAM_HAVE_THERMAL_PALETTE && !strcmp(parameter->key,"palette"))
#ifdef CAMERA_APP_ZT30_DEV
        && strcmp(parameter->key,"hfov_cd")
#endif
        ) return false;
    if (!APCAM_HAVE_THERMAL_VIDEO && !strcmp(parameter->key,"photo_scope")) return false;
    return true;
}

// Numeric domains and storage limits come from the same schema as MAVLink
// and the app; translated labels and presentation-specific steps remain here.
static void parameter_limits(const struct parameter *parameter, double &minimum, double &maximum)
{
    minimum = parameter->minimum;
    maximum = parameter->maximum;
    const APC_Config::Field *field = APC_Config::find(parameter->section, parameter->key);
    if (!field) return;
    if (field->kind == APC_Config::Kind::Int || field->kind == APC_Config::Kind::Uint) {
        minimum = field->minimum;
        maximum = field->maximum;
    } else if (field->kind == APC_Config::Kind::String || field->kind == APC_Config::Kind::Timezone) {
        if (maximum >= field->size) maximum = field->size - 1U;
    }
}

static bool collect_described_parameters(const APC_HTTPRequest *request,
                                         const struct parameter *parameters,
                                         size_t parameter_count,
                                         struct ini_update *updates,
                                         size_t capacity, size_t *count,
                                         char *error, size_t error_size)
{
    for (size_t i = 0; i < parameter_count; i++) {
        const struct parameter *parameter = &parameters[i];
        double minimum, maximum;
        parameter_limits(parameter, minimum, maximum);
        size_t value_length = 0;
        char *value;

        if (!parameter_shown(parameter)) {
#ifdef CAMERA_APP_ZT30_DEV
            value = form_value(request, parameter->form_name, &value_length);
            if (value) {
                free(value);
                snprintf(error, error_size, "%s is not configurable on the native ZT30 build", parameter->form_name);
                return false;
            }
#endif
            continue;
        }
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
                valid = parse_long_strict(value, (long)minimum,
                                          (long)maximum, &integer_value);
                if (valid) snprintf(normalized, sizeof(normalized), "%ld", integer_value);
                break;
            case PARAM_FLOAT:
                valid = parse_double_strict(value, minimum, maximum, &float_value);
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
            case PARAM_TIMEZONE:
            case PARAM_TEXT:
            case PARAM_PASSWORD:
                valid = valid_text_parameter(parameter, value);
                if (valid) snprintf(normalized, sizeof(normalized), "%s", value);
                break;
            }
        }
        const APC_Config::Field *field = APC_Config::find(parameter->section, parameter->key);
        if (valid && field) valid = APC_Config::validate(*field, normalized);
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
    const APC_HTTPRequest *request, struct ini_update *updates, size_t capacity,
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
    const char *video3_port = "0", *video3_name = "";
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
        SUPPORT_VALUE("video3_port", video3_port);
        SUPPORT_VALUE("video3_name", video3_name);
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
        if (invalid == S_COUNT && strcmp(video3_port, "0") != 0) {
            if (!*video3_name) invalid = S_P_PROXY_VIDEO3_NAME;
            else if (!strcmp(video3_port, video1_port) || !strcmp(video3_port, video2_port))
                invalid = S_P_PROXY_VIDEO3_PORT;
        }
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
    const char *p = (const char*)(buffer);
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

static bool change_admin_password(const APC_HTTPRequest *request,
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

static bool sync_time_from_browser(const APC_HTTPRequest *request,
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
    {
    struct timespec requested = {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
    };
    if (clock_settime(CLOCK_REALTIME, &requested) < 0) {
        snprintf(error, error_size, T(S_E_SET_TIME), strerror(errno));
        goto done;
    }
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

static bool add_authorized_keys(const APC_HTTPRequest *request,
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
    {
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
    candidates = (struct public_key_line*)(calloc(max_candidates, sizeof(*candidates)));
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
    updated = (char*)(malloc(updated_len + 1));
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
    }
done:
    if (lock_fd >= 0) close(lock_fd);
    free(candidates);
    free(updated);
    free(existing);
    free(input);
    return ok;
}

static bool remove_authorized_key(const APC_HTTPRequest *request,
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
    updated = (char*)(malloc(existing_len + 1));
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

#if (APCAM_TARGET == APCAM_TARGET_MT11) || (defined(MT11_WEB_SITL) && !defined(WEB_SUPERVISED_TEST)) || defined(ZT30_WEB_NATIVE)
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
    {
    int input_error = ferror(input);
    free(line);
    fclose(input);
    if (output >= 0) close(output);
    return input_error ? 1 : 0;
    }
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
#ifdef CAMERA_APP_ZT30_DEV
    ca_config before, candidate;
    ca_config_defaults(&before);
    ca_config_defaults(&candidate);
    if (ca_config_load(&before, config_path, error, error_size) < 0) {
        if (errno != EINVAL && errno != ENOENT) return false;
        // Permit repair of invalid/missing INI, but only supported/default
        // values for unavailable controls. Never guess the corrupt old values.
        ca_config_defaults(&before);
        APC_ZT30_Config::effective(before);
    }
    if (ca_config_parse(&candidate, config, length, error, error_size) < 0 ||
        !APC_ZT30_Config::validate_change(before, candidate, error, error_size)) return false;
#endif
    bool existing = stat(config_path, &st) == 0;
    if (!existing) {
#ifdef CAMERA_APP_ZT30_DEV
        if (errno == ENOENT) {
            st = {}; st.st_mode = 0644; st.st_uid = geteuid(); st.st_gid = getegid();
        } else
#endif
        {
            snprintf(error, error_size, T(S_E_CANNOT_STAT), config_path, strerror(errno));
            return false;
        }
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

    if (existing && (!copy_to_temp(config_path, backup_temp, st.st_mode & 0777, st.st_uid, st.st_gid) ||
        rename(backup_temp, backup_path) < 0)) {
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
    char executable[PATH_MAX];
#ifdef __APPLE__
    ssize_t length = proc_pidpath(pid, executable, sizeof(executable));
    if (length <= 0) return CAMERA_NONE;
    length = strlen(executable);
#else
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%ld/exe", (long)pid);
    ssize_t length = readlink(proc_path, executable, sizeof(executable) - 1);
    if (length < 0) return CAMERA_NONE;
    executable[length] = '\0';
#endif
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

#if APCAM_TARGET == APCAM_TARGET_ZR10 || defined(__APPLE__)
    /* macOS canonicalizes /var to /private/var.
     * The initial SD test leaves the original executable at its stock path. */

    char resolved[PATH_MAX];
    if (realpath(REPLACEMENT_CAMERA_PATH, resolved) != NULL &&
        strcmp(executable, resolved) == 0) return CAMERA_REPLACEMENT;
#endif
    if (strcmp(executable, REPLACEMENT_CAMERA_PATH) == 0) return CAMERA_REPLACEMENT;
#ifdef ZT30_WEB_NATIVE
    // An atomically installed inode may retain its original staging dentry.
    // Match only our updater path and the canonical launch argument.
    if (zt30_slot_app_pid(pid, executable, APP_DIR)) return CAMERA_REPLACEMENT;
    if (zt30_staged_app_pid(pid, executable, REPLACEMENT_CAMERA_PATH,
                            "/customer/.apcg-update.")) return CAMERA_REPLACEMENT;
#endif
    return CAMERA_NONE;
}


static size_t camera_pids_kind(enum camera_kind wanted, pid_t *pids, size_t max_pids)
{
#ifdef __APPLE__
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (bytes <= 0 || bytes > INT_MAX - 1024) return 0;
    bytes += 1024; // Allow for processes created between the two calls.
    auto *all = static_cast<pid_t *>(malloc(bytes));
    if (!all) return 0;
    int used = proc_listpids(PROC_ALL_PIDS, 0, all, bytes);
    size_t count = 0;
    for (int i = 0; i < used / (int)sizeof(pid_t); i++) {
        enum camera_kind found = pid_camera_kind(all[i]);
        if (found != CAMERA_NONE && (wanted == CAMERA_ANY || wanted == found)) {
            if (count < max_pids) pids[count] = all[i];
            count++;
        }
    }
    free(all);
    return count;
#else
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
#endif
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
        .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) },
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

static bool send_live_control(const APC_HTTPRequest *request, char *error,
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
    {
    if (!packet.action) { snprintf(error,error_size,"%s",T(S_E_UNKNOWN_ACTION)); goto done; }
    bool pulse=packet.action>=APCAM_MANUAL_LEFT && packet.action<=APCAM_MANUAL_DOWN;
    if (pulse || packet.action==APCAM_MANUAL_ZOOM) {
        if (!value) { snprintf(error,error_size,"%s",T(pulse ? S_E_MISSING_RATE : S_E_MISSING_ZOOM)); goto done; }
        char *end;
        errno=0; packet.value=strtof(value,&end);
        if (errno || end==value || *end || !isfinite(packet.value) ||
            packet.value<(pulse ? 5 : 1) || packet.value>(pulse ? 60 : APC_Camera::get_singleton().zoom_control_max())) {
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
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(port),.sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) }};
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
    }
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
    const uint8_t payload[] = {(uint8_t)(enabled ? 1U : 0U)};
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

#if APCAM_WEB_CONTROL_MAVLINK && APCAM_HAVE_PHOTO
/* MAVLink-controlled cameras: send the command a GCS would, and report
 * the camera's COMMAND_ACK, which follows the saved photo. */
static bool trigger_camera_photo(char *error, size_t error_size)
{
    const struct timeval timeout = {.tv_sec = 10, .tv_usec = 0};
    mavlink_message_t message;
    mavlink_status_t status = {};
    bool acknowledged = false, accepted = false;

#ifndef MT11_WEB_TEST
    if (current_camera_kind() == CAMERA_NONE) {
        snprintf(error, error_size, "%s", T(S_E_CAMERA_NOT_RUNNING));
        return false;
    }
#endif
    int fd = z1_web_socket(camera_api_port());
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        snprintf(error, error_size, T(S_E_CAMERA_API), strerror(errno));
        if (fd >= 0) close(fd);
        return false;
    }
    mavlink_msg_command_long_pack(255, 191, &message, 0, 0, MAV_CMD_IMAGE_START_CAPTURE, 0,
                                  0, 0, 1, 0, 0, 0, 0);
    if (!z1_web_send(fd, &message)) {
        snprintf(error, error_size, T(S_E_SEND_SHUTTER), strerror(errno));
        close(fd);
        return false;
    }
    /* Heartbeats keep arriving, so the receive timeout alone never expires. */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout.tv_sec;
    while (!acknowledged) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) break;
        uint8_t buffer[MAVLINK_MAX_PACKET_LEN * 2];
        ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n && !acknowledged; i++) {
            if (!mavlink_parse_char(0, buffer[i], &message, &status) ||
                message.msgid != MAVLINK_MSG_ID_COMMAND_ACK) continue;
            mavlink_command_ack_t ack;
            mavlink_msg_command_ack_decode(&message, &ack);
            if (ack.command != MAV_CMD_IMAGE_START_CAPTURE) continue;
            acknowledged = true;
            accepted = ack.result == MAV_RESULT_ACCEPTED;
        }
    }
    close(fd);
    if (!acknowledged) {
        snprintf(error, error_size, "%s", T(S_E_SHUTTER_UNCONFIRMED));
        return false;
    }
    if (!accepted) {
        snprintf(error, error_size, "%s", T(S_E_CAPTURE_FAILED));
        return false;
    }
    return true;
}
#else
static bool trigger_camera_photo(char *error, size_t error_size)
{
    const struct timeval timeout = {.tv_sec = 10, .tv_usec = 0};
    const uint8_t capture[] = {0U};
    uint8_t feedback[16];
    size_t feedback_length = 0U;
    int fd;

    if (!APCAM_HAVE_PHOTO) {
        snprintf(error, error_size, "%s", T(S_E_CAPTURE_FAILED));
        return false;
    }
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
#endif


#ifdef APP_LOG_COMMAND
/* Tail of a log command's output, for camera apps logging to a journal. */
static char *read_current_app_log(size_t *length)
{
    FILE *pipe = popen(APP_LOG_COMMAND " 2>&1", "r");
    char *log = (char *)malloc(APP_LOG_DISPLAY_SIZE + 1U);
    size_t used = 0;
    char chunk[4096];
    size_t got;

    *length = 0;
    if (pipe == NULL || log == NULL) {
        if (pipe != NULL) (void)pclose(pipe);
        free(log);
        return strdup("");
    }
    while ((got = fread(chunk, 1, sizeof(chunk), pipe)) > 0) {
        /* chunk is smaller than the display size; keep the newest bytes */
        if (used + got > APP_LOG_DISPLAY_SIZE) {
            size_t drop = used + got - APP_LOG_DISPLAY_SIZE;
            memmove(log, log + drop, used - drop);
            used -= drop;
        }
        memcpy(log + used, chunk, got);
        used += got;
    }
    (void)pclose(pipe);
    log[used] = '\0';
    *length = used;
    return log;
}
#else
static char *read_current_app_log(size_t *length)
{
    return read_app_log_paths(REPLACEMENT_LOG_PATH, REPLACEMENT_LOG_OLD_PATH, length);
}
#endif

static int lock_restart(char *error, size_t error_size, bool nonblocking = false)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/camera-app.restart.lock", RUNTIME_DIR) >= (int)sizeof(path)) {
        snprintf(error, error_size, "%s", T(S_E_APP_SELECTION_PATH));
        return -1;
    }
    int lock = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);

    int result = -1;
#ifdef ZT30_WEB_NATIVE
    const int64_t deadline = apcam::probe_milliseconds() + 1500;
    if (lock >= 0) do {
        result = flock(lock, LOCK_EX | LOCK_NB);
        if (!result || nonblocking || (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) ||
            apcam::probe_milliseconds() >= deadline) break;
        usleep(20000);
    } while (true);
#else
    (void)nonblocking;
    if (lock >= 0) result = flock(lock, LOCK_EX);
#endif
    if (result < 0) {
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

static void append_filesystem_status(APC_StringBuffer *page,
                                     const char *label, const char *path)
{
    struct statvfs fs;
    char used[32];
    char total[32];
    unsigned percent = 0;
    bool mounted = !WEB_MEDIA_IS_MOUNT || strcmp(path, MEDIA_ROOT) != 0 || is_mounted(path);

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
    page->appendf("<tr><th>%s</th><td>%s%s / %s (%u%%)</td></tr>",
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
    if (strcasecmp(ext, "ts") == 0) return "video/mp2t";
    if (strcasecmp(ext, "webm") == 0) return "video/webm";
    if (strcasecmp(ext, "mov") == 0) return "video/quicktime";
    if (strcasecmp(ext, "mkv") == 0) return "video/x-matroska";
    if (strcasecmp(ext, "pcap") == 0) return "application/vnd.tcpdump.pcap";
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
    return strncmp(mime, "video/", 6) == 0 && strcmp(mime, "video/mp2t") != 0;
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

static void append_path_query(APC_StringBuffer *page, const char *route,
                              const char *path)
{
    page->append(route);
    page->append_url(path);
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

static const char *page_style = "style.css";

static void append_head(APC_StringBuffer *page, const char *title,
                        const char *style, const char *extra_head)
{
    APC_StringBuffer escaped;
    escaped.append_html(title);
    webroot.render(*page, "head.html", {
        languages[current_language].html_lang, extra_head ? extra_head : "",
        escaped.data(), style});
#ifdef ZT30_WEB_DEV
    page->append("<p role=status>ZT30 development load: temporary web and SSH services. "
                 "Vendor video remains separate; camera changes are unavailable in this load.</p>");
#endif
}

static void append_language_select(APC_StringBuffer *page, const char *id)
{
    page->appendf("<select name=lang id=%s aria-label=\"", id);
    page->append_html(T(S_LANGUAGE));
    page->append("\">");
    for (size_t i = 0; i < LANG_COUNT; i++) {
        page->appendf("<option value=%s%s>", languages[i].code,
                   i == (size_t)current_language ? " selected" : "");
        page->append_html(translations[S_LANGUAGE_NAME][i]);
        page->append("</option>");
    }
    page->append("</select>");
}

static void append_logo(APC_StringBuffer *page, const char *href)
{
    char label[160];

    snprintf(label, sizeof(label), T(S_NAV_TOP), PRODUCT_NAME);
    page->appendf("<a class=brand-logo href=\"%s\" aria-label=\"", href);
    page->append_html(label);
    page->append("\"><img alt=\"ArduPilot\" referrerpolicy=no-referrer src=\""
                    ARDUPILOT_LOGO_URL "\"></a>");
}

/* route and path_argument name the current page so the language form can
 * return to it */
static void append_nav(APC_StringBuffer *page, const char *route,
                       const char *path_argument)
{
    append_logo(page, "/#top");
    page->appendf("<nav><a href=\"/\">%s</a><a href=\"/parameters\">%s</a>"
                     "<a href=\"/raw\">%s</a><a href=\"/users\">%s</a><a href=\"/files?path=",
               T(S_NAV_STATUS), T(S_NAV_PARAMETERS), T(S_NAV_RAW), T(S_NAV_USERS));
    page->append_url(MEDIA_ROOT);
    page->appendf("\">%s</a><a href=\"/live\">%s</a><a href=\"/sensors\">%s</a>"
                     "<a href=\"/log\">%s</a>",
               T(S_NAV_FILES), T(S_NAV_LIVE), T(S_NAV_SENSORS), T(S_NAV_DEBUG));
    page->appendf("<form class=\"nav-form lang-form\" method=post action=/language>"
                     "<input type=hidden name=csrf value=\"%s\">"
                     "<input type=hidden name=next value=\"", csrf_token);
    page->append_html(route);
    if (path_argument != NULL) {
        page->append("?path=");
        page->append_url(path_argument);
    }
    page->append("\">");
    append_language_select(page, "lang-nav");
    webroot.render(*page, "nav-actions.html", {T(S_APPLY), csrf_token, T(S_NAV_LOGOUT)});
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

static void append_notice(APC_StringBuffer *page, const char *message, bool is_error)
{
    char *copy;

    if (message == NULL) return;
    copy = strndup(message, utf8_complete_length(message, strlen(message)));
    if (copy == NULL) return;
    page->appendf("<p class=\"notice%s\">", is_error ? " error" : "");
    page->append_html(copy);
    page->append("</p>");
    free(copy);
}

static void append_parameter_field(APC_StringBuffer *page, const char *config,
                                   const struct parameter *parameter,
                                   const APC_HTTPRequest *submitted)
{
    double minimum, maximum;
    parameter_limits(parameter, minimum, maximum);
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
        static_assert(sizeof(replacement_defaults) /
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

    page->append("<div class=field><label for=\"");
    page->append_html(parameter->form_name);
    page->append("\">");
    page->append_html(T(parameter->label));
    page->append("</label>");
    if (parameter->kind == PARAM_TIMEZONE) {
        page->append("<select required name=\"timezone\" id=\"timezone\">");
        // Keep an existing POSIX setting selected when saving unrelated fields.
        if (present && !APC_Timezone::rule(display_value)) {
            page->append("<option selected value=\"");
            page->append_html(display_value);
            page->append("\">");
            page->append_html(display_value);
            page->append("</option>");
        }
        for (const auto &zone : APC_Timezone::zones) {
            page->append("<option value=\"");
            page->append_html(zone.name);
            page->appendf("\"%s>", present && !strcmp(display_value, zone.name) ? " selected" : "");
            page->append_html(zone.name);
            page->append("</option>");
        }
        page->append("</select>");
    } else if (parameter->kind == PARAM_ENUM || parameter->kind == PARAM_BOOLEAN) {
        page->append("<select required name=\"");
        page->append_html(parameter->form_name);
        page->append("\" id=\"");
        page->append_html(parameter->form_name);
        page->append("\">");
        if (!present || !option_contains(parameter, display_value)) {
            page->append("<option selected disabled value=\"\">");
            page->append_html(T(S_UNKNOWN_CURRENT_VALUE));
            page->append("</option>");
        }
        for (size_t i = 0; i < parameter->option_count; i++) {
            page->append("<option value=\"");
            page->append_html(parameter->options[i].value);
            page->appendf("\"%s>", present && strcmp(display_value, parameter->options[i].value) == 0 ? " selected" : "");
            page->append_html(T(parameter->options[i].label));
            page->append("</option>");
        }
        page->append("</select>");
    } else {
        bool optional = (parameter->kind == PARAM_TEXT || parameter->kind == PARAM_PASSWORD) &&
                        minimum == 0;
        page->append(optional ? "<input name=\"" : "<input required name=\"");
        page->append_html(parameter->form_name);
        page->append("\" id=\"");
        page->append_html(parameter->form_name);
        if (parameter->kind == PARAM_IPV4) {
            page->append("\" type=text inputmode=numeric value=\"");
        } else if (parameter->kind == PARAM_TEXT || parameter->kind == PARAM_PASSWORD) {
            page->appendf("\" type=%s autocomplete=off minlength=\"%.0f\" maxlength=\"%.0f\"",
                       parameter->kind == PARAM_PASSWORD ? "password" : "text",
                       minimum, maximum);
            if (!strcmp(parameter->form_name, "network_primary_address") || !strcmp(parameter->form_name, "network_secondary_address")) {
                page->append(" pattern=\"[0-9]{1,3}(\\.[0-9]{1,3}){3}/([1-9]|[12][0-9]|3[0-2])\" title=\"");
                page->append_html(T(parameter->help));
                page->append("\"");
            } else if (!strcmp(parameter->form_name, "network_uart_device_override")) {
                page->append(" list=uart_device_override_choices");
            } else if (!strcmp(parameter->key, "alias")) {
                page->append(" pattern=\"[A-Za-z0-9._-]*\" title=\"");
                page->append_html(T(parameter->help));
                page->append("\"");
            }
            page->append(" value=\"");
        } else {
            page->appendf("\" type=number min=\"%.8g\" max=\"%.8g\" step=\"%.8g\" value=\"",
                       minimum, maximum, parameter->step);
        }
        if (present) page->append_html(display_value);
        page->append("\">");
        if (!strcmp(parameter->form_name, "network_uart_device_override")) {
            /* Suggestions only: any valid device or udp:// address can be typed. */
            page->append("<datalist id=uart_device_override_choices>");
            for (const char *choice : UART_DEVICE_OVERRIDE_CHOICES) {
                page->append("<option value=\"");
                page->append_html(choice);
                page->append("\">");
            }
            page->append("</datalist>");
        }
    }
    free(submitted_value);
    page->append("<div class=help>");
    page->append_html(T(parameter->help));
    if (strcmp(parameter->section, "support_proxy") == 0 ||
        (strcmp(parameter->section, "network") == 0 && strcmp(parameter->key, "capture") != 0 &&
         strcmp(parameter->key, "video1_uri") != 0) ||
        strcmp(parameter->form_name, "orientation") == 0 ||
        strcmp(parameter->form_name, "uart_protocol") == 0 ||
        strcmp(parameter->form_name, "mavlink_system_id") == 0 ||
        strcmp(parameter->form_name, "mavlink_camera_component_id") == 0 ||
        strcmp(parameter->form_name, "mavlink_tcp_port") == 0 ||
        strcmp(parameter->form_name, "mavlink_udp_port") == 0) {
        page->append(" ");
        page->append_html(T(S_RESTART_REQUIRED));
    }
    page->appendf(" <code>[%s] %s</code></div></div>", parameter->section, parameter->key);
}


#if WEB_HAVE_SOC_TEMPERATURE
static bool read_soc_temperature(int *millidegrees)
{
/* Kernel thermal zone where available; otherwise the SS928 sensor registers. */
#ifdef SOC_TEMPERATURE_PATH
#ifdef SOC_TEMPERATURE_LABELLED_CELSIUS
    constexpr bool labelled=true;
#else
    constexpr bool labelled=false;
#endif
    return apcam_read_soc_temperature(SOC_TEMPERATURE_PATH,labelled,millidegrees);
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

#ifdef ZT30_WEB_NATIVE
#include "zt30-bundle-status.h"
#endif

static char *render_page(const char *message, bool message_is_error, size_t *page_len)
{
    APC_StringBuffer page;
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
    strftime(now_text, sizeof(now_text), "%Y-%m-%d %H:%M:%S %Z (UTC%z)", &tm_now);
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

    page.reset();
    snprintf(title, sizeof(title), T(S_TITLE_CONTROL), PRODUCT_NAME);
    append_head(&page, title, page_style, NULL);
    page.append("<header id=top><h1>");
    page.append_html(title);
    page.appendf("</h1><div class=muted>%s</div></header>", T(S_STATUS_SUBTITLE));
    append_nav(&page, "/", NULL);
    append_notice(&page, message, message_is_error);
    page.appendf("<div class=grid><section class=card><h2>%s</h2><table>",
               T(S_STATUS_HEADING));
    page.appendf("<tr><th>%s</th><td id=firmware-version>", T(S_STATUS_FIRMWARE_VERSION));
    page.append_html(CA_FIRMWARE_VERSION);
    page.append(" (<span id=firmware-git-hash>");
    page.append_html(CA_FIRMWARE_GIT_HASH);
    page.append("</span>)</td></tr>");
#ifdef ZT30_WEB_NATIVE
    append_zt30_bundle_status(page, pid_count ? pids[0] : 0);
#endif
    page.appendf("<tr><th>%s</th><td>%s", T(S_STATUS_CAMERA_APP),
               pid_count ? camera_label(active_kind) : T(S_APP_STOPPED));
    if (pid_count) page.appendf(T(S_STATUS_PID_RSS), (long)pids[0], camera_rss);
    page.append("</td></tr>");
    page.appendf("<tr><th>%s</th><td>", T(S_STATUS_WEB_SERVICE));
    page.appendf(T(S_STATUS_WEB_PID_RSS), (long)getpid(), web_rss);
    webroot.render(page, "status-time.html", {T(S_STATUS_TIME), now_text, csrf_token, T(S_STATUS_SYNC)});
    page.appendf("<tr><th>%s</th><td>%s</td></tr>", T(S_STATUS_UPTIME), uptime_text);
#if APCAM_TARGET == APCAM_TARGET_A8
    {
        unsigned busy = 0, cpus = 0;
        page.appendf("<tr><th>%s</th><td>", T(S_STATUS_CPU));
        if (read_cpu_busy(&busy, &cpus)) {
            page.appendf(T(S_STATUS_CPU_BUSY), busy, cpus,
                       (int)strcspn(load_text, " "), load_text);
        } else {
            page.append(T(S_UNAVAILABLE));
        }
        page.append("</td></tr>");
    }
#else
    page.appendf("<tr><th>%s</th><td>%s</td></tr>", T(S_STATUS_LOAD), load_text);
#endif
#if WEB_HAVE_SOC_TEMPERATURE
    page.appendf("<tr><th>%s</th><td>", T(S_STATUS_SOC_TEMPERATURE));
    if (have_soc_temperature) {
        int absolute = soc_temperature < 0 ? -soc_temperature : soc_temperature;
#ifdef SOC_TEMPERATURE_LABELLED_CELSIUS
        page.appendf(T(S_STATUS_SOC_AVERAGE),soc_temperature < 0 ? "-" : "",absolute / 1000);
#else
        page.appendf(T(S_STATUS_SOC_AVERAGE),
                   soc_temperature < 0 ? "-" : "", absolute / 1000,
                   (absolute % 1000) / 100);
#endif
    } else {
        page.append(T(S_UNAVAILABLE));
    }
    page.append("</td></tr>");
#endif
    page.appendf("<tr><th>%s</th><td>", T(S_STATUS_MEMORY));
    page.appendf(T(S_STATUS_MEMORY_VALUE), mem_available / 1024, mem_total / 1024);
    page.appendf("</td></tr><tr><th>%s</th><td>%s</td></tr>", T(S_STATUS_IPV4),
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
#if WEB_MEDIA_IS_MOUNT
    append_filesystem_status(&page, T(S_STORAGE_MICROSD), MEDIA_ROOT);
#else
    append_filesystem_status(&page, T(S_STORAGE_RECORDINGS), MEDIA_ROOT);
#endif
    webroot.render(page, "status-actions.html", {T(S_STATUS_REFRESH), T(S_STATUS_ACTIONS), csrf_token});
    page.append(T(S_STATUS_RESTART));
    page.append("</button></form>");
#ifdef ZT30_WEB_NATIVE
    std::string previous_bundle, previous_error;
    if (apcam_zt30::link_slot(APP_DIR, "previous", previous_bundle, previous_error))
        page.appendf("<form id=application-rollback method=post action=/upgrade/rollback><input type=hidden name=csrf value=\"%s\"><button>%s</button></form>", csrf_token, T(S_APP_ROLLBACK));
#endif
#if WEB_HAVE_FIRMWARE_UPLOAD
    page.appendf("<form id=firmware-upload method=post action=/upgrade>"
                      "<input id=firmware type=file accept=" FIRMWARE_SUFFIX " hidden>"
                      "<button id=select-firmware type=button>%s</button>"
                      "<progress id=firmware-progress value=0 max=100 hidden></progress>"
                      "<output id=firmware-status class=upload-status></output></form>"
                      "<script src=/upgrade.js data-csrf=\"%s\" defer></script>"
                      "<p class=muted>", T(S_STATUS_UPGRADE_BUTTON), csrf_token);
    page.appendf(T(S_STATUS_UPGRADE_HELP), FIRMWARE_NAME_PATTERNS);
#ifdef ZT30_WEB_NATIVE
    // Actual pending trials are shown by append_zt30_bundle_status().
#elif WEB_INSTALLS_FIRMWARE
    page.append(T(S_STATUS_UPGRADE_INSTALL_Z1));
#elif APCAM_TARGET == APCAM_TARGET_MT11
    page.append(T(S_STATUS_UPGRADE_SYNC_MT11));
#else
    page.appendf(T(S_STATUS_UPGRADE_SYNC_A8), FIRMWARE_INSTALL_NAME);
#endif
    page.append("</p>");
#endif
    webroot.render(page, "status-reboot.html", {csrf_token, T(S_STATUS_REBOOT_CONFIRM), T(S_STATUS_REBOOT_BUTTON)});
    page.appendf(T(S_STATUS_AUTH_NOTE), PASSWORD_PATH);
    page.append("</p></section></div></body></html>");
    *page_len = page.size();
    return page.release();
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
    APC_StringBuffer page;
    struct recent_photo_list photos;
    char title[96];

    find_recent_photos(&photos);
    page.reset();
    snprintf(title, sizeof(title), T(S_TITLE_SENSORS), PRODUCT_NAME);
    append_head(&page, title, page_style, NULL);
    page.appendf("<header id=top><h1>%s</h1><div class=muted>%s</div></header>",
               T(S_NAV_SENSORS),
               T(TARGET_TEXT(S_SENSORS_SUBTITLE_MT11, S_SENSORS_SUBTITLE_A8)));
    append_nav(&page, "/sensors", NULL);
    append_notice(&page, message, message_is_error);
    page.append("<div class=grid>");
#if WEB_HAVE_THERMAL
    webroot.render(page, "sensors-values.html", {T(S_SENSORS_LIVE), T(S_SENSORS_LIDAR), T(S_LOADING), T(S_ENABLE), T(S_SENSORS_MIN), T(S_LOADING), T(S_SENSORS_MAX), T(S_LOADING), T(S_SENSORS_CPU), T(S_LOADING), T(S_SENSORS_UPDATING), T(S_SENSORS_LASER_NOTICE)});
#endif
    webroot.render(page, "sensors-capture.html", {T(S_SENSORS_SHUTTER), T(S_SENSORS_SHUTTER_TEXT), csrf_token, T(S_SENSORS_CAPTURE)});
#if APCAM_HAVE_THERMAL_VIDEO
    page.appendf("<p class=muted>%s</p>", T(S_SENSORS_SCOPE_NOTE));
#endif
    page.appendf("</section></div><section class=card><h2>%s</h2>", T(S_SENSORS_RECENT));
    if (photos.count == 0U) {
        page.append("<p>");
        page.appendf(T(S_SENSORS_NO_PHOTOS), CAPTURE_ROOT);
        page.append("</p>");
    } else {
        page.append("<div class=gallery>");
        for (size_t index = 0; index < photos.count; index++) {
            const struct recent_photo *photo = &photos.items[index];
            char time_text[64];
            char size_text[32];
            const char *name = strrchr(photo->path, '/');
            format_file_time(photo->modified.tv_sec, time_text);
            format_bytes((uint64_t)photo->size, size_text);
            if (name == NULL) name = photo->path;
            else name++;
            page.append("<figure class=photo><a href=\"");
            append_path_query(&page, "/view?path=", photo->path);
            page.append("\"><img loading=lazy alt=\"");
            page.append_html(name);
            page.append("\" src=\"");
            append_path_query(&page, "/file?path=", photo->path);
            page.append("\"></a><figcaption><strong>");
            page.append_html(name);
            page.append("</strong><br><span class=muted>");
            page.append_html(time_text);
            page.append(" &middot; ");
            page.append_html(size_text);
            page.append("</span></figcaption></figure>");
        }
        page.append("</div>");
    }
    page.append("<p><a href=\"/files?path=");
    page.append_url(CAPTURE_ROOT);
    page.appendf("\">%s</a></p></section>", T(S_SENSORS_BROWSE));
#if WEB_HAVE_THERMAL
    page.appendf("<script src=/sensors.js data-csrf=\"%s\" defer></script>", csrf_token);
#endif
    page.append("</body></html>");
    *page_len = page.size();
    return page.release();
}

static char *render_live_page(size_t *page_len)
{
    APC_StringBuffer page;
    char title[96];

    page.reset();
    snprintf(title, sizeof(title), T(S_TITLE_LIVE), PRODUCT_NAME);
    append_head(&page, title, page_style, NULL);
    page.appendf("<header id=top><h1>%s</h1><div class=muted>%s</div></header>",
               T(S_LIVE_HEADING), T(S_LIVE_SUBTITLE));
    append_nav(&page, "/live", NULL);
    /* The second stream is only offered when the target has one. */
    char secondary[160] = "";
    if (APCAM_NUM_STREAMS >= 2)
        snprintf(secondary, sizeof(secondary), "<option value=1>%s</option>", T(S_LIVE_SECONDARY));
    webroot.render(page, "live-video.html", {T(S_LIVE_STREAM), T(S_LIVE_MAIN), secondary, T(S_LIVE_STARTING), T(S_LIVE_HELP), T(S_LIVE_PTZ), T(S_LIVE_ENABLE_MANUAL), T(S_LIVE_MANUAL_NOTICE)});
    webroot.render(page, "live-controls.html", {T(S_LIVE_CENTRE), T(S_LIVE_RATE), T(S_LIVE_ZOOM), APC_TemplateValue("%g", (double)APC_Camera::get_singleton().zoom_control_max()), T(S_LIVE_NO_COMMANDS), T(S_LIVE_ATTITUDE), T(S_LIVE_ATTITUDE_UNAVAILABLE), T(S_LIVE_YAW), T(S_LIVE_ROLL), T(S_LIVE_PITCH), T(S_LIVE_YAW), T(S_LIVE_ROLL_RATE), T(S_LIVE_PITCH_RATE), T(S_LIVE_YAW_RATE), T(S_LIVE_WAITING_GIMBAL), csrf_token});
#if !APCAM_HAVE_ZOOM
    page.append("<style>.zoom-row:has(#live-zoom){display:none}</style>");
#endif
#if !APCAM_HAVE_GIMBAL_RATES
    page.append("<style>.attitude-rates{display:none}</style>");
#endif
    *page_len = page.size();
    return page.release();
}

static char *render_users_page(const char *message, bool message_is_error,
                               size_t *page_len)
{
    APC_StringBuffer page;
    char title[96];
#if WEB_HAVE_SSH_KEYS
    size_t keys_len = 0;
    char *keys = read_authorized_keys(&keys_len);
    size_t key_count = 0;
    size_t unmanaged_count = 0;
#endif

    page.reset();
    snprintf(title, sizeof(title), T(S_TITLE_USERS), PRODUCT_NAME);
    append_head(&page, title, page_style, NULL);
    page.appendf("<header id=top><h1>%s</h1><div class=muted>%s</div></header>",
               T(S_USERS_HEADING),
               T(TARGET_TEXT(S_USERS_SUBTITLE_MT11, S_USERS_SUBTITLE_A8)));
    append_nav(&page, "/users", NULL);
    append_notice(&page, message, message_is_error);
    webroot.render(page, "users-password.html", {T(S_USERS_PASSWORD_HEADING), T(S_USERS_PASSWORD_TEXT), csrf_token, T(S_USERS_NEW_PASSWORD), T(S_USERS_CONFIRM_PASSWORD), T(S_USERS_CHANGE_BUTTON), T(S_USERS_HTTP_NOTICE)});
#if WEB_HAVE_SSH_KEYS
    webroot.render(page, "users-ssh.html", {T(S_USERS_ADD_KEYS), csrf_token, T(S_USERS_ADD_KEYS_BUTTON), T(S_USERS_ADD_KEYS_HELP), T(S_USERS_AUTHORIZED)});
    if (keys == NULL) {
        page.appendf("<p class=\"notice error\">%s</p>", T(S_USERS_KEYS_UNREADABLE));
    } else {
        page.appendf("<table><thead><tr><th>%s</th><th>%s</th><th>%s</th><th>%s</th>"
                          "</tr></thead><tbody>", T(S_USERS_KEY_TYPE), T(S_USERS_KEY),
                   T(S_USERS_KEY_COMMENT), T(S_USERS_KEY_ACTION));
        for (size_t offset = 0; offset < keys_len;) {
            size_t line_end = offset;
            struct public_key_view key;
            while (line_end < keys_len && keys[line_end] != '\n') line_end++;
            if (parse_public_key(keys + offset, line_end - offset, &key)) {
                size_t preview = key.blob_len < 24 ? key.blob_len : 24;
                key_count++;
                page.append("<tr><td><code>");
                page.append_html_n(key.type, key.type_len);
                page.append("</code></td><td><code>");
                page.append_html_n(key.blob, preview);
                if (preview < key.blob_len) page.append("&hellip;");
                page.append("</code></td><td>");
                if (key.comment_len > 0) page.append_html_n(key.comment, key.comment_len);
                else page.appendf("<span class=muted>%s</span>", T(S_USERS_KEY_NO_COMMENT));
                page.appendf("</td><td><form method=post action=/users/keys/remove>"
                                  "<input type=hidden name=csrf value=\"%s\">"
                                  "<input type=hidden name=public_key value=\"", csrf_token);
                page.append_html_n(key.type, key.type_len);
                page.append(" ");
                page.append_html_n(key.blob, key.blob_len);
                page.appendf("\"><label><input type=checkbox name=confirm value=yes required> "
                                  "%s</label><button class=danger type=submit>%s</button>"
                                  "</form></td></tr>", T(S_USERS_KEY_CONFIRM), T(S_USERS_KEY_REMOVE));
            } else {
                size_t start = offset;
                while (start < line_end && isspace((unsigned char)keys[start])) start++;
                if (start < line_end && keys[start] != '#') unmanaged_count++;
            }
            offset = line_end + (line_end < keys_len ? 1 : 0);
        }
        page.append("</tbody></table>");
        if (key_count == 0) page.appendf("<p>%s</p>", T(S_USERS_NO_KEYS));
        if (unmanaged_count == 1) {
            page.appendf("<p class=\"notice error\">%s</p>", T(S_USERS_UNMANAGED_ONE));
        } else if (unmanaged_count > 1) {
            page.append("<p class=\"notice error\">");
            page.appendf(T(S_USERS_UNMANAGED_MANY), unmanaged_count);
            page.append("</p>");
        }
    }
    page.appendf("<p class=notice>%s</p></section>", T(S_USERS_LAST_KEY_NOTICE));
    free(keys);
#else
    page.append("</div>");
#endif
    page.append("</body></html>");
    *page_len = page.size();
    return page.release();
}

static bool network_capture_status(char *message, size_t capacity, bool *failed)
{
    char path[PATH_MAX + sizeof(".capture")];
    snprintf(path, sizeof(path), "%s.capture", CAMERA_READY_PATH);
    size_t length;
    char *status = read_file(path, 512, &length);
    char *ready = read_file(CAMERA_READY_PATH, 4096, &length);
    const char *pid_line = ready ? strstr(ready, "\npid=") : NULL;
    const char *text = status ? strchr(status, '\n') : NULL;
    long pid, running;
    unsigned error;
    bool available = status && text && pid_line && sscanf(status, "%ld %u", &pid, &error) == 2 &&
        sscanf(pid_line, "\npid=%ld", &running) == 1 && pid > 1 && pid == running && kill((pid_t)pid, 0) == 0;
    *failed = available && error != 0;
    snprintf(message, capacity, "%s", available ? text + 1 : "");
    message[strcspn(message, "\r\n")] = 0;
    free(status);
    free(ready);
    return available;
}

static char *render_parameter_page(const char *message, bool message_is_error,
                                   const APC_HTTPRequest *submitted, size_t *page_len)
{
    APC_StringBuffer page;
    enum camera_kind kind = configuration_camera_kind();
    size_t config_len = 0;
    char *config;

    if (kind == CAMERA_NONE) kind = DEFAULT_CAMERA_KIND;
    config = read_file(config_path(kind), MAX_CONFIG, &config_len);

    if (config == NULL) config = strdup("");
    if (config == NULL) return NULL;

    page.reset();
    if (kind == CAMERA_REPLACEMENT) {
        append_head(&page, T(S_TITLE_APP_PARAMETERS), page_style, NULL);
        page.appendf("<header><h1>%s</h1><div class=muted>", T(S_TITLE_APP_PARAMETERS));
        page.append_html(T(S_APP_PARAMETERS_SUBTITLE));
        page.append("</div></header>");
        append_nav(&page, "/parameters", NULL);
        char applied[512];
        if (message == NULL && live_config_notice(config, config_len, applied, sizeof(applied), &message_is_error))
            message = applied;
        append_notice(&page, message, message_is_error);
#ifdef CAMERA_APP_ZT30_DEV
        unsigned native_width, native_height;
        ca_video_resolution_size(CA_VIDEO_720P, &native_width, &native_height);
        page.appendf("<p class=notice>%s</p><p class=help>", T(S_ZT30_NATIVE_PARAMETERS));
        page.appendf(T(S_ZT30_FIXED_VIDEO), native_width, native_height,
                     APCAM_THERMAL_STREAM_WIDTH, APCAM_THERMAL_STREAM_HEIGHT);
        page.append("</p>");
#endif
        webroot.render(page, "parameters-form.html", {T(S_APP_PARAMETERS_NOTICE), csrf_token, T(S_PARAMS_CATEGORIES)});
        for (unsigned tab = 0; tab < TAB_COUNT; tab++) {
            page.appendf("<button type=button role=tab id=tab-%s aria-controls=parameters-%s "
                             "aria-selected=false tabindex=-1>%s</button>",
                       parameter_tabs[tab].name, parameter_tabs[tab].name, T(parameter_tabs[tab].label));
        }
        page.append("</div>");
        for (unsigned tab = 0; tab < TAB_COUNT; tab++) {
            page.appendf("<section class=\"card parameter-panel\" id=parameters-%s "
                             "aria-labelledby=tab-%s><h2>%s</h2><div class=fields>",
                       parameter_tabs[tab].name, parameter_tabs[tab].name, T(parameter_tabs[tab].label));
            for (size_t i = 0; i < sizeof(replacement_parameters) / sizeof(replacement_parameters[0]); i++) {
                const struct parameter *parameter = &replacement_parameters[i];
                if (parameter_shown(parameter) && parameter_tab(parameter) == tab)
                    append_parameter_field(&page, config, parameter, submitted);
            }
            page.append("</div>");
#if !APCAM_HAVE_EXTERNAL_UART && APCAM_HAVE_SIYI
            if (tab == TAB_SYSTEM) {
                page.append("<p class=help>");
                page.append_html(T(S_H_UART_MCU));
                page.append("</p>");
            }
#endif
            if (tab == TAB_SURVEY) {
                page.append("<p>Survey tracks fixed ground points using the selected pattern. "
                    "Save settings before starting. Uses uploaded terrain first, with flight-controller terrain as fallback. Requires gimbal ownership.</p>"
                    "<p><button type=submit formnovalidate formaction=/survey name=survey_mode value=2>Start / resume survey</button> "
                    "<button type=submit formnovalidate formaction=/survey name=survey_mode value=0>Stop survey</button></p>"
                    "<pre id=survey-status aria-live=polite>Waiting for status</pre>"
                    "<p><label>Terrain ZIP from terrain.ardupilot.org <input id=terrain-file type=file accept=.zip></label> "
                    "<button id=terrain-upload type=button>Upload terrain</button></p>"
                    "<p>Stored on the SD card. Upload replaces the installed terrain area and takes effect automatically. "
                    "Maximum ZIP size: 512 MiB. Keep the browser open until validation finishes.</p>"
                    "<output id=terrain-upload-status aria-live=polite></output>"
                    "<p id=survey-pattern-description></p><p>Flight direction &#8593;</p><table id=survey-pattern-grid><tr><td>1 Front-left</td><td>2 Front-centre</td><td>3 Front-right</td></tr>"
                    "<tr><td>6 Down-left</td><td>5 Down-centre</td><td>4 Down-right</td></tr>"
                    "<tr><td>7 Rear-left</td><td>8 Rear-centre</td><td>9 Rear-right</td></tr></table>"
                    "<p>Overlap is a requested target. Slew and burst time may reduce achieved overlap. "
                    "Negative nominal overlap indicates gaps; check actual captured footprints in MAVProxy.</p>");
            }
            if (tab == TAB_NETWORK) {
                char capture_message[512];
                bool failed;
                bool available = network_capture_status(capture_message, sizeof(capture_message), &failed);
                page.appendf("<p class=\"notice%s\" id=network-capture-status%s>",
                             failed ? " error" : "", available ? "" : " hidden");
                page.append_html(T(S_P_NETWORK_CAPTURE));
                page.append(": <span>");
                page.append_html(capture_message);
                page.append("</span></p>");
                page.append("<p><a id=network-capture-files href=\"/files?path=");
                page.append_url(MEDIA_ROOT);
                page.append_url("/DCIM/network");
                page.append("\">");
                page.append_html(T(S_NETWORK_CAPTURE_FILES));
                page.append("</a></p>");
#ifdef MT11_WEB_SITL
                page.appendf("<p class=help>%s</p><p id=network-reconnect data-sitl=true hidden>", T(S_NETWORK_SITL));
#else
                page.append("<p class=help id=network-reconnect hidden>");
#endif
                page.appendf("%s <a id=network-link></a></p>", T(S_NETWORK_RECONNECT));
            }
            page.append("</section>");
        }
        webroot.render(page, "parameters-actions.html", {T(S_PARAMS_SAVE_ALL), T(S_PARAMS_SAVE), T(S_PARAMS_SAVE_RESTART)});
        free(config);
        *page_len = page.size();
        return page.release();
    }
    return NULL;
}

static char *render_raw_page(const char *message, bool message_is_error, size_t *page_len)
{
    APC_StringBuffer page;
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
    page.reset();
    snprintf(title, sizeof(title), T(S_TITLE_RAW), PRODUCT_NAME);
    append_head(&page, title, page_style, NULL);
    page.append("<header><h1>");
    page.appendf(T(S_RAW_HEADING),
               kind == CAMERA_REPLACEMENT ? "camera.ini" : "config.ini");
    page.append("</h1><div class=muted>");
    page.appendf(T(S_RAW_SUBTITLE), camera_label(kind));
    page.append("</div></header>");
    append_nav(&page, "/raw", NULL);
    append_notice(&page, message, message_is_error);
    page.appendf("<section class=card><form method=post action=/config>"
                      "<input type=hidden name=csrf value=\"%s\">"
                      "<textarea name=config spellcheck=false>", csrf_token);
    page.append_html(config);
    webroot.render(page, "raw-actions.html", {T(S_RAW_SAVE), T(S_PARAMS_SAVE_RESTART)});
    page.appendf(T(S_RAW_HELP), path, backup);
    page.append("</p></section></body></html>");
    free(config);
    *page_len = page.size();
    return page.release();
}

static const char *files_script = "files.js";

static char *render_files_page(const char *requested_path, const char *message,
                               bool message_is_error, size_t *page_len,
                               char *error, size_t error_size)
{
    APC_StringBuffer page;
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
    page.reset();
    snprintf(title, sizeof(title), T(S_TITLE_FILES), PRODUCT_NAME);
    append_head(&page, title, page_style, NULL);
    page.appendf("<header><h1>%s</h1><div class=muted>", T(S_FILES_HEADING));
    page.appendf(T(S_FILES_SUBTITLE), MEDIA_ROOT);
    page.append("</div></header>");
    append_nav(&page, "/files", resolved);
    append_notice(&page, message, message_is_error);
    page.appendf("<section class=card><form class=pathbox method=get action=/files>"
                      "<input aria-label=\"%s\" required type=text name=path value=\"",
               T(S_FILES_PATH));
    page.append_html(resolved);
    page.appendf("\"><button type=submit>%s</button></form>"
                      "<p><a href=\"/files?path=%%2F\">/</a> &middot; "
                      "<a href=\"/files?path=", T(S_FILES_OPEN));
    page.append_url(APP_DIR);
    page.append("\">");
    page.append_html(APP_DIR);
    page.append("</a> &middot; <a href=\"/files?path=");
    page.append_url(MEDIA_ROOT);
    page.append("\">");
    page.append_html(MEDIA_ROOT);
    page.append("</a></p></section>"
                    "<section class=card><h2>");
    page.append_html(resolved);
    webroot.render(page, "files-controls.html", {});
    const unsigned labels[] = {S_FILES_NAME, S_FILES_TYPE, S_FILES_SIZE,
                                   S_FILES_MODIFIED, S_FILES_MODE};
    for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        page.appendf("<th scope=col aria-sort=none><button type=button class=file-sort "
                          "data-column=%u>%s</button></th>", i, T((string_id)(labels[i])));
    }
    page.appendf("<th scope=col>%s</th></tr></thead><tbody>", T(S_FILES_ACTIONS));
    if (strcmp(resolved, "/") != 0) {
        page.append("<tr data-parent=1><td><a href=\"");
        append_path_query(&page, "/files?path=", parent);
        page.appendf("\">../</a></td><td>%s</td><td>-</td><td>-</td><td>-</td><td></td></tr>",
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
        page.appendf("<tr data-directory=%u><td><a href=\"", target_is_dir ? 1U : 0U);
        append_path_query(&page, route, child);
        page.append("\">");
        page.append_html(entry->d_name);
        if (target_is_dir) page.append("/");
        page.appendf("</a></td><td>%c%s</td><td data-sort=\"%lld\">%s</td>"
                          "<td data-sort=\"%lld\">%s</td><td data-sort=%u>%04o</td><td class=file-actions>",
                   file_type_char(lst.st_mode), S_ISLNK(lst.st_mode) ? T(S_FILES_LINK) : "",
                   S_ISREG(lst.st_mode) ? (long long)lst.st_size : -1LL, size_text,
                   (long long)lst.st_mtime, time_text,
                   (unsigned)(lst.st_mode & 07777), (unsigned)(lst.st_mode & 07777));
        if (S_ISREG(target.st_mode)) {
            page.append("<a href=\"");
            append_path_query(&page, "/file?path=", child);
            page.appendf("&amp;download=1\">%s</a>", T(S_FILES_DOWNLOAD));
        }
        if (path_can_be_deleted(child)) {
            page.append(" &middot; <a class=delete-link href=\"");
            append_path_query(&page, "/delete-confirm?path=", child);
            page.appendf("\">%s</a>", T(S_FILES_DELETE));
        }
        page.append("</td></tr>");
        shown++;
    }
    closedir(directory);
    page.append("</tbody></table>");
    if (shown == 5000) page.appendf("<p class=notice>%s</p>", T(S_FILES_TRUNCATED));
    page.append("<p class=muted>");
    page.appendf(T(S_FILES_LEGEND), MEDIA_ROOT);
    page.append("</p></section><script src=/files.js defer></script></body></html>");
    *page_len = page.size();
    return page.release();
}

static char *render_view_page(const char *requested_path, size_t *page_len,
                              char *error, size_t error_size)
{
    APC_StringBuffer page;
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
    page.reset();
    snprintf(title, sizeof(title), T(S_TITLE_VIEWER), PRODUCT_NAME);
    append_head(&page, title, page_style, NULL);
    page.appendf("<header><h1>%s</h1><div class=muted>", T(S_VIEWER_HEADING));
    page.append_html(resolved);
    page.append("</div></header>");
    append_nav(&page, "/view", resolved);
    page.append("<section class=card><p><a href=\"");
    append_path_query(&page, "/files?path=", parent);
    page.appendf("\">%s</a> &middot; <a href=\"", T(S_VIEWER_BACK));
    append_path_query(&page, "/file?path=", resolved);
    page.appendf("&amp;download=1\">%s</a>", T(S_FILES_DOWNLOAD));
    if (path_can_be_deleted(resolved)) {
        page.append(" &middot; <a class=delete-link href=\"");
        append_path_query(&page, "/delete-confirm?path=", resolved);
        page.appendf("\">%s</a>", T(S_FILES_DELETE));
    }
    page.appendf("</p><p class=muted>%s; %s</p>", mime, size_text);
    if (mime_is_image(mime)) {
        page.appendf("<img class=preview alt=\"%s\" src=\"", T(S_VIEWER_IMAGE_ALT));
        append_path_query(&page, "/file?path=", resolved);
        page.append("\">");
    } else if (strcmp(mime, "video/mp2t") == 0) {
        page.append("<div id=recording-player data-url=\"");
        append_path_query(&page, "/file?path=", resolved);
        page.appendf("\" data-loading=\"%s\" data-unavailable=\"%s\" data-error=\"%s\" data-seek-error=\"%s\">",
                     T(S_TS_LOADING), T(S_TS_UNAVAILABLE), T(S_TS_ERROR), T(S_TS_SEEK_ERROR));
        page.appendf("<video id=recording-video class=preview playsinline preload=none></video>"
                     "<div id=recording-controls><button id=recording-play type=button disabled data-play=\"%s\" data-pause=\"%s\">%s</button> "
                     "<span id=recording-time>00:00 / --:--</span> "
                     "<button id=recording-fullscreen type=button>%s</button>"
                     "<p><input id=recording-seek type=range min=0 max=0 step=0.1 value=0 disabled aria-label=\"%s\"></p></div>"
                     "<p id=recording-status role=status>%s</p></div>"
                     "<script src=/mpegts.js defer></script><script src=/recording.js defer></script>",
                     T(S_TS_PLAY), T(S_TS_PAUSE), T(S_TS_PLAY), T(S_TS_FULLSCREEN), T(S_TS_POSITION), T(S_TS_LOADING));
    } else if (mime_is_video(mime)) {
        page.append("<video class=preview controls preload=metadata src=\"");
        append_path_query(&page, "/file?path=", resolved);
        page.appendf("\">%s</video>", T(S_VIEWER_NO_VIDEO));
    } else {
        page.appendf("<p>%s</p>", T(S_VIEWER_NO_PREVIEW));
    }
    page.append("</section></body></html>");
    *page_len = page.size();
    return page.release();
}

static char *render_delete_page(const char *requested_path, size_t *page_len,
                                char *error, size_t error_size)
{
    APC_StringBuffer page;
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
    page.reset();
    append_head(&page, T(S_TITLE_DELETE), page_style, NULL);
    page.appendf("<header><h1>%s</h1></header>", T(S_TITLE_DELETE));
    append_nav(&page, "/delete-confirm", resolved);
    page.appendf("<section class=card><p class=\"notice error\">%s", T(S_DELETE_TEXT));
    page.append_html(resolved);
    if (S_ISDIR(st.st_mode)) page.append(T(S_DELETE_RECURSIVE));
    page.appendf("%s</p><form method=post action=/delete>"
                      "<input type=hidden name=csrf value=\"%s\">"
                      "<input type=hidden name=path value=\"", T(S_DELETE_NO_UNDO), csrf_token);
    page.append_html(resolved);
    webroot.render(page, "delete-confirmation.html", {T(S_DELETE_CONFIRM), T(S_DELETE_BUTTON)});
    *page_len = page.size();
    return page.release();
}

static const char *log_page_style = "log.css";

/* the captured application output itself is shown as-is in every language */
static char *render_log_page(size_t *page_len)
{
    APC_StringBuffer page;
    size_t log_len = 0;
    char *log = read_current_app_log(&log_len);
    char title[96];

    if (log == NULL) return NULL;
    page.reset();
    snprintf(title, sizeof(title), T(S_TITLE_DEBUG), PRODUCT_NAME);
    append_head(&page, title, log_page_style, "<meta http-equiv=refresh content=3>");
    page.appendf("<header id=top><h1>%s</h1><div class=muted>%s</div></header>",
               T(S_DEBUG_HEADING), T(S_DEBUG_SUBTITLE));
    append_nav(&page, "/log", NULL);
    page.appendf("<p><a href=\"/log.txt\">%s</a></p><pre>", T(S_DEBUG_PLAIN_TEXT));
    if (log_len == 0) {
        page.append(T(TARGET_TEXT(S_DEBUG_EMPTY_MT11, S_DEBUG_EMPTY_A8)));
    } else {
        page.append_log_html(log, log_len);
    }
    page.append("</pre><p class=muted>");
    page.appendf(T(S_DEBUG_FOOTER), log_len, APP_LOG_ROTATE_SIZE / 1024U);
    page.append("</p></body></html>");
    free(log);
    *page_len = page.size();
    return page.release();
}

// Rendering can fail because assets are unavailable, not only for allocation.
static bool send_missing_webroot_error(int fd)
{
    if (webroot.available()) return false;
    send_text_error(fd, 503, "Service Unavailable",
                    "Web interface assets unavailable. Reinstall the complete webroot or firmware package.\n", nullptr);
    return true;
}

static char *render_login_page(const char *message, bool message_is_error,
                               size_t *page_len)
{
#if APCAM_TARGET == APCAM_TARGET_Z1_MINI
    if (!webroot.available()) return APC_RecoveryPage::render(true, login_token, message, page_len);
#endif
    APC_StringBuffer page;
    char title[96];

    page.reset();
    snprintf(title, sizeof(title), T(S_TITLE_LOGIN), PRODUCT_NAME);
    append_head(&page, title, page_style, NULL);
    page.append("<header id=top><h1>");
    page.append_html(title);
    page.appendf("</h1><div class=muted>%s</div></header>", T(S_LOGIN_SUBTITLE));
    append_logo(&page, "/");
    append_notice(&page, message, message_is_error);
    webroot.render(page, "login-form.html", {login_token, T(S_LOGIN_USERNAME), T(S_LOGIN_PASSWORD), T(S_LANGUAGE)});
    append_language_select(&page, "lang-login");
    page.appendf("</div><button type=submit>%s</button></form><p class=help>", T(S_LOGIN_BUTTON));
    page.appendf(T(S_LOGIN_FIRST_TIME), PASSWORD_PATH);
    page.appendf("</p><p class=muted>%s</p></section>"
                      "<script src=/language.js defer></script></body></html>",
               T(S_LOGIN_SCRIPTED));
    *page_len = page.size();
    return page.release();
}

static void send_login_page(int fd, int status, const char *message, bool is_error,
                            const char *extra_headers)
{
    size_t length;
    char *page = render_login_page(message, is_error, &length);

    if (page == NULL) {
        if (send_missing_webroot_error(fd)) return;
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
static bool same_origin(const APC_HTTPRequest *request)
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

static void handle_login(int fd, const APC_HTTPRequest *request, const char *peer)
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

static void select_language(const APC_HTTPRequest *request)
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
static bool wants_html(const APC_HTTPRequest *request)
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
        if (send_missing_webroot_error(fd)) return;
        send_text_errorf(fd, 500, "Internal Server Error", S_OUT_OF_MEMORY);
        return;
    }
    send_response(fd, is_error ? 400 : 200, is_error ? "Bad Request" : "OK",
                  "text/html; charset=utf-8", page, length, NULL);
    free(page);
}

static void send_parameter_page(int fd, const char *message, bool is_error,
                                const APC_HTTPRequest *submitted)
{
    size_t length;
    char *page = render_parameter_page(message, is_error, submitted, &length);
    if (page == NULL) {
        if (send_missing_webroot_error(fd)) return;
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
        if (send_missing_webroot_error(fd)) return;
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
        if (send_missing_webroot_error(fd)) return;
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
        if (send_missing_webroot_error(fd)) return;
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
        if (send_missing_webroot_error(fd)) return;
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
    APC_StringBuffer json;
    int cpu_millidegrees = 0;
#if WEB_HAVE_SOC_TEMPERATURE
    bool have_cpu = read_soc_temperature(&cpu_millidegrees);
#else
    bool have_cpu = false;
#endif

    read_camera_sensors(&sensors);
    json.reset();
    json.append("{");
    if (sensors.have_lidar) {
        json.appendf("\"lidar_m\":%.1f",
                   sensors.lidar_decimetres / 10.0);
    } else {
        json.append("\"lidar_m\":null");
    }
    if (sensors.have_lidar) {
        json.append(",\"lidar_enabled\":true");
    } else if (sensors.have_thermal) {
        /* This MT11 firmware stops replying to 0x15 when the laser is off. A
         * simultaneous thermal reply proves that the camera API itself is
         * available, allowing the two cases to be distinguished. */
        json.append(",\"lidar_enabled\":false");
    } else {
        json.append(",\"lidar_enabled\":null");
    }
    if (sensors.have_thermal) {
        json.appendf(",\"minimum_c\":%.2f,\"minimum_x\":%u,\"minimum_y\":%u"
                   ",\"maximum_c\":%.2f,\"maximum_x\":%u,\"maximum_y\":%u",
                   sensors.minimum_centi_c / 100.0, sensors.minimum_x,
                   sensors.minimum_y, sensors.maximum_centi_c / 100.0,
                   sensors.maximum_x, sensors.maximum_y);
    } else {
        json.append(",\"minimum_c\":null,\"minimum_x\":null,\"minimum_y\":null"
                  ",\"maximum_c\":null,\"maximum_x\":null,\"maximum_y\":null");
    }
    if (have_cpu) {
        json.appendf(",\"cpu_c\":%.1f", cpu_millidegrees / 1000.0);
    } else {
        json.append(",\"cpu_c\":null");
    }
    json.append("}\n");
    send_response(fd, 200, "OK", "application/json; charset=utf-8",
                  json.data(), json.size(), "Cache-Control: no-store\r\n");
    json.reset();
}
#endif

static void send_live_attitude_json(int fd)
{
    struct camera_attitude_values attitude;
    APC_StringBuffer json;

    read_camera_attitude(&attitude);
    json.reset();
    if (attitude.have_attitude) {
#if APCAM_TARGET == APCAM_TARGET_Z1_MINI
        json.appendf("{\"yaw_deg\":%.1f,\"pitch_deg\":%.1f,\"roll_deg\":%.1f,"
                   "\"yaw_rate_dps\":null,\"pitch_rate_dps\":null,\"roll_rate_dps\":null}\n",
                   attitude.yaw_tenths / 10.0, attitude.pitch_tenths / 10.0,
                   attitude.roll_tenths / 10.0);
#else
        json.appendf("{\"yaw_deg\":%.1f,\"pitch_deg\":%.1f,\"roll_deg\":%.1f,"
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
        json.append("{\"yaw_deg\":null,\"pitch_deg\":null,\"roll_deg\":null,"
                  "\"yaw_rate_dps\":null,\"pitch_rate_dps\":null,"
                  "\"roll_rate_dps\":null}\n");
    }
    send_response(fd, 200, "OK", "application/json; charset=utf-8",
                  json.data(), json.size(), "Cache-Control: no-store\r\n");
    json.reset();
}

/* JavaScript is served per request with its user-visible strings in a
 * leading "const L = {...}" object */
static bool sb_append_js(APC_StringBuffer *sb, const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        char escaped[8];
        if (*p == '\\' || *p == '\'') {
            escaped[0] = '\\';
            escaped[1] = (char)*p;
            if (!sb->append_n(escaped, 2)) return false;
        } else if (*p < 0x20 || *p == '<' || *p == '>' || *p == '&') {
            snprintf(escaped, sizeof(escaped), "\\x%02x", *p);
            if (!sb->append(escaped)) return false;
        } else if (!sb->append_n((const char *)p, 1)) {
            return false;
        }
    }
    return true;
}

static void send_network_capture_status(int fd)
{
    char message[512];
    bool failed;
    bool available = network_capture_status(message, sizeof(message), &failed);
    APC_StringBuffer json;
    json.appendf("{\"available\":%s,\"error\":%s,\"message\":\"", available ? "true" : "false", failed ? "true" : "false");
    for (const unsigned char *p = (const unsigned char *)message; *p; p++) {
        if (*p == '"' || *p == '\\') json.appendf("\\%c", *p);
        else if (*p < 32) json.appendf("\\u%04x", *p);
        else json.append_n((const char *)p, 1);
    }
    json.append("\"}\n");
    send_response(fd, 200, "OK", "application/json; charset=utf-8", json.data(), json.size(), "Cache-Control: no-store\r\n");
}

struct js_string {
    const char *key;
    const char *text;
};

static void send_asset(int fd, const char *name, const char *mime)
{
    APC_StringBuffer body;
    if (!webroot.append(body, name)) {
        send_text_error(fd, 503, "Service Unavailable", "Web asset unavailable\n", NULL);
        return;
    }
    send_response(fd, 200, "OK", mime, body.data(), body.size(), NULL);
}

static void send_script(int fd, const struct js_string *items, size_t count,
                        const char *body)
{
    APC_StringBuffer script;

    script.reset();
    script.append("const L = {");
    for (size_t i = 0; i < count; i++) {
        script.appendf("%s%s:'", i != 0 ? "," : "", items[i].key);
        sb_append_js(&script, items[i].text);
        script.append("'");
    }
    script.append("};\n");
#if APCAM_TARGET == APCAM_TARGET_Z1_MINI
    if (strcmp(body, "upgrade.js") == 0 && !webroot.available()) {
        if (!script.append(apcam_recovery_upgrade_js)) {
            send_text_errorf(fd, 500, "Internal Server Error", S_OUT_OF_MEMORY);
            return;
        }
    } else
#endif
    if (!webroot.append(script, body)) {
        send_text_error(fd, 503, "Service Unavailable", "Web asset unavailable\n", NULL);
        return;
    }
    send_response(fd, 200, "OK", "application/javascript; charset=utf-8",
                  script.data(), script.size(), NULL);
    script.reset();
}

static const char *parameters_script = "parameters.js";

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
static const char *sensors_script = "sensors.js";

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

static const char *live_script = "live.js";

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

static const char *status_script = "status.js";

/* the nav and login language selectors apply on change; the fallback
 * button is hidden once the script runs */
static const char *language_script = "language.js";

static const char *upgrade_script = "upgrade.js";

#ifdef ZT30_WEB_NATIVE
#define UPGRADE_CONFIRM_STRING S_JS_FW_CONFIRM_APP
#define UPGRADE_UPLOADED_STRING S_APP_UPDATE_PENDING
#define UPGRADE_WRITE_ROOT RUNTIME_DIR
#elif WEB_INSTALLS_FIRMWARE
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
#ifdef ZT30_WEB_NATIVE
        {"applicationUpdate", "true"}, {"maxSize", "33554432"}, {"appFailed", T(S_APP_UPDATE_FAILED)},
        {"confirmRollback", T(S_APP_ROLLBACK)},
#endif
        {"mediaRoot", UPGRADE_WRITE_ROOT}, {"reboots", WEB_UPGRADE_REBOOTS_JS},
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
static const char *users_script = "users.js";

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

[[maybe_unused]] static void sync_firmware_storage(void)
{
#ifndef MT11_WEB_TEST
    sync();
#endif
}

#if APCAM_TARGET == APCAM_TARGET_ZR10
#include "zr10_firmware.h"
#endif

/* Stream the request body to output; errno describes a failure. */
static bool receive_upload_body(int fd, const APC_HTTPRequest *request, int output,
                                size_t *received, unsigned duration_ms = 0)
{
    char expect[64];
#ifdef ZT30_WEB_NATIVE
    const int64_t deadline = duration_ms ? apcam::probe_milliseconds()+duration_ms : 0;
#else
    (void)duration_ms;
#endif
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
#ifdef ZT30_WEB_NATIVE
        if (deadline) {
            const int64_t left = deadline-apcam::probe_milliseconds();
            pollfd wait{fd, POLLIN, 0};
            if (left <= 0 || poll(&wait, 1, int(left)) <= 0) {errno = ETIMEDOUT; return false;}
        }
#endif
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

#include "APC_TerrainUpload.h"

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
static bool remove_path_tree(const char *path);

/* Atomically swap two paths; unsupported filesystems fail safely. */
static int exchange_paths(const char *a, const char *b)
{
#if defined(__CYGWIN__) || defined(__APPLE__)
#ifdef WEB_PORTABLE_SITL
    /* The simulator's Cygwin filesystem has no renameat2 exchange. Keep the
     * old tree recoverable while swapping this isolated temporary runtime. */
    char backup[PATH_MAX];
    if (snprintf(backup, sizeof(backup), "%s.exchange", b) >= (int)sizeof(backup)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (access(backup, F_OK) == 0) {
        struct stat current;
        if (lstat(b, &current) < 0) {
            if (errno != ENOENT || rename(backup, b) < 0) return -1;
        } else if (!remove_path_tree(backup)) {
            return -1;
        }
    } else if (errno != ENOENT) return -1;
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
    if (memchr(name, 0, length)) return false;
    static const char *const allowed[] = {
        "gcu/ap/SHA256SUMS", "gcu/ap/manifest.json", "gcu/ap/service.sh",
        "gcu/ap/camera-app", "gcu/ap/z1mini-web", "gcu/ap/ax-capture",
        "gcu/ap/camera.ini.default", "gcu/ap/web.pass.default", "gcu/ap/README.md",
        "gcu/ipc/run.sh", "gcu/ipc/camera_gcu.sh",
    };

    static const char prefix[] = "gcu/ap/webroot/";
    if (length > sizeof(prefix) - 1 && memcmp(name, prefix, sizeof(prefix) - 1) == 0) {
        char asset[128];
        const size_t count = length - (sizeof(prefix) - 1);
        if (count >= sizeof(asset)) return false;
        memcpy(asset, name + sizeof(prefix) - 1, count);
        asset[count] = 0;
        return APC_WebRoot::known_asset(asset);
    }
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strlen(allowed[i]) == length && memcmp(name, allowed[i], length) == 0) {
            return true;
        }
    }
    return false;
}

#include "gcu_manifest.h"

static bool gcu_zip_extra_ok(const unsigned char *extra, size_t length)
{
    while (length) {
        if (length<4) return false;
        unsigned kind=zip_le16(extra), size=zip_le16(extra+2);
        if (kind==1 || size>length-4) return false; /* ZIP64 */
        extra+=4+size; length-=4+size;
    }
    return true;
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
    directory = (unsigned char*)(malloc(directory_size));
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
        uint32_t local_offset=zip_le32(header+42);
        unsigned char local[30], local_extra[1024];
        if (zip_le32(header+20)==UINT32_MAX || uncompressed==UINT32_MAX ||
            local_offset==UINT32_MAX || !gcu_zip_extra_ok(header+46+name_length, extra_length)) {
            problem="ZIP64 or invalid extra fields";
            goto fail;
        }
        if ((uint64_t)local_offset+sizeof(local)>directory_offset ||
            pread(fd,local,sizeof(local),local_offset)!=(ssize_t)sizeof(local) ||
            zip_le32(local)!=0x04034b50) {
            problem="invalid local file header";
            goto fail;
        }
        unsigned local_name=zip_le16(local+26), local_length=zip_le16(local+28);
        if (zip_le32(local+18)==UINT32_MAX || zip_le32(local+22)==UINT32_MAX ||
            local_length>sizeof(local_extra) ||
            (uint64_t)local_offset+30+local_name+local_length>directory_offset ||
            pread(fd,local_extra,local_length,(off_t)local_offset+30+local_name)!=(ssize_t)local_length ||
            !gcu_zip_extra_ok(local_extra,local_length)) {
            problem="ZIP64 or invalid local extra fields";
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
static int run_tool(const char *directory, const char *const argv[], int client,
                    rlim_t max_file_size)
{
    struct sigaction previous;
    struct sigaction reap {};
    reap.sa_handler = SIG_DFL;
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
        execvp(argv[0], const_cast<char *const *>(argv));
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
        const char *const unzip_argv[] = {"unzip", "-o", "-q", (char *)package,
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
    const char *const sums_argv[] = {"sha256sum", "-c", "SHA256SUMS", NULL};
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
    if (manifest == NULL || !gcu_manifest_valid(manifest, length, &needs_isp)) {
        free(manifest);
        snprintf(error, error_size, "invalid " PRODUCT_NAME " package manifest");
        return 1;
    }
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

/* Remove uploads before replying, so a completed request has no temporary file. */
static void cleanup_gcu_upload(int *output, bool *created, const char *path)
{
    int saved=errno;
    if (*output>=0) { close(*output); *output=-1; }
    if (*created) { (void)unlink(path); *created=false; }
    errno=saved;
}

static void handle_firmware_install(int fd, const APC_HTTPRequest *request, const char *peer)
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
    {
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
        cleanup_gcu_upload(&output, &created, package);
        send_text_errorf(fd, 507, "Insufficient Storage", S_E_FW_TMP_SPACE, RUNTIME_DIR);
        goto done;
    }
    if (!receive_upload_body(fd, request, output, &received) || fsync(output) < 0) goto receive_failed;
    problem = check_gcu_package(output, received, &extracted, &package_index);
    if (problem != NULL) {
        log_message("firmware %s from %s rejected: %s", filename, peer, problem);
        cleanup_gcu_upload(&output, &created, package);
        send_text_errorf(fd, 400, "Bad Request", S_E_FW_PACKAGE, problem);
        goto done;
    }
    if (statvfs(GCU_ROOT, &space) == 0 &&
        (uint64_t)space.f_bavail * space.f_frsize < extracted + 1024U * 1024U) {
        cleanup_gcu_upload(&output, &created, package);
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
        cleanup_gcu_upload(&output, &created, package);
        if (result == 1) {
            send_text_errorf(fd, 400, "Bad Request", S_E_FW_PACKAGE, error);
        } else {
            send_text_errorf(fd, 500, "Internal Server Error", S_E_FW_INSTALL, error);
        }
        goto done;
    }
    log_message("firmware %s (%zu bytes) installed by %s; rebooting", filename, received, peer);
    cleanup_gcu_upload(&output, &created, package);
    send_text_errorf(fd, 201, "Created", S_FW_INSTALLED_Z1, filename);
    schedule_reboot();
    goto done;

receive_failed:
    log_message("firmware upload from %s failed after %zu bytes: %s", peer, received,
                strerror(errno));
    cleanup_gcu_upload(&output, &created, package);
    send_text_errorf(fd, 400, "Bad Request", S_E_FW_FAILED, received, strerror(errno));
    }
done:
    cleanup_gcu_upload(&output, &created, package);
    if (upgrade_lock >= 0) close(upgrade_lock);
}

#endif

#ifdef ZT30_WEB_NATIVE
#include "zt30-upgrade.h"
#endif

static void handle_firmware_upload(int fd, const APC_HTTPRequest *request,
                                   const char *peer)
{
#ifdef ZT30_WEB_NATIVE
    handle_zt30_upgrade(fd, request, peer);
#elif WEB_INSTALLS_FIRMWARE
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
    {
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
#if APCAM_TARGET == APCAM_TARGET_MT11 || !defined(FIRMWARE_INSTALL_NAME)
    send_text_errorf(fd, 201, "Created", S_FW_UPLOADED_MT11, filename);
#else
    send_text_errorf(fd, 201, "Created", S_FW_UPLOADED_A8, filename, FIRMWARE_INSTALL_NAME);
#endif
    goto done;

receive_failed:
    log_message("firmware upload from %s failed after %zu bytes: %s", peer,
                received, strerror(errno));
    send_text_errorf(fd, 400, "Bad Request", S_E_FW_FAILED, received, strerror(errno));
    }
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
        if (send_missing_webroot_error(fd)) return;
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
        if (send_missing_webroot_error(fd)) return;
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
        if (send_missing_webroot_error(fd)) return;
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
        if (send_missing_webroot_error(fd)) return;
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

static enum range_result parse_byte_range(const APC_HTTPRequest *request, uint64_t size,
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

static void send_file_response(int client, const APC_HTTPRequest *request,
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
    /* camera-app holds an exclusive lock while appending each MP4 frame or raw Matroska cluster.
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

static void send_file_response_async(int client, const APC_HTTPRequest *request,
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

static const char *reboot_script = "reboot.js";

static void send_rebooting_page(int fd)
{
    APC_StringBuffer body;
    char title[96];

    snprintf(title, sizeof(title), T(S_TITLE_REBOOTING), PRODUCT_NAME);
    body.reset();
    append_head(&body, title, page_style, NULL);
    body.appendf("<main class=card><h1 id=reboot-heading>%s</h1>"
                      "<p id=reboot-status role=status aria-live=polite data-wait=\"", T(S_REBOOTING_HEADING));
    body.append_html(T(S_REBOOTING_TEXT));
    body.append("\" data-back=\"");
    body.append_html(T(S_JS_FW_BACK));
    body.append("\" data-login=\"");
    body.append_html(T(S_JS_FW_BACK_LOGIN));
    body.append("\" data-timeout=\"");
    body.append_html(T(S_JS_FW_TIMEOUT));
    body.append("\">");
    body.append_html(T(S_REBOOTING_TEXT));
    body.appendf("</p><p><a id=reboot-home href=/ class=button>%s</a></p></main>"
                      "<script id=reboot-monitor src=/reboot.js data-csrf=\"%s\" defer></script></body></html>",
               T(S_REBOOT_HOME), csrf_token);
    if (!body.valid()) {
        if (!send_missing_webroot_error(fd))
            send_text_errorf(fd, 500, "Internal Server Error", S_OUT_OF_MEMORY);
        return;
    }
    send_response(fd, 200, "OK", "text/html; charset=utf-8", body.data(), body.size(), NULL);
    body.reset();
}

static void schedule_reboot(void)
{
#if defined(MT11_WEB_SITL) || defined(MT11_WEB_TEST)
    log_message("SITL reboot request ignored");
#else
    pid_t child = fork();
    if (child != 0) return;
    if (listen_fd >= 0) close(listen_fd);
    sleep(2);
    sync();
#ifdef REBOOT_COMMAND
    /* Let the init system stop services and unmount cleanly. */
    execl("/bin/sh", "sh", "-c", REBOOT_COMMAND, (char *)NULL);
#endif
    reboot(RB_AUTOBOOT);
    _exit(1);
#endif
}

/* camera-app applies [general] timezone to its own process only; use the
 * same zone here so page times match the camera clock and its recordings */
static void apply_configured_timezone(void)
{
    size_t length;
    char zone[128];
    char *config = read_file(REPLACEMENT_CONFIG_PATH, MAX_CONFIG, &length);

    snprintf(zone, sizeof(zone), "%s", APCAM_DEFAULT_TIMEZONE);
    if (config != NULL) ini_get_value(config, "general", "timezone", zone, sizeof(zone));
    if (zone[0] != '\0') {
        if (APC_Timezone::apply(zone) < 0) log_message("cannot apply timezone %s", zone);
    }
    free(config);
}

// A camera restarted by this web service uses TMP=RUNTIME_DIR. The web
// process itself may still have its launcher's original temporary directory.
static char *read_survey_status(size_t *length)
{
    const char *directories[] = {nullptr, RUNTIME_DIR};
    for (const char *directory : directories) {
        char path[4096];
        if (!apcam_survey_status_path(REPLACEMENT_CONFIG_PATH, path, sizeof(path), directory)) continue;
        struct stat st{};
        if (stat(path, &st) != 0 || time(NULL) - st.st_mtime > 5) continue;
        char *body = read_file(path, 4096, length);
        if (body) return body;
    }
    return nullptr;
}

static void handle_request(int fd, const char *peer)
{
    APC_HTTPRequest request = {};
    int receive_status = request.receive(fd);
    char session_token[65];
    bool login_route;
    int auth;

    apply_configured_timezone();

    /* Browsers commonly leave speculative connections idle.  Closing an
     * incomplete connection quietly avoids presenting an unsolicited 400 as
     * the result of an unrelated navigation on another connection. */
    if (receive_status == -1) return;
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
    if (strcmp(request.method, "GET") == 0 &&
        (strcmp(request.path, "/style.css") == 0 || strcmp(request.path, "/log.css") == 0)) {
        send_asset(fd, request.path + 1, "text/css; charset=utf-8");
        goto done;
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
    /* No credentials or tokens in this script. Serve it even if a restart
     * expired the session between loading the reboot page and its script. */
    if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/reboot.js") == 0) {
        send_asset(fd, reboot_script, "application/javascript; charset=utf-8");
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
            send_asset(fd, language_script, "application/javascript; charset=utf-8");
        } else if (strcmp(request.method, "GET") == 0 &&
                   (strcmp(request.path, "/upgrade-status") == 0 || strcmp(request.path, "/upgrade-result") == 0 ||
                    strcmp(request.path, "/network-capture-status") == 0)) {
            /* Reboot polling must reach its onload handler when the cookie
             * expires. A Basic challenge can instead block on a browser auth
             * dialog. No status/token is disclosed to this unauthenticated request. */
            send_text_error(fd, 401, "Unauthorized", T(S_AUTH_REQUIRED), NULL);
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
#ifdef ZT30_WEB_DEV
    /* This initial load has no replacement camera process. Only authentication
     * changes are writable, and their files live entirely under the RAM root.
     * SSH keys are installed by the host controller, never via the web login. */
    if (strcmp(request.method, "POST") == 0 && !login_route &&
        strcmp(request.path, "/logout") != 0 && strcmp(request.path, "/language") != 0 &&
        strcmp(request.path, "/users/password") != 0) {
        send_text_error(fd, 503, "Service Unavailable",
                        "Camera changes are unavailable in the initial ZT30 development load.", NULL);
        goto done;
    }
#endif
    if (login_route) {
        if (strcmp(request.method, "POST") == 0) handle_login(fd, &request, peer);
        else send_redirect(fd, "/");
    } else if (request.streaming_body) {
        if (!strcmp(request.path, "/survey/terrain")) handle_terrain_upload(fd, &request);
        else if (WEB_HAVE_FIRMWARE_UPLOAD) handle_firmware_upload(fd, &request, peer);
        else send_text_errorf(fd, 404, "Not Found", S_NOT_FOUND);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/") == 0) {
#if APCAM_TARGET == APCAM_TARGET_Z1_MINI
        if (!webroot.available()) {
            size_t length;
            char *page = APC_RecoveryPage::render(false, csrf_token, nullptr, &length);
            if (page) {
                send_response(fd, 200, "OK", "text/html; charset=utf-8", page, length, nullptr);
                free(page);
            } else send_text_errorf(fd, 500, "Internal Server Error", S_OUT_OF_MEMORY);
        } else
#endif
        send_page(fd, NULL, false);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/network-capture-status") == 0) {
        send_network_capture_status(fd);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/parameters") == 0) {
        send_parameter_page(fd, NULL, false, NULL);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/mpegts.js") == 0) {
        send_asset(fd, "mpegts.js", "application/javascript; charset=utf-8");
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/recording.js") == 0) {
        send_asset(fd, "recording.js", "application/javascript; charset=utf-8");
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/mpegts-license") == 0) {
        send_asset(fd, "mpegts-license.html", "text/html; charset=utf-8");
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/files.js") == 0) {
        send_asset(fd, files_script, "application/javascript; charset=utf-8");
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
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/live/video2.mp4") == 0 &&
               APCAM_NUM_STREAMS >= 2) {
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
        send_asset(fd, status_script, "application/javascript; charset=utf-8");
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/language.js") == 0) {
        send_asset(fd, language_script, "application/javascript; charset=utf-8");
#if WEB_HAVE_SSH_KEYS
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/users.js") == 0) {
        send_users_script(fd);
#endif
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/upgrade.js") == 0) {
        send_upgrade_script(fd);
#ifdef ZT30_WEB_NATIVE
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/upgrade-result") == 0) {
        send_zt30_upgrade_result(fd);
#endif
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
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/rebooting") == 0) {
        send_rebooting_page(fd);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/survey/status") == 0) {
        size_t length = 0;
        char *body = read_survey_status(&length);
        const char *empty="{\"state\":\"camera unavailable\"}";
        send_response(fd,200,"OK","application/json",body?body:empty,body?length:strlen(empty),"Cache-Control: no-store\r\n");
        free(body);
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
        } else if (strcmp(request.path, "/survey") == 0) {
            size_t length=0; char *mode=form_value(&request,"survey_mode",&length);
            if(!mode || length!=1 || (mode[0]!='0' && mode[0]!='2')) {
                send_response(fd,400,"Bad Request","text/plain","Invalid survey mode",19,NULL);
            } else {
                char path[sizeof(REPLACEMENT_CONFIG_PATH) + sizeof(".survey.command")];
                char tmp[sizeof(path) + sizeof(".tmp")];
                snprintf(path,sizeof(path),"%s.survey.command",REPLACEMENT_CONFIG_PATH);
                snprintf(tmp,sizeof(tmp),"%s.tmp",path);
                FILE *f=fopen(tmp,"w"); bool ok=false;
                if(f) { bool written=fprintf(f,"%c\n",mode[0])==2; ok=fclose(f)==0 && written; }
                if(ok && rename(tmp,path)==0) send_redirect_headers(fd,"/parameters#survey",NULL);
                else send_response(fd,500,"Error","text/plain","Could not request survey mode",29,NULL);
            }
            free(mode);
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
#ifdef ZT30_WEB_NATIVE
        } else if (strcmp(request.path, "/upgrade/rollback") == 0) {
            handle_zt30_upgrade(fd, &request, peer, true);
#endif
        } else if (strcmp(request.path, "/restart") == 0) {
            if (restart_camera(error, sizeof(error))) {
                log_message("camera application restarted by %s", peer);
                send_redirect(fd, "/");
            } else {
                log_message("camera restart failed for %s: %s", peer, error);
                send_page(fd, error, true);
            }
        } else if (strcmp(request.path, "/parameters") == 0) {
            struct ini_update updates[80] = {};
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
                log_message("camera reboot requested by %s", peer);
                /* redirect so a browser refresh re-fetches the notice
                 * instead of resubmitting the reboot form */
                send_redirect(fd, "/rebooting");
                schedule_reboot();
            }
        } else {
            send_text_errorf(fd, 404, "Not Found", S_UNKNOWN_POST);
        }
    } else {
        send_text_errorf(fd, 404, "Not Found", S_NOT_FOUND);
    }
done:
    // Request storage is released by its owner on every exit path.
    return;
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
    static char webroot_path[PATH_MAX];
#ifdef WEB_PORTABLE_SITL
    if (portable_paths_init() < 0) { fprintf(stderr, "Invalid portable SITL paths\n"); return 2; }
#endif
#ifndef WEBROOT_PATH
#define WEBROOT_PATH ""
#endif
    const char *assets = WEBROOT_PATH;
#ifdef ZT30_WEB_NATIVE
    // The listener keeps its own release's assets while another is staged.
    const char *slot_assets = getenv("CAMERA_APP_WEBROOT");
    if (slot_assets && *slot_assets) assets = slot_assets;
#endif
#ifdef WEB_PORTABLE_SITL
    assets = getenv("CAMERA_GIMBAL_SITL_WEBROOT");
#endif
    if (assets && assets[0]) webroot.set_path(assets);
    else {
        const size_t root_length = strlen(APP_DIR);
        if (root_length > sizeof(webroot_path) - sizeof("/webroot")) {
            fprintf(stderr, "Webroot path is too long\n");
            return 2;
        }
        memcpy(webroot_path, APP_DIR, root_length);
        memcpy(webroot_path + root_length, "/webroot", sizeof("/webroot"));
        webroot.set_path(webroot_path);
    }
    unsigned port = DEFAULT_PORT;

#if (APCAM_TARGET == APCAM_TARGET_MT11) || (defined(MT11_WEB_SITL) && !defined(WEB_SUPERVISED_TEST)) || defined(ZT30_WEB_NATIVE)
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
            if (translations[id][lang] == NULL || translations[id][lang][0] == '\0') {
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
