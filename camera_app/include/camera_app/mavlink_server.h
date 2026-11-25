#ifndef CAMERA_APP_MAVLINK_SERVER_H
#define CAMERA_APP_MAVLINK_SERVER_H

#include "camera_app/config.h"
#include <stddef.h>
#include <stdint.h>

struct ca_backend;
struct ca_media;
struct ca_mavlink_server;

struct ca_mavlink_server_config {
    const bool *manual_control;
    unsigned tcp_port;
    unsigned udp_port;
    const char *uart_device;
    unsigned rtsp_port;
    const char *record_root;
    const char *capture_root;
    const char *log_root;
    const char *config_path;
    const char *network_error;
    enum ca_photo_scope photo_scope;
    struct ca_config settings;
    struct ca_backend *backend;
    struct ca_media *media;
    const char *rclone_path;   /* uploads; targets with APCAM_HAVE_UPLOAD */
    const char *rclone_config;
};

int ca_mavlink_server_open(struct ca_mavlink_server **server,
                           const struct ca_mavlink_server_config *config);
int ca_mavlink_server_fd(const struct ca_mavlink_server *server);
int ca_mavlink_server_handle(struct ca_mavlink_server *server);
void ca_mavlink_server_suspend_gimbal(struct ca_mavlink_server *server);
/* true while a survey holds the gimbal; external pointing is ignored then */
bool ca_mavlink_server_survey_owns_gimbal(const struct ca_mavlink_server *server);
void ca_mavlink_server_feed_mcu(struct ca_mavlink_server *server,
                                const uint8_t *data, size_t length);
void ca_mavlink_server_periodic(struct ca_mavlink_server *server);
void ca_mavlink_server_close(struct ca_mavlink_server *server);

#endif
