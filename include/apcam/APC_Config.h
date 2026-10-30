#pragma once

#include "config.h"
#include "target.h"
#include "lens.h"
#include "network.h"
#include "APC_Timezone.h"
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// One schema for INI parsing, web validation and numeric MAVLink/XML/log
// metadata. Display translations belong to the UI, never to this schema.
class APC_Config {
public:
    enum class Kind {
        Timezone,
        String,
        Enum,
        Int,
        Bool,
        Uint,
    };

    struct Field {
        const char *section;
        const char *key;
        Kind kind;
        size_t offset;
        size_t size;
        const struct ca_config_option *options;
        size_t option_count;
        int minimum;
        int maximum;
        const char *param_name;
    };

private:
    inline static constexpr ca_config_option photo_scope_options[] = {
        {"thermal", CA_PHOTO_SCOPE_THERMAL},
        {"all", CA_PHOTO_SCOPE_ALL},
    };

#if APCAM_HAVE_PHOTO_RESOLUTION
    inline static constexpr ca_config_option photo_resolution_options[] = {
        {"video", CA_PHOTO_RES_VIDEO},
        {APCAM_PHOTO_BINNED_NAME, CA_PHOTO_RES_BINNED},
        {APCAM_PHOTO_FULL_NAME, CA_PHOTO_RES_FULL},
    };
#endif
    inline static constexpr ca_config_option autorecord_options[] = {
        {"false", CA_AUTORECORD_DISABLED}, {"true", CA_AUTORECORD_ENABLED},
        {"while_armed", CA_AUTORECORD_WHILE_ARMED},
    };

    inline static constexpr ca_config_option orientation_options[] = {
        {"auto", CA_MOUNT_AUTO}, {"upright", CA_MOUNT_UPRIGHT},
        {"inverted", CA_MOUNT_INVERTED},
    };
    inline static constexpr ca_config_option uart_protocol_options[] = {
        {"none", CA_UART_NONE},
#if APCAM_HAVE_SIYI
        // Retain saved configurations and explicitly supplied test/adapter
        // UARTs even when the physical connector is not owned by Linux.
        {"siyi", CA_UART_SIYI},
#endif
        {"mavlink", CA_UART_MAVLINK},
    };
    inline static constexpr ca_config_option main_resolution_options[] = {
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_2160P
        {"3840x2160", CA_VIDEO_2160P},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_1440P
        {"2560x1440", CA_VIDEO_1440P},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_1080P
        {"1920x1080", CA_VIDEO_1080P},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_720P
        {"1280x720", CA_VIDEO_720P},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_540P
        {"960x540", CA_VIDEO_540P},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_480P
        {"854x480", CA_VIDEO_480P},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_360P
        {"640x360", CA_VIDEO_360P},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_240P
        {"426x240", CA_VIDEO_240P},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_180P
        {"320x180", CA_VIDEO_180P},
#endif
    };
    inline static constexpr ca_config_option sub_resolution_options[] = {
#if APCAM_SUB_RESOLUTIONS & APCAM_RES_MASK_2160P
        {"3840x2160", CA_VIDEO_2160P},
#endif
#if APCAM_SUB_RESOLUTIONS & APCAM_RES_MASK_1440P
        {"2560x1440", CA_VIDEO_1440P},
#endif
#if APCAM_SUB_RESOLUTIONS & APCAM_RES_MASK_1080P
        {"1920x1080", CA_VIDEO_1080P},
#endif
#if APCAM_SUB_RESOLUTIONS & APCAM_RES_MASK_720P
        {"1280x720", CA_VIDEO_720P},
#endif
    };
    inline static constexpr ca_config_option recording_resolution_options[] = {
#if APCAM_RECORDING_RESOLUTIONS & APCAM_RES_MASK_2160P
        {"3840x2160", CA_VIDEO_2160P},
#endif
#if APCAM_RECORDING_RESOLUTIONS & APCAM_RES_MASK_1440P
        {"2560x1440", CA_VIDEO_1440P},
#endif
#if APCAM_RECORDING_RESOLUTIONS & APCAM_RES_MASK_1080P
        {"1920x1080", CA_VIDEO_1080P},
#endif
#if APCAM_RECORDING_RESOLUTIONS & APCAM_RES_MASK_720P
        {"1280x720", CA_VIDEO_720P},
#endif
    };
    inline static constexpr ca_config_option codec_options[] = {
        { "h264", CA_VIDEO_H264 },
#if APCAM_STREAM_CODECS & 2
        { "h265", CA_VIDEO_H265 },
#endif
    };
    inline static constexpr ca_config_option telemetry_options[] = {
        {"apcg", CA_VIDEO_TELEMETRY_APCG},
        {"misb", CA_VIDEO_TELEMETRY_MISB},
    };
    inline static constexpr ca_config_option palette_options[] = {
        {"white_hot", CA_PALETTE_WHITE_HOT}, {"sepia", CA_PALETTE_SEPIA},
        {"ironbow", CA_PALETTE_IRONBOW}, {"rainbow", CA_PALETTE_RAINBOW},
        {"night", CA_PALETTE_NIGHT}, {"aurora", CA_PALETTE_AURORA},
        {"red_hot", CA_PALETTE_RED_HOT}, {"jungle", CA_PALETTE_JUNGLE},
        {"medical", CA_PALETTE_MEDICAL}, {"black_hot", CA_PALETTE_BLACK_HOT},
        {"glory_hot", CA_PALETTE_GLORY_HOT},
    };
    inline static constexpr ca_config_option iso_options[] = {
        {"auto", CA_ISO_AUTO}, {"100", CA_ISO_100}, {"200", CA_ISO_200},
        {"400", CA_ISO_400}, {"800", CA_ISO_800}, {"1600", CA_ISO_1600},
        {"3200", CA_ISO_3200},
    };
    inline static constexpr ca_config_option shutter_options[] = {
        {"auto", CA_SHUTTER_AUTO}, {"1/30", CA_SHUTTER_1_30},
        {"1/50", CA_SHUTTER_1_50}, {"1/100", CA_SHUTTER_1_100},
        {"1/250", CA_SHUTTER_1_250}, {"1/500", CA_SHUTTER_1_500},
        {"1/750", CA_SHUTTER_1_750}, {"1/1000", CA_SHUTTER_1_1000},
        {"1/2000", CA_SHUTTER_1_2000},
    };
    inline static constexpr ca_config_option metering_options[] = {
        {"average", CA_METERING_AVERAGE}, {"center", CA_METERING_CENTER},
        {"spot", CA_METERING_SPOT},
    };
    inline static constexpr ca_config_option wb_options[] = {
        {"auto", CA_WB_AUTO}, {"daylight", CA_WB_DAYLIGHT},
        {"cloudy", CA_WB_CLOUDY}, {"fluorescent", CA_WB_FLUORESCENT},
        {"incandescent", CA_WB_INCANDESCENT},
    };

    inline static constexpr ca_config_option tracking_options[] = {
        {"angle", CA_TRACK_ANGLE}, {"rate", CA_TRACK_RATE},
    };

    inline static constexpr ca_config_option survey_patterns[] = {
        {"both", CA_SURVEY_BOTH}, {"left_right", CA_SURVEY_LEFT_RIGHT}, {"fore_aft", CA_SURVEY_FORE_AFT},
        {"fore_only", CA_SURVEY_FORE_ONLY},
    };

    inline static constexpr ca_config_option survey_lenses[] = {
#if APCAM_HAVE_THERMAL || defined(CAMERA_APP_ZT30_DEV)
        {"thermal", 0},
#endif
        {"wide", 1},
#if APCAM_HAVE_ZOOM_LENS
        {"zoom", 2},
#endif
    };

    inline static constexpr Field _fields[] = {
        {"video", "telemetry", Kind::Enum, offsetof(ca_config, video_telemetry_format),
         sizeof(ca_video_telemetry_format), telemetry_options,
         sizeof(telemetry_options) / sizeof(telemetry_options[0]), 0, 0, "VIDEO_META_FMT"},
#ifdef CAMERA_APP_ZT30_DEV
        {"thermal", "hfov_cd", Kind::Uint, offsetof(ca_config, thermal_hfov_cd), sizeof(unsigned), NULL, 0, 0, 17900, "THERMAL_HFOV_CD"},
#endif
        {"survey", "pattern", Kind::Enum, offsetof(ca_config, survey.pattern), sizeof(unsigned),
         survey_patterns, sizeof(survey_patterns)/sizeof(survey_patterns[0]), 0, 0, "SURV_PATTERN"},
        {"survey", "lens", Kind::Enum, offsetof(ca_config, survey.lens), sizeof(unsigned),
         survey_lenses, sizeof(survey_lenses)/sizeof(survey_lenses[0]), 0, 0, "SURV_LENS"},
        {"survey", "overlap", Kind::Uint, offsetof(ca_config, survey.overlap), sizeof(unsigned), NULL, 0, 0, 90, "SURV_OVERLAP"},
        {"survey", "fore_pct", Kind::Uint, offsetof(ca_config, survey.fore_pct), sizeof(unsigned), NULL, 0, 10, 200, "SURV_FORE"},
        {"survey", "aft_pct", Kind::Uint, offsetof(ca_config, survey.aft_pct), sizeof(unsigned), NULL, 0, 10, 200, "SURV_AFT"},
        {"survey", "burst", Kind::Uint, offsetof(ca_config, survey.burst), sizeof(unsigned), NULL, 0, 1, 25, "SURV_BURST"},
        {"survey", "burst_ms", Kind::Uint, offsetof(ca_config, survey.burst_ms), sizeof(unsigned), NULL, 0, 40, 2000, "SURV_BURST_MS"},
        {"survey", "dwell_ms", Kind::Uint, offsetof(ca_config, survey.dwell_ms), sizeof(unsigned), NULL, 0, 200, 10000, "SURV_DWELL_MS"},
        {"survey", "settle_ms", Kind::Uint, offsetof(ca_config, survey.settle_ms), sizeof(unsigned), NULL, 0, 0, 2000, "SURV_SETTLE_MS"},
        {"survey", "error_cd", Kind::Uint, offsetof(ca_config, survey.error_cd), sizeof(unsigned), NULL, 0, 10, 1000, "SURV_ERROR_CD"},
        {"survey", "rate_cd", Kind::Uint, offsetof(ca_config, survey.rate_cd), sizeof(unsigned), NULL, 0, 10, 2000, "SURV_RATE_CD"},
        {"survey", "terrain_ms", Kind::Uint, offsetof(ca_config, survey.terrain_ms), sizeof(unsigned), NULL, 0, 500, 10000, "SURV_TERR_MS"},
        {"survey", "terrain_m", Kind::Uint, offsetof(ca_config, survey.terrain_m), sizeof(unsigned), NULL, 0, 50, 2000, "SURV_TERR_M"},
        {"survey", "terrain_tol", Kind::Uint, offsetof(ca_config, survey.terrain_tol), sizeof(unsigned), NULL, 0, 1, 100, "SURV_TERR_TOL"},
        {"survey", "min_speed", Kind::Uint, offsetof(ca_config, survey.min_speed), sizeof(unsigned), NULL, 0, 1, 30, "SURV_MIN_SPEED"},
        {"survey", "turn_deg", Kind::Uint, offsetof(ca_config, survey.turn_deg), sizeof(unsigned), NULL, 0, 5, 45, "SURV_TURN_DEG"},

        {"logging", "disarmed", Kind::Bool, offsetof(struct ca_config, log_disarmed),
         sizeof(((struct ca_config *)0)->log_disarmed), NULL, 0U, 0, 0, "LOG_DISARMED"},
        {"general", "timezone", Kind::Timezone,
         offsetof(struct ca_config, timezone),
         sizeof(((struct ca_config *)0)->timezone), NULL, 0U, 0, 0, NULL},
        {"capture", "photo_scope", Kind::Enum,
         offsetof(struct ca_config, photo_scope),
         sizeof(((struct ca_config *)0)->photo_scope), photo_scope_options,
         sizeof(photo_scope_options) / sizeof(photo_scope_options[0]), 0, 0, "PHOTO_SCOPE"},
#if APCAM_HAVE_PHOTO_RESOLUTION
        {"capture", "resolution", Kind::Enum,
         offsetof(struct ca_config, photo_resolution),
         sizeof(((struct ca_config *)0)->photo_resolution), photo_resolution_options,
         sizeof(photo_resolution_options) / sizeof(photo_resolution_options[0]), 0, 0, "PHOTO_RES"},
#endif
        {"mount", "orientation", Kind::Enum, offsetof(struct ca_config, orientation),
         sizeof(((struct ca_config *)0)->orientation), orientation_options,
         sizeof(orientation_options) / sizeof(orientation_options[0]), 0, 0, "MOUNT_ORIENT"},
        {"uart", "protocol", Kind::Enum, offsetof(struct ca_config, uart_protocol),
         sizeof(((struct ca_config *)0)->uart_protocol), uart_protocol_options,
         sizeof(uart_protocol_options) / sizeof(uart_protocol_options[0]), 0, 0, "UART_PROTOCOL"},
        {"thermal", "palette", Kind::Enum, offsetof(struct ca_config, thermal_palette),
         sizeof(((struct ca_config *)0)->thermal_palette), palette_options,
         sizeof(palette_options) / sizeof(palette_options[0]), 0, 0, "THERMAL_PALETTE"},
        {"recording", "autorecord", Kind::Enum, offsetof(struct ca_config, autorecord),
         sizeof(((struct ca_config *)0)->autorecord), autorecord_options,
         sizeof(autorecord_options) / sizeof(autorecord_options[0]), 0, 0, "REC_AUTOSTART"},
        {"recording", "resolution", Kind::Enum,
         offsetof(struct ca_config, recording_resolution),
         sizeof(((struct ca_config *)0)->recording_resolution), recording_resolution_options,
         sizeof(recording_resolution_options) / sizeof(recording_resolution_options[0]), 0, 0, "REC_RESOLUTION"},
        {"stream.main", "resolution", Kind::Enum,
         offsetof(struct ca_config, main_resolution),
         sizeof(((struct ca_config *)0)->main_resolution), main_resolution_options,
         sizeof(main_resolution_options) / sizeof(main_resolution_options[0]), 0, 0, "VIDEO_MAIN_RES"},
        {"stream.main", "codec", Kind::Enum, offsetof(struct ca_config, main_codec),
         sizeof(((struct ca_config *)0)->main_codec), codec_options,
         sizeof(codec_options) / sizeof(codec_options[0]), 0, 0, "VIDEO_MAIN_CODEC"},
#if APCAM_HAVE_VIDEO_BITRATE
        {"stream.main", "bitrate_kbps", Kind::Uint, offsetof(struct ca_config, main_bitrate_kbps),
         sizeof(((struct ca_config *)0)->main_bitrate_kbps), NULL, 0U,
         APCAM_MAIN_BITRATE_MIN_KBPS, APCAM_MAIN_BITRATE_MAX_KBPS, "VIDEO_MAIN_KBPS"},
#endif
#if APCAM_HAVE_VIDEO_FRAME_RATE
        {"stream.main", "frame_rate", Kind::Uint, offsetof(struct ca_config, main_frame_rate),
         sizeof(((struct ca_config *)0)->main_frame_rate), NULL, 0U,
         APCAM_MAIN_FPS_MIN, APCAM_FRAME_RATE, "VIDEO_MAIN_FPS"},
#endif
        {"stream.sub", "resolution", Kind::Enum,
         offsetof(struct ca_config, sub_resolution),
         sizeof(((struct ca_config *)0)->sub_resolution), sub_resolution_options,
         sizeof(sub_resolution_options) / sizeof(sub_resolution_options[0]), 0, 0, "VIDEO_SUB_RES"},
        {"stream.sub", "codec", Kind::Enum, offsetof(struct ca_config, sub_codec),
         sizeof(((struct ca_config *)0)->sub_codec), codec_options,
         sizeof(codec_options) / sizeof(codec_options[0]), 0, 0, "VIDEO_SUB_CODEC"},
        {"stream.main", "alias", Kind::String, offsetof(struct ca_config, main_alias),
         sizeof(((struct ca_config *)0)->main_alias), NULL, 0U, 0, 63, NULL},
        {"stream.sub", "alias", Kind::String, offsetof(struct ca_config, sub_alias),
         sizeof(((struct ca_config *)0)->sub_alias), NULL, 0U, 0, 63, NULL},
        {"overlay", "cross", Kind::Bool, offsetof(struct ca_config, osd_cross),
         sizeof(((struct ca_config *)0)->osd_cross), NULL, 0U, 0, 0, "OSD_CROSS"},
#if APCAM_HAVE_OVERLAY_RECORDING_SELECT
        {"overlay", "recording", Kind::Bool, offsetof(struct ca_config, osd_recording),
         sizeof(((struct ca_config *)0)->osd_recording), NULL, 0U, 0, 0, "OSD_RECORD"},
#endif
#if APCAM_HAVE_THERMAL || defined(CAMERA_APP_ZT30_DEV)
        {"overlay", "thermal_fov", Kind::Bool, offsetof(struct ca_config, osd_thermal_fov),
         sizeof(((struct ca_config *)0)->osd_thermal_fov), NULL, 0U, 0, 0, "OSD_THERMAL_FOV"},
#endif
        {"image", "brightness", Kind::Int, offsetof(struct ca_config, brightness),
         sizeof(((struct ca_config *)0)->brightness), NULL, 0U, 0, 100, "IMG_BRIGHTNESS"},
        {"image", "saturation", Kind::Int, offsetof(struct ca_config, saturation),
         sizeof(((struct ca_config *)0)->saturation), NULL, 0U, 0, 100, "IMG_SATURATION"},
        {"image", "contrast", Kind::Int, offsetof(struct ca_config, contrast),
         sizeof(((struct ca_config *)0)->contrast), NULL, 0U, 0, 100, "IMG_CONTRAST"},
        {"image", "exposure_compensation", Kind::Int,
         offsetof(struct ca_config, exposure_compensation),
         sizeof(((struct ca_config *)0)->exposure_compensation), NULL, 0U, -10, 10, "IMG_EXPOSURE"},
        {"image", "iso", Kind::Enum, offsetof(struct ca_config, iso),
         sizeof(((struct ca_config *)0)->iso), iso_options,
         sizeof(iso_options) / sizeof(iso_options[0]), 0, 0, "IMG_ISO"},
        {"image", "shutter", Kind::Enum, offsetof(struct ca_config, shutter),
         sizeof(((struct ca_config *)0)->shutter), shutter_options,
         sizeof(shutter_options) / sizeof(shutter_options[0]), 0, 0, "IMG_SHUTTER"},
        {"image", "metering", Kind::Enum, offsetof(struct ca_config, metering),
         sizeof(((struct ca_config *)0)->metering), metering_options,
         sizeof(metering_options) / sizeof(metering_options[0]), 0, 0, "IMG_METERING"},
        {"image", "white_balance", Kind::Enum,
         offsetof(struct ca_config, white_balance),
         sizeof(((struct ca_config *)0)->white_balance), wb_options,
         sizeof(wb_options) / sizeof(wb_options[0]), 0, 0, "IMG_WHITE_BAL"},
        {"mavlink", "position_targeting", Kind::Bool,
         offsetof(struct ca_config, position_targeting),
         sizeof(((struct ca_config *)0)->position_targeting), NULL, 0U, 0, 0, "MAV_POS_TARGET"},
        {"mavlink", "tracking_method", Kind::Enum, offsetof(struct ca_config, tracking_method),
         sizeof(((struct ca_config *)0)->tracking_method), tracking_options,
         sizeof(tracking_options) / sizeof(tracking_options[0]), 0, 0, "TRACK_METHOD"},
        {"mavlink", "tcp_port", Kind::Uint,
         offsetof(struct ca_config, mavlink_tcp_port),
         sizeof(((struct ca_config *)0)->mavlink_tcp_port), NULL, 0U, 0, 65535, "MAV_TCP_PORT"},
        {"mavlink", "udp_port", Kind::Uint,
         offsetof(struct ca_config, mavlink_udp_port),
         sizeof(((struct ca_config *)0)->mavlink_udp_port), NULL, 0U, 0, 65535, "MAV_UDP_PORT"},
        {"mavlink", "system_id", Kind::Uint,
         offsetof(struct ca_config, mavlink_system_id),
         sizeof(((struct ca_config *)0)->mavlink_system_id), NULL, 0U, 0, 255,
         "MAV_SYSID"},
        {"mavlink", "camera_component_id", Kind::Uint,
         offsetof(struct ca_config, mavlink_camera_component_id),
         sizeof(((struct ca_config *)0)->mavlink_camera_component_id), NULL, 0U, 100, 105,
         "MAV_CAM_COMP_ID"},
        {"support_proxy", "enabled", Kind::Bool,
         offsetof(struct ca_config, support.enabled),
         sizeof(((struct ca_config *)0)->support.enabled), NULL, 0U, 0, 1, "PROXY_ENABLE"},
        {"support_proxy", "host", Kind::String,
         offsetof(struct ca_config, support.host),
         sizeof(((struct ca_config *)0)->support.host), NULL, 0U, 0, 127, NULL},
        {"support_proxy", "mavlink_port", Kind::Uint,
         offsetof(struct ca_config, support.mavlink_port),
         sizeof(((struct ca_config *)0)->support.mavlink_port), NULL, 0U, 0, 65535, "PROXY_MAV_PORT"},
        {"support_proxy", "signing", Kind::Bool,
         offsetof(struct ca_config, support.signing),
         sizeof(((struct ca_config *)0)->support.signing), NULL, 0U, 0, 1, "PROXY_SIGN"},
        {"support_proxy", "signing_passphrase", Kind::String,
         offsetof(struct ca_config, support.signing_passphrase),
         sizeof(((struct ca_config *)0)->support.signing_passphrase), NULL, 0U, 0, 127, NULL},
        {"support_proxy", "signing_link_id", Kind::Uint,
         offsetof(struct ca_config, support.signing_link_id),
         sizeof(((struct ca_config *)0)->support.signing_link_id), NULL, 0U, 0, 255, "PROXY_SIGN_ID"},
        {"support_proxy", "video1_port", Kind::Uint,
         offsetof(struct ca_config, support.video1_port),
         sizeof(((struct ca_config *)0)->support.video1_port), NULL, 0U, 0, 65535, "PROXY_VID1_PORT"},
        {"support_proxy", "video2_port", Kind::Uint,
         offsetof(struct ca_config, support.video2_port),
         sizeof(((struct ca_config *)0)->support.video2_port), NULL, 0U, 0, 65535, "PROXY_VID2_PORT"},
        {"support_proxy", "video1_name", Kind::String,
         offsetof(struct ca_config, support.video1_name),
         sizeof(((struct ca_config *)0)->support.video1_name), NULL, 0U, 0, 63, NULL},
        {"support_proxy", "video2_name", Kind::String,
         offsetof(struct ca_config, support.video2_name),
         sizeof(((struct ca_config *)0)->support.video2_name), NULL, 0U, 0, 63, NULL},
#if APCAM_HAVE_THERMAL
        {"support_proxy", "video3_port", Kind::Uint,
         offsetof(struct ca_config, support.video3_port),
         sizeof(((struct ca_config *)0)->support.video3_port), NULL, 0U, 0, 65535, "PROXY_VID3_PORT"},
        {"support_proxy", "video3_name", Kind::String,
         offsetof(struct ca_config, support.video3_name),
         sizeof(((struct ca_config *)0)->support.video3_name), NULL, 0U, 0, 63, NULL},
#endif
        {"support_proxy", "publish_password", Kind::String,
         offsetof(struct ca_config, support.publish_password),
         sizeof(((struct ca_config *)0)->support.publish_password), NULL, 0U, 0, 127, NULL},
        {"support_proxy", "network_interface", Kind::String,
         offsetof(struct ca_config, support.network_interface),
         sizeof(((struct ca_config *)0)->support.network_interface), NULL, 0U, 1, 15, NULL},
        {"support_proxy", "network_address", Kind::String,
         offsetof(struct ca_config, support.network_address),
         sizeof(((struct ca_config *)0)->support.network_address), NULL, 0U, 0, 31, NULL},
        {"support_proxy", "network_gateway", Kind::String,
         offsetof(struct ca_config, support.network_gateway),
         sizeof(((struct ca_config *)0)->support.network_gateway), NULL, 0U, 0, 15, NULL},
        {"network", "interface", Kind::String,
         offsetof(struct ca_config, network.interface),
         sizeof(((struct ca_config *)0)->network.interface), NULL, 0U, 1, 15, NULL},
        {"network", "primary_address", Kind::String,
         offsetof(struct ca_config, network.primary_address),
         sizeof(((struct ca_config *)0)->network.primary_address), NULL, 0U, 0, 31, NULL},
        {"network", "secondary_address", Kind::String,
         offsetof(struct ca_config, network.secondary_address),
         sizeof(((struct ca_config *)0)->network.secondary_address), NULL, 0U, 0, 31, NULL},
        {"network", "gateway", Kind::String,
         offsetof(struct ca_config, network.gateway),
         sizeof(((struct ca_config *)0)->network.gateway), NULL, 0U, 0, 15, NULL},
        {"network", "capture", Kind::Bool,
         offsetof(struct ca_config, network_capture),
         sizeof(((struct ca_config *)0)->network_capture), NULL, 0U, 0, 1, NULL},
#if !APCAM_HAVE_NETWORK_CONFIG
        {"network", "video1_uri", Kind::String,
         offsetof(struct ca_config, video1_uri),
         sizeof(((struct ca_config *)0)->video1_uri), NULL, 0U, 0, 159, NULL},
#endif
#if APCAM_HAVE_THERMAL || APCAM_HAVE_RAW_THERMAL
        {"thermal", "stream_fps", Kind::Uint, offsetof(struct ca_config, raw_stream_fps),
         sizeof(((struct ca_config *)0)->raw_stream_fps), NULL, 0U, 1, 25, "RAW_STREAM_FPS"},
#endif
#if APCAM_HAVE_THERMAL || APCAM_HAVE_RAW_THERMAL
        {"thermal", "record_fps", Kind::Uint, offsetof(struct ca_config, raw_record_fps),
         sizeof(((struct ca_config *)0)->raw_record_fps), NULL, 0U, 0, 25, "RAW_RECORD_FPS"},
#endif
    };

public:
    static constexpr size_t count() { return sizeof(_fields) / sizeof(_fields[0]); }
    static constexpr const Field *fields() { return _fields; }
    static const Field *field(size_t index) { return index < count() ? &_fields[index] : nullptr; }
    static const Field *find(const char *section, const char *key)
    {
        if (!section || !key) return nullptr;
        for (const auto &item : _fields) {
            if (!strcmp(item.section, section) && !strcmp(item.key, key)) return &item;
        }
        return nullptr;
    }
    static bool validate(const Field &field, const char *value)
    {
        ca_config scratch {};
        return value && set_field(&scratch, &field, value) == 0;
    }
    static bool assign(ca_config &config, const Field &field, const char *value)
    {
        return value && set_field(&config, &field, value) == 0;
    }
    static bool support_valid(const struct ca_support_config *support)
    {
        /* A target with one stream has no video2: its stored port and name are
         * ignored, as the web page does not offer them. */
        const bool video2 = APCAM_NUM_STREAMS >= 2;
        return !support->enabled ||
            (support->host[0] && (!support->signing || support->signing_passphrase[0]) &&
             (!support->video1_port || support->video1_name[0]) &&
             (!video2 || !support->video2_port || support->video2_name[0]) &&
             (!video2 || !support->video1_port || support->video1_port != support->video2_port) &&
             (!support->video3_port || (support->video3_name[0] &&
               support->video3_port != support->video1_port &&
               (!video2 || support->video3_port != support->video2_port))));
    }

private:
    static bool valid_timezone(const char *value)
    {
        size_t length = strlen(value);

        if (length == 0U || length >= CA_CONFIG_TIMEZONE_MAX) return false;
        for (size_t i = 0; i < length; i++) {
            unsigned char c = (unsigned char)value[i];
            if (iscntrl(c) || isspace(c)) return false;
        }
        return APC_Timezone::valid(value);
    }

    static int set_field(struct ca_config *config, const Field *field,
                         const char *value)
    {
        uint8_t *destination = (uint8_t *)config + field->offset;

        if (field->kind == Kind::String) {
            size_t length = strlen(value);
            if (length >= field->size || length < (size_t)field->minimum) return -1;
            for (size_t i = 0; i < length; i++) {
                unsigned char c = (unsigned char)value[i];
                if (c < 32U || c > 126U || c == '"') return -1;
            }
            if (strcmp(field->key, "host") == 0 || strcmp(field->key, "alias") == 0 ||
                strcmp(field->key, "network_interface") == 0 || strcmp(field->key, "interface") == 0) {
                for (size_t i = 0; i < length; i++) {
                    unsigned char c = (unsigned char)value[i];
                    if (!isalnum(c) && c != '.' && c != '-' && c != '_') return -1;
                }
            }
            if (length != 0U && (strcmp(field->key, "network_gateway") == 0 || strcmp(field->key, "gateway") == 0)) {
                struct in_addr address;
                if (inet_pton(AF_INET, value, &address) != 1) return -1;
            }
            if (length != 0U && strcmp(field->key, "network_address") == 0) {
                char address[32];
                if (length >= sizeof(address)) return -1;
                memcpy(address, value, length + 1U);
                char *slash = strchr(address, '/');
                struct in_addr parsed;
                if (slash == NULL) return -1;
                *slash++ = '\0';
                char *end;
                long prefix = strtol(slash, &end, 10);
                if (end == slash || *end != '\0' || prefix < 1 || prefix > 32 ||
                    inet_pton(AF_INET, address, &parsed) != 1) return -1;
            }
            if (length && (!strcmp(field->key, "primary_address") || !strcmp(field->key, "secondary_address"))) {
                struct in_addr address;
                unsigned prefix;
                if (!apcam_ipv4_prefix(value, &address, &prefix)) return -1;
            }
            memcpy(destination, value, length + 1U);
            return 0;
        }
        if (field->kind == Kind::Timezone) {
            if (!valid_timezone(value) || strlen(value) >= field->size) return -1;
            memcpy(destination, value, strlen(value) + 1U);
            return 0;
        }
        if (field->kind == Kind::Enum && field->size == sizeof(int)) {
            for (size_t i = 0; i < field->option_count; i++) {
                if (strcmp(value, field->options[i].name) == 0) {
                    int selected = field->options[i].value;
                    memcpy(destination, &selected, sizeof(selected));
                    return 0;
                }
            }
        }
        if (field->kind == Kind::Int && field->size == sizeof(int)) {
            char *end = NULL;
            long parsed;
            errno = 0;
            parsed = strtol(value, &end, 10);
            if (errno == 0 && end != value && *end == '\0' &&
                parsed >= field->minimum && parsed <= field->maximum) {
                int selected = (int)parsed;
                memcpy(destination, &selected, sizeof(selected));
                return 0;
            }
        }
        if (field->kind == Kind::Bool && field->size == sizeof(bool)) {
            bool selected;
            if (strcmp(value, "true") == 0) selected = true;
            else if (strcmp(value, "false") == 0) selected = false;
            else return -1;
            memcpy(destination, &selected, sizeof(selected));
            return 0;
        }
        if (field->kind == Kind::Uint && field->size == sizeof(unsigned)) {
            char *end = NULL;
            unsigned long parsed;
            errno = 0;
            parsed = strtoul(value, &end, 10);
            if (errno == 0 && end != value && *end == '\0' &&
                parsed >= (unsigned long)field->minimum &&
                parsed <= (unsigned long)field->maximum) {
                unsigned selected = (unsigned)parsed;
                memcpy(destination, &selected, sizeof(selected));
                return 0;
            }
        }
        return -1;
    }

};
