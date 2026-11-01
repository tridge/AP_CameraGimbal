#pragma once
#include <array>

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

enum string_id {
    S_ZT30_THERMAL_HFOV,
    S_ZT30_THERMAL_HFOV_HELP,
    S_ZT30_NATIVE_PARAMETERS,
    S_ZT30_FIXED_VIDEO,
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
#if APCAM_HAVE_PHOTO_RESOLUTION
    S_OPT_PHOTO_RES_VIDEO,
    S_OPT_PHOTO_RES_BINNED,
    S_OPT_PHOTO_RES_FULL,
#endif
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
    S_OPT_RES_540,
    S_OPT_RES_480,
    S_OPT_RES_360,
    S_OPT_RES_240,
    S_OPT_RES_180,
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
#if APCAM_HAVE_VIDEO_BITRATE
    S_P_MAIN_BITRATE,
    S_H_MAIN_BITRATE,
#endif
#if APCAM_HAVE_VIDEO_FRAME_RATE
    S_P_MAIN_FPS,
    S_H_MAIN_FPS,
#endif
#if APCAM_HAVE_PHOTO_RESOLUTION
    S_P_PHOTO_RESOLUTION,
    S_H_PHOTO_RESOLUTION,
#endif
    S_P_ORIENTATION,
    S_H_ORIENTATION,
    S_P_UART_PROTOCOL,
    S_H_UART_PROTOCOL_MT11,
    S_H_UART_PROTOCOL_A8,
    S_H_UART_MCU,
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
    S_P_OSD_CROSS, S_H_OSD_CROSS, S_P_OSD_RECORD, S_H_OSD_RECORD, S_P_OSD_THERMAL_FOV, S_H_OSD_THERMAL_FOV,
    S_P_TRACK_METHOD, S_H_TRACK_METHOD, S_OPT_TRACK_ANGLE, S_OPT_TRACK_RATE,
    S_RESTART_REQUIRED,
    S_P_POSITION_TARGETING,
    S_H_POSITION_TARGETING,
    S_P_THERMAL_PALETTE,
    S_H_THERMAL_PALETTE,
    S_PARAMS_SURVEY,
    S_SURV_PATTERN,
    S_SURV_PATTERN_HELP,
    S_SURV_BOTH,
    S_SURV_LEFT_RIGHT,
    S_SURV_FORE_AFT,
    S_SURV_FORE_ONLY,
    S_SURV_LENS,
    S_SURV_LENS_HELP,
    S_SURV_THERMAL,
    S_SURV_WIDE,
    S_SURV_ZOOM,
    S_SURV_OVERLAP,
    S_SURV_OVERLAP_HELP,
    S_SURV_FORE_PCT,
    S_SURV_FORE_PCT_HELP,
    S_SURV_AFT_PCT,
    S_SURV_AFT_PCT_HELP,
    S_SURV_BURST,
    S_SURV_BURST_HELP,
    S_SURV_BURST_MS,
    S_SURV_BURST_MS_HELP,
    S_SURV_DWELL_MS,
    S_SURV_DWELL_MS_HELP,
    S_SURV_SETTLE_MS,
    S_SURV_SETTLE_MS_HELP,
    S_SURV_ERROR_CD,
    S_SURV_ERROR_CD_HELP,
    S_SURV_RATE_CD,
    S_SURV_RATE_CD_HELP,
    S_SURV_TERRAIN_MS,
    S_SURV_TERRAIN_MS_HELP,
    S_SURV_TERRAIN_M,
    S_SURV_TERRAIN_M_HELP,
    S_SURV_TERRAIN_TOL,
    S_SURV_TERRAIN_TOL_HELP,
    S_SURV_MIN_SPEED,
    S_SURV_MIN_SPEED_HELP,
    S_SURV_TURN_DEG,
    S_SURV_TURN_DEG_HELP,
    S_P_RAW_STREAM_FPS,
    S_H_RAW_STREAM_FPS,
    S_P_RAW_RECORD_FPS,
    S_H_RAW_RECORD_FPS,
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
    S_P_VIDEO_TELEMETRY,
    S_H_VIDEO_TELEMETRY,
    S_OPT_META_APCG,
    S_OPT_META_MISB,
    S_P_SUB_RESOLUTION,
    S_H_SUB_RESOLUTION_MT11,
    S_H_SUB_RESOLUTION_A8,
    S_P_SUB_CODEC,
    S_H_SUB_CODEC,
    S_P_MAIN_ALIAS,
    S_H_MAIN_ALIAS,
    S_P_SUB_ALIAS,
    S_H_SUB_ALIAS,
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
    S_P_PROXY_VIDEO3_PORT,
    S_H_PROXY_VIDEO3_PORT,
    S_P_PROXY_VIDEO3_NAME,
    S_H_PROXY_VIDEO3_NAME,
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
    S_P_NETWORK_CAPTURE,
    S_H_NETWORK_CAPTURE,
    S_P_NETWORK_VIDEO1_URI,
    S_H_NETWORK_VIDEO1_URI,
    S_NETWORK_CAPTURE_FILES,
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
#if !WEB_MEDIA_IS_MOUNT
    S_STORAGE_RECORDINGS,
#endif
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
    S_REBOOT_HOME,
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
    S_TS_LOADING, S_TS_SEEK_ERROR, S_TS_UNAVAILABLE, S_TS_ERROR, S_TS_PLAY, S_TS_PAUSE, S_TS_FULLSCREEN, S_TS_POSITION,
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
    S_JS_FW_CONFIRM_APP,
    S_APP_UPDATE_PENDING,
    S_APP_UPDATE_FAILED,
    S_APP_ROLLBACK,
    S_BUNDLE_SELECTED,
    S_BUNDLE_CAMERA,
    S_BUNDLE_WEB,
    S_BUNDLE_STATE,
    S_COUNT
};

using Translations = std::array<std::array<const char *, LANG_COUNT>, S_COUNT>;
static constexpr Translations translations = [] {
    Translations result {};
    result[S_LANGUAGE_NAME] = {"English", "简体中文", "日本語"};
    result[S_LANGUAGE] = {"Language", "语言", "言語"};
    result[S_APPLY] = {"Apply", "应用", "適用"};
    result[S_NAV_TOP] = {"Go to the top of the %s control page", "回到 %s 控制页面顶部", "%s コントロールページの先頭へ"};
    result[S_NAV_STATUS] = {"Status", "状态", "ステータス"};
    result[S_NAV_PARAMETERS] = {"Parameters", "参数", "パラメータ"};
    result[S_NAV_RAW] = {"Raw config", "原始配置", "設定ファイル"};
    result[S_NAV_USERS] = {"Users", "用户", "ユーザー"};
    result[S_NAV_FILES] = {"Files", "文件", "ファイル"};
    result[S_NAV_LIVE] = {"Live", "实时画面", "ライブ"};
#if WEB_HAVE_THERMAL
    result[S_NAV_SENSORS] = {"Sensors", "传感器", "センサー"};
    result[S_TITLE_SENSORS] = {"%s sensors", "%s 传感器", "%s センサー"};
#else
    result[S_NAV_SENSORS] = {"Photos", "照片", "写真"};
    result[S_TITLE_SENSORS] = {"%s photos", "%s 照片", "%s 写真"};
#endif
    result[S_NAV_DEBUG] = {"Debug", "调试", "デバッグ"};
    result[S_NAV_LOGOUT] = {"Log out", "退出登录", "ログアウト"};
    result[S_UNAVAILABLE] = {"unavailable", "不可用", "取得不可"};
    result[S_OUT_OF_MEMORY] = {"Out of memory", "内存不足", "メモリ不足です"};
    result[S_NOT_FOUND] = {"Not found", "未找到", "見つかりません"};
    result[S_TITLE_LOGIN] = {"%s login", "%s 登录", "%s ログイン"};
    result[S_LOGIN_SUBTITLE] = {"Administrative interface", "管理界面", "管理インターフェース"};
    result[S_LOGIN_USERNAME] = {"Username", "用户名", "ユーザー名"};
    result[S_LOGIN_PASSWORD] = {"Password", "密码", "パスワード"};
    result[S_LOGIN_BUTTON] = {"Log in", "登录", "ログイン"};
    result[S_LOGIN_FIRST_TIME] = {"First time here? The username is <code>admin</code> and the password is <code>ardupilot</code> unless it has been changed on the Users page. The camera keeps the password in <code>%s</code>.", "首次使用？用户名为 <code>admin</code>，密码为 <code>ardupilot</code>（除非已在“用户”页面修改）。相机将密码保存在 <code>%s</code> 中。", "初めてお使いですか？ユーザー名は <code>admin</code>、パスワードは「ユーザー」ページで変更していない限り <code>ardupilot</code> です。パスワードはカメラ内の <code>%s</code> に保存されています。"};
    result[S_LOGIN_SCRIPTED] = {"Scripted clients can keep using HTTP Basic authentication with the same credentials. This service is HTTP, not HTTPS; keep it on the isolated camera network.", "脚本客户端可继续使用相同凭据进行 HTTP Basic 认证。本服务使用 HTTP 而非 HTTPS，请仅在隔离的相机网络中使用。", "スクリプトからは同じ認証情報で HTTP Basic 認証を引き続き利用できます。このサービスは HTTPS ではなく HTTP です。隔離されたカメラ用ネットワーク内でのみ使用してください。"};
    result[S_LOGIN_EXPIRED] = {"The login form has expired; try again", "登录表单已过期，请重试", "ログインフォームの有効期限が切れました。もう一度お試しください"};
    result[S_LOGIN_INCORRECT] = {"Incorrect username or password", "用户名或密码错误", "ユーザー名またはパスワードが正しくありません"};
    result[S_LOGIN_ORIGIN] = {"Cross-site login request rejected", "已拒绝跨站登录请求", "他サイトからのログイン要求を拒否しました"};
    result[S_LOGOUT_FAILED] = {"Cannot revoke the login session on the camera; try again", "无法在相机上撤销登录会话，请重试", "カメラ上のログインセッションを取り消せません。もう一度お試しください"};
    result[S_LOGIN_SESSION_FAILED] = {"Cannot create a login session on the camera", "无法在相机上创建登录会话", "カメラ上にログインセッションを作成できません"};
    result[S_AUTH_REQUIRED] = {"Authentication required", "需要认证", "認証が必要です"};
    result[S_PASSWORD_FILE_MISSING] = {"Missing or empty %s", "%s 缺失或为空", "%s がないか空です"};
    result[S_LANGUAGE_INVALID] = {"Unknown language", "未知的语言", "不明な言語です"};
    result[S_OPT_DISABLED] = {"Disabled", "禁用", "無効"};
    result[S_OPT_ENABLED] = {"Enabled", "启用", "有効"};
    result[S_OPT_WHILE_ARMED] = {"While Armed", "解锁时", "アーム中"};
    result[S_OPT_AUTO] = {"Auto", "自动", "自動"};
    result[S_OPT_ISO_100] = {"ISO 100", "ISO 100", "ISO 100"};
    result[S_OPT_ISO_200] = {"ISO 200", "ISO 200", "ISO 200"};
    result[S_OPT_ISO_400] = {"ISO 400", "ISO 400", "ISO 400"};
    result[S_OPT_ISO_800] = {"ISO 800", "ISO 800", "ISO 800"};
    result[S_OPT_ISO_1600] = {"ISO 1600", "ISO 1600", "ISO 1600"};
    result[S_OPT_ISO_3200] = {"ISO 3200", "ISO 3200", "ISO 3200"};
    result[S_OPT_SHUTTER_30] = {"1/30 s", "1/30 秒", "1/30 秒"};
    result[S_OPT_SHUTTER_50] = {"1/50 s", "1/50 秒", "1/50 秒"};
    result[S_OPT_SHUTTER_100] = {"1/100 s", "1/100 秒", "1/100 秒"};
    result[S_OPT_SHUTTER_250] = {"1/250 s", "1/250 秒", "1/250 秒"};
    result[S_OPT_SHUTTER_500] = {"1/500 s", "1/500 秒", "1/500 秒"};
    result[S_OPT_SHUTTER_750] = {"1/750 s", "1/750 秒", "1/750 秒"};
    result[S_OPT_SHUTTER_1000] = {"1/1000 s", "1/1000 秒", "1/1000 秒"};
    result[S_OPT_SHUTTER_2000] = {"1/2000 s", "1/2000 秒", "1/2000 秒"};
    result[S_OPT_METER_AVERAGE] = {"Average", "平均测光", "平均測光"};
    result[S_OPT_METER_CENTER] = {"Center-weighted", "中央重点测光", "中央重点測光"};
    result[S_OPT_METER_SPOT_CENTER] = {"Spot (center)", "点测光（中央）", "スポット測光（中央）"};
    result[S_OPT_WB_DAYLIGHT] = {"Daylight", "日光", "太陽光"};
    result[S_OPT_WB_CLOUDY] = {"Cloudy", "阴天", "曇天"};
    result[S_OPT_WB_FLUORESCENT] = {"Fluorescent", "荧光灯", "蛍光灯"};
    result[S_OPT_WB_INCANDESCENT] = {"Incandescent", "白炽灯", "白熱灯"};
    result[S_OPT_SCOPE_THERMAL] = {"Thermal only", "仅热成像", "サーマルのみ"};
    result[S_OPT_SCOPE_ALL] = {"All lenses", "全部镜头", "すべてのレンズ"};
#if APCAM_HAVE_PHOTO_RESOLUTION
    result[S_OPT_PHOTO_RES_VIDEO] = {"Video frame (no pause)", "视频帧（不中断）", "動画フレーム（中断なし）"};
    result[S_OPT_PHOTO_RES_BINNED] = {"Binned sensor " APCAM_PHOTO_BINNED_NAME " (pauses video)", "传感器合并模式 " APCAM_PHOTO_BINNED_NAME "（暂停视频）", "ビニング " APCAM_PHOTO_BINNED_NAME "（動画を一時停止）"};
    result[S_OPT_PHOTO_RES_FULL] = {"Full sensor " APCAM_PHOTO_FULL_NAME " (pauses video)", "传感器全分辨率 " APCAM_PHOTO_FULL_NAME "（暂停视频）", "フル解像度 " APCAM_PHOTO_FULL_NAME "（動画を一時停止）"};
#endif
    result[S_OPT_ORIENT_AUTO] = {"Automatic (gimbal-reported)", "自动（由云台上报）", "自動（ジンバルの報告に従う）"};
    result[S_OPT_ORIENT_UPRIGHT] = {"Upright", "正装", "正立"};
    result[S_OPT_ORIENT_INVERTED] = {"Inverted", "倒装", "倒立"};
    result[S_OPT_UART_NONE] = {"None", "无", "なし"};
    result[S_OPT_UART_SIYI] = {"SIYI", "SIYI", "SIYI"};
    result[S_OPT_UART_MAVLINK] = {"MAVLink", "MAVLink", "MAVLink"};
    result[S_OPT_RES_720] = {"1280 x 720", "1280 x 720", "1280 x 720"};
    result[S_OPT_RES_1080] = {"1920 x 1080", "1920 x 1080", "1920 x 1080"};
    result[S_OPT_RES_1440] = {"2560 x 1440", "2560 x 1440", "2560 x 1440"};
    result[S_OPT_RES_4K] = {"3840 x 2160 / 4K", "3840 x 2160 / 4K", "3840 x 2160 / 4K"};
    result[S_OPT_RES_540] = {"960 x 540", "960 x 540", "960 x 540"};
    result[S_OPT_RES_480] = {"854 x 480", "854 x 480", "854 x 480"};
    result[S_OPT_RES_360] = {"640 x 360", "640 x 360", "640 x 360"};
    result[S_OPT_RES_240] = {"426 x 240", "426 x 240", "426 x 240"};
    result[S_OPT_RES_180] = {"320 x 180", "320 x 180", "320 x 180"};
    result[S_OPT_CODEC_H264] = {"H.264 / AVC", "H.264 / AVC", "H.264 / AVC"};
    result[S_OPT_CODEC_H265] = {"H.265 / HEVC", "H.265 / HEVC", "H.265 / HEVC"};
    result[S_OPT_PAL_WHITE_HOT] = {"White hot", "白热", "ホワイトホット"};
    result[S_OPT_PAL_SEPIA] = {"Sepia", "棕褐色", "セピア"};
    result[S_OPT_PAL_IRONBOW] = {"Ironbow", "铁红", "アイアンボウ"};
    result[S_OPT_PAL_RAINBOW] = {"Rainbow", "彩虹", "レインボー"};
    result[S_OPT_PAL_NIGHT] = {"Night", "夜视", "ナイト"};
    result[S_OPT_PAL_AURORA] = {"Aurora", "极光", "オーロラ"};
    result[S_OPT_PAL_RED_HOT] = {"Red hot", "红热", "レッドホット"};
    result[S_OPT_PAL_JUNGLE] = {"Jungle", "丛林", "ジャングル"};
    result[S_OPT_PAL_MEDICAL] = {"Medical", "医疗", "メディカル"};
    result[S_OPT_PAL_BLACK_HOT] = {"Black hot", "黑热", "ブラックホット"};
    result[S_OPT_PAL_GLORY_HOT] = {"Glory hot", "辉光", "グローリーホット"};
    result[S_P_BRIGHTNESS] = {"Brightness", "亮度", "明るさ"};
    result[S_P_SATURATION] = {"Saturation", "饱和度", "彩度"};
    result[S_P_CONTRAST] = {"Contrast", "对比度", "コントラスト"};
    result[S_P_EXPOSURE_COMP] = {"Exposure compensation", "曝光补偿", "露出補正"};
    result[S_P_ISO] = {"ISO", "ISO", "ISO"};
    result[S_P_METERING] = {"Metering mode", "测光模式", "測光モード"};
    result[S_P_WHITE_BALANCE] = {"White balance", "白平衡", "ホワイトバランス"};
    result[S_H_CARDV_BRIGHTNESS] = {"ISP brightness.", "ISP 亮度。", "ISP の明るさ。"};
    result[S_H_CARDV_SATURATION] = {"ISP saturation.", "ISP 饱和度。", "ISP の彩度。"};
    result[S_H_CARDV_CONTRAST] = {"ISP contrast.", "ISP 对比度。", "ISP のコントラスト。"};
    result[S_P_TIMEZONE] = {"Timezone", "时区", "タイムゾーン"};
    result[S_H_TIMEZONE] = {"Built-in IANA timezone name. GMT (UTC+0) is the default; daylight saving is automatic.", "内置 IANA 时区名称。默认为 GMT（UTC+0），自动应用夏令时。", "内蔵 IANA タイムゾーン名。デフォルトは GMT（UTC+0）で、夏時間は自動適用されます。"};
    result[S_P_PHOTO_SCOPE] = {"Photo capture scope", "拍照范围", "静止画の撮影範囲"};
    result[S_H_PHOTO_SCOPE] = {"Thermal saves the radiometric plane only. All saves both visible lenses, the thermal display and the radiometric plane.", "“仅热成像”只保存辐射测温数据；“全部镜头”保存两个可见光镜头的图像、热成像显示图像和辐射测温数据。", "「サーマルのみ」は放射温度データのみを保存します。「すべてのレンズ」は 2 つの可視光レンズの画像、サーマル表示画像、放射温度データをすべて保存します。"};
/* A target whose backend has its own bitrate rule sets the minimum to 0 for automatic. */
#if APCAM_MAIN_BITRATE_MIN_KBPS == 0
#define MAIN_BITRATE_AUTO_EN " 0 selects the camera's own automatic bitrate."
#define MAIN_BITRATE_AUTO_ZH "0 表示由相机自动选择码率。"
#define MAIN_BITRATE_AUTO_JA "0 はカメラの自動設定です。"
#else
#define MAIN_BITRATE_AUTO_EN ""
#define MAIN_BITRATE_AUTO_ZH ""
#define MAIN_BITRATE_AUTO_JA ""
#endif
#if APCAM_HAVE_VIDEO_BITRATE
    result[S_P_MAIN_BITRATE] = {"Main RTSP bitrate (kbps)", "主 RTSP 流码率 (kbps)", "メイン RTSP ビットレート (kbps)"};
    result[S_H_MAIN_BITRATE] = {
        "Bitrate in kbps for the RTSP stream and recordings, " APCAM_STRING_VALUE(APCAM_MAIN_BITRATE_MIN_KBPS) "–" APCAM_STRING_VALUE(APCAM_MAIN_BITRATE_MAX_KBPS)
            "; default " APCAM_STRING_VALUE(APCAM_DEFAULT_MAIN_BITRATE_KBPS) "." MAIN_BITRATE_AUTO_EN " Applies immediately, without a restart.",
        "用于 RTSP 流和录像的码率 (kbps)，" APCAM_STRING_VALUE(APCAM_MAIN_BITRATE_MIN_KBPS) "–" APCAM_STRING_VALUE(APCAM_MAIN_BITRATE_MAX_KBPS)
            "，默认 " APCAM_STRING_VALUE(APCAM_DEFAULT_MAIN_BITRATE_KBPS) "。" MAIN_BITRATE_AUTO_ZH "立即生效，无需重启。",
        "RTSP 配信と録画のビットレート (kbps)、" APCAM_STRING_VALUE(APCAM_MAIN_BITRATE_MIN_KBPS) "～" APCAM_STRING_VALUE(APCAM_MAIN_BITRATE_MAX_KBPS)
            "、初期値 " APCAM_STRING_VALUE(APCAM_DEFAULT_MAIN_BITRATE_KBPS) "。" MAIN_BITRATE_AUTO_JA "再起動なしで即時反映されます。"};
#endif
#if APCAM_HAVE_VIDEO_FRAME_RATE
    result[S_P_MAIN_FPS] = {"Main RTSP frame rate (fps)", "主 RTSP 流帧率 (fps)", "メイン RTSP フレームレート (fps)"};
    result[S_H_MAIN_FPS] = {
        APCAM_STRING_VALUE(APCAM_MAIN_FPS_MIN) "–" APCAM_STRING_VALUE(APCAM_FRAME_RATE) " fps; default " APCAM_STRING_VALUE(APCAM_FRAME_RATE)
            ", the highest rate this camera supports. Applies after the camera app restarts.",
        APCAM_STRING_VALUE(APCAM_MAIN_FPS_MIN) "–" APCAM_STRING_VALUE(APCAM_FRAME_RATE) " fps，默认 " APCAM_STRING_VALUE(APCAM_FRAME_RATE)
            "，为本相机支持的最高帧率。重启相机程序后生效。",
        APCAM_STRING_VALUE(APCAM_MAIN_FPS_MIN) "～" APCAM_STRING_VALUE(APCAM_FRAME_RATE) " fps、初期値 " APCAM_STRING_VALUE(APCAM_FRAME_RATE)
            "（このカメラが対応する最高値）。カメラアプリの再起動後に反映されます。"};
#endif
#if APCAM_HAVE_PHOTO_RESOLUTION
    result[S_P_PHOTO_RESOLUTION] = {"Photo resolution", "照片分辨率", "写真の解像度"};
    result[S_H_PHOTO_RESOLUTION] = {"A video frame is captured without interrupting streaming or recording. Larger photos switch the camera sensor mode, pausing video and recording for under a second per photo (about 0.5 s binned, 0.7 s full), and also show the full sensor height.", "视频帧拍照不会中断视频流或录像。更大的照片需要切换传感器模式，每张照片会暂停视频和录像不到一秒（合并模式约 0.5 秒，全分辨率约 0.7 秒），并包含传感器的完整高度。", "動画フレームはストリーミングや録画を中断せずに撮影します。より大きな写真はセンサーモードを切り替えるため、1 枚ごとに動画と録画が 1 秒未満（ビニング約 0.5 秒、フル解像度約 0.7 秒）停止し、センサーの全高を写します。"};
#endif
    result[S_P_ORIENTATION] = {"Mounting orientation", "安装方向", "取り付け方向"};
    result[S_H_ORIENTATION] = {"Automatic follows the direction reported by the gimbal controller; a forced mode rotates all video paths by 180 degrees when needed.", "“自动”跟随云台控制器上报的方向；强制模式会在需要时将所有视频通路旋转 180 度。", "「自動」はジンバルコントローラーが報告する向きに従います。固定モードでは必要に応じてすべての映像経路を 180 度回転します。"};
    result[S_P_UART_PROTOCOL] = {"UART4 protocol", "UART4 协议", "UART4 プロトコル"};
    result[S_H_UART_PROTOCOL_MT11] = {"External /dev/ttyAMA4 flight-controller link at 230400 baud, 8 data bits, no parity and one stop bit.", "通过 /dev/ttyAMA4 连接外部飞控，230400 波特率、8 数据位、无校验、1 停止位。", "/dev/ttyAMA4 経由の外部フライトコントローラー接続。230400 baud、データ 8 ビット、パリティなし、ストップ 1 ビット。"};
    result[S_H_UART_PROTOCOL_A8] = {"External flight-controller UART link at 230400 baud, 8 data bits, no parity and one stop bit.", "外部飞控 UART 连接，230400 波特率、8 数据位、无校验、1 停止位。", "外部フライトコントローラーとの UART 接続。230400 baud、データ 8 ビット、パリティなし、ストップ 1 ビット。"};
    result[S_H_UART_MCU] = {"The external UART is handled by the gimbal controller. This firmware cannot select its protocol or disable its input. The raw uart.protocol setting does not control that connector.", "外部 UART 由云台控制器处理。本固件无法选择其协议或禁用其输入。原始配置中的 uart.protocol 设置不控制该接口。", "外部 UART はジンバルコントローラーが処理します。このファームウェアではプロトコルの選択や入力の無効化はできません。生の設定の uart.protocol はこの端子を制御しません。"};
    result[S_P_MAVLINK_CAMERA_COMPID] = {"MAVLink camera component ID", "MAVLink 相机组件 ID", "MAVLink カメラコンポーネント ID"};
    result[S_H_MAVLINK_CAMERA_COMPID] = {"Select Camera 1–6 (IDs 100–105); the matching gimbal uses ID 154, 171, 172, 173, 174 or 175. Use a different camera ID for each camera on the same vehicle.", "选择相机 1–6（ID 100–105），对应的云台 ID 为 154、171、172、173、174 或 175。同一载具上的每台相机应使用不同的相机 ID。", "カメラ 1–6（ID 100–105）を選択。対応するジンバル ID は 154、171、172、173、174、175 です。同じ機体の各カメラには異なる ID を設定してください。"};
    result[S_OPT_CAMERA_COMP1] = {"Camera 1 (100)", "相机 1 (100)", "カメラ 1 (100)"};
    result[S_OPT_CAMERA_COMP2] = {"Camera 2 (101)", "相机 2 (101)", "カメラ 2 (101)"};
    result[S_OPT_CAMERA_COMP3] = {"Camera 3 (102)", "相机 3 (102)", "カメラ 3 (102)"};
    result[S_OPT_CAMERA_COMP4] = {"Camera 4 (103)", "相机 4 (103)", "カメラ 4 (103)"};
    result[S_OPT_CAMERA_COMP5] = {"Camera 5 (104)", "相机 5 (104)", "カメラ 5 (104)"};
    result[S_OPT_CAMERA_COMP6] = {"Camera 6 (105)", "相机 6 (105)", "カメラ 6 (105)"};
    result[S_P_MAVLINK_SYSID] = {"MAVLink system ID", "MAVLink 系统 ID", "MAVLink システム ID"};
    result[S_H_MAVLINK_SYSID] = {"0 automatically uses the first flight controller heartbeat's system ID (GCS heartbeats are ignored). 1–255 sets a fixed ID. Takes effect after camera-app restarts.", "0 自动使用首个飞控心跳的系统 ID（忽略地面站心跳）。1–255 为固定 ID。重启相机应用后生效。", "0 は最初のフライトコントローラーのハートビートから自動取得（GCS は無視）。1–255 は固定 ID。カメラアプリの再起動後に有効。"};
    result[S_P_MAVLINK_TCP] = {"MAVLink TCP port", "MAVLink TCP 端口", "MAVLink TCP ポート"};
    result[S_H_MAVLINK_TCP] = {"MAVLink 2 camera and gimbal listener. Set to 0 to disable TCP.", "MAVLink 2 相机与云台监听端口。设为 0 可禁用 TCP。", "MAVLink 2 カメラ／ジンバルの待ち受けポート。0 で TCP を無効にします。"};
    result[S_P_MAVLINK_UDP] = {"MAVLink UDP port", "MAVLink UDP 端口", "MAVLink UDP ポート"};
    result[S_H_MAVLINK_UDP] = {"MAVLink 2 camera and gimbal listener. Set to 0 to disable UDP.", "MAVLink 2 相机与云台监听端口。设为 0 可禁用 UDP。", "MAVLink 2 カメラ／ジンバルの待ち受けポート。0 で UDP を無効にします。"};
    result[S_P_LOG_DISARMED] = {"Log when disarmed", "未解锁时记录日志", "非アーム時にログ記録"};
    result[S_H_LOG_DISARMED] = {"Write diagnostic BIN logs to SD card logs/. Armed flights are always logged. Applies on Save.", "诊断日志保存至 SD 卡 logs/。解锁时始终记录，保存后生效。", "診断 BIN ログを SD カードの logs/ に保存。アーム時は常に記録。保存時に反映。"};
    result[S_P_OSD_CROSS] = {"Targeting cross", "瞄准十字", "照準クロス"};
    result[S_H_OSD_CROSS] = {"Centre diagonal cross in live video. Recording overlays are controlled separately where supported. Applies immediately.", "在实时视频中心显示对角十字。支持的相机可单独控制录像叠加图形。立即生效。", "ライブ映像の中央に対角クロスを表示。対応機種では録画への表示を別途設定できます。即時適用。"};
    result[S_P_OSD_RECORD] = {"Overlays in recordings", "录像中的叠加图形", "録画へのオーバーレイ"};
    result[S_H_OSD_RECORD] = {"Include enabled overlays in recordings. Disabled keeps recordings clean while live video still shows overlays. Applies immediately.", "在录像中包含已启用的叠加图形。禁用时仅在实时视频中显示。立即生效。", "有効なオーバーレイを録画に含めます。無効の場合もライブ映像には表示されます。即時適用。"};
    result[S_P_OSD_THERMAL_FOV] = {"Thermal FOV box in RGB", "RGB 中的热成像视场框", "RGB 映像の熱画像視野枠"};
    result[S_H_OSD_THERMAL_FOV] = {"Dashed thermal field-of-view box in RGB video; follows RGB zoom. Recording overlays are controlled separately. Assumes aligned lenses; nearby objects may differ due to parallax. Applies immediately.", "RGB 视频中的热成像虚线视场框，随变焦变化。录像叠加图形可单独控制。假设镜头对齐，近距离存在视差。立即生效。", "RGB 映像にズーム連動の熱画像視野枠を表示。録画への表示は別途設定できます。光軸一致を仮定し近距離では視差があります。即時適用。"};
    result[S_P_TRACK_METHOD] = {"Tracking control method", "跟踪控制方式", "追尾制御方式"};
    result[S_H_TRACK_METHOD] = {"Angle sends absolute positions. Rate follows predicted target motion with pointing-error correction. Applies to geographic ROI tracking; changes apply when saved.", "角度模式发送绝对位置；速率模式结合预测运动与指向误差修正。用于地理 ROI 跟踪，保存后生效。", "角度は絶対位置、速度は予測運動と指向誤差補正で制御します。地理 ROI 追尾に使用し、保存時に反映します。"};
    result[S_OPT_TRACK_ANGLE] = {"Angle", "角度", "角度"};
    result[S_OPT_TRACK_RATE] = {"Rate", "速率", "速度"};
    result[S_RESTART_REQUIRED] = {"Requires camera app restart.", "需要重启相机应用。", "カメラアプリの再起動が必要です。"};
    result[S_P_POSITION_TARGETING] = {"Position targeting", "位置目标指向", "位置ターゲット指向"};
    result[S_H_POSITION_TARGETING] = {"When enabled, advertise and handle geographic ROI targets in the camera. Disable to make ArduPilot calculate and send angle targets. Changes apply when saved; the updated capability is advertised to the flight controller.", "启用时，由相机宣告并处理地理 ROI 目标。禁用时，由 ArduPilot 计算并发送角度目标。保存后生效。", "有効にすると、カメラが地理 ROI ターゲットを通知して処理します。無効にすると、ArduPilot が角度ターゲットを計算して送信します。保存時に反映します。"};
    result[S_P_THERMAL_PALETTE] = {"Thermal palette", "热成像调色板", "サーマルパレット"};
    result[S_H_THERMAL_PALETTE] = {"Pseudo-colour palette applied by the thermal module to video and still images.", "热成像模块应用于视频和照片的伪彩调色板。", "サーマルモジュールが映像と静止画に適用する疑似カラーパレット。"};
    result[S_PARAMS_SURVEY] = {"Survey", "Survey", "Survey"};
    result[S_SURV_PATTERN] = {"Survey pattern", "Survey pattern", "Survey pattern"};
    result[S_SURV_PATTERN_HELP] = {"Fore-only uses three forward views. Left/right and fore/aft use two views. Both uses the full nine-view grid.", "Fore-only uses three forward views. Left/right and fore/aft use two views. Both uses the full nine-view grid.", "Fore-only uses three forward views. Left/right and fore/aft use two views. Both uses the full nine-view grid."};
    result[S_SURV_BOTH] = {"Both (nine views)", "Both (nine views)", "Both (nine views)"};
    result[S_SURV_LEFT_RIGHT] = {"Left/right only", "Left/right only", "Left/right only"};
    result[S_SURV_FORE_AFT] = {"Fore/aft only", "Fore/aft only", "Fore/aft only"};
    result[S_SURV_FORE_ONLY] = {"Fore-only (three pitch angles)", "Fore-only (three pitch angles)", "Fore-only (three pitch angles)"};
    result[S_SURV_LENS] = {"Survey camera", "Survey camera", "Survey camera"};
    result[S_SURV_LENS_HELP] = {"Select the physical camera used for survey captures.", "Select the physical camera used for survey captures.", "Select the physical camera used for survey captures."};
    result[S_SURV_THERMAL] = {"Raw thermal", "Raw thermal", "Raw thermal"};
    result[S_SURV_WIDE] = {"Wide RGB", "Wide RGB", "Wide RGB"};
    result[S_SURV_ZOOM] = {"Zoom RGB", "Zoom RGB", "Zoom RGB"};
    result[S_SURV_OVERLAP] = {"Overlap (%)", "Overlap (%)", "Overlap (%)"};
    result[S_SURV_OVERLAP_HELP] = {"Requested overlap in both flight and cross-track directions; achieved overlap is reported separately.", "Requested overlap in both flight and cross-track directions; achieved overlap is reported separately.", "Requested overlap in both flight and cross-track directions; achieved overlap is reported separately."};
    result[S_SURV_FORE_PCT] = {"Forward distance (% AGL)", "Forward distance (% AGL)", "Forward distance (% AGL)"};
    result[S_SURV_FORE_PCT_HELP] = {"Forward viewing distance as a percentage of height above ground.", "Forward viewing distance as a percentage of height above ground.", "Forward viewing distance as a percentage of height above ground."};
    result[S_SURV_AFT_PCT] = {"Rear distance (% AGL)", "Rear distance (% AGL)", "Rear distance (% AGL)"};
    result[S_SURV_AFT_PCT_HELP] = {"Rear viewing distance as a percentage of height above ground.", "Rear viewing distance as a percentage of height above ground.", "Rear viewing distance as a percentage of height above ground."};
    result[S_SURV_BURST] = {"Images per position", "Images per position", "Images per position"};
    result[S_SURV_BURST_HELP] = {"Distinct frames per target. Bursts are shortened at the slot deadline.", "Distinct frames per target. Bursts are shortened at the slot deadline.", "Distinct frames per target. Bursts are shortened at the slot deadline."};
    result[S_SURV_BURST_MS] = {"Burst interval (ms)", "Burst interval (ms)", "Burst interval (ms)"};
    result[S_SURV_BURST_MS_HELP] = {"Minimum interval between distinct exposures.", "Minimum interval between distinct exposures.", "Minimum interval between distinct exposures."};
    result[S_SURV_DWELL_MS] = {"Capture dwell limit (ms)", "Capture dwell limit (ms)", "Capture dwell limit (ms)"};
    result[S_SURV_DWELL_MS_HELP] = {"Additional time after predicted slew. Capture at the deadline even if still unsettled.", "Additional time after predicted slew. Capture at the deadline even if still unsettled.", "Additional time after predicted slew. Capture at the deadline even if still unsettled."};
    result[S_SURV_SETTLE_MS] = {"Settling window (ms)", "Settling window (ms)", "Settling window (ms)"};
    result[S_SURV_SETTLE_MS_HELP] = {"Consecutive time within pointing and residual-rate tolerances.", "Consecutive time within pointing and residual-rate tolerances.", "Consecutive time within pointing and residual-rate tolerances."};
    result[S_SURV_ERROR_CD] = {"Pointing tolerance (0.01 deg)", "Pointing tolerance (0.01 deg)", "Pointing tolerance (0.01 deg)"};
    result[S_SURV_ERROR_CD_HELP] = {"Angular difference between actual and required line of sight.", "Angular difference between actual and required line of sight.", "Angular difference between actual and required line of sight."};
    result[S_SURV_RATE_CD] = {"Residual rate tolerance (0.01 deg/s)", "Residual rate tolerance (0.01 deg/s)", "Residual rate tolerance (0.01 deg/s)"};
    result[S_SURV_RATE_CD_HELP] = {"Measured camera motion minus the rate needed to track the fixed ground point.", "Measured camera motion minus the rate needed to track the fixed ground point.", "Measured camera motion minus the rate needed to track the fixed ground point."};
    result[S_SURV_TERRAIN_MS] = {"Terrain maximum age (ms)", "Terrain maximum age (ms)", "Terrain maximum age (ms)"};
    result[S_SURV_TERRAIN_MS_HELP] = {"When uploaded terrain is unavailable, pause captures if the flight-controller TERRAIN_REPORT becomes stale.", "When uploaded terrain is unavailable, pause captures if the flight-controller TERRAIN_REPORT becomes stale.", "When uploaded terrain is unavailable, pause captures if the flight-controller TERRAIN_REPORT becomes stale."};
    result[S_SURV_TERRAIN_M] = {"Terrain report radius (m)", "Terrain report radius (m)", "Terrain report radius (m)"};
    result[S_SURV_TERRAIN_M_HELP] = {"Maximum horizontal distance from aircraft to terrain report location.", "Maximum horizontal distance from aircraft to terrain report location.", "Maximum horizontal distance from aircraft to terrain report location."};
    result[S_SURV_TERRAIN_TOL] = {"Terrain consistency (m)", "Terrain consistency (m)", "Terrain consistency (m)"};
    result[S_SURV_TERRAIN_TOL_HELP] = {"Maximum mismatch between reported AGL and aligned aircraft altitude minus ground elevation.", "Maximum mismatch between reported AGL and aligned aircraft altitude minus ground elevation.", "Maximum mismatch between reported AGL and aligned aircraft altitude minus ground elevation."};
    result[S_SURV_MIN_SPEED] = {"Minimum ground speed (m/s)", "Minimum ground speed (m/s)", "Minimum ground speed (m/s)"};
    result[S_SURV_MIN_SPEED_HELP] = {"Wait below this speed; resume 2 m/s above it after a straight-flight settling period.", "Wait below this speed; resume 2 m/s above it after a straight-flight settling period.", "Wait below this speed; resume 2 m/s above it after a straight-flight settling period."};
    result[S_SURV_TURN_DEG] = {"Turn threshold (deg)", "Turn threshold (deg)", "Turn threshold (deg)"};
    result[S_SURV_TURN_DEG_HELP] = {"Start a new ground grid after course changes this far from the current leg.", "Start a new ground grid after course changes this far from the current leg.", "Start a new ground grid after course changes this far from the current leg."};
    result[S_P_RAW_STREAM_FPS] = {"Raw thermal streaming rate (fps)", "原始热成像流帧率 (fps)", "生サーマル配信レート (fps)"};
    result[S_H_RAW_STREAM_FPS] = {"1–25 fps; default 5. Applies immediately. Slow connections receive fewer frames; every delivered frame remains lossless.", "1–25 fps，默认 5，立即生效。低速连接会跳帧，但保留每帧的全部数据。", "1～25 fps、初期値5。即時反映。低速回線ではフレームを間引きますが、各フレームは可逆圧縮です。"};
    result[S_P_RAW_RECORD_FPS] = {"Raw thermal recording rate (fps)", "原始热成像录像帧率 (fps)", "生サーマル録画レート (fps)"};
    result[S_H_RAW_RECORD_FPS] = {"0 disables; 1–25 fps, default 5. Saves lossless Matroska beside video files and follows the same recording controls and automatic recording policy. Applies immediately.", "0 关闭；1–25 fps，默认 5。与视频文件一起保存无损 Matroska，遵循相同的录像控制和自动录像设置。立即生效。", "0で無効、1～25 fps、初期値5。動画と同じ場所に可逆圧縮Matroskaを保存し、同じ録画操作と自動録画設定に従います。即時反映。"};
    result[S_P_AUTORECORD] = {"Automatic recording", "自动录像", "自動録画"};
    result[S_H_AUTORECORD_APP] = {"Enabled starts recording immediately and at startup. While Armed follows the current armed state and stops on disarm, using MAVLink HEARTBEAT from the selected system's autopilot (component 1).", "启用会在应用启动后开始录像。解锁时模式根据所选系统飞控（组件 1）的 MAVLink 心跳，在解锁时开始录像、上锁时停止。", "有効では起動時に録画を開始します。アーム中では選択したシステムのオートパイロット（コンポーネント 1）の MAVLink HEARTBEAT に従い、アームで開始、ディスアームで停止します。"};
    result[S_P_RECORDING_RESOLUTION] = {"Recording resolution", "录像分辨率", "録画解像度"};
    result[S_H_RECORDING_RESOLUTION_MT11] = {"Resolution used by the visible recording encoder; the MT11 thermal recording remains 1280 x 720.", "可见光录像编码器使用的分辨率；MT11 的热成像录像固定为 1280 x 720。", "可視光録画エンコーダーの解像度。MT11 のサーマル録画は 1280 x 720 固定です。"};
    result[S_H_RECORDING_RESOLUTION_A8] = {"Resolution used by the recording encoder.", "录像编码器使用的分辨率。", "録画エンコーダーの解像度。"};
    result[S_P_MAIN_RESOLUTION] = {"Main RTSP resolution", "主 RTSP 流分辨率", "メイン RTSP 解像度"};
    result[S_H_MAIN_RESOLUTION_MT11] = {"RGB resolution of rtsp://CAMERA:8554/video1; thermal output is limited to 1280 x 720.", "rtsp://CAMERA:8554/video1 的 RGB 分辨率；热成像输出上限为 1280 x 720。", "rtsp://CAMERA:8554/video1 の RGB 解像度。サーマル出力は 1280 x 720 が上限です。"};
    result[S_H_MAIN_RESOLUTION_A8] = {"Resolution of rtsp://CAMERA:8554/video1.", "rtsp://CAMERA:8554/video1 的分辨率。", "rtsp://CAMERA:8554/video1 の解像度。"};
    result[S_P_MAIN_CODEC] = {"Main RTSP codec", "主 RTSP 流编码格式", "メイン RTSP コーデック"};
    result[S_P_VIDEO_TELEMETRY] = {"Video telemetry format", "视频遥测格式", "映像テレメトリー形式"};
    result[S_H_VIDEO_TELEMETRY] = {"Applies after app restart. MISB adds /video1.misb (and /video2.misb when available) with video and KLV in MPEG-TS, and saves .ts recordings. Normal RTSP/browser previews retain APCG JSON telemetry.", "重启应用后生效。MISB 添加 /video1.misb（如有则包含 /video2.misb），以 MPEG-TS 传输视频和 KLV，并保存 .ts 录像。普通 RTSP/浏览器预览保留 APCG JSON 遥测。", "アプリ再起動後に反映。MISB は映像とKLVのMPEG-TSを /video1.misb（対応時は /video2.misb）で配信し、.ts で録画します。通常のRTSP・ブラウザープレビューはAPCG JSONを維持します。"};
    result[S_OPT_META_APCG] = {"APCG JSON (default)", "APCG JSON（默认）", "APCG JSON（初期値）"};
    result[S_OPT_META_MISB] = {"MISB ST 0601 KLV / MPEG-TS", "MISB ST 0601 KLV / MPEG-TS", "MISB ST 0601 KLV / MPEG-TS"};
    result[S_H_MAIN_CODEC] = {"Codec of rtsp://CAMERA:8554/video1.", "rtsp://CAMERA:8554/video1 的编码格式。", "rtsp://CAMERA:8554/video1 のコーデック。"};
    result[S_P_SUB_RESOLUTION] = {"Sub RTSP resolution", "子 RTSP 流分辨率", "サブ RTSP 解像度"};
    result[S_H_SUB_RESOLUTION_MT11] = {"RGB resolution of rtsp://CAMERA:8554/video2; thermal output is limited to 1280 x 720.", "rtsp://CAMERA:8554/video2 的 RGB 分辨率；热成像输出上限为 1280 x 720。", "rtsp://CAMERA:8554/video2 の RGB 解像度。サーマル出力は 1280 x 720 が上限です。"};
    result[S_H_SUB_RESOLUTION_A8] = {"Resolution of rtsp://CAMERA:8554/video2.", "rtsp://CAMERA:8554/video2 的分辨率。", "rtsp://CAMERA:8554/video2 の解像度。"};
    result[S_P_SUB_CODEC] = {"Sub RTSP codec", "子 RTSP 流编码格式", "サブ RTSP コーデック"};
    result[S_H_SUB_CODEC] = {"Codec of rtsp://CAMERA:8554/video2.", "rtsp://CAMERA:8554/video2 的编码格式。", "rtsp://CAMERA:8554/video2 のコーデック。"};
    result[S_P_MAIN_ALIAS] = {"Main RTSP alias", "主 RTSP 流别名", "メイン RTSP エイリアス"};
    result[S_H_MAIN_ALIAS] = {"Extra path serving the same stream as /video1, for example the vendor's main.264. Letters, digits, dot, dash and underscore; blank for none.", "以另一路径提供与 /video1 相同的视频流，例如厂商的 main.264。仅限字母、数字、点、连字符和下划线；留空则不提供。", "/video1 と同じストリームを別のパス名でも提供します（例: メーカー標準の main.264）。英数字、ドット、ハイフン、アンダースコアのみ。空欄で無効。"};
    result[S_P_SUB_ALIAS] = {"Sub RTSP alias", "子 RTSP 流别名", "サブ RTSP エイリアス"};
    result[S_H_SUB_ALIAS] = {"Extra path serving the same stream as /video2; blank for none.", "以另一路径提供与 /video2 相同的视频流；留空则不提供。", "/video2 と同じストリームを別のパス名でも提供します。空欄で無効。"};
    result[S_H_BRIGHTNESS_MT11] = {"Visible-camera ISP brightness, applied to both RGB sensors.", "可见光相机 ISP 亮度，同时应用于两个 RGB 传感器。", "可視光カメラの ISP の明るさ。両方の RGB センサーに適用されます。"};
    result[S_H_SATURATION_MT11] = {"Visible-camera ISP saturation, applied to both RGB sensors.", "可见光相机 ISP 饱和度，同时应用于两个 RGB 传感器。", "可視光カメラの ISP の彩度。両方の RGB センサーに適用されます。"};
    result[S_H_CONTRAST_MT11] = {"Visible-camera ISP contrast, applied to both RGB sensors.", "可见光相机 ISP 对比度，同时应用于两个 RGB 传感器。", "可視光カメラの ISP のコントラスト。両方の RGB センサーに適用されます。"};
    result[S_H_EXPOSURE_COMP_APP] = {"Tenths of an EV from -1.0 to +1.0 EV.", "以 0.1 EV 为单位，范围 -1.0 至 +1.0 EV。", "0.1 EV 単位で -1.0〜+1.0 EV。"};
    result[S_H_ISO_APP] = {"Automatic gain or a fixed visible-camera gain preset.", "自动增益或可见光相机的固定增益预设。", "自動ゲイン、または可視光カメラの固定ゲインプリセット。"};
    result[S_P_SHUTTER] = {"Shutter speed", "快门速度", "シャッター速度"};
    result[S_H_SHUTTER_APP] = {"Automatic exposure time or a fixed visible-camera shutter speed.", "自动曝光时间或可见光相机的固定快门速度。", "自動で調整する露光時間、または可視光カメラの固定シャッター速度。"};
    result[S_H_METERING_APP] = {"Spot metering uses the center of the frame.", "点测光以画面中央为基准。", "スポット測光は画面中央を使用します。"};
    result[S_H_WB_APP] = {"Automatic white balance or a fixed RGB-gain preset.", "自动白平衡或固定的 RGB 增益预设。", "オートホワイトバランス、または固定の RGB ゲインプリセット。"};
    result[S_E_CONFIG_SIZE] = {"Config size must be between 1 and %u bytes", "配置大小必须在 1 至 %u 字节之间", "設定のサイズは 1〜%u バイトである必要があります"};
    result[S_E_INI_UNTERMINATED_SECTION] = {"Line %u has an unterminated section", "第 %u 行的节名未闭合", "%u 行目のセクションが閉じられていません"};
    result[S_E_INI_TEXT_AFTER_SECTION] = {"Line %u has text after its section", "第 %u 行的节名之后有多余文本", "%u 行目のセクションの後に余分な文字があります"};
    result[S_E_INI_NOT_ASSIGNMENT] = {"Line %u is neither a section nor key=value", "第 %u 行既不是节也不是 key=value", "%u 行目はセクションでも key=value でもありません"};
    result[S_E_INI_EMPTY_KEY] = {"Line %u has an empty key", "第 %u 行的键为空", "%u 行目のキーが空です"};
    result[S_E_INI_NO_SECTION] = {"Config must contain a section and at least one assignment", "配置必须包含一个节和至少一个赋值", "設定にはセクションと 1 つ以上の代入が必要です"};
    result[S_E_INI_DUPLICATE_KEY] = {"Duplicate config key [%s] %s", "配置键 [%s] %s 重复", "設定キー [%s] %s が重複しています"};
    result[S_E_INI_MISSING_KEY] = {"Config key [%s] %s is missing", "缺少配置键 [%s] %s", "設定キー [%s] %s がありません"};
    result[S_E_CONFIG_TOO_LARGE] = {"Updated config is too large", "更新后的配置过大", "更新後の設定が大きすぎます"};
    result[S_E_CONFIG_ALLOC] = {"Cannot allocate updated config", "无法为更新后的配置分配内存", "更新後の設定用のメモリを確保できません"};
    result[S_E_TOO_MANY_PARAMETERS] = {"Too many parameters or parameter value too long", "参数过多或参数值过长", "パラメータが多すぎるか、値が長すぎます"};
    result[S_E_INVALID_VALUE] = {"Invalid value for %s", "“%s”的值无效", "「%s」の値が無効です"};
    result[S_E_PATH_TOO_LONG] = {"Path is too long", "路径过长", "パスが長すぎます"};
    result[S_E_CANNOT_WRITE] = {"Cannot write %s: %s", "无法写入 %s：%s", "%s に書き込めません: %s"};
    result[S_E_CANNOT_CLOSE] = {"Cannot close %s: %s", "无法关闭 %s：%s", "%s を閉じられません: %s"};
    result[S_E_CANNOT_INSTALL] = {"Cannot install %s: %s", "无法安装 %s：%s", "%s を配置できません: %s"};
    result[S_E_CANNOT_SYNC] = {"Cannot sync %s: %s", "无法同步 %s：%s", "%s を同期できません: %s"};
    result[S_E_LOCK_USERS] = {"Cannot lock user settings: %s", "无法锁定用户设置：%s", "ユーザー設定をロックできません: %s"};
    result[S_E_KEYS_RUNTIME_DIR] = {"Keys were saved persistently but runtime directory failed: %s", "密钥已持久保存，但运行时目录操作失败：%s", "鍵は永続保存されましたが、実行時ディレクトリの処理に失敗しました: %s"};
    result[S_E_KEYS_RUNTIME_ACTIVATION] = {"Keys were saved persistently but runtime activation failed: %s", "密钥已持久保存，但运行时激活失败：%s", "鍵は永続保存されましたが、実行時への反映に失敗しました: %s"};
    result[S_E_PASSWORD_LENGTH] = {"Password must be between 8 and 128 bytes of UTF-8", "密码的 UTF-8 编码长度必须为 8 至 128 字节", "パスワードは UTF-8 で 8〜128 バイトにしてください"};
    result[S_E_PASSWORD_CONTROL] = {"Password must not contain control characters", "密码不能包含控制字符", "パスワードに制御文字は使えません"};
    result[S_E_PASSWORD_MISMATCH] = {"Password confirmation does not match", "两次输入的密码不一致", "確認用パスワードが一致しません"};
    result[S_E_BROWSER_TIME] = {"Browser supplied an invalid time; reload the page and retry", "浏览器提供的时间无效，请刷新页面后重试", "ブラウザから無効な時刻が送られました。ページを再読み込みして再試行してください"};
    result[S_E_SET_TIME] = {"Cannot set camera time: %s", "无法设置相机时间：%s", "カメラの時刻を設定できません: %s"};
    result[S_E_KEY_REQUIRED] = {"At least one public key is required", "至少需要一个公钥", "公開鍵が少なくとも 1 つ必要です"};
    result[S_E_KEYS_TOO_LARGE] = {"Submitted keys exceed %u bytes", "提交的密钥超过 %u 字节", "送信された鍵が %u バイトを超えています"};
    result[S_E_KEY_MALFORMED] = {"Key on line %zu is unsupported or malformed; options are not accepted", "第 %zu 行的密钥不受支持或格式错误；不接受密钥选项", "%zu 行目の鍵は未対応か形式が不正です。鍵オプションは受け付けません"};
    result[S_E_KEY_DUPLICATE_LINE] = {"The submitted keys contain a duplicate on line %zu", "提交的密钥中第 %zu 行重复", "送信された鍵の %zu 行目が重複しています"};
    result[S_E_KEYS_READ] = {"Cannot read authorized keys: %s", "无法读取已授权密钥：%s", "authorized_keys を読み取れません: %s"};
    result[S_E_KEY_ALREADY] = {"Submitted key %zu is already authorized", "提交的第 %zu 个密钥已被授权", "送信された %zu 番目の鍵はすでに登録済みです"};
    result[S_E_KEY_FILE_TOO_LARGE] = {"Authorized key file would exceed %u bytes", "已授权密钥文件将超过 %u 字节", "authorized_keys ファイルが %u バイトを超えてしまいます"};
    result[S_E_KEY_REMOVE_UNCONFIRMED] = {"SSH key removal was not confirmed", "未确认删除 SSH 密钥", "SSH 鍵の削除が確認されていません"};
    result[S_E_KEY_SELECTION] = {"Invalid public key selection", "所选公钥无效", "選択された公開鍵が無効です"};
    result[S_E_KEY_ABSENT] = {"Public key was already absent", "该公钥已不存在", "その公開鍵はすでに存在しません"};
    result[S_E_KEY_LAST] = {"Refusing to remove the last usable SSH key; add its replacement first", "拒绝删除最后一个可用的 SSH 密钥；请先添加替代密钥", "最後の有効な SSH 鍵は削除できません。先に代わりの鍵を追加してください"};
    result[S_E_CANNOT_STAT] = {"Cannot stat %s: %s", "无法获取 %s 的状态：%s", "%s の情報を取得できません: %s"};
    result[S_E_CONFIG_PATH_LONG] = {"Config path is too long", "配置路径过长", "設定ファイルのパスが長すぎます"};
    result[S_E_CONFIG_BACKUP] = {"Cannot create config backup: %s", "无法创建配置备份：%s", "設定のバックアップを作成できません: %s"};
    result[S_E_CONFIG_WRITE] = {"Cannot write new config: %s", "无法写入新配置：%s", "新しい設定を書き込めません: %s"};
    result[S_E_CONFIG_INSTALL] = {"Cannot install new config: %s", "无法安装新配置：%s", "新しい設定を配置できません: %s"};
    result[S_E_CANNOT_READ] = {"Cannot read %s: %s", "无法读取 %s：%s", "%s を読み取れません: %s"};
    result[S_E_NO_CONFIG_DATA] = {"Form did not contain config data", "表单中没有配置数据", "フォームに設定データが含まれていません"};
    result[S_APP_REPLACEMENT] = {"ArduPilot camera app", "ArduPilot 相机应用", "ArduPilot カメラアプリ"};
    result[S_APP_GENERIC] = {"camera application", "相机应用", "カメラアプリ"};
    result[S_APP_STOPPED] = {"stopped", "已停止", "停止中"};
    result[S_E_APP_SELECTION_PATH] = {"Application selection path is too long", "应用选择文件路径过长", "アプリ選択ファイルのパスが長すぎます"};
    result[S_E_CAMERA_NOT_RUNNING] = {"Camera application is not running", "相机应用未运行", "カメラアプリが動作していません"};
    result[S_E_CAMERA_API] = {"Cannot contact camera API: %s", "无法连接相机 API：%s", "カメラ API に接続できません: %s"};
    result[S_E_MISSING_ACTION] = {"Missing control action", "缺少控制动作", "制御アクションがありません"};
    result[S_E_MISSING_RATE] = {"Missing gimbal rate", "缺少云台速率", "ジンバル速度がありません"};
    result[S_E_RATE_RANGE] = {"Gimbal rate must be between 5 and 60 degrees per second", "云台速率必须在每秒 5 至 60 度之间", "ジンバル速度は毎秒 5〜60 度の範囲で指定してください"};
    result[S_E_MISSING_ZOOM] = {"Missing zoom value", "缺少变焦值", "ズーム値がありません"};
    result[S_E_ZOOM_RANGE] = {"Zoom is outside the supported range", "变焦倍率超出支持范围", "ズームが対応範囲外です"};
    result[S_E_UNKNOWN_ACTION] = {"Unknown control action", "未知的控制动作", "不明な制御アクションです"};
    result[S_E_SEND_CONTROL] = {"Cannot send camera control: %s", "无法发送相机控制命令：%s", "カメラ制御コマンドを送信できません: %s"};
    result[S_E_SEND_LIDAR] = {"Cannot send LiDAR control: %s", "无法发送激光测距控制命令：%s", "LiDAR 制御コマンドを送信できません: %s"};
    result[S_E_SEND_SHUTTER] = {"Cannot send shutter command: %s", "无法发送快门命令：%s", "シャッターコマンドを送信できません: %s"};
    result[S_E_SHUTTER_UNCONFIRMED] = {"Camera did not confirm the shutter command; check recent photos before retrying", "相机未确认快门命令；重试前请先查看最近的照片", "シャッターコマンドに対するカメラの確認応答がありませんでした。再試行の前に最近の写真を確認してください"};
    result[S_E_CAPTURE_FAILED] = {"Camera reported photo capture failure", "相机报告拍照失败", "カメラが撮影失敗を報告しました"};
    result[S_E_APP_STOP] = {"Camera application did not stop cleanly; refusing to force-kill it", "相机应用未能正常停止；拒绝强制终止", "カメラアプリが正常に停止しませんでした。強制終了は行いません"};
    result[S_E_CANNOT_START] = {"Cannot start %s: %s", "无法启动 %s：%s", "%s を起動できません: %s"};
    result[S_E_NOT_READY] = {"%s did not report ready", "%s 未报告就绪", "%s が準備完了を報告しませんでした"};
    result[S_E_REQUEST_RECORD] = {"Cannot record the application request: %s", "无法记录应用操作请求：%s", "アプリ操作要求を記録できません: %s"};
    result[S_E_REQUEST_LOCK] = {"Cannot lock the application request: %s", "无法锁定应用操作请求：%s", "アプリ操作要求をロックできません: %s"};
    result[S_E_APP_NOT_UP] = {"%s did not come up; the launcher may have fallen back to the other application", "%s 未能启动；启动器可能已回退到另一个应用", "%s が起動しませんでした。ランチャーがもう一方のアプリに切り替えた可能性があります"};
    result[S_E_SWITCH_LOCK] = {"Cannot lock application switching: %s", "无法锁定应用切换：%s", "アプリ切り替えをロックできません: %s"};
    result[S_NOT_MOUNTED] = {"not mounted; ", "未挂载；", "未マウント。"};
    result[S_E_PATH_ABSOLUTE] = {"Path must be absolute", "路径必须是绝对路径", "パスは絶対パスである必要があります"};
    result[S_E_PATH_RESOLVE] = {"Cannot resolve path: %s", "无法解析路径：%s", "パスを解決できません: %s"};
    result[S_E_TARGET_MISSING] = {"Target does not exist", "目标不存在", "対象が存在しません"};
    result[S_E_DELETE_BENEATH] = {"Deletion is permitted only beneath %s", "只允许删除 %s 之下的内容", "削除できるのは %s 配下のみです"};
    result[S_E_TARGET_CHANGED] = {"Target changed while preparing deletion", "准备删除时目标发生了变化", "削除の準備中に対象が変化しました"};
    result[S_E_DELETE_FAILED] = {"Deletion failed: %s", "删除失败：%s", "削除に失敗しました: %s"};
    result[S_E_OPEN_DIRECTORY] = {"Cannot open directory: %s", "无法打开目录：%s", "ディレクトリを開けません: %s"};
    result[S_E_VIEW_REGULAR] = {"View target must be a regular file", "查看的目标必须是普通文件", "表示対象は通常のファイルである必要があります"};
    result[S_E_DELETE_EXISTING] = {"Deletion is permitted only for existing targets beneath %s", "只允许删除 %s 之下已存在的目标", "削除できるのは %s 配下に存在する対象のみです"};
    result[S_E_DELETE_RESOLVE] = {"Cannot resolve delete target", "无法解析删除目标", "削除対象を解決できません"};
    result[S_E_DELETE_UNCONFIRMED] = {"Deletion confirmation was not checked", "未勾选删除确认", "削除の確認にチェックが入っていません"};
    result[S_E_DELETE_PATH] = {"Invalid or missing delete path", "删除路径无效或缺失", "削除パスが無効か指定されていません"};
    result[S_E_DOWNLOAD_REGULAR] = {"Download target must be a regular file", "下载的目标必须是普通文件", "ダウンロード対象は通常のファイルである必要があります"};
    result[S_E_PATH_LONG_OR_INVALID] = {"Invalid or overly long path", "路径无效或过长", "パスが無効か長すぎます"};
    result[S_E_PATH_MISSING] = {"Invalid or missing path", "路径无效或缺失", "パスが無効か指定されていません"};
    result[S_PATH_DELETED] = {"Path deleted permanently", "路径已永久删除", "パスを完全に削除しました"};
    result[S_UNKNOWN_CURRENT_VALUE] = {"Unknown/missing current value", "当前值未知或缺失", "現在値が不明または未設定"};
    result[S_TITLE_CONTROL] = {"%s control", "%s 控制", "%s コントロール"};
    result[S_STATUS_SUBTITLE] = {"Authenticated administrative interface", "已认证的管理界面", "認証済み管理インターフェース"};
    result[S_STATUS_HEADING] = {"Status", "状态", "ステータス"};
    result[S_PARAMS_PROXY] = {"SupportProxy", "SupportProxy", "SupportProxy"};
    result[S_P_PROXY_ENABLED] = {"SupportProxy", "SupportProxy", "SupportProxy"};
    result[S_H_PROXY_ENABLED] = {"Enable video publishing and a bidirectional MAVLink relay. Changes take effect after restarting the camera app.", "启用视频发布和双向 MAVLink 转发。设置在重启相机应用后生效。", "映像送信と双方向 MAVLink 転送を有効にします。変更はカメラアプリの再起動後に反映されます。"};
    result[S_P_PROXY_HOST] = {"Proxy host", "代理服务器", "プロキシホスト"};
    result[S_H_PROXY_HOST] = {"SupportProxy server IPv4 address or hostname.", "SupportProxy 服务器的 IPv4 地址或主机名。", "SupportProxy サーバーの IPv4 アドレスまたはホスト名。"};
    result[S_P_PROXY_MAVLINK_PORT] = {"MAVLink port", "MAVLink 端口", "MAVLink ポート"};
    result[S_H_PROXY_MAVLINK_PORT] = {"SupportProxy user-side UDP port. Set to 0 to disable MAVLink forwarding.", "SupportProxy 用户侧 UDP 端口。设为 0 禁用 MAVLink 转发。", "SupportProxy のユーザー側 UDP ポート。0 で MAVLink 転送を無効にします。"};
    result[S_P_PROXY_SIGNING] = {"MAVLink signing", "MAVLink 签名", "MAVLink 署名"};
    result[S_H_PROXY_SIGNING] = {"Sign outgoing MAVLink 2 packets and require valid signed packets from SupportProxy. Configure bidi_sign on the proxy entry.", "对发出的 MAVLink 2 数据签名，并要求代理的数据具有有效签名。请在代理条目上启用 bidi_sign。", "送信する MAVLink 2 パケットに署名し、プロキシからの有効な署名を必須にします。プロキシの項目で bidi_sign を有効にしてください。"};
    result[S_P_PROXY_SIGNING_PASSPHRASE] = {"Signing passphrase", "签名口令", "署名パスフレーズ"};
    result[S_H_PROXY_SIGNING_PASSPHRASE] = {"Use the same passphrase as the SupportProxy entry. Applies only to the proxy link.", "使用与 SupportProxy 条目相同的口令。仅用于代理链路。", "SupportProxy の項目と同じパスフレーズを使用します。プロキシ接続にのみ適用されます。"};
    result[S_P_PROXY_SIGNING_LINK_ID] = {"Signing link ID", "签名链路 ID", "署名リンク ID"};
    result[S_H_PROXY_SIGNING_LINK_ID] = {"MAVLink signing link ID for this camera connection.", "此相机连接的 MAVLink 签名链路 ID。", "このカメラ接続の MAVLink 署名リンク ID。"};
    result[S_P_PROXY_VIDEO1_PORT] = {"Video 1 port", "视频 1 端口", "映像 1 ポート"};
    result[S_H_PROXY_VIDEO1_PORT] = {"RTSP publishing port for video1. Set to 0 to disable this stream.", "video1 的 RTSP 发布端口。设为 0 禁用此视频流。", "video1 の RTSP 送信ポート。0 でこのストリームを無効にします。"};
    result[S_P_PROXY_VIDEO1_NAME] = {"Video 1 stream name", "视频 1 流名称", "映像 1 ストリーム名"};
    result[S_H_PROXY_VIDEO1_NAME] = {"Stream name used in the RTSP publishing URL.", "RTSP 发布 URL 中使用的流名称。", "RTSP 送信 URL で使用するストリーム名。"};
    result[S_E_PROXY_VIDEO_PORTS] = {"Enabled video streams must use different ports.", "启用的视频流必须使用不同端口。", "有効な映像ストリームには異なるポートを指定してください。"};
    result[S_P_PROXY_VIDEO2_PORT] = {"Video 2 port", "视频 2 端口", "映像 2 ポート"};
    result[S_H_PROXY_VIDEO2_PORT] = {"RTSP publishing port for video2. Set to 0 to disable this stream.", "video2 的 RTSP 发布端口。设为 0 禁用此视频流。", "video2 の RTSP 送信ポート。0 でこのストリームを無効にします。"};
    result[S_P_PROXY_VIDEO2_NAME] = {"Video 2 stream name", "视频 2 流名称", "映像 2 ストリーム名"};
    result[S_H_PROXY_VIDEO2_NAME] = {"Stream name used in the RTSP publishing URL.", "RTSP 发布 URL 中使用的流名称。", "RTSP 送信 URL で使用するストリーム名。"};
    result[S_P_PROXY_VIDEO3_PORT] = {"Raw thermal port", "原始热成像端口", "生サーマル映像ポート"};
    result[S_H_PROXY_VIDEO3_PORT] = {"Matroska HTTP publishing port for video3. Set to 0 to disable this stream.", "video3 的 Matroska HTTP 发布端口。设为 0 禁用此视频流。", "video3 の Matroska HTTP 送信ポート。0 でこのストリームを無効にします。"};
    result[S_P_PROXY_VIDEO3_NAME] = {"Raw thermal stream name", "原始热成像流名称", "生サーマル映像ストリーム名"};
    result[S_H_PROXY_VIDEO3_NAME] = {"Name advertised for the raw thermal stream.", "发布的原始热成像流名称。", "通知される生サーマル映像ストリーム名。"};
    result[S_P_PROXY_PUBLISH_PASSWORD] = {"Video publish password", "视频发布密码", "映像送信パスワード"};
    result[S_H_PROXY_PUBLISH_PASSWORD] = {"Optional SupportProxy video publish password. Without it, allow publishing via the MAVLink session on the proxy.", "可选的 SupportProxy 视频发布密码。留空时需在代理上允许通过 MAVLink 会话发布。", "任意の SupportProxy 映像送信パスワード。空欄の場合、プロキシで MAVLink セッション経由の送信を許可してください。"};
    result[S_P_NETWORK_PRIMARY] = {"Primary IPv4 address/prefix", "主 IPv4 地址/前缀", "プライマリ IPv4 アドレス/プレフィックス"};
    result[S_H_NETWORK_PRIMARY] = {"For example 192.168.144.27/24. Replaces existing IPv4 addresses on this interface with the primary and optional secondary address. Blank leaves existing addresses in place.", "例如 192.168.144.27/24。用主地址及可选的次地址替换此接口的现有 IPv4 地址。留空保留现有地址。", "例：192.168.144.27/24。このインターフェースの IPv4 アドレスをプライマリと任意のセカンダリアドレスに置き換えます。空欄では既存のアドレスを維持します。"};
    result[S_E_NETWORK] = {"Use distinct host addresses and a gateway reachable through one of the configured subnets.", "请使用不同的主机地址及可通过已配置子网访问的网关。", "異なるホストアドレスと、設定したサブネットから到達可能なゲートウェイを指定してください。"};
    result[S_NETWORK_CONNECTION_LOST] = {"Connection lost during restart. Use the link below to reconnect, or reload this page to check whether the settings were saved.", "重启时连接断开。请使用下方链接重新连接，或重新加载此页检查设置是否已保存。", "再起動中に接続が切れました。下のリンクで再接続するか、このページを再読み込みして設定が保存されたか確認してください。"};
    result[S_NETWORK_SITL] = {"SITL uses the host network; address and gateway settings are saved but do not change the host network.", "SITL 使用主机网络；地址和网关设置会保存，但不会更改主机网络。", "SITL はホストのネットワークを使用します。アドレスとゲートウェイの設定は保存されますが、ホストのネットワークは変更しません。"};
    result[S_NETWORK_RECONNECT] = {"Network changes apply on camera-app restart. If the primary address changes, reconnect here after restarting:", "网络更改在相机应用重启后生效。主地址更改后，请重新连接：", "ネットワーク設定はカメラアプリの再起動後に反映されます。プライマリアドレスを変更した場合、再起動後はこちらに接続してください："};
    result[S_P_NETWORK_INTERFACE] = {"Network interface", "网络接口", "ネットワークインターフェース"};
    result[S_P_NETWORK_CAPTURE] = {"Network capture", "网络抓包", "ネットワークキャプチャ"};
    result[S_H_NETWORK_CAPTURE] = {"Off by default. Save to start or stop capturing all network interfaces, including loopback. Keeps the latest four 16 MiB PCAP files in DCIM/network on the microSD card. Disable before downloading and open the files in Wireshark.", "默认关闭。保存后开始或停止抓取所有网络接口（包括回环）的数据包。在 microSD 卡的 DCIM/network 中保留最新的四个 16 MiB PCAP 文件。下载前请禁用，然后用 Wireshark 打开。", "初期設定は無効です。保存するとループバックを含む全インターフェースのキャプチャを開始・停止します。microSD の DCIM/network に最新の16 MiB PCAPファイルを4個保持します。ダウンロード前に無効にし、Wiresharkで開いてください。"};
    result[S_P_NETWORK_VIDEO1_URI] = {"Video stream 1 URI", "视频流 1 URI", "映像ストリーム 1 URI"};
    result[S_H_NETWORK_VIDEO1_URI] = {"Complete rtsp:// or rtsps:// URI the GCS should use for video stream 1, for example rtsp://10.0.0.5:8554/video1. Sent as is in the video-stream-information message's URI field. The camera does not check that it serves this path; use the stream alias to serve extra paths. If left blank the URI is built from the Pi's own address, which is a guess when the Pi has several networks. Applies without restarting.", "地面站接收视频流 1 应使用的完整 rtsp:// 或 rtsps:// URI，例如 rtsp://10.0.0.5:8554/video1。通过 video-stream-information 消息的 URI 字段原样发送。相机不会检查是否提供该路径；可使用流别名提供额外路径。留空则根据树莓派自身的地址生成 URI；当树莓派有多个网络时，该地址只是推测。无需重启即可生效。", "GCS が映像ストリーム 1 に使用する完全な rtsp:// または rtsps:// URI（例: rtsp://10.0.0.5:8554/video1）。video-stream-information メッセージの URI フィールドでそのまま送信されます。カメラがこのパスを配信しているかは確認されません。追加のパスにはストリームのエイリアスを使用してください。空欄の場合は Pi 自身のアドレスから URI を生成しますが、Pi に複数のネットワークがある場合は推測になります。再起動なしで反映されます。"};
    result[S_NETWORK_CAPTURE_FILES] = {"Browse network captures", "浏览网络抓包文件", "キャプチャファイルを表示"};
    result[S_H_NETWORK_INTERFACE] = {"Interface used for camera addresses and the default gateway, independent of SupportProxy.", "用于相机地址和默认网关的接口，独立于 SupportProxy。", "カメラのアドレスとデフォルトゲートウェイのインターフェース。SupportProxy とは独立しています。"};
    result[S_P_NETWORK_ADDRESS] = {"Secondary IPv4 address/prefix", "附加 IPv4 地址/前缀", "追加 IPv4 アドレス/プレフィックス"};
    result[S_H_NETWORK_ADDRESS] = {"Optional second address, for example 192.168.20.25/24. Leave blank to remove the secondary address previously set here.", "可选次地址，例如 192.168.20.25/24。留空删除先前在此设置的次地址。", "任意のセカンダリアドレス（例：192.168.20.25/24）。空欄にすると以前ここで設定したアドレスを削除します。"};
    result[S_P_NETWORK_GATEWAY] = {"Default gateway", "默认网关", "デフォルトゲートウェイ"};
    result[S_H_NETWORK_GATEWAY] = {"Optional IPv4 gateway, independent of SupportProxy. Leave blank to remove the gateway previously set here.", "可选 IPv4 网关，独立于 SupportProxy。留空删除先前在此设置的网关。", "任意の IPv4 ゲートウェイ。SupportProxy とは独立しています。空欄にすると以前ここで設定したゲートウェイを削除します。"};
    result[S_STATUS_FIRMWARE_VERSION] = {"Firmware version", "固件版本", "ファームウェアバージョン"};
    result[S_STATUS_CAMERA_APP] = {"Camera app", "相机应用", "カメラアプリ"};
    result[S_STATUS_PID_RSS] = {" (PID %ld, RSS %ld KiB)", "（PID %ld，RSS %ld KiB）", "（PID %ld、RSS %ld KiB）"};
    result[S_STATUS_WEB_SERVICE] = {"Web service", "Web 服务", "Web サービス"};
    result[S_STATUS_WEB_PID_RSS] = {"PID %ld, RSS %ld KiB", "PID %ld，RSS %ld KiB", "PID %ld、RSS %ld KiB"};
    result[S_STATUS_TIME] = {"Time", "时间", "時刻"};
    result[S_STATUS_SYNC] = {"Sync", "同步", "同期"};
    result[S_STATUS_UPTIME] = {"Uptime", "运行时间", "稼働時間"};
    result[S_STATUS_UPTIME_FORMAT] = {"%u d %02u:%02u:%02u", "%u 天 %02u:%02u:%02u", "%u 日 %02u:%02u:%02u"};
    result[S_STATUS_CPU] = {"CPU", "CPU", "CPU"};
    result[S_STATUS_CPU_BUSY] = {"%u%% busy across %u cores (load average %.*s is inflated by the media driver's kernel threads)", "占用 %u%%，共 %u 个核心（负载平均值 %.*s 因媒体驱动的内核线程而偏高）", "使用率 %u%%、%u コア（ロードアベレージ %.*s はメディアドライバーのカーネルスレッドにより高めに出ます）"};
    result[S_STATUS_LOAD] = {"Load", "负载", "負荷"};
    result[S_STATUS_SOC_TEMPERATURE] = {"SoC temperature", "SoC 温度", "SoC 温度"};
#if APCAM_TARGET == APCAM_TARGET_ZT30
    result[S_STATUS_SOC_AVERAGE] = {"%s%d &deg;C", "%s%d &deg;C", "%s%d &deg;C"};
#elif APCAM_TARGET == APCAM_TARGET_Z1_MINI
    result[S_STATUS_SOC_AVERAGE] = {"%s%d.%d &deg;C", "%s%d.%d &deg;C", "%s%d.%d &deg;C"};
#else
    result[S_STATUS_SOC_AVERAGE] = {"%s%d.%d &deg;C (three-sensor average)", "%s%d.%d &deg;C（三个传感器的平均值）", "%s%d.%d &deg;C（3 センサーの平均）"};
#endif
    result[S_STATUS_MEMORY] = {"Memory", "内存", "メモリ"};
    result[S_STATUS_MEMORY_VALUE] = {"%ld MiB available / %ld MiB", "可用 %ld MiB / 共 %ld MiB", "空き %ld MiB / 合計 %ld MiB"};
    result[S_STATUS_IPV4] = {"IPv4", "IPv4", "IPv4"};
    result[S_STORAGE_TMPFS] = {"Temporary files (RAM)", "临时文件（内存）", "一時ファイル（RAM）"};
    result[S_STORAGE_ROOTFS] = {"Rootfs", "根文件系统", "ルートファイルシステム"};
    result[S_STORAGE_APPLICATION] = {"Application", "应用分区", "アプリ領域"};
    result[S_STORAGE_SETTINGS] = {"Settings", "设置分区", "設定領域"};
    result[S_STORAGE_MICROSD] = {"microSD", "microSD 卡", "microSD"};
#if !WEB_MEDIA_IS_MOUNT
    result[S_STORAGE_RECORDINGS] = {"Recordings", "录像存储", "録画保存先"};
#endif
    result[S_STATUS_REFRESH] = {"Refresh status", "刷新状态", "ステータスを更新"};
    result[S_STATUS_ACTIONS] = {"Actions", "操作", "操作"};
    result[S_STATUS_RESTART] = {"Restart camera app", "重新启动相机应用", "カメラアプリを再起動"};
    result[S_STATUS_UPGRADE_BUTTON] = {"Upgrade Firmware&hellip;", "升级固件&hellip;", "ファームウェアを更新&hellip;"};
    result[S_STATUS_UPGRADE_HELP] = {"Select a <code>%s</code> package. ", "请选择 <code>%s</code> 升级包。", "<code>%s</code> パッケージを選択してください。"};
    result[S_STATUS_UPGRADE_SYNC_MT11] = {"A complete upload is synced as <code>.bin.tmp</code>, atomically renamed to <code>.bin</code>, and synced again before the updater can discover it.", "上传完成后先以 <code>.bin.tmp</code> 同步写入，再原子地重命名为 <code>.bin</code> 并再次同步，之后升级程序才会发现它。", "アップロードが完了すると <code>.bin.tmp</code> として同期し、<code>.bin</code> にアトミックにリネームして再度同期してから、アップデーターが検出できるようになります。"};
    result[S_STATUS_UPGRADE_SYNC_A8] = {"A complete upload is synced to the microSD card as <code>%s</code>; U-Boot installs it on the next reboot.", "上传完成后会以 <code>%s</code> 同步写入 microSD 卡；U-Boot 会在下次重启时安装。", "アップロードが完了すると <code>%s</code> として microSD カードに同期され、次回の再起動時に U-Boot がインストールします。"};
    result[S_STATUS_UPGRADE_INSTALL_Z1] = {"The package is verified, installed into the application partition and the camera reboots. Settings and the web password are kept.", "升级包经校验后安装到应用分区，随后相机重启。设置和网页密码将被保留。", "パッケージは検証後にアプリ領域へインストールされ、カメラが再起動します。設定とウェブパスワードは保持されます。"};
    result[S_STATUS_REBOOT_CONFIRM] = {"I confirm this camera should reboot", "我确认要重启此相机", "このカメラを再起動することを確認しました"};
    result[S_STATUS_REBOOT_BUTTON] = {"Reboot camera", "重启相机", "カメラを再起動"};
    result[S_STATUS_AUTH_NOTE] = {"Authentication user: <code>admin</code>. The password is read from <code>%s</code> for every request. This service is HTTP, not HTTPS; keep it on the isolated camera network.", "认证用户：<code>admin</code>。每次请求都会从 <code>%s</code> 读取密码。本服务使用 HTTP 而非 HTTPS，请仅在隔离的相机网络中使用。", "認証ユーザー: <code>admin</code>。パスワードはリクエストごとに <code>%s</code> から読み込まれます。このサービスは HTTPS ではなく HTTP です。隔離されたカメラ用ネットワーク内でのみ使用してください。"};
    result[S_TIME_SYNCED] = {"Camera time synchronized with browser", "相机时间已与浏览器同步", "カメラの時刻をブラウザと同期しました"};
    result[S_REBOOT_UNCONFIRMED] = {"Reboot confirmation was not checked", "未勾选重启确认", "再起動の確認にチェックが入っていません"};
    result[S_TITLE_REBOOTING] = {"%s rebooting", "%s 正在重启", "%s 再起動中"};
    result[S_REBOOTING_HEADING] = {"Camera rebooting", "相机正在重启", "カメラを再起動しています"};
    result[S_REBOOTING_TEXT] = {"Waiting for the camera to restart…", "正在等待相机重新启动…", "カメラの再起動を待っています…"};
    result[S_REBOOT_HOME] = {"Open main page", "打开主页", "メインページを開く"};
    result[S_CSRF_RELOAD] = {"Invalid or expired form token; reload the page", "表单令牌无效或已过期，请刷新页面", "フォームトークンが無効または期限切れです。ページを再読み込みしてください"};
    result[S_CSRF_INVALID] = {"Invalid or expired form token", "表单令牌无效或已过期", "フォームトークンが無効または期限切れです"};
    result[S_UNKNOWN_POST] = {"Unknown action", "未知的操作", "不明な操作です"};
    result[S_SENSORS_SUBTITLE_MT11] = {"Live range and temperature telemetry with still capture", "实时距离与温度遥测，以及拍照", "距離・温度のライブ計測と静止画撮影"};
    result[S_SENSORS_SUBTITLE_A8] = {"Still capture and recent photos", "拍照与最近的照片", "静止画撮影と最近の写真"};
    result[S_SENSORS_LIVE] = {"Live sensors", "实时传感器数据", "センサーの現在値"};
    result[S_SENSORS_LIDAR] = {"LiDAR range", "激光测距", "LiDAR 距離"};
    result[S_LOADING] = {"Loading&hellip;", "加载中&hellip;", "読み込み中&hellip;"};
    result[S_ENABLE] = {"Enable", "启用", "有効化"};
    result[S_DISABLE] = {"Disable", "禁用", "無効化"};
    result[S_SENSORS_MIN] = {"Minimum temperature", "最低温度", "最低温度"};
    result[S_SENSORS_MAX] = {"Maximum temperature", "最高温度", "最高温度"};
    result[S_SENSORS_CPU] = {"CPU temperature", "CPU 温度", "CPU 温度"};
    result[S_SENSORS_UPDATING] = {"Updating twice per second.", "每秒更新两次。", "毎秒 2 回更新します。"};
    result[S_SENSORS_LASER_NOTICE] = {"Enabling LiDAR turns on the camera's laser. Keep people clear of its path. A zero range means there is no valid return.", "启用激光测距会打开相机的激光器，请确保光路上没有人员。距离为零表示没有有效回波。", "LiDAR を有効にするとカメラのレーザーが点灯します。光路に人が入らないようにしてください。距離 0 は有効な反射がないことを示します。"};
    result[S_SENSORS_SHUTTER] = {"Shutter", "快门", "シャッター"};
    result[S_SENSORS_SHUTTER_TEXT] = {"Capture a still using the running camera application and save it to the microSD card.", "使用当前运行的相机应用拍摄一张照片并保存到 microSD 卡。", "動作中のカメラアプリで静止画を撮影し、microSD カードに保存します。"};
    result[S_SENSORS_CAPTURE] = {"Capture photo", "拍照", "撮影"};
    result[S_SENSORS_SCOPE_NOTE] = {"The ArduPilot camera app's <strong>Photo capture scope</strong> parameter controls which lenses are saved. New packages default to All lenses.", "ArduPilot 相机应用的<strong>拍照范围</strong>参数决定保存哪些镜头的图像。新版升级包默认为“全部镜头”。", "ArduPilot カメラアプリの<strong>静止画の撮影範囲</strong>パラメータで、どのレンズの画像を保存するかが決まります。新しいパッケージの既定値は「すべてのレンズ」です。"};
    result[S_SENSORS_RECENT] = {"Recent photos", "最近的照片", "最近の写真"};
    result[S_SENSORS_NO_PHOTOS] = {"No JPEG photos were found beneath <code>%s</code>.", "在 <code>%s</code> 下未找到 JPEG 照片。", "<code>%s</code> 配下に JPEG 写真が見つかりませんでした。"};
    result[S_SENSORS_BROWSE] = {"Browse all captures", "浏览全部拍摄文件", "撮影ファイルをすべて表示"};
    result[S_PHOTO_CAPTURED] = {"Photo captured and saved to the microSD card", "照片已拍摄并保存到 microSD 卡", "写真を撮影し、microSD カードに保存しました"};
    result[S_E_LIDAR_ACTION] = {"Unknown LiDAR action", "未知的激光测距操作", "不明な LiDAR 操作です"};
    result[S_LIDAR_ENABLED] = {"LiDAR enabled", "激光测距已启用", "LiDAR を有効にしました"};
    result[S_LIDAR_DISABLED] = {"LiDAR disabled", "激光测距已禁用", "LiDAR を無効にしました"};
    result[S_JS_UNAVAILABLE] = {"Unavailable", "不可用", "取得不可"};
    result[S_JS_TEMPERATURE_AT] = {" °C at (", " °C，位于 (", " °C、座標 ("};
    result[S_JS_NO_RETURN] = {"No valid return", "无有效回波", "有効な反射なし"};
    result[S_JS_UPDATED] = {"Updated ", "更新于 ", "更新時刻: "};
    result[S_JS_TWICE_PER_SECOND] = {" · twice per second", " · 每秒两次", " · 毎秒 2 回"};
    result[S_JS_SENSOR_FAILED] = {"Sensor update failed: ", "传感器更新失败：", "センサーの更新に失敗しました: "};
    result[S_JS_UNKNOWN_ERROR] = {"unknown error", "未知错误", "不明なエラー"};
    result[S_JS_WAITING_RANGE] = {"Waiting for range…", "等待测距数据…", "距離の取得待ち…"};
    result[S_JS_LIDAR_CONTROL_FAILED] = {"LiDAR control failed: ", "激光测距控制失败：", "LiDAR の制御に失敗しました: "};
    result[S_TITLE_LIVE] = {"%s live video", "%s 实时视频", "%s ライブ映像"};
    result[S_LIVE_HEADING] = {"Live video", "实时视频", "ライブ映像"};
    result[S_LIVE_SUBTITLE] = {"Native H.264 preview from the running ArduPilot camera app", "来自运行中的 ArduPilot 相机应用的原生 H.264 预览", "動作中の ArduPilot カメラアプリからのネイティブ H.264 プレビュー"};
    result[S_LIVE_STREAM] = {"Stream", "视频流", "ストリーム"};
    result[S_LIVE_MAIN] = {"Main / video1", "主码流 / video1", "メイン / video1"};
    result[S_LIVE_SECONDARY] = {"Secondary / video2", "子码流 / video2", "サブ / video2"};
    result[S_LIVE_STARTING] = {"Starting live stream&hellip;", "正在启动实时视频流&hellip;", "ライブストリームを開始しています&hellip;"};
    result[S_LIVE_HELP] = {"This uses the camera's encoded H.264 frames directly; no video proxy or transcoder is installed. Set the selected stream codec to H.264 if it is unavailable.", "此功能直接使用相机编码的 H.264 帧，未安装视频代理或转码器。若无法播放，请将所选视频流的编码格式设为 H.264。", "カメラがエンコードした H.264 フレームをそのまま使用します。映像プロキシやトランスコーダーはインストールされていません。表示できない場合は、選択したストリームのコーデックを H.264 に設定してください。"};
    result[S_LIVE_PTZ] = {"Pan, tilt and zoom", "水平转动、俯仰与变焦", "パン・チルト・ズーム"};
    result[S_LIVE_ENABLE_MANUAL] = {"Enable manual gimbal control", "启用手动云台控制", "ジンバルの手動操作を有効にする"};
    result[S_LIVE_MANUAL_NOTICE] = {"Manual control temporarily blocks other gimbal commands and pauses ROI tracking. It clears on camera app restart or reboot, and expires if this page disconnects. Direction buttons send bounded 180 ms pulses and always issue a stop.", "手动控制会暂时阻止其他云台命令并暂停 ROI 跟踪。重启相机应用或相机后将清除，页面断开连接后会过期。方向按钮发送限定为 180 ms 的脉冲，并且始终会随后发送停止命令。", "手動操作中は他のジンバルコマンドと ROI 追跡を一時停止します。アプリやカメラの再起動、ページの切断で解除されます。方向ボタンは 180 ms に制限したパルスを送り、必ず停止コマンドを送信します。"};
    result[S_LIVE_CENTRE] = {"Centre", "回中", "センター"};
    result[S_LIVE_RATE] = {"Command rate", "控制速率", "操作速度"};
    result[S_LIVE_ZOOM] = {"Zoom", "变焦", "ズーム"};
    result[S_LIVE_NO_COMMANDS] = {"No manual commands sent.", "尚未发送手动命令。", "手動コマンドは送信されていません。"};
    result[S_LIVE_ATTITUDE] = {"Gimbal attitude", "云台姿态", "ジンバル姿勢"};
    result[S_LIVE_ATTITUDE_UNAVAILABLE] = {"Gimbal attitude unavailable", "云台姿态不可用", "ジンバル姿勢を取得できません"};
    result[S_LIVE_YAW] = {"Yaw", "偏航", "ヨー"};
    result[S_LIVE_ROLL] = {"Roll", "横滚", "ロール"};
    result[S_LIVE_PITCH] = {"Pitch", "俯仰", "ピッチ"};
    result[S_LIVE_YAW_RATE] = {"Yaw rate", "偏航速率", "ヨー速度"};
    result[S_LIVE_ROLL_RATE] = {"Roll rate", "横滚速率", "ロール速度"};
    result[S_LIVE_PITCH_RATE] = {"Pitch rate", "俯仰速率", "ピッチ速度"};
    result[S_LIVE_WAITING_GIMBAL] = {"Waiting for gimbal&hellip;", "等待云台&hellip;", "ジンバルの応答待ち&hellip;"};
    result[S_COMMAND_SENT] = {"Command sent", "命令已发送", "コマンドを送信しました"};
    result[S_JS_CONNECTING] = {"Connecting…", "正在连接…", "接続中…"};
    result[S_JS_RETRYING] = {" · retrying", " · 正在重试", " · 再試行中"};
    result[S_JS_LIVE_PREFIX] = {"Live · ", "实时 · ", "ライブ · "};
    result[S_JS_STREAM_ENDED] = {"Camera stream ended", "相机视频流已结束", "カメラのストリームが終了しました"};
    result[S_JS_LIVE_UNAVAILABLE] = {"Live video unavailable: ", "实时视频不可用：", "ライブ映像を表示できません: "};
    result[S_JS_ERROR_ABORTED] = {"aborted", "已中止", "中断"};
    result[S_JS_ERROR_NETWORK] = {"network", "网络错误", "ネットワーク"};
    result[S_JS_ERROR_DECODE] = {"decode", "解码错误", "デコード"};
    result[S_JS_ERROR_UNSUPPORTED] = {"unsupported", "不支持", "非対応"};
    result[S_JS_ERROR_CODE] = {"error ", "错误 ", "エラー "};
    result[S_JS_ENABLE_FIRST] = {"Enable manual gimbal control first.", "请先启用手动云台控制。", "先にジンバルの手動操作を有効にしてください。"};
    result[S_JS_CONTROL_FAILED] = {"Control failed: ", "控制失败：", "操作に失敗しました: "};
    result[S_JS_MANUAL_ENABLED] = {"Manual control enabled.", "手动控制已启用。", "手動操作を有効にしました。"};
    result[S_JS_MANUAL_DISABLED] = {"Manual control disabled.", "手动控制已禁用。", "手動操作を無効にしました。"};
    result[S_JS_ATTITUDE_LABEL] = {"Gimbal roll %s degrees, pitch %s degrees, yaw %s degrees", "云台横滚 %s 度，俯仰 %s 度，偏航 %s 度", "ジンバルのロール %s 度、ピッチ %s 度、ヨー %s 度"};
    result[S_JS_LIVE_ATTITUDE] = {"Live attitude · 4 Hz", "实时姿态 · 4 Hz", "姿勢のリアルタイム表示 · 4 Hz"};
    result[S_JS_ATTITUDE_FAILED] = {"Attitude update failed: ", "姿态更新失败：", "姿勢の更新に失敗しました: "};
    result[S_TITLE_USERS] = {"%s users", "%s 用户", "%s ユーザー"};
    result[S_USERS_HEADING] = {"Users", "用户", "ユーザー"};
    result[S_USERS_SUBTITLE_MT11] = {"Administrative credentials and SSH access", "管理凭据与 SSH 访问", "管理者の認証情報と SSH アクセス"};
    result[S_USERS_SUBTITLE_A8] = {"Administrative credentials", "管理凭据", "管理者の認証情報"};
    result[S_USERS_PASSWORD_HEADING] = {"Admin password", "管理员密码", "管理者パスワード"};
    result[S_USERS_PASSWORD_TEXT] = {"The username remains <code>admin</code>. The new password applies to the next login and to the next HTTP Basic request; existing browser sessions stay logged in.", "用户名保持为 <code>admin</code>。新密码对下一次登录和下一次 HTTP Basic 请求生效；已登录的浏览器会话保持登录状态。", "ユーザー名は <code>admin</code> のままです。新しいパスワードは次回のログインと次の HTTP Basic リクエストから適用され、ログイン済みのブラウザセッションはそのまま維持されます。"};
    result[S_USERS_NEW_PASSWORD] = {"New password", "新密码", "新しいパスワード"};
    result[S_USERS_CONFIRM_PASSWORD] = {"Confirm password", "确认密码", "パスワードの確認"};
    result[S_USERS_CHANGE_BUTTON] = {"Change admin password", "修改管理员密码", "管理者パスワードを変更"};
    result[S_USERS_HTTP_NOTICE] = {"This service runs over HTTP without TLS. Change credentials only on the isolated management network.", "本服务通过不带 TLS 的 HTTP 运行。请仅在隔离的管理网络中修改凭据。", "このサービスは TLS なしの HTTP で動作します。認証情報の変更は隔離された管理ネットワーク内でのみ行ってください。"};
    result[S_USERS_ADD_KEYS] = {"Add SSH public keys", "添加 SSH 公钥", "SSH 公開鍵を追加"};
    result[S_USERS_ADD_KEYS_BUTTON] = {"Add public-key files&hellip;", "添加公钥文件&hellip;", "公開鍵ファイルを追加&hellip;"};
    result[S_USERS_ADD_KEYS_HELP] = {"Select one or more <code>.pub</code> files. The whole batch is validated before anything changes. Key options such as forced commands are intentionally rejected. Changes update persistent and live Dropbear files.", "选择一个或多个 <code>.pub</code> 文件。整批文件会在做出任何更改前先行验证。强制命令等密钥选项会被有意拒绝。更改会同时更新 Dropbear 的持久文件和运行时文件。", "<code>.pub</code> ファイルを 1 つ以上選択してください。変更前に一括で検証されます。強制コマンドなどの鍵オプションは意図的に拒否されます。変更は Dropbear の永続ファイルと実行時ファイルの両方に反映されます。"};
    result[S_USERS_AUTHORIZED] = {"Authorized SSH keys", "已授权的 SSH 密钥", "登録済みの SSH 鍵"};
    result[S_USERS_KEYS_UNREADABLE] = {"Unable to read the authorized-key file.", "无法读取已授权密钥文件。", "authorized_keys ファイルを読み取れません。"};
    result[S_USERS_KEY_TYPE] = {"Type", "类型", "種類"};
    result[S_USERS_KEY] = {"Key", "密钥", "鍵"};
    result[S_USERS_KEY_COMMENT] = {"Comment", "备注", "コメント"};
    result[S_USERS_KEY_ACTION] = {"Action", "操作", "操作"};
    result[S_USERS_KEY_NO_COMMENT] = {"none", "无", "なし"};
    result[S_USERS_KEY_CONFIRM] = {"confirm", "确认", "確認"};
    result[S_USERS_KEY_REMOVE] = {"Remove", "删除", "削除"};
    result[S_USERS_NO_KEYS] = {"No supported public keys are currently authorized.", "当前没有已授权的受支持公钥。", "現在、対応形式の公開鍵は登録されていません。"};
    result[S_USERS_UNMANAGED_ONE] = {"1 unrecognized authorized_keys line preserved but not editable here.", "有 1 行无法识别的 authorized_keys 条目已保留，但无法在此编辑。", "認識できない authorized_keys の行が 1 行あります。保持されますが、ここでは編集できません。"};
    result[S_USERS_UNMANAGED_MANY] = {"%zu unrecognized authorized_keys lines preserved but not editable here.", "有 %zu 行无法识别的 authorized_keys 条目已保留，但无法在此编辑。", "認識できない authorized_keys の行が %zu 行あります。保持されますが、ここでは編集できません。"};
    result[S_USERS_LAST_KEY_NOTICE] = {"The last usable SSH key cannot be removed. Add and test a replacement before removing old keys. Existing SSH sessions are not closed.", "最后一个可用的 SSH 密钥无法删除。删除旧密钥前，请先添加并测试替代密钥。现有 SSH 会话不会被关闭。", "最後の有効な SSH 鍵は削除できません。古い鍵を削除する前に、代わりの鍵を追加して動作を確認してください。既存の SSH セッションは切断されません。"};
    result[S_PASSWORD_CHANGED] = {"Admin password changed; use it for the next login", "管理员密码已修改，下次登录时请使用新密码", "管理者パスワードを変更しました。次回のログインから新しいパスワードを使ってください"};
    result[S_KEY_ADDED_ONE] = {"1 SSH public key added", "已添加 1 个 SSH 公钥", "SSH 公開鍵を 1 件追加しました"};
    result[S_KEYS_ADDED_MANY] = {"%zu SSH public keys added", "已添加 %zu 个 SSH 公钥", "SSH 公開鍵を %zu 件追加しました"};
    result[S_KEY_REMOVED] = {"SSH public key removed", "SSH 公钥已删除", "SSH 公開鍵を削除しました"};
    result[S_JS_SELECT_PUB] = {"Select only .pub public-key files.", "只能选择 .pub 公钥文件。", ".pub の公開鍵ファイルのみ選択してください。"};
    result[S_JS_READING_KEYS] = {"Reading public-key files…", "正在读取公钥文件…", "公開鍵ファイルを読み込んでいます…"};
    result[S_JS_FILES_EMPTY] = {"The selected files are empty.", "所选文件为空。", "選択したファイルは空です。"};
    result[S_JS_KEYS_TOO_LARGE] = {"The selected public keys exceed 64 KiB.", "所选公钥超过 64 KiB。", "選択した公開鍵が 64 KiB を超えています。"};
    result[S_JS_UPLOADING_KEYS] = {"Uploading public keys…", "正在上传公钥…", "公開鍵をアップロードしています…"};
    result[S_JS_FILES_UNREADABLE] = {"Unable to read the selected files.", "无法读取所选文件。", "選択したファイルを読み取れません。"};
    result[S_TITLE_APP_PARAMETERS] = {"Parameters", "参数", "パラメータ"};
    result[S_APP_PARAMETERS_SUBTITLE] = {"Configure the camera, networking and video", "配置相机、网络和视频", "カメラ、ネットワーク、映像の設定"};
    result[S_APP_PARAMETERS_NOTICE] = {"Use <strong>Save</strong> to apply live settings automatically. Video-format changes briefly reconnect streams and wait until recording stops. Settings marked as requiring restart use <strong>Save and restart</strong>.", "<strong>保存</strong>后自动应用实时设置。视频格式更改会短暂重连视频，并等待录像停止。标记需要重启的设置请使用<strong>保存并重新启动</strong>。", "<strong>保存</strong>で設定を自動反映します。映像形式の変更は録画停止後にストリームを再接続します。再起動が必要と表示された設定は<strong>保存して再起動</strong>を使ってください。"};
    result[S_PARAMS_SYSTEM] = {"System", "系统", "システム"};
    result[S_PARAMS_NETWORK] = {"Network", "网络", "ネットワーク"};
    result[S_PARAMS_VIDEO] = {"Video", "视频", "映像"};
    result[S_PARAMS_CATEGORIES] = {"Parameter categories", "参数分类", "パラメータ分類"};
    result[S_PARAMS_SAVE_ALL] = {"Save applies changes from all tabs.", "保存将应用所有选项卡中的更改。", "保存はすべてのタブの変更を反映します。"};
    result[S_PARAMS_SAVE] = {"Save parameters", "保存参数", "パラメータを保存"};
    result[S_PARAMS_SAVE_RESTART] = {"Save and restart camera app", "保存并重新启动相机应用", "保存してカメラアプリを再起動"};
    result[S_PARAMS_SAVED_RESTARTED] = {"Parameters saved and %s restarted", "参数已保存，%s 已重新启动", "パラメータを保存し、%s を再起動しました"};
    result[S_PARAMS_SAVED] = {"Parameters saved; waiting for %s to apply live settings", "参数已保存；重新启动 %s 后生效", "パラメータを保存しました。反映するには %s を再起動してください"};
    result[S_E_CONFIG_UNREADABLE] = {"Unable to read config or out of memory", "无法读取配置或内存不足", "設定を読み取れないか、メモリが不足しています"};
    result[S_TITLE_RAW] = {"%s raw config", "%s 原始配置", "%s 設定ファイル"};
    result[S_RAW_HEADING] = {"Raw %s", "原始 %s", "%s の直接編集"};
    result[S_RAW_SUBTITLE] = {"Advanced editor for the running %s", "面向当前运行的 %s 的高级编辑器", "動作中の %s 向けの高度なエディター"};
    result[S_RAW_SAVE] = {"Save config", "保存配置", "設定を保存"};
    result[S_RAW_HELP] = {"Saves are syntax-checked and atomic. Editing <code>%s</code>; the previous file is kept at <code>%s</code>. The Parameters page performs stronger per-value validation.", "保存前会进行语法检查，且保存是原子操作。当前编辑 <code>%s</code>；旧文件保留在 <code>%s</code>。“参数”页面会对每个值进行更严格的校验。", "保存時に構文チェックを行い、アトミックに書き込みます。編集対象は <code>%s</code> で、以前のファイルは <code>%s</code> に保持されます。「パラメータ」ページでは値ごとにより厳密な検証が行われます。"};
    result[S_CONFIG_SAVED_RESTARTED] = {"Config saved and %s restarted", "配置已保存，%s 已重新启动", "設定を保存し、%s を再起動しました"};
    result[S_CONFIG_SAVED] = {"Config saved", "配置已保存", "設定を保存しました"};
    result[S_TITLE_FILES] = {"%s files", "%s 文件", "%s ファイル"};
    result[S_FILES_HEADING] = {"Filesystem", "文件系统", "ファイルシステム"};
    result[S_FILES_SUBTITLE] = {"Authenticated browsing and downloads; deletion is restricted to %s", "经认证的浏览与下载；删除仅限于 %s", "認証済みの閲覧とダウンロード。削除は %s 配下に限られます"};
    result[S_FILES_PATH] = {"Path", "路径", "パス"};
    result[S_FILES_OPEN] = {"Open path", "打开路径", "パスを開く"};
    result[S_FILES_NAME] = {"Name", "名称", "名前"};
    result[S_FILES_TYPE] = {"Type", "类型", "種類"};
    result[S_FILES_SIZE] = {"Size", "大小", "サイズ"};
    result[S_FILES_MODIFIED] = {"Modified", "修改时间", "更新日時"};
    result[S_FILES_MODE] = {"Mode", "权限", "モード"};
    result[S_FILES_ACTIONS] = {"Actions", "操作", "操作"};
    result[S_FILES_DIRECTORY] = {"directory", "目录", "ディレクトリ"};
    result[S_FILES_LINK] = {" (link)", "（链接）", "（リンク）"};
    result[S_FILES_DOWNLOAD] = {"Download", "下载", "ダウンロード"};
    result[S_FILES_DELETE] = {"Delete", "删除", "削除"};
    result[S_FILES_TRUNCATED] = {"Only the first 5000 entries are shown.", "仅显示前 5000 个条目。", "先頭の 5000 件のみ表示しています。"};
    result[S_FILES_LEGEND] = {"Type letters: d directory, f regular file, l symbolic link, c character device, b block device, p FIFO, s socket. Files outside %s can be viewed or downloaded but never deleted through this service.", "类型字母：d 目录，f 普通文件，l 符号链接，c 字符设备，b 块设备，p FIFO，s 套接字。%s 之外的文件可以查看或下载，但不能通过本服务删除。", "種類の記号: d ディレクトリ、f 通常ファイル、l シンボリックリンク、c キャラクターデバイス、b ブロックデバイス、p FIFO、s ソケット。%s の外にあるファイルは表示・ダウンロードはできますが、このサービスからは削除できません。"};
    result[S_TITLE_VIEWER] = {"%s file viewer", "%s 文件查看器", "%s ファイルビューアー"};
    result[S_TS_SEEK_ERROR] = {"Unable to seek to that position. Try another position or download the recording.", "无法跳转至该位置。请尝试其他位置或下载录像。", "その位置に移動できません。別の位置を試すか録画をダウンロードしてください。"};
    result[S_TS_LOADING] = {"Loading video…", "正在加载视频…", "動画を読み込み中…"};
    result[S_TS_UNAVAILABLE] = {"Playback is unavailable in this browser. Download the recording to view it.", "此浏览器无法播放。请下载录像查看。", "このブラウザーでは再生できません。録画をダウンロードしてください。"};
    result[S_TS_ERROR] = {"Unable to read or play this recording. Download it or reopen the page to retry.", "无法读取或播放此录像。请下载或重新打开页面重试。", "録画を読み込めないか再生できません。ダウンロードするかページを開き直してください。"};
    result[S_TS_PLAY] = {"Play", "播放", "再生"};
    result[S_TS_PAUSE] = {"Pause", "暂停", "一時停止"};
    result[S_TS_FULLSCREEN] = {"Fullscreen", "全屏", "全画面"};
    result[S_TS_POSITION] = {"Recording position", "录像位置", "録画位置"};
    result[S_VIEWER_HEADING] = {"File viewer", "文件查看器", "ファイルビューアー"};
    result[S_VIEWER_BACK] = {"Back to directory", "返回目录", "ディレクトリに戻る"};
    result[S_VIEWER_IMAGE_ALT] = {"Image preview", "图片预览", "画像プレビュー"};
    result[S_VIEWER_NO_VIDEO] = {"This browser cannot play the video; use Download.", "此浏览器无法播放该视频，请使用“下载”。", "このブラウザでは動画を再生できません。「ダウンロード」を使用してください。"};
    result[S_VIEWER_NO_PREVIEW] = {"No inline preview is available for this file type. Use Download.", "此文件类型没有内嵌预览，请使用“下载”。", "このファイル形式はインラインプレビューに対応していません。「ダウンロード」を使用してください。"};
    result[S_TITLE_DELETE] = {"Confirm deletion", "确认删除", "削除の確認"};
    result[S_DELETE_TEXT] = {"This permanently deletes ", "这将永久删除 ", "次の対象を完全に削除します: "};
    result[S_DELETE_RECURSIVE] = {" and all files and directories beneath it", " 及其下的所有文件和目录", "（配下のすべてのファイルとディレクトリを含む）"};
    result[S_DELETE_NO_UNDO] = {". This operation cannot be undone.", "。此操作无法撤销。", "。この操作は元に戻せません。"};
    result[S_DELETE_CONFIRM] = {"I understand this deletion is permanent", "我了解此删除操作不可恢复", "この削除が元に戻せないことを理解しました"};
    result[S_DELETE_BUTTON] = {"Delete permanently", "永久删除", "完全に削除"};
    result[S_TITLE_DEBUG] = {"%s debug", "%s 调试", "%s デバッグ"};
    result[S_DEBUG_HEADING] = {"Debug", "调试", "デバッグ"};
    result[S_DEBUG_SUBTITLE] = {"Camera application output; refreshes every 3 seconds", "相机应用输出；每 3 秒刷新一次", "カメラアプリの出力。3 秒ごとに更新します"};
    result[S_DEBUG_PLAIN_TEXT] = {"Plain text", "纯文本", "プレーンテキスト"};
    result[S_DEBUG_EMPTY_MT11] = {"No captured output yet. Restart the camera application to begin capture.", "尚未捕获到输出。重新启动相机应用以开始捕获。", "まだ出力を取得していません。取得を開始するにはカメラアプリを再起動してください。"};
    result[S_DEBUG_EMPTY_A8] = {"No camera application output yet.", "尚无相机应用日志输出。", "カメラアプリのログ出力はまだありません。"};
    result[S_DEBUG_FOOTER] = {"Showing the newest %zu bytes. The RAM log rotates at %u KiB and retains one previous segment.", "显示最新的 %zu 字节。内存日志在 %u KiB 时轮转，并保留上一段。", "最新の %zu バイトを表示しています。RAM 上のログは %u KiB でローテーションし、直前のログファイルを 1 世代保持します。"};
    result[S_E_LOG_UNREADABLE] = {"Unable to read log", "无法读取日志", "ログを読み取れません"};
    result[S_E_FW_SIZE] = {"Firmware size must be between 1 byte and 128 MiB", "固件大小必须在 1 字节至 128 MiB 之间", "ファームウェアのサイズは 1 バイト〜128 MiB である必要があります"};
    result[S_E_FW_CONTENT_TYPE] = {"Firmware upload must use application/octet-stream", "固件上传必须使用 application/octet-stream", "ファームウェアのアップロードは application/octet-stream で行う必要があります"};
    result[S_E_FW_NAME] = {"Filename must match %s and contain only safe ASCII characters", "文件名必须符合 %s 且只包含安全的 ASCII 字符", "ファイル名は %s の形式で、安全な ASCII 文字のみを含む必要があります"};
    result[S_E_FW_NAME_LONG] = {"Firmware filename is too long", "固件文件名过长", "ファームウェアのファイル名が長すぎます"};
    result[S_E_FW_LOCK] = {"Cannot lock firmware uploads: %s", "无法锁定固件上传：%s", "ファームウェアのアップロードをロックできません: %s"};
    result[S_E_FW_MICROSD] = {"Cannot access mounted microSD: %s", "无法访问已挂载的 microSD 卡：%s", "マウントされた microSD にアクセスできません: %s"};
    result[S_E_FW_SPACE] = {"Not enough free space on the microSD card", "microSD 卡剩余空间不足", "microSD カードの空き容量が不足しています"};
    result[S_E_FW_INSPECT] = {"Cannot inspect %s: %s", "无法检查 %s：%s", "%s を確認できません: %s"};
    result[S_E_FW_EXISTS] = {"Another %s firmware package already exists on the card", "卡上已存在另一个 %s 固件升级包", "カード上に別の %s ファームウェアパッケージがすでに存在します"};
    result[S_E_FW_NAME_EXISTS] = {"A firmware file with that name already exists on the card", "卡上已存在同名的固件文件", "同じ名前のファームウェアファイルがカード上にすでに存在します"};
    result[S_E_FW_TEMP_EXISTS] = {"A temporary upload with that name already exists on the card", "卡上已存在同名的临时上传文件", "同じ名前の一時アップロードファイルがカード上にすでに存在します"};
    result[S_E_FW_CREATE] = {"Cannot create %s/%s: %s", "无法创建 %s/%s：%s", "%s/%s を作成できません: %s"};
    result[S_E_FW_PUBLISH] = {"Cannot publish %s/%s: %s", "无法发布 %s/%s：%s", "%s/%s を公開できません: %s"};
    result[S_E_FW_APPEARED] = {"A firmware file with that name appeared during upload", "上传期间出现了同名的固件文件", "アップロード中に同じ名前のファームウェアファイルが現れました"};
    result[S_E_FW_PUBLISH_SAFE] = {"Cannot safely publish %s/%s: %s", "无法安全地发布 %s/%s：%s", "%s/%s を安全に公開できません: %s"};
    result[S_E_FW_DIR_SYNC] = {"Firmware was renamed but directory sync failed: %s", "固件已重命名，但目录同步失败：%s", "ファームウェアのリネームは完了しましたが、ディレクトリの同期に失敗しました: %s"};
    result[S_FW_UPLOADED_MT11] = {"Firmware %.200s uploaded and synced to the microSD card. The updater checks about every 5 seconds and should start soon; do not interrupt power.", "固件 %.200s 已上传并同步到 microSD 卡。升级程序约每 5 秒检查一次，应很快开始；请勿断电。", "ファームウェア %.200s をアップロードし、microSD カードに同期しました。アップデーターは約 5 秒ごとに確認するため、まもなく開始されます。電源を切らないでください。"};
    result[S_FW_UPLOADED_A8] = {"Firmware %.200s uploaded and synced to the card as %s. Reboot the camera to install it; do not interrupt power while it installs.", "固件 %.200s 已上传并以 %s 同步到卡上。重启相机以安装；安装期间请勿断电。", "ファームウェア %.200s をアップロードし、%s としてカードに同期しました。インストールするにはカメラを再起動してください。インストール中は電源を切らないでください。"};
    result[S_FW_INSTALLED_Z1] = {"Firmware %.200s installed. The camera is rebooting; do not interrupt power.", "固件 %.200s 已安装。相机正在重启，请勿断电。", "ファームウェア %.200s をインストールしました。カメラが再起動中です。電源を切らないでください。"};
    result[S_E_FW_PACKAGE] = {"Invalid firmware package: %s", "固件升级包无效：%s", "ファームウェアパッケージが無効です: %s"};
    result[S_E_FW_INSTALL] = {"Firmware installation failed: %s", "固件安装失败：%s", "ファームウェアのインストールに失敗しました: %s"};
    result[S_E_FW_TMP] = {"Cannot store the upload in %s: %s", "无法将上传内容保存到 %s：%s", "%s にアップロードを保存できません: %s"};
    result[S_E_FW_TMP_SPACE] = {"Not enough free space in %s", "%s 剩余空间不足", "%s の空き容量が不足しています"};
    result[S_E_FW_FAILED] = {"Firmware upload failed after %zu bytes: %s", "固件上传在 %zu 字节后失败：%s", "ファームウェアのアップロードが %zu バイトで失敗しました: %s"};
    result[S_JS_FW_TIMEOUT] = {"Timed out after 120 seconds waiting for the camera. Check its power and network connection before retrying.", "等待相机超过 120 秒已超时。重试前请检查其电源和网络连接。", "カメラの応答を 120 秒待ちましたがタイムアウトしました。再試行の前に電源とネットワーク接続を確認してください。"};
    result[S_JS_FW_TIMEOUT_ALERT] = {"The camera did not return within 120 seconds. Check its power and network connection.", "相机在 120 秒内未恢复。请检查其电源和网络连接。", "カメラが 120 秒以内に復帰しませんでした。電源とネットワーク接続を確認してください。"};
    result[S_JS_FW_BACK] = {"Camera restarted and is back online.", "相机已重启并恢复在线。", "カメラが再起動し、オンラインに復帰しました。"};
    result[S_JS_FW_BACK_LOGIN] = {"The camera is responding again; log in to continue.", "相机已有响应，请重新登录以继续。", "カメラから応答がありました。続けるには再度ログインしてください。"};
    result[S_JS_FW_BACK_ALERT] = {"The camera is back online after the firmware upgrade.", "固件升级后相机已恢复在线。", "ファームウェア更新後、カメラがオンラインに復帰しました。"};
    result[S_JS_FW_REBOOTING] = {"Camera is rebooting; waiting for it to return…", "相机正在重启，等待其恢复…", "カメラが再起動中です。復帰を待っています…"};
    result[S_JS_FW_WAITING_UPDATER] = {"Waiting for the updater to reboot the camera…", "等待升级程序重启相机…", "アップデーターによるカメラの再起動を待っています…"};
    result[S_JS_FW_NAME] = {"Filename must match %s using ASCII letters, digits, dot, underscore or hyphen.", "文件名必须符合 %s，且只能使用 ASCII 字母、数字、点、下划线或连字符。", "ファイル名は %s の形式で、ASCII の英字・数字・ドット・アンダースコア・ハイフンのみ使用できます。"};
    result[S_JS_FW_SIZE] = {"Firmware size must be between 1 byte and 128 MiB.", "固件大小必须在 1 字节至 128 MiB 之间。", "ファームウェアのサイズは 1 バイト〜128 MiB である必要があります。"};
    result[S_JS_FW_CONFIRM] = {"Upload %s and start the automatic firmware upgrade? Do not interrupt camera power.", "上传 %s 并开始自动固件升级？请勿中断相机电源。", "%s をアップロードして自動ファームウェア更新を開始しますか？カメラの電源を切らないでください。"};
    result[S_JS_FW_CONFIRM_A8] = {"Upload %s? Reboot the camera afterwards to install it, and do not interrupt power while it installs.", "上传 %s？上传完成后需重启相机以安装固件，安装期间请勿断电。", "%s をアップロードしますか？アップロード後、インストールするにはカメラを再起動してください。インストール中は電源を切らないでください。"};
    result[S_JS_FW_CONFIRM_Z1] = {"Upload and install %s? The camera reboots when installation completes; do not interrupt power.", "上传并安装 %s？安装完成后相机将重启，请勿断电。", "%s をアップロードしてインストールしますか？インストール完了後にカメラが再起動します。電源を切らないでください。"};
    result[S_JS_FW_INSTALLED] = {"Firmware installed. Waiting for the camera to reboot…", "固件已安装。等待相机重启…", "ファームウェアをインストールしました。カメラの再起動を待っています…"};
    result[S_JS_FW_WRITING] = {"Writing %s…", "正在写入 %s…", "%s を書き込んでいます…"};
    result[S_JS_FW_HTTP] = {"Upload returned HTTP ", "上传返回 HTTP ", "アップロードの応答: HTTP "};
    result[S_JS_FW_UPLOADED] = {"Firmware uploaded. Waiting for the updater to reboot the camera…", "固件已上传。等待升级程序重启相机…", "ファームウェアをアップロードしました。アップデーターによるカメラの再起動を待っています…"};
    result[S_JS_FW_CLOSED] = {"Connection closed during upload. Check camera status before retrying.", "上传期间连接已关闭。重试前请检查相机状态。", "アップロード中に接続が閉じられました。再試行の前にカメラの状態を確認してください。"};
    result[S_E_BODY_TOO_LARGE] = {"Request body too large", "请求正文过大", "リクエスト本文が大きすぎます"};
    result[S_E_HEADERS_TOO_LARGE] = {"Headers too large", "请求头过大", "ヘッダーが大きすぎます"};
    result[S_E_MALFORMED] = {"Malformed HTTP request", "HTTP 请求格式错误", "不正な HTTP リクエストです"};
    result[S_E_LIVE_UNAVAILABLE] = {"Native live video is not available", "原生实时视频不可用", "ネイティブライブ映像を利用できません"};
    result[S_E_LIVE_H264] = {"This stream must be configured for H.264", "此视频流必须配置为 H.264", "このストリームは H.264 に設定する必要があります"};
    result[S_ZT30_THERMAL_HFOV] = {"Thermal horizontal FOV (0.01 degrees)", "热成像水平视场角（0.01度）", "熱画像の水平画角（0.01度）"};
    result[S_ZT30_THERMAL_HFOV_HELP] = {"Default 2420 assumes the MT11 horizontal FOV of 24.2 degrees. Replace it with a measured ZT30 value when available. 0 disables thermal FOV geometry.", "默认2420假设与MT11相同，即24.2度。可替换为ZT30实测值；0表示禁用热成像视场几何。", "既定値2420はMT11と同じ24.2度と仮定します。ZT30の実測値が得られたら置き換えてください。0は熱画像の画角計算を無効にします。"};
    result[S_ZT30_NATIVE_PARAMETERS] = {"Image controls apply to both RGB lenses without restarting. Overlays and survey settings also apply without restarting. Video formats and network addresses apply after restarting the application.", "图像控制无需重启即可应用于两个 RGB 镜头。叠加和测绘设置也无需重启。视频格式和网络地址在重启应用程序后生效。", "画像調整は再起動せずに両方の RGB レンズへ適用されます。オーバーレイと測量設定も再起動せずに適用されます。映像形式とネットワークアドレスはアプリの再起動後に適用されます。"};
    result[S_ZT30_FIXED_VIDEO] = {"Browser preview: RGB %u × %u H264; thermal %u × %u H264. The RGB RTSP format is configured below.", "浏览器预览：RGB %u × %u H264；热成像 %u × %u H264。RGB RTSP 格式可在下方设置。", "ブラウザープレビュー: RGB %u × %u H264、熱画像 %u × %u H264。RGB RTSP 形式は以下で設定できます。"};
    result[S_JS_FW_CONFIRM_APP] = {"Install %s? The camera application and web interface will restart.", "安装 %s？相机应用程序和网页界面将重启。", "%s をインストールしますか？カメラアプリとウェブ画面が再起動します。"};
    result[S_APP_UPDATE_PENDING] = {"Checking the new application and web interface…", "正在检查新的应用程序和网页界面…", "新しいアプリとウェブ画面を確認しています…"};
    result[S_APP_UPDATE_FAILED] = {"The application update was rolled back. Check camera status.", "应用程序更新已回滚。请检查相机状态。", "アプリの更新を元に戻しました。カメラの状態を確認してください。"};
    result[S_APP_ROLLBACK] = {"Restore previous application", "恢复上一个应用程序", "以前のアプリに戻す"};
    result[S_BUNDLE_SELECTED] = {"Selected application", "选定的应用程序", "選択されたアプリ"};
    result[S_BUNDLE_CAMERA] = {"Running camera bundle", "运行中的相机程序包", "実行中のカメラパッケージ"};
    result[S_BUNDLE_WEB] = {"Running web bundle", "运行中的网页程序包", "実行中のウェブパッケージ"};
    result[S_BUNDLE_STATE] = {"Application update", "应用程序更新", "アプリの更新"};
    return result;
}();
/* ---- end of user-visible strings ---- */
