#ifndef CAMERA_APP_METADATA_H
#define CAMERA_APP_METADATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Latest vehicle/gimbal state, kept process-wide so the still writer can
 * stamp captures without the media backends knowing about MAVLink. */

#define CA_METADATA_MAX_AGE_MS 10000U
#define CA_METADATA_MODEL_MAX 32U
#define CA_METADATA_SEGMENTS_MAX 4096U

struct ca_metadata {
    char model[CA_METADATA_MODEL_MAX];
    bool have_position;
    unsigned position_age_ms;
    int32_t lat_e7;
    int32_t lon_e7;
    float alt_amsl_m;
    float alt_relative_m;
    float heading_rad;
    bool have_velocity;
    unsigned velocity_age_ms;
    float vn_m_s, ve_m_s, vd_m_s;
    bool have_vehicle_attitude;
    unsigned vehicle_attitude_age_ms;
    float vehicle_roll_rad;
    float vehicle_pitch_rad;
    float vehicle_yaw_rad;
    float vehicle_yaw_rate_rad_s;
    bool have_gimbal_attitude;
    unsigned gimbal_attitude_age_ms;
    /* yaw in the vehicle frame */
    float gimbal_roll_rad;
    float gimbal_pitch_rad;
    float gimbal_yaw_rad;
    float gimbal_yaw_rate_rad_s;
    float gimbal_pitch_rate_rad_s;
    /* 0 when unknown */
    float zoom;
    /* Effective horizontal FOV after optical/digital zoom, degrees; 0 unknown.
     * Supplied by the producer for this frame, not process-wide state. */
    float hfov_deg;
};

void ca_metadata_set_model(const char *model);
/* Optional timestamps are sample times in CLOCK_MONOTONIC milliseconds;
 * zero retains arrival-time behaviour for local/legacy producers. */
void ca_metadata_set_position(int32_t lat_e7, int32_t lon_e7, float alt_amsl_m,
                              float alt_relative_m, float heading_rad, uint64_t timestamp_ms = 0);
void ca_metadata_set_vehicle_attitude(float roll_rad, float pitch_rad,
                                      float yaw_rad);
void ca_metadata_set_gimbal_attitude(float roll_rad, float pitch_rad,
                                     float yaw_rad);
void ca_metadata_set_velocity(float vn_m_s, float ve_m_s, float vd_m_s, uint64_t timestamp_ms = 0);
void ca_metadata_set_vehicle_attitude_motion(float roll_rad, float pitch_rad,
                                             float yaw_rad, float yaw_rate_rad_s, uint64_t timestamp_ms = 0);
/* Preserve the backend sample time when polling cached gimbal state. */
void ca_metadata_set_gimbal_attitude_sample(float roll_rad, float pitch_rad,
                                            float yaw_rad, uint64_t timestamp_ms);
void ca_metadata_set_gimbal_attitude_motion(float roll_rad, float pitch_rad,
                                            float yaw_rad, float yaw_rate_rad_s,
                                            uint64_t timestamp_ms);
void ca_metadata_set_zoom(float zoom);
/* copies the current state; sources older than CA_METADATA_MAX_AGE_MS are
 * reported as absent */
void ca_metadata_snapshot(struct ca_metadata *snapshot);
// Bounded history on the same monotonic clock as received sensor frames.
void ca_metadata_record_sample(uint64_t now_ms);
bool ca_metadata_at(uint64_t frame_ms, struct ca_metadata *snapshot);

/* EXIF and XMP APP1 segments for a JPEG captured at captured_at, to be
 * placed right after SOI; returns the length or 0 if capacity is too small */
size_t ca_metadata_jpeg_segments(const struct ca_metadata *metadata,
                                 const struct timespec *captured_at,
                                 uint8_t *output, size_t capacity);

#endif
