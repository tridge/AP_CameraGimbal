#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/video_metadata.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static enum ca_video_telemetry_format telemetry_format = CA_VIDEO_TELEMETRY_APCG;

void ca_video_metadata_set_format(enum ca_video_telemetry_format format)
{
    telemetry_format = format;
}

enum ca_video_telemetry_format ca_video_metadata_format(void)
{
    return telemetry_format;
}

const char *ca_video_recording_extension(void)
{
    return telemetry_format == CA_VIDEO_TELEMETRY_MISB ? "ts" : "mp4";
}

/* AP_CameraGimbal telemetry schema v1. Keep this UUID stable across releases. */
const uint8_t ca_video_metadata_uuid[16] = {
    0x8d, 0x64, 0x6b, 0x4e, 0x55, 0x6f, 0x4a, 0x90,
    0x8b, 0x7c, 0x35, 0xe6, 0x29, 0x51, 0x03, 0x21,
};

size_t ca_video_metadata_json(const struct ca_metadata *m,
                               const struct timespec *utc, uint64_t pts90k,
                               char *output, size_t capacity)
{
    size_t used = 0U;
    if (m == NULL || utc == NULL || output == NULL || capacity == 0U) return 0U;
#define JSON(...) do { \
    int n = snprintf(output + used, capacity - used, __VA_ARGS__); \
    if (n < 0 || (size_t)n >= capacity - used) return 0U; \
    used += (size_t)n; \
} while (0)
    JSON("{\"schema\":\"apcg.telemetry.v1\",\"pts90k\":%" PRIu64
         ",\"utc_us\":%" PRId64, pts90k,
         (int64_t)utc->tv_sec * 1000000 + utc->tv_nsec / 1000);
    JSON(",\"position\":");
    if (m->have_position && isfinite(m->alt_amsl_m) &&
        isfinite(m->alt_relative_m)) {
        JSON("{\"lat_e7\":%" PRId32 ",\"lon_e7\":%" PRId32
             ",\"alt_amsl_m\":%.3f,\"alt_relative_m\":%.3f,\"age_ms\":%u}",
             m->lat_e7, m->lon_e7, (double)m->alt_amsl_m,
             (double)m->alt_relative_m, m->position_age_ms);
    } else JSON("null");
    JSON(",\"velocity\":");
    if (m->have_velocity && isfinite(m->vn_m_s) && isfinite(m->ve_m_s) && isfinite(m->vd_m_s)) {
        JSON("{\"vn_m_s\":%.3f,\"ve_m_s\":%.3f,\"vd_m_s\":%.3f,\"age_ms\":%u}",
             (double)m->vn_m_s, (double)m->ve_m_s, (double)m->vd_m_s, m->velocity_age_ms);
    } else JSON("null");
    JSON(",\"vehicle_attitude\":");
    if (m->have_vehicle_attitude && isfinite(m->vehicle_roll_rad) &&
        isfinite(m->vehicle_pitch_rad) && isfinite(m->vehicle_yaw_rad)) {
        JSON("{\"roll_rad\":%.6f,\"pitch_rad\":%.6f,\"yaw_rad\":%.6f,\"age_ms\":%u",
             (double)m->vehicle_roll_rad, (double)m->vehicle_pitch_rad,
             (double)m->vehicle_yaw_rad, m->vehicle_attitude_age_ms);
        /* Optional additive v1 field; older readers ignore it. */
        if (isfinite(m->vehicle_yaw_rate_rad_s)) {
            JSON(",\"yaw_rate_rad_s\":%.6f", (double)m->vehicle_yaw_rate_rad_s);
        }
        JSON("}");
    } else JSON("null");
    JSON(",\"gimbal_attitude\":");
    if (m->have_gimbal_attitude && isfinite(m->gimbal_roll_rad) &&
        isfinite(m->gimbal_pitch_rad) && isfinite(m->gimbal_yaw_rad)) {
        JSON("{\"roll_rad\":%.6f,\"pitch_rad\":%.6f,\"yaw_rad\":%.6f,\"age_ms\":%u",
             (double)m->gimbal_roll_rad, (double)m->gimbal_pitch_rad,
             (double)m->gimbal_yaw_rad, m->gimbal_attitude_age_ms);
        if (isfinite(m->gimbal_yaw_rate_rad_s)) {
            JSON(",\"yaw_rate_rad_s\":%.6f", (double)m->gimbal_yaw_rate_rad_s);
        }
        JSON("}");
    } else JSON("null");
    JSON(",\"heading_rad\":");
    if (m->have_position && isfinite(m->heading_rad)) JSON("%.6f", (double)m->heading_rad);
    else JSON("null");
    JSON(",\"zoom\":");
    if (isfinite(m->zoom) && m->zoom > 0.0f) JSON("%.3f", (double)m->zoom);
    else JSON("null");
    JSON(",\"hfov_deg\":");
    if (isfinite(m->hfov_deg) && m->hfov_deg > 0.0f && m->hfov_deg < 180.0f)
        JSON("%.4f", (double)m->hfov_deg);
    else JSON("null");
    JSON("}");
#undef JSON
    return used;
}

size_t ca_video_metadata_sei(enum ca_video_codec codec, const char *json,
                              size_t length, uint8_t *output, size_t capacity)
{
    uint8_t rbsp[CA_VIDEO_METADATA_JSON_MAX + 32U];
    size_t used = 0U, written = 0U;
    unsigned zeros = 0U;
    if (json == NULL || output == NULL || length == 0U ||
        length >= CA_VIDEO_METADATA_JSON_MAX ||
        (codec != CA_VIDEO_H264 && codec != CA_VIDEO_H265)) return 0U;
    rbsp[used++] = 5U; /* user_data_unregistered */
    size_t payload = sizeof(ca_video_metadata_uuid) + length;
    while (payload >= 255U) { rbsp[used++] = 255U; payload -= 255U; }
    rbsp[used++] = (uint8_t)payload;
    memcpy(rbsp + used, ca_video_metadata_uuid, sizeof(ca_video_metadata_uuid));
    used += sizeof(ca_video_metadata_uuid);
    memcpy(rbsp + used, json, length);
    used += length;
    rbsp[used++] = 0x80U; /* rbsp_trailing_bits */
    if (capacity < 6U) return 0U;
    output[written++] = 0U; output[written++] = 0U;
    output[written++] = 0U; output[written++] = 1U;
    output[written++] = codec == CA_VIDEO_H264 ? 6U : 39U << 1U;
    if (codec == CA_VIDEO_H265) output[written++] = 1U; /* temporal_id_plus1 */
    for (size_t i = 0U; i < used; i++) {
        if (zeros == 2U && rbsp[i] <= 3U) {
            if (written == capacity) return 0U;
            output[written++] = 3U;
            zeros = 0U;
        }
        if (written == capacity) return 0U;
        output[written++] = rbsp[i];
        zeros = rbsp[i] == 0U ? zeros + 1U : 0U;
    }
    return written;
}

static size_t start_code(const uint8_t *data, size_t length, size_t at)
{
    if (length - at >= 4U && data[at] == 0U && data[at + 1U] == 0U &&
        data[at + 2U] == 0U && data[at + 3U] == 1U) return 4U;
    if (length - at >= 3U && data[at] == 0U && data[at + 1U] == 0U &&
        data[at + 2U] == 1U) return 3U;
    return 0U;
}

bool ca_annexb_next(const uint8_t *data, size_t length, size_t *offset,
                    size_t *nal, size_t *nal_length)
{
    size_t at = *offset;
    while (at < length && start_code(data, length, at) == 0U) at++;
    if (at == length) return false;
    *nal = at + start_code(data, length, at);
    at = *nal;
    while (at < length && start_code(data, length, at) == 0U) at++;
    *offset = at;
    while (at > *nal && data[at - 1U] == 0U) at--;
    *nal_length = at - *nal;
    return true;
}

bool ca_video_nal_is_vcl(enum ca_video_codec codec, uint8_t header)
{
    unsigned type = codec == CA_VIDEO_H265 ? (header >> 1U) & 63U : header & 31U;
    return codec == CA_VIDEO_H265 ? type <= 31U : type >= 1U && type <= 5U;
}

int ca_video_metadata_insert(enum ca_video_codec codec,
                              const uint8_t *data, size_t length, uint64_t pts90k, float hfov_deg,
                              uint8_t **output, size_t *output_length)
{
    size_t offset = 0U, nal, nal_length;
    *output = NULL;
    *output_length = 0U;
    while (ca_annexb_next(data, length, &offset, &nal, &nal_length)) {
        if (nal_length == 0U || !ca_video_nal_is_vcl(codec, data[nal])) continue;
        struct ca_metadata metadata;
        struct timespec utc;
        char json[CA_VIDEO_METADATA_JSON_MAX];
        uint8_t sei[CA_VIDEO_METADATA_SEI_MAX];
        ca_metadata_snapshot(&metadata);
        metadata.hfov_deg = hfov_deg;
        if (clock_gettime(CLOCK_REALTIME, &utc) < 0) return -1;
        size_t json_length = ca_video_metadata_json(&metadata, &utc, pts90k,
                                                    json, sizeof(json));
        size_t sei_length = ca_video_metadata_sei(codec, json, json_length, sei, sizeof(sei));
        if (sei_length == 0U || length > SIZE_MAX - sei_length) { errno = EOVERFLOW; return -1; }
        uint8_t *annotated = (uint8_t*)(malloc(length + sei_length));
        if (annotated == NULL) return -1;
        /* nal points just after its start code. Keeping that start code
         * before SEI and adding a new one before VCL preserves AU ordering. */
        size_t code = nal >= 4U && data[nal - 4U] == 0U ? 4U : 3U;
        size_t insert = nal - code;
        memcpy(annotated, data, insert);
        memcpy(annotated + insert, sei, sei_length);
        memcpy(annotated + insert + sei_length, data + insert, length - insert);
        *output = annotated;
        *output_length = length + sei_length;
        return 1;
    }
    return 0;
}
