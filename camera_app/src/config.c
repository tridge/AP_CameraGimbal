#define _GNU_SOURCE
#include "apcam/network.h"
#include "apcam/target.h"
#include "camera_app/config.h"

#include <ctype.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_LINE_MAX 512U

enum config_value_kind {
    CONFIG_TIMEZONE,
    CONFIG_STRING,
    CONFIG_ENUM,
    CONFIG_INT,
    CONFIG_BOOL,
    CONFIG_UINT,
};

struct config_field {
    const char *section;
    const char *key;
    enum config_value_kind kind;
    size_t offset;
    size_t size;
    const struct ca_config_option *options;
    size_t option_count;
    int minimum;
    int maximum;
    const char *param_name;
};

static const struct ca_config_option photo_scope_options[] = {
    {"thermal", CA_PHOTO_SCOPE_THERMAL},
    {"all", CA_PHOTO_SCOPE_ALL},
};

static const struct ca_config_option autorecord_options[] = {
    {"false", CA_AUTORECORD_DISABLED}, {"true", CA_AUTORECORD_ENABLED},
    {"while_armed", CA_AUTORECORD_WHILE_ARMED},
};

static const struct ca_config_option orientation_options[] = {
    {"auto", CA_MOUNT_AUTO}, {"upright", CA_MOUNT_UPRIGHT},
    {"inverted", CA_MOUNT_INVERTED},
};
static const struct ca_config_option uart_protocol_options[] = {
    {"none", CA_UART_NONE},
#if APCAM_HAVE_EXTERNAL_UART
    {"siyi", CA_UART_SIYI},
#endif
    {"mavlink", CA_UART_MAVLINK},
};
static const struct ca_config_option main_resolution_options[] = {
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_720P
    {"1280x720", CA_VIDEO_720P},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_1080P
    {"1920x1080", CA_VIDEO_1080P},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_1440P
    {"2560x1440", CA_VIDEO_1440P},
#endif
#if APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_2160P
    {"3840x2160", CA_VIDEO_2160P},
#endif
};
static const struct ca_config_option sub_resolution_options[] = {
#if APCAM_SUB_RESOLUTIONS & APCAM_RES_MASK_720P
    {"1280x720", CA_VIDEO_720P},
#endif
#if APCAM_SUB_RESOLUTIONS & APCAM_RES_MASK_1080P
    {"1920x1080", CA_VIDEO_1080P},
#endif
#if APCAM_SUB_RESOLUTIONS & APCAM_RES_MASK_1440P
    {"2560x1440", CA_VIDEO_1440P},
#endif
#if APCAM_SUB_RESOLUTIONS & APCAM_RES_MASK_2160P
    {"3840x2160", CA_VIDEO_2160P},
#endif
};
static const struct ca_config_option recording_resolution_options[] = {
#if APCAM_RECORDING_RESOLUTIONS & APCAM_RES_MASK_720P
    {"1280x720", CA_VIDEO_720P},
#endif
#if APCAM_RECORDING_RESOLUTIONS & APCAM_RES_MASK_1080P
    {"1920x1080", CA_VIDEO_1080P},
#endif
#if APCAM_RECORDING_RESOLUTIONS & APCAM_RES_MASK_1440P
    {"2560x1440", CA_VIDEO_1440P},
#endif
#if APCAM_RECORDING_RESOLUTIONS & APCAM_RES_MASK_2160P
    {"3840x2160", CA_VIDEO_2160P},
#endif
};
static const struct ca_config_option codec_options[] = {
    { "h264", CA_VIDEO_H264 },
#if APCAM_STREAM_CODECS & 2
    { "h265", CA_VIDEO_H265 },
#endif
};
static const struct ca_config_option palette_options[] = {
    {"white_hot", CA_PALETTE_WHITE_HOT}, {"sepia", CA_PALETTE_SEPIA},
    {"ironbow", CA_PALETTE_IRONBOW}, {"rainbow", CA_PALETTE_RAINBOW},
    {"night", CA_PALETTE_NIGHT}, {"aurora", CA_PALETTE_AURORA},
    {"red_hot", CA_PALETTE_RED_HOT}, {"jungle", CA_PALETTE_JUNGLE},
    {"medical", CA_PALETTE_MEDICAL}, {"black_hot", CA_PALETTE_BLACK_HOT},
    {"glory_hot", CA_PALETTE_GLORY_HOT},
};
static const struct ca_config_option iso_options[] = {
    {"auto", CA_ISO_AUTO}, {"100", CA_ISO_100}, {"200", CA_ISO_200},
    {"400", CA_ISO_400}, {"800", CA_ISO_800}, {"1600", CA_ISO_1600},
    {"3200", CA_ISO_3200},
};
static const struct ca_config_option shutter_options[] = {
    {"auto", CA_SHUTTER_AUTO}, {"1/30", CA_SHUTTER_1_30},
    {"1/50", CA_SHUTTER_1_50}, {"1/100", CA_SHUTTER_1_100},
    {"1/250", CA_SHUTTER_1_250}, {"1/500", CA_SHUTTER_1_500},
    {"1/750", CA_SHUTTER_1_750}, {"1/1000", CA_SHUTTER_1_1000},
    {"1/2000", CA_SHUTTER_1_2000},
};
static const struct ca_config_option metering_options[] = {
    {"average", CA_METERING_AVERAGE}, {"center", CA_METERING_CENTER},
    {"spot", CA_METERING_SPOT},
};
static const struct ca_config_option wb_options[] = {
    {"auto", CA_WB_AUTO}, {"daylight", CA_WB_DAYLIGHT},
    {"cloudy", CA_WB_CLOUDY}, {"fluorescent", CA_WB_FLUORESCENT},
    {"incandescent", CA_WB_INCANDESCENT},
};

static const struct ca_config_option tracking_options[] = {
    {"angle", CA_TRACK_ANGLE}, {"rate", CA_TRACK_RATE},
};

static const struct config_field config_fields[] = {
    {"logging", "disarmed", CONFIG_BOOL, offsetof(struct ca_config, log_disarmed),
     sizeof(((struct ca_config *)0)->log_disarmed), NULL, 0U, 0, 0, "LOG_DISARMED"},
    {"general", "timezone", CONFIG_TIMEZONE,
     offsetof(struct ca_config, timezone),
     sizeof(((struct ca_config *)0)->timezone), NULL, 0U, 0, 0, NULL},
    {"capture", "photo_scope", CONFIG_ENUM,
     offsetof(struct ca_config, photo_scope),
     sizeof(((struct ca_config *)0)->photo_scope), photo_scope_options,
     sizeof(photo_scope_options) / sizeof(photo_scope_options[0]), 0, 0, "PHOTO_SCOPE"},
    {"mount", "orientation", CONFIG_ENUM, offsetof(struct ca_config, orientation),
     sizeof(((struct ca_config *)0)->orientation), orientation_options,
     sizeof(orientation_options) / sizeof(orientation_options[0]), 0, 0, "MOUNT_ORIENT"},
    {"uart", "protocol", CONFIG_ENUM, offsetof(struct ca_config, uart_protocol),
     sizeof(((struct ca_config *)0)->uart_protocol), uart_protocol_options,
     sizeof(uart_protocol_options) / sizeof(uart_protocol_options[0]), 0, 0, "UART_PROTOCOL"},
    {"thermal", "palette", CONFIG_ENUM, offsetof(struct ca_config, thermal_palette),
     sizeof(((struct ca_config *)0)->thermal_palette), palette_options,
     sizeof(palette_options) / sizeof(palette_options[0]), 0, 0, "THERMAL_PALETTE"},
    {"recording", "autorecord", CONFIG_ENUM, offsetof(struct ca_config, autorecord),
     sizeof(((struct ca_config *)0)->autorecord), autorecord_options,
     sizeof(autorecord_options) / sizeof(autorecord_options[0]), 0, 0, "REC_AUTOSTART"},
    {"recording", "resolution", CONFIG_ENUM,
     offsetof(struct ca_config, recording_resolution),
     sizeof(((struct ca_config *)0)->recording_resolution), recording_resolution_options,
     sizeof(recording_resolution_options) / sizeof(recording_resolution_options[0]), 0, 0, "REC_RESOLUTION"},
    {"stream.main", "resolution", CONFIG_ENUM,
     offsetof(struct ca_config, main_resolution),
     sizeof(((struct ca_config *)0)->main_resolution), main_resolution_options,
     sizeof(main_resolution_options) / sizeof(main_resolution_options[0]), 0, 0, "VIDEO_MAIN_RES"},
    {"stream.main", "codec", CONFIG_ENUM, offsetof(struct ca_config, main_codec),
     sizeof(((struct ca_config *)0)->main_codec), codec_options,
     sizeof(codec_options) / sizeof(codec_options[0]), 0, 0, "VIDEO_MAIN_CODEC"},
    {"stream.sub", "resolution", CONFIG_ENUM,
     offsetof(struct ca_config, sub_resolution),
     sizeof(((struct ca_config *)0)->sub_resolution), sub_resolution_options,
     sizeof(sub_resolution_options) / sizeof(sub_resolution_options[0]), 0, 0, "VIDEO_SUB_RES"},
    {"stream.sub", "codec", CONFIG_ENUM, offsetof(struct ca_config, sub_codec),
     sizeof(((struct ca_config *)0)->sub_codec), codec_options,
     sizeof(codec_options) / sizeof(codec_options[0]), 0, 0, "VIDEO_SUB_CODEC"},
    {"overlay", "cross", CONFIG_BOOL, offsetof(struct ca_config, osd_cross),
     sizeof(((struct ca_config *)0)->osd_cross), NULL, 0U, 0, 0, "OSD_CROSS"},
#if APCAM_HAVE_OVERLAY_RECORDING_SELECT
    {"overlay", "recording", CONFIG_BOOL, offsetof(struct ca_config, osd_recording),
     sizeof(((struct ca_config *)0)->osd_recording), NULL, 0U, 0, 0, "OSD_RECORD"},
#endif
#if APCAM_HAVE_THERMAL
    {"overlay", "thermal_fov", CONFIG_BOOL, offsetof(struct ca_config, osd_thermal_fov),
     sizeof(((struct ca_config *)0)->osd_thermal_fov), NULL, 0U, 0, 0, "OSD_THERMAL_FOV"},
#endif
    {"image", "brightness", CONFIG_INT, offsetof(struct ca_config, brightness),
     sizeof(((struct ca_config *)0)->brightness), NULL, 0U, 0, 100, "IMG_BRIGHTNESS"},
    {"image", "saturation", CONFIG_INT, offsetof(struct ca_config, saturation),
     sizeof(((struct ca_config *)0)->saturation), NULL, 0U, 0, 100, "IMG_SATURATION"},
    {"image", "contrast", CONFIG_INT, offsetof(struct ca_config, contrast),
     sizeof(((struct ca_config *)0)->contrast), NULL, 0U, 0, 100, "IMG_CONTRAST"},
    {"image", "exposure_compensation", CONFIG_INT,
     offsetof(struct ca_config, exposure_compensation),
     sizeof(((struct ca_config *)0)->exposure_compensation), NULL, 0U, -10, 10, "IMG_EXPOSURE"},
    {"image", "iso", CONFIG_ENUM, offsetof(struct ca_config, iso),
     sizeof(((struct ca_config *)0)->iso), iso_options,
     sizeof(iso_options) / sizeof(iso_options[0]), 0, 0, "IMG_ISO"},
    {"image", "shutter", CONFIG_ENUM, offsetof(struct ca_config, shutter),
     sizeof(((struct ca_config *)0)->shutter), shutter_options,
     sizeof(shutter_options) / sizeof(shutter_options[0]), 0, 0, "IMG_SHUTTER"},
    {"image", "metering", CONFIG_ENUM, offsetof(struct ca_config, metering),
     sizeof(((struct ca_config *)0)->metering), metering_options,
     sizeof(metering_options) / sizeof(metering_options[0]), 0, 0, "IMG_METERING"},
    {"image", "white_balance", CONFIG_ENUM,
     offsetof(struct ca_config, white_balance),
     sizeof(((struct ca_config *)0)->white_balance), wb_options,
     sizeof(wb_options) / sizeof(wb_options[0]), 0, 0, "IMG_WHITE_BAL"},
    {"mavlink", "position_targeting", CONFIG_BOOL,
     offsetof(struct ca_config, position_targeting),
     sizeof(((struct ca_config *)0)->position_targeting), NULL, 0U, 0, 0, "MAV_POS_TARGET"},
    {"mavlink", "tracking_method", CONFIG_ENUM, offsetof(struct ca_config, tracking_method),
     sizeof(((struct ca_config *)0)->tracking_method), tracking_options,
     sizeof(tracking_options) / sizeof(tracking_options[0]), 0, 0, "TRACK_METHOD"},
    {"mavlink", "tcp_port", CONFIG_UINT,
     offsetof(struct ca_config, mavlink_tcp_port),
     sizeof(((struct ca_config *)0)->mavlink_tcp_port), NULL, 0U, 0, 65535, "MAV_TCP_PORT"},
    {"mavlink", "udp_port", CONFIG_UINT,
     offsetof(struct ca_config, mavlink_udp_port),
     sizeof(((struct ca_config *)0)->mavlink_udp_port), NULL, 0U, 0, 65535, "MAV_UDP_PORT"},
    {"mavlink", "system_id", CONFIG_UINT,
     offsetof(struct ca_config, mavlink_system_id),
     sizeof(((struct ca_config *)0)->mavlink_system_id), NULL, 0U, 0, 255,
     "MAV_SYSID"},
    {"mavlink", "camera_component_id", CONFIG_UINT,
     offsetof(struct ca_config, mavlink_camera_component_id),
     sizeof(((struct ca_config *)0)->mavlink_camera_component_id), NULL, 0U, 100, 105,
     "MAV_CAM_COMP_ID"},
    {"support_proxy", "enabled", CONFIG_BOOL,
     offsetof(struct ca_config, support.enabled),
     sizeof(((struct ca_config *)0)->support.enabled), NULL, 0U, 0, 1, "PROXY_ENABLE"},
    {"support_proxy", "host", CONFIG_STRING,
     offsetof(struct ca_config, support.host),
     sizeof(((struct ca_config *)0)->support.host), NULL, 0U, 0, 127, NULL},
    {"support_proxy", "mavlink_port", CONFIG_UINT,
     offsetof(struct ca_config, support.mavlink_port),
     sizeof(((struct ca_config *)0)->support.mavlink_port), NULL, 0U, 0, 65535, "PROXY_MAV_PORT"},
    {"support_proxy", "signing", CONFIG_BOOL,
     offsetof(struct ca_config, support.signing),
     sizeof(((struct ca_config *)0)->support.signing), NULL, 0U, 0, 1, "PROXY_SIGN"},
    {"support_proxy", "signing_passphrase", CONFIG_STRING,
     offsetof(struct ca_config, support.signing_passphrase),
     sizeof(((struct ca_config *)0)->support.signing_passphrase), NULL, 0U, 0, 127, NULL},
    {"support_proxy", "signing_link_id", CONFIG_UINT,
     offsetof(struct ca_config, support.signing_link_id),
     sizeof(((struct ca_config *)0)->support.signing_link_id), NULL, 0U, 0, 255, "PROXY_SIGN_ID"},
    {"support_proxy", "video1_port", CONFIG_UINT,
     offsetof(struct ca_config, support.video1_port),
     sizeof(((struct ca_config *)0)->support.video1_port), NULL, 0U, 0, 65535, "PROXY_VID1_PORT"},
    {"support_proxy", "video2_port", CONFIG_UINT,
     offsetof(struct ca_config, support.video2_port),
     sizeof(((struct ca_config *)0)->support.video2_port), NULL, 0U, 0, 65535, "PROXY_VID2_PORT"},
    {"support_proxy", "video1_name", CONFIG_STRING,
     offsetof(struct ca_config, support.video1_name),
     sizeof(((struct ca_config *)0)->support.video1_name), NULL, 0U, 0, 63, NULL},
    {"support_proxy", "video2_name", CONFIG_STRING,
     offsetof(struct ca_config, support.video2_name),
     sizeof(((struct ca_config *)0)->support.video2_name), NULL, 0U, 0, 63, NULL},
    {"support_proxy", "publish_password", CONFIG_STRING,
     offsetof(struct ca_config, support.publish_password),
     sizeof(((struct ca_config *)0)->support.publish_password), NULL, 0U, 0, 127, NULL},
    {"support_proxy", "network_interface", CONFIG_STRING,
     offsetof(struct ca_config, support.network_interface),
     sizeof(((struct ca_config *)0)->support.network_interface), NULL, 0U, 1, 15, NULL},
    {"support_proxy", "network_address", CONFIG_STRING,
     offsetof(struct ca_config, support.network_address),
     sizeof(((struct ca_config *)0)->support.network_address), NULL, 0U, 0, 31, NULL},
    {"support_proxy", "network_gateway", CONFIG_STRING,
     offsetof(struct ca_config, support.network_gateway),
     sizeof(((struct ca_config *)0)->support.network_gateway), NULL, 0U, 0, 15, NULL},
    {"network", "interface", CONFIG_STRING,
     offsetof(struct ca_config, network.interface),
     sizeof(((struct ca_config *)0)->network.interface), NULL, 0U, 1, 15, NULL},
    {"network", "primary_address", CONFIG_STRING,
     offsetof(struct ca_config, network.primary_address),
     sizeof(((struct ca_config *)0)->network.primary_address), NULL, 0U, 0, 31, NULL},
    {"network", "secondary_address", CONFIG_STRING,
     offsetof(struct ca_config, network.secondary_address),
     sizeof(((struct ca_config *)0)->network.secondary_address), NULL, 0U, 0, 31, NULL},
    {"network", "gateway", CONFIG_STRING,
     offsetof(struct ca_config, network.gateway),
     sizeof(((struct ca_config *)0)->network.gateway), NULL, 0U, 0, 15, NULL},
};

static bool support_valid(const struct ca_support_config *support)
{
    return !support->enabled ||
        (support->host[0] && (!support->signing || support->signing_passphrase[0]) &&
         (!support->video1_port || support->video1_name[0]) &&
         (!support->video2_port || support->video2_name[0]) &&
         (!support->video1_port || support->video1_port != support->video2_port));
}

static char *trim(char *text)
{
    char *end;

    while (isspace((unsigned char)*text)) text++;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return text;
}

static bool valid_timezone(const char *value)
{
    size_t length = strlen(value);

    if (length == 0U || length >= CA_CONFIG_TIMEZONE_MAX) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)value[i];
        if (iscntrl(c) || isspace(c)) return false;
    }
    return true;
}

static int set_field(struct ca_config *config, const struct config_field *field,
                     const char *value)
{
    uint8_t *destination = (uint8_t *)config + field->offset;

    if (field->kind == CONFIG_STRING) {
        size_t length = strlen(value);
        if (length >= field->size || length < (size_t)field->minimum) return -1;
        for (size_t i = 0; i < length; i++) {
            unsigned char c = (unsigned char)value[i];
            if (c < 32U || c > 126U || c == '"') return -1;
        }
        if (strcmp(field->key, "host") == 0 ||
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
            snprintf(address, sizeof(address), "%s", value);
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
    if (field->kind == CONFIG_TIMEZONE) {
        if (!valid_timezone(value) || strlen(value) >= field->size) return -1;
        memcpy(destination, value, strlen(value) + 1U);
        return 0;
    }
    if (field->kind == CONFIG_ENUM && field->size == sizeof(int)) {
        for (size_t i = 0; i < field->option_count; i++) {
            if (strcmp(value, field->options[i].name) == 0) {
                int selected = field->options[i].value;
                memcpy(destination, &selected, sizeof(selected));
                return 0;
            }
        }
    }
    if (field->kind == CONFIG_INT && field->size == sizeof(int)) {
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
    if (field->kind == CONFIG_BOOL && field->size == sizeof(bool)) {
        bool selected;
        if (strcmp(value, "true") == 0) selected = true;
        else if (strcmp(value, "false") == 0) selected = false;
        else return -1;
        memcpy(destination, &selected, sizeof(selected));
        return 0;
    }
    if (field->kind == CONFIG_UINT && field->size == sizeof(unsigned)) {
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

void ca_config_defaults(struct ca_config *config)
{
    if (config == NULL) return;
    memset(config, 0, sizeof(*config));
    memcpy(config->timezone, APCAM_DEFAULT_TIMEZONE, sizeof(APCAM_DEFAULT_TIMEZONE));
    config->photo_scope = APCAM_DEFAULT_PHOTO_SCOPE;
    config->orientation = APCAM_DEFAULT_ORIENTATION;
    config->uart_protocol = CA_UART_NONE;
    config->thermal_palette = CA_PALETTE_WHITE_HOT;
    config->autorecord = CA_AUTORECORD_DISABLED;
    config->main_resolution = APCAM_DEFAULT_MAIN_RESOLUTION;
    config->sub_resolution = APCAM_DEFAULT_SUB_RESOLUTION;
    config->recording_resolution = APCAM_DEFAULT_RECORDING_RESOLUTION;
    config->main_codec = CA_VIDEO_H264;
    config->sub_codec = CA_VIDEO_H264;
    config->brightness = 50;
    config->saturation = 50;
    config->contrast = 50;
    config->exposure_compensation = 0;
    config->iso = CA_ISO_AUTO;
    config->shutter = CA_SHUTTER_AUTO;
    config->metering = CA_METERING_AVERAGE;
    config->white_balance = CA_WB_AUTO;
    config->position_targeting = APCAM_DEFAULT_POSITION_TARGETING;
    config->osd_recording = !APCAM_HAVE_OVERLAY_RECORDING_SELECT;
    config->tracking_method = CA_TRACK_ANGLE;
    config->mavlink_system_id = APCAM_DEFAULT_SYSTEM_ID;
    config->mavlink_camera_component_id = 100U; /* MAV_COMP_ID_CAMERA */
    config->mavlink_tcp_port = 14550U;
    config->mavlink_udp_port = 14550U;
    config->support.mavlink_port = 10001U;
    config->support.signing_link_id = 1U;
    config->support.video1_port = 0U;
    config->support.video2_port = 0U;
    strcpy(config->support.video1_name, "video1");
    strcpy(config->support.video2_name, "video2");
    strcpy(config->support.network_interface, "eth0");
    strcpy(config->network.interface, "eth0");
}

const char *ca_mount_orientation_name(enum ca_mount_orientation orientation)
{
    if (orientation == CA_MOUNT_UPRIGHT) return "upright";
    if (orientation == CA_MOUNT_INVERTED) return "inverted";
    return "auto";
}

const char *ca_uart_protocol_name(enum ca_uart_protocol protocol)
{
    if (protocol == CA_UART_SIYI) return "siyi";
    if (protocol == CA_UART_MAVLINK) return "mavlink";
    return "none";
}

const char *ca_video_resolution_name(enum ca_video_resolution resolution)
{
    if (resolution == CA_VIDEO_720P) return "1280x720";
    if (resolution == CA_VIDEO_1440P) return "2560x1440";
    if (resolution == CA_VIDEO_2160P) return "3840x2160";
    return "1920x1080";
}

void ca_video_resolution_size(enum ca_video_resolution resolution,
                              unsigned *width, unsigned *height)
{
    unsigned w = 1920U, h = 1080U;
    if (resolution == CA_VIDEO_720P) { w = 1280U; h = 720U; }
    else if (resolution == CA_VIDEO_1440P) { w = 2560U; h = 1440U; }
    else if (resolution == CA_VIDEO_2160P) { w = 3840U; h = 2160U; }
    if (width != NULL) *width = w;
    if (height != NULL) *height = h;
}

const char *ca_video_codec_name(enum ca_video_codec codec)
{
    return codec == CA_VIDEO_H265 ? "h265" : "h264";
}

const char *ca_thermal_palette_name(enum ca_thermal_palette palette)
{
    for (size_t i = 0; i < sizeof(palette_options) / sizeof(palette_options[0]); i++) {
        if (palette_options[i].value == (int)palette) return palette_options[i].name;
    }
    return "white_hot";
}

const char *ca_photo_scope_name(enum ca_photo_scope scope)
{
    return scope == CA_PHOTO_SCOPE_ALL ? "all" : "thermal";
}

int ca_config_load(struct ca_config *config, const char *path,
                   char *error, size_t error_size)
{
    FILE *file;
    char line[CONFIG_LINE_MAX];
    char section[64] = "";
    unsigned line_number = 0U;
    bool seen[sizeof(config_fields) / sizeof(config_fields[0])] = {false};
    struct ca_config parsed;

    if (config == NULL || path == NULL || *path == '\0') {
        errno = EINVAL;
        return -1;
    }
    parsed = *config;
    file = fopen(path, "r");
    if (file == NULL) return -1;

    while (fgets(line, sizeof(line), file) != NULL) {
        char *text;
        char *equals;
        size_t length;

        line_number++;
        length = strlen(line);
        if (length != 0U && line[length - 1U] != '\n' && !feof(file)) {
            snprintf(error, error_size, "line %u is too long", line_number);
            errno = EINVAL;
            goto fail;
        }
        text = trim(line);
        if (*text == '\0' || *text == '#' || *text == ';') continue;
        if (*text == '[') {
            char *close = strchr(text + 1, ']');
            char *name;
            if (close == NULL || *trim(close + 1) != '\0') {
                snprintf(error, error_size, "line %u has an invalid section", line_number);
                errno = EINVAL;
                goto fail;
            }
            *close = '\0';
            name = trim(text + 1);
            if (*name == '\0' || strlen(name) >= sizeof(section)) {
                snprintf(error, error_size, "line %u has an invalid section name", line_number);
                errno = EINVAL;
                goto fail;
            }
            memcpy(section, name, strlen(name) + 1U);
            continue;
        }
        equals = strchr(text, '=');
        if (equals == NULL) {
            snprintf(error, error_size, "line %u is not key=value", line_number);
            errno = EINVAL;
            goto fail;
        }
        *equals = '\0';
        char *key = trim(text);
        char *value = trim(equals + 1);
        size_t value_length = strlen(value);
        if (value_length >= 2U && value[0] == '"' &&
            value[value_length - 1U] == '"') {
            value[value_length - 1U] = '\0';
            value = strcmp(section, "support_proxy") == 0 ? value + 1 : trim(value + 1);
        }

        for (size_t i = 0; i < sizeof(config_fields) / sizeof(config_fields[0]);
             i++) {
            const struct config_field *field = &config_fields[i];
            if (strcmp(section, field->section) != 0 ||
                strcmp(key, field->key) != 0) continue;
            if (seen[i] || set_field(&parsed, field, value) < 0) {
                snprintf(error, error_size,
                         "line %u has an invalid or duplicate %s",
                         line_number, field->key);
                errno = EINVAL;
                goto fail;
            }
            seen[i] = true;
            break;
        }
        /* Unknown keys are retained by the web editor and ignored here. This
         * permits newer configurations to be used with an older binary. */
    }
    if (ferror(file)) {
        snprintf(error, error_size, "read failed: %s", strerror(errno));
        goto fail;
    }
    if (!support_valid(&parsed.support)) {
        snprintf(error, error_size, "SupportProxy needs a host, distinct enabled video ports, stream names and a passphrase when signing is enabled");
        errno = EINVAL;
        goto fail;
    }
    /* New Network keys take precedence even when explicitly empty. Legacy
     * proxy network settings retain their old enabled-only behavior until saved. */
    if (parsed.support.enabled) {
        for (size_t i = 0; i < sizeof(config_fields) / sizeof(config_fields[0]); i++) {
            const struct config_field *f = &config_fields[i];
            if (seen[i] || strcmp(f->section, "network")) continue;
            const char *legacy = !strcmp(f->key, "interface") ? parsed.support.network_interface :
                !strcmp(f->key, "secondary_address") ? parsed.support.network_address :
                !strcmp(f->key, "gateway") ? parsed.support.network_gateway : NULL;
            if (legacy) snprintf((char *)&parsed + f->offset, f->size, "%s", legacy);
        }
    }
    if (!apcam_network_valid(parsed.network.primary_address, parsed.network.secondary_address, parsed.network.gateway)) {
        snprintf(error, error_size, "Network needs distinct host addresses and a gateway reachable through either configured subnet");
        errno = EINVAL;
        goto fail;
    }
    fclose(file);
    *config = parsed;
    return 0;

fail:
    fclose(file);
    return -1;
}

static const struct config_field *param_field(size_t index)
{
    for (size_t i = 0; i < sizeof(config_fields) / sizeof(config_fields[0]); i++) {
        if (config_fields[i].param_name == NULL) continue;
        if (index-- == 0U) return &config_fields[i];
    }
    return NULL;
}

size_t ca_config_param_count(void)
{
    size_t count = 0U;
    for (size_t i = 0; i < sizeof(config_fields) / sizeof(config_fields[0]); i++) {
        if (config_fields[i].param_name != NULL) count++;
    }
    return count;
}

const char *ca_config_param_name(size_t index)
{
    const struct config_field *field = param_field(index);
    return field != NULL ? field->param_name : NULL;
}

int ca_config_param_find(const char *name)
{
    for (size_t i = 0; i < ca_config_param_count(); i++) {
        if (strcmp(name, ca_config_param_name(i)) == 0) return (int)i;
    }
    return -1;
}

size_t ca_config_param_options(size_t index, const struct ca_config_option **options,
                                int *minimum, int *maximum)
{
    const struct config_field *field = param_field(index);
    *options = field ? field->options : NULL;
    *minimum = field ? field->minimum : 0;
    *maximum = field ? (field->kind == CONFIG_BOOL ? 1 : field->maximum) : 0;
    return field ? field->option_count : 0;
}

int ca_config_param_get(const struct ca_config *config, size_t index)
{
    const struct config_field *field = param_field(index);
    if (field == NULL) return 0;
    const uint8_t *source = (const uint8_t *)config + field->offset;
    if (field->kind == CONFIG_BOOL) {
        bool value;
        memcpy(&value, source, sizeof(value));
        return value ? 1 : 0;
    }
    if (field->kind == CONFIG_UINT) {
        unsigned value;
        memcpy(&value, source, sizeof(value));
        return (int)value;
    }
    int value;
    memcpy(&value, source, sizeof(value));
    return value;
}

/* Replace only the requested key, retaining comments, unknown settings and
 * edits made by the web UI since startup. Rename prevents a partial INI from
 * being observed by readers if power is lost during the write. */
static int save_field(const char *path, const struct config_field *field,
                       const char *value)
{
    FILE *input = fopen(path, "r");
    FILE *output = NULL;
    char *temporary = NULL;
    char *line = NULL;
    size_t capacity = 0U;
    bool in_section = false, written = false, key_present = false;
    struct stat st;
    int result = -1;
    if (input == NULL && errno != ENOENT) return -1;
    /* INI sections may repeat. Locate the key before inserting a missing
     * one, or an earlier partial section could create a duplicate key. */
    while (input != NULL && getline(&line, &capacity, input) >= 0) {
        char *text = trim(line);
        if (*text == '[') {
            char *close = strchr(text + 1, ']');
            if (close != NULL) *close = '\0';
            in_section = close != NULL &&
                         strcmp(trim(text + 1), field->section) == 0;
        } else if (in_section && *text != '#' && *text != ';') {
            char *equals = strchr(text, '=');
            if (equals != NULL) {
                *equals = '\0';
                if (strcmp(trim(text), field->key) == 0) key_present = true;
            }
        }
    }
    if (input != NULL && (ferror(input) || fseek(input, 0, SEEK_SET) != 0)) goto done;
    in_section = false;
    if (asprintf(&temporary, "%s.XXXXXX", path) < 0) goto done;
    int fd = mkstemp(temporary);
    if (fd < 0) goto done;
    output = fdopen(fd, "w");
    if (output == NULL) { close(fd); goto done; }
    if (input != NULL && fstat(fileno(input), &st) == 0 &&
        fchmod(fd, st.st_mode & 0777) < 0) goto done;
    while (input != NULL && getline(&line, &capacity, input) >= 0) {
        char *copy = strdup(line);
        if (copy == NULL) goto done;
        char *text = trim(copy);
        if (*text == '[') {
            if (in_section && !written && !key_present) {
                fprintf(output, "%s = %s\n", field->key, value);
                written = true;
            }
            char *close = strchr(text + 1, ']');
            if (close != NULL) *close = '\0';
            in_section = close != NULL &&
                         strcmp(trim(text + 1), field->section) == 0;
        } else if (in_section && *text != '#' && *text != ';') {
            char *equals = strchr(text, '=');
            if (equals != NULL) {
                *equals = '\0';
                if (strcmp(trim(text), field->key) == 0) {
                    fprintf(output, "%s = %s\n", field->key, value);
                    written = true;
                    free(copy);
                    continue;
                }
            }
        }
        free(copy);
        fputs(line, output);
        /* Ensure a missing final newline cannot join an appended setting. */
        if (*line != '\0' && line[strlen(line) - 1U] != '\n') fputc('\n', output);
    }
    if (input != NULL && ferror(input)) goto done;
    if (!written) {
        if (!in_section) fprintf(output, "\n[%s]\n", field->section);
        fprintf(output, "%s = %s\n", field->key, value);
    }
    if (ferror(output) || fflush(output) != 0 || fsync(fileno(output)) < 0) goto done;
    if (fclose(output) != 0) { output = NULL; goto done; }
    output = NULL;
    if (rename(temporary, path) < 0) goto done;
    result = 0;
done: {
        int saved_errno = errno;
        if (input != NULL) fclose(input);
        if (output != NULL) fclose(output);
        if (temporary != NULL) unlink(temporary);
        free(temporary);
        free(line);
        errno = saved_errno;
        return result;
    }
}

int ca_config_param_save(struct ca_config *config, const char *path,
                          size_t index, float value)
{
    const struct config_field *field = param_field(index);
    char number[32];
    const char *text = number;
    char error[160];
    struct ca_config updated;
    if (field == NULL || !isfinite(value) || value < -65535.0f ||
        value > 65535.0f || value != (float)(int)value) {
        errno = EINVAL;
        return -1;
    }
    int selected = (int)value;
    snprintf(number, sizeof(number), "%d", selected);
    if (field->kind == CONFIG_ENUM) {
        text = NULL;
        for (size_t i = 0; i < field->option_count; i++) {
            if (field->options[i].value == selected) text = field->options[i].name;
        }
    } else if (field->kind == CONFIG_BOOL) {
        text = selected == 0 ? "false" : selected == 1 ? "true" : NULL;
    }
    ca_config_defaults(&updated);
    if (ca_config_load(&updated, path, error, sizeof(error)) < 0 && errno != ENOENT) {
        return -1;
    }
    if (text == NULL || set_field(&updated, field, text) < 0 || !support_valid(&updated.support)) {
        errno = EINVAL;
        return -1;
    }
    if (save_field(path, field, text) < 0) return -1;
    *config = updated;
    return 0;
}

#define IMAGE_FIELDS(X) X(brightness) X(saturation) X(contrast) X(exposure_compensation) \
    X(iso) X(shutter) X(metering) X(white_balance)

bool ca_config_image_equal(const struct ca_config *a, const struct ca_config *b)
{
#define SAME(field) if (a->field != b->field) return false;
    IMAGE_FIELDS(SAME)
#undef SAME
    return true;
}

void ca_config_copy_image(struct ca_config *destination, const struct ca_config *source)
{
#define COPY(field) destination->field = source->field;
    IMAGE_FIELDS(COPY)
#undef COPY
}

int ca_config_param_assign(struct ca_config *config, size_t index, float value)
{
    const struct config_field *field = param_field(index);
    if (!field || !isfinite(value) || value < -65535 || value > 65535 || value != truncf(value)) {
        errno = EINVAL;
        return -1;
    }
    int selected = (int)value;
    char number[32];
    snprintf(number, sizeof(number), "%d", selected);
    const char *text = number;
    if (field->kind == CONFIG_ENUM) {
        text = NULL;
        for (size_t i = 0; i < field->option_count; i++) {
            if (field->options[i].value == selected) text = field->options[i].name;
        }
    } else if (field->kind == CONFIG_BOOL) {
        text = selected == 0 ? "false" : selected == 1 ? "true" : NULL;
    }
    struct ca_config updated = *config;
    if (!text || set_field(&updated, field, text) < 0 || !support_valid(&updated.support)) {
        errno = EINVAL;
        return -1;
    }
    *config = updated;
    return 0;
}
