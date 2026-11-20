#ifndef CAMERA_APP_BACKEND_H
#define CAMERA_APP_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "camera_app/thermal.h"
#include "camera_app/config.h"

typedef void (*ca_siyi_emit_fn)(void *opaque, const uint8_t *packet,
                                size_t length);
typedef int (*ca_recording_set_fn)(void *opaque, bool active);
typedef bool (*ca_recording_get_fn)(void *opaque);
typedef int (*ca_zoom_set_fn)(void *opaque, float zoom);
typedef float (*ca_zoom_get_fn)(void *opaque);
typedef int (*ca_lens_set_fn)(void *opaque, bool wide);
typedef bool (*ca_lens_wide_fn)(void *opaque);
typedef int (*ca_thermal_main_set_fn)(void *opaque, bool thermal_main);
typedef bool (*ca_thermal_main_get_fn)(void *opaque);
typedef int (*ca_autofocus_fn)(void *opaque, uint16_t x, uint16_t y);
typedef int (*ca_manual_focus_fn)(void *opaque, int direction);
typedef bool (*ca_thermal_range_fn)(void *opaque,
                                    struct ca_thermal_range *range);
typedef int (*ca_photo_capture_fn)(void *opaque);
typedef int (*ca_thermal_gain_get_fn)(void *opaque, uint8_t *gain);
typedef int (*ca_thermal_gain_set_fn)(void *opaque, uint8_t gain);
typedef int (*ca_thermal_palette_get_fn)(void *opaque, uint8_t *palette);
typedef int (*ca_thermal_palette_set_fn)(void *opaque, uint8_t palette);
typedef int (*ca_inverted_set_fn)(void *opaque, bool inverted);

struct ca_backend;
struct ca_private_frame;
typedef void (*ca_private_emit_fn)(void *opaque, const ca_private_frame *frame);

struct ca_gimbal_attitude {
    float roll_rad;
    float pitch_rad;
    float yaw_rad;
    float roll_rate_rad_s;
    float pitch_rate_rad_s;
    float yaw_rate_rad_s;
    uint64_t timestamp_ms;
};

struct ca_backend_config {
    const bool *manual_control, *manual_command;
    const char *name;
    const char *uart_device;
    ca_siyi_emit_fn emit;
    void *emit_opaque;
    /* Raw external UART bytes delegated by the SIYI MCU. */
    ca_siyi_emit_fn uart_emit;
    void *uart_opaque;
    ca_private_emit_fn private_emit;
    void *private_opaque;
    ca_recording_set_fn recording_set;
    ca_recording_get_fn recording_get;
    void *recording_opaque;
    ca_zoom_set_fn zoom_set;
    ca_zoom_get_fn zoom_get;
    void *zoom_opaque;
    ca_lens_set_fn lens_set;
    ca_lens_wide_fn lens_wide;
    void *lens_opaque;
    ca_thermal_main_set_fn thermal_main_set;
    ca_thermal_main_get_fn thermal_main_get;
    void *thermal_main_opaque;
    ca_autofocus_fn autofocus;
    void *autofocus_opaque;
    ca_manual_focus_fn manual_focus;
    void *manual_focus_opaque;
    ca_thermal_range_fn thermal_range;
    void *thermal_opaque;
    ca_photo_capture_fn photo_capture;
    void *photo_capture_opaque;
    ca_thermal_gain_get_fn thermal_gain_get;
    ca_thermal_gain_set_fn thermal_gain_set;
    void *thermal_gain_opaque;
    ca_thermal_palette_get_fn thermal_palette_get;
    ca_thermal_palette_set_fn thermal_palette_set;
    void *thermal_palette_opaque;
    enum ca_mount_orientation orientation;
    ca_inverted_set_fn inverted_set;
    void *inverted_opaque;
};

/* Incoming vendor motion must not override the web's temporary lease. */
static inline bool ca_backend_manual_blocked(const bool *active, const bool *command,
                                             uint8_t opcode, const uint8_t *payload, size_t length)
{
    if (!active || !*active || (command && *command)) return false;
    return opcode==0x07 || opcode==0x08 || opcode==0x0e || opcode==0x40 ||
           opcode==0x55 || opcode==0x56 ||
           (opcode==0x0c && length==1 && payload[0]>=3 && payload[0]<=5);
}

int ca_backend_open(struct ca_backend **backend,
                    const struct ca_backend_config *config);
int ca_backend_fd(const struct ca_backend *backend);
int ca_backend_handle_fd(struct ca_backend *backend);
int ca_backend_handle_siyi(struct ca_backend *backend,
                           const uint8_t *packet, size_t length);
/* SIYI private network-to-MCU route; A8/ZR10 convert v3 fields to v2 UART. */
int ca_backend_handle_private(ca_backend *backend, const ca_private_frame *frame);
/* Return bytes to the MCU-owned external UART, independently of uart.protocol. */
int ca_backend_write_external_uart(struct ca_backend *backend, const uint8_t *data, size_t length);
void ca_backend_periodic(struct ca_backend *backend);
int ca_backend_request_gimbal_attitude(struct ca_backend *backend);
bool ca_backend_gimbal_attitude(const struct ca_backend *backend,
                                struct ca_gimbal_attitude *attitude);
int ca_backend_set_gimbal_angles(struct ca_backend *backend, float pitch_rad,
                                 float yaw_rad);
int ca_backend_set_gimbal_rates(struct ca_backend *backend,
                                float pitch_rate_rad_s,
                                float yaw_rate_rad_s);
int ca_backend_set_zoom(struct ca_backend *backend, float ratio);
int ca_backend_set_zoom_rate(struct ca_backend *backend, float rate);
int ca_backend_set_gimbal_neutral(struct ca_backend *backend);
bool ca_backend_recording(const struct ca_backend *backend);
const char *ca_backend_name(const struct ca_backend *backend);
void ca_backend_close(struct ca_backend *backend);
/* Whether a gimbal is connected; MAVLink advertises one only if so. Defined
 * only by backends of targets with APCAM_GIMBAL_OPTIONAL. */
bool ca_backend_has_gimbal(const struct ca_backend *backend);

#endif
