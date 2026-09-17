#ifndef CAMERA_APP_CONFIG_H
#define CAMERA_APP_CONFIG_H

#include <stddef.h>
#include <stdbool.h>

#define CA_CONFIG_DEFAULT_PATH "/app/camera.ini"
#define CA_CONFIG_TIMEZONE_MAX 128U

enum ca_photo_scope {
    CA_PHOTO_SCOPE_THERMAL = 0,
    CA_PHOTO_SCOPE_ALL = 1,
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
};

enum ca_video_codec {
    CA_VIDEO_H264 = 0,
    CA_VIDEO_H265,
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

struct ca_support_config {
    bool enabled;
    char host[128];
    unsigned mavlink_port;
    bool signing;
    char signing_passphrase[128];
    unsigned signing_link_id;
    unsigned video1_port;
    unsigned video2_port;
    char video1_name[64];
    char video2_name[64];
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

struct ca_config {
    struct ca_network_config network;
    struct ca_support_config support;
    char timezone[CA_CONFIG_TIMEZONE_MAX];
    enum ca_photo_scope photo_scope;
    enum ca_mount_orientation orientation;
    enum ca_uart_protocol uart_protocol;
    enum ca_thermal_palette thermal_palette;
    enum ca_autorecord_mode autorecord;
    enum ca_video_resolution main_resolution;
    enum ca_video_resolution sub_resolution;
    enum ca_video_resolution recording_resolution;
    enum ca_video_codec main_codec;
    enum ca_video_codec sub_codec;
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
/* Numeric configuration values use stable MAVLink names and enum ordinals.
 * Writes persist to the INI; the caller applies supported runtime settings. */
size_t ca_config_param_count(void);
struct ca_config_option {
    const char *name;
    int value;
};
/* Metadata from the same table used to validate persistent parameter writes. */
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

#endif
