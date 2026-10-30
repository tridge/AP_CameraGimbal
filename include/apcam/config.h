#ifndef CAMERA_APP_CONFIG_H
#define CAMERA_APP_CONFIG_H

#include <stddef.h>
#include <stdbool.h>

/* Target-independent code (for example the camera FTP service) builds
 * without a target; every file of a target build defines APCAM_TARGET. */
#ifdef APCAM_TARGET
#include "target.h"
#endif

#define CA_CONFIG_DEFAULT_PATH "/app/camera.ini"
#define CA_CONFIG_TIMEZONE_MAX 128U

enum ca_photo_scope {
    CA_PHOTO_SCOPE_THERMAL = 0,
    CA_PHOTO_SCOPE_ALL = 1,
};

/* Photo size: a video frame, or a sensor mode switch to a larger size. */
enum ca_photo_resolution {
    CA_PHOTO_RES_VIDEO = 0,
    CA_PHOTO_RES_BINNED,
    CA_PHOTO_RES_FULL,
};

enum ca_mount_orientation {
    CA_MOUNT_AUTO = 0,
    CA_MOUNT_UPRIGHT,
    CA_MOUNT_INVERTED,
};

enum ca_uart_protocol {
    CA_UART_NONE = 0,
    CA_UART_SIYI,
    CA_UART_MAVLINK,
};

enum ca_video_resolution {
    CA_VIDEO_720P = 0,
    CA_VIDEO_1080P,
    CA_VIDEO_2160P,
    CA_VIDEO_1440P,
    CA_VIDEO_540P,
    CA_VIDEO_480P,
    CA_VIDEO_360P,
    CA_VIDEO_240P,
    CA_VIDEO_180P,
};

enum ca_video_codec {
    CA_VIDEO_H264 = 0,
    CA_VIDEO_H265,
};

/* Persistent wire values; append new formats without renumbering these. */
enum ca_video_telemetry_format {
    CA_VIDEO_TELEMETRY_APCG = 0,
    CA_VIDEO_TELEMETRY_MISB = 1,
};

enum ca_thermal_palette {
    CA_PALETTE_WHITE_HOT = 0,
    CA_PALETTE_SEPIA = 2,
    CA_PALETTE_IRONBOW = 3,
    CA_PALETTE_RAINBOW = 4,
    CA_PALETTE_NIGHT = 5,
    CA_PALETTE_AURORA = 6,
    CA_PALETTE_RED_HOT = 7,
    CA_PALETTE_JUNGLE = 8,
    CA_PALETTE_MEDICAL = 9,
    CA_PALETTE_BLACK_HOT = 10,
    CA_PALETTE_GLORY_HOT = 11,
};

enum ca_iso_mode {
    CA_ISO_AUTO = 0,
    CA_ISO_100,
    CA_ISO_200,
    CA_ISO_400,
    CA_ISO_800,
    CA_ISO_1600,
    CA_ISO_3200,
};

enum ca_shutter_mode {
    CA_SHUTTER_AUTO = 0,
    CA_SHUTTER_1_30,
    CA_SHUTTER_1_50,
    CA_SHUTTER_1_100,
    CA_SHUTTER_1_250,
    CA_SHUTTER_1_500,
    CA_SHUTTER_1_750,
    CA_SHUTTER_1_1000,
    CA_SHUTTER_1_2000,
};

enum ca_metering_mode {
    CA_METERING_AVERAGE = 0,
    CA_METERING_CENTER,
    CA_METERING_SPOT,
};

enum ca_white_balance {
    CA_WB_AUTO = 0,
    CA_WB_DAYLIGHT,
    CA_WB_CLOUDY,
    CA_WB_FLUORESCENT,
    CA_WB_INCANDESCENT,
};

enum ca_autorecord_mode {
    CA_AUTORECORD_DISABLED = 0,
    CA_AUTORECORD_ENABLED = 1,
    CA_AUTORECORD_WHILE_ARMED = 2,
};

enum ca_tracking_method {
    CA_TRACK_ANGLE = 0,
    CA_TRACK_RATE = 1,
};

#ifdef __cplusplus
extern "C" {
#endif

struct ca_support_config {
    bool enabled;
    char host[128];
    unsigned mavlink_port;
    bool signing;
    char signing_passphrase[128];
    unsigned signing_link_id;
    unsigned video1_port;
    unsigned video2_port;
    unsigned video3_port;
    char video1_name[64];
    char video2_name[64];
    char video3_name[64];
    char publish_password[128];
    /* Legacy INI keys; new settings belong to ca_network_config below. */
    char network_interface[16];
    char network_address[32];
    char network_gateway[16];
};

struct ca_network_config {
    char interface[16];
    char primary_address[32];
    char secondary_address[32];
    char gateway[16];
};

enum ca_survey_pattern { CA_SURVEY_BOTH=0, CA_SURVEY_LEFT_RIGHT=1, CA_SURVEY_FORE_AFT=2, CA_SURVEY_FORE_ONLY=3 };

struct ca_survey_config {
    unsigned pattern;
    unsigned lens; // 0 raw thermal, 1 wide RGB, 2 zoom RGB
    unsigned overlap;
    unsigned fore_pct;
    unsigned aft_pct;
    unsigned burst;
    unsigned burst_ms;
    unsigned dwell_ms;
    unsigned settle_ms;
    unsigned error_cd;
    unsigned rate_cd;
    unsigned terrain_ms;
    unsigned terrain_m;
    unsigned terrain_tol;
    unsigned min_speed;
    unsigned turn_deg;
};

struct ca_config {
    struct ca_survey_config survey;
    struct ca_network_config network;
    bool network_capture;
    /* Complete URI advertised for stream 1 on host-OS targets; empty = built
     * from the host. Size: APCAM_VIDEO_URI_SIZE. */
    char video1_uri[160];
    struct ca_support_config support;
    char timezone[CA_CONFIG_TIMEZONE_MAX];
    enum ca_photo_scope photo_scope;
#if defined(APCAM_HAVE_VIDEO_BITRATE) && APCAM_HAVE_VIDEO_BITRATE
    unsigned main_bitrate_kbps;
#endif
#if defined(APCAM_HAVE_VIDEO_FRAME_RATE) && APCAM_HAVE_VIDEO_FRAME_RATE
    unsigned main_frame_rate;
#endif
#if defined(APCAM_HAVE_PHOTO_RESOLUTION) && APCAM_HAVE_PHOTO_RESOLUTION
    enum ca_photo_resolution photo_resolution;
#endif
    enum ca_mount_orientation orientation;
    enum ca_uart_protocol uart_protocol;
    enum ca_thermal_palette thermal_palette;
#ifdef CAMERA_APP_ZT30_DEV
    unsigned thermal_hfov_cd; // 0: geometry disabled, otherwise horizontal FOV in 0.01 degrees.
#endif
    enum ca_autorecord_mode autorecord;
    unsigned raw_stream_fps;
    unsigned raw_record_fps;
    enum ca_video_resolution main_resolution;
    enum ca_video_resolution sub_resolution;
    enum ca_video_resolution recording_resolution;
    enum ca_video_codec main_codec;
    enum ca_video_codec sub_codec;
    enum ca_video_telemetry_format video_telemetry_format;
    /* additional RTSP path names for the same streams, empty for none */
    char main_alias[64];
    char sub_alias[64];
    bool osd_cross;
    bool osd_recording;
    bool osd_thermal_fov;
    int brightness;
    int saturation;
    int contrast;
    int exposure_compensation;
    enum ca_iso_mode iso;
    enum ca_shutter_mode shutter;
    enum ca_metering_mode metering;
    enum ca_white_balance white_balance;
    bool position_targeting;
    enum ca_tracking_method tracking_method;
    bool log_disarmed;
    unsigned mavlink_system_id;
    unsigned mavlink_camera_component_id;
    unsigned mavlink_tcp_port;
    unsigned mavlink_udp_port;
};

bool ca_config_image_equal(const struct ca_config *a, const struct ca_config *b);
void ca_config_copy_image(struct ca_config *destination, const struct ca_config *source);
/* Validate and update a copy of the configuration without writing the INI. */
int ca_config_param_assign(struct ca_config *config, size_t index, float value);
void ca_config_defaults(struct ca_config *config);
int ca_config_load(struct ca_config *config, const char *path,
                   char *error, size_t error_size);
#ifdef CAMERA_APP_ZT30_DEV
/* Parse candidate bytes with the same strict parser as application startup. */
int ca_config_parse(struct ca_config *config, const char *text, size_t length,
                    char *error, size_t error_size);
#endif
/* Numeric configuration values use stable MAVLink names and enum ordinals.
 * Writes persist to the INI; the caller applies supported runtime settings. */
size_t ca_config_param_count(void);
struct ca_config_option {
    const char *name;
    int value;
};
/* Metadata from the same table used to validate persistent parameter writes. */
bool ca_config_param_is_bool(size_t index);
size_t ca_config_param_options(size_t index, const struct ca_config_option **options,
                                int *minimum, int *maximum);
const char *ca_config_param_name(size_t index);
int ca_config_param_find(const char *name);
int ca_config_param_get(const struct ca_config *config, size_t index);
int ca_config_param_save(struct ca_config *config, const char *path,
                          size_t index, float value);
const char *ca_photo_scope_name(enum ca_photo_scope scope);
const char *ca_mount_orientation_name(enum ca_mount_orientation orientation);
const char *ca_uart_protocol_name(enum ca_uart_protocol protocol);
const char *ca_video_resolution_name(enum ca_video_resolution resolution);
void ca_video_resolution_size(enum ca_video_resolution resolution,
                              unsigned *width, unsigned *height);
const char *ca_video_codec_name(enum ca_video_codec codec);
const char *ca_thermal_palette_name(enum ca_thermal_palette palette);

#ifdef __cplusplus
}
#endif

#endif
