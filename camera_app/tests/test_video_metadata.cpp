#include "camera_app/video_metadata.h"
#include "camera_app/video_fov.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_sei(enum ca_video_codec codec)
{
    char payload[CA_VIDEO_METADATA_JSON_MAX - 1U] = {};
    uint8_t sei[CA_VIDEO_METADATA_SEI_MAX];
    uint8_t rbsp[sizeof(sei)];
    size_t size = ca_video_metadata_sei(codec, payload, sizeof(payload), sei, sizeof(sei));
    assert(size != 0U);
    assert(ca_video_metadata_sei(codec, payload, sizeof(payload), sei, size - 1U) == 0U);
    assert(ca_video_metadata_sei(codec, payload, sizeof(payload), sei, sizeof(sei)) == size);
    assert(memcmp(sei, "\0\0\0\1", 4U) == 0);
    assert(sei[4] == (codec == CA_VIDEO_H264 ? 6U : 78U));
    if (codec == CA_VIDEO_H265) assert(sei[5] == 1U);
    size_t count = 0U;
    unsigned zeros = 0U;
    for (size_t i = codec == CA_VIDEO_H264 ? 5U : 6U; i < size; i++) {
        if (zeros == 2U) {
            assert(sei[i] >= 3U);
            if (sei[i] == 3U) { zeros = 0U; continue; }
        }
        rbsp[count++] = sei[i];
        zeros = sei[i] == 0U ? zeros + 1U : 0U;
    }
    assert(rbsp[0] == 5U);
    size_t at = 1U, payload_size = 0U;
    while (rbsp[at] == 255U) { payload_size += 255U; at++; }
    payload_size += rbsp[at++];
    assert(payload_size == sizeof(payload) + 16U);
    assert(memcmp(rbsp + at, ca_video_metadata_uuid, 16U) == 0);
    assert(memcmp(rbsp + at + 16U, payload, sizeof(payload)) == 0);
    assert(rbsp[count - 1U] == 0x80U);
    assert(count == at + payload_size + 1U);
}

int main(void)
{
    struct ca_metadata m = {};
    struct timespec utc = {.tv_sec = 1700000000, .tv_nsec = 123456000};
    char json[CA_VIDEO_METADATA_JSON_MAX];
    size_t size = ca_video_metadata_json(&m, &utc, 90000U, json, sizeof(json));
    assert(size > 0U);
    assert(strstr(json, "\"position\":null") != NULL);
    assert(strstr(json, "\"vehicle_attitude\":null") != NULL);
    assert(strstr(json, "\"gimbal_attitude\":null") != NULL);
    assert(strstr(json, "\"utc_us\":1700000000123456") != NULL);
    assert(strstr(json, "\"pts90k\":90000") != NULL);
    assert(ca_video_metadata_json(&m, &utc, 0, json, 2U) == 0U);
    ca_metadata_set_velocity(25.0f, -10.0f, 2.0f);
    ca_metadata_set_vehicle_attitude_motion(0.1f, -0.2f, 0.3f, 0.4f);
    ca_metadata_snapshot(&m);
    assert(m.have_velocity && m.vn_m_s == 25.0f && m.ve_m_s == -10.0f && m.vd_m_s == 2.0f);
    assert(m.vehicle_yaw_rate_rad_s == 0.4f);
    assert(ca_video_metadata_json(&m, &utc, 0, json, sizeof(json)) > 0U);
    assert(strstr(json, "\"vn_m_s\":25.000") != NULL);
    assert(strstr(json, "\"yaw_rate_rad_s\":0.400000") != NULL);
    ca_metadata_set_vehicle_attitude(0.0f, 0.0f, 0.0f);
    ca_metadata_snapshot(&m);
    assert(ca_video_metadata_json(&m, &utc, 0, json, sizeof(json)) > 0U);
    assert(strstr(json, "yaw_rate_rad_s") == NULL);
    // A real backend sample preserves both measured rate and sample age.
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    uint64_t gimbal_ms = uint64_t(now.tv_sec) * 1000 + now.tv_nsec / 1000000 - 200;
    ca_metadata_set_gimbal_attitude_motion(0, -.2f, .3f, -.4f, gimbal_ms);
    ca_metadata_snapshot(&m);
    assert(m.have_gimbal_attitude && m.gimbal_yaw_rate_rad_s == -.4f);
    assert(m.gimbal_attitude_age_ms >= 200);
    assert(ca_video_metadata_json(&m, &utc, 0, json, sizeof(json)) > 0U);
    assert(strstr(json, "\"yaw_rate_rad_s\":-0.400000") != NULL);
    // Legacy attitude-only producers must clear any previously measured rate.
    ca_metadata_set_gimbal_attitude(0, 0, 0);
    ca_metadata_snapshot(&m);
    assert(isnan(m.gimbal_yaw_rate_rad_s));
    assert(ca_video_metadata_json(&m, &utc, 0, json, sizeof(json)) > 0U);
    assert(strstr(json, "yaw_rate_rad_s") == NULL);
    m.have_position = m.have_vehicle_attitude = m.have_gimbal_attitude = true;
    m.lat_e7 = -353632610; m.lon_e7 = 1491652300;
    m.alt_amsl_m = 620.25f; m.alt_relative_m = 36.5f;
    m.vehicle_yaw_rad = NAN; m.gimbal_pitch_rad = INFINITY;
    assert(ca_video_metadata_json(&m, &utc, 0, json, sizeof(json)) > 0U);
    assert(strstr(json, "\"lat_e7\":-353632610") != NULL);
    assert(strstr(json, "\"vehicle_attitude\":null") != NULL);
    assert(strstr(json, "\"gimbal_attitude\":null") != NULL);
    assert(strstr(json, "\"hfov_deg\":null") != NULL);
    m.hfov_deg = ca_video_hfov(88.0f, 2.0f);
    assert(m.hfov_deg > 51.0f && m.hfov_deg < 52.0f);
    assert(ca_video_metadata_json(&m, &utc, 0, json, sizeof(json)) > 0U);
    assert(strstr(json, "\"hfov_deg\":51.") != NULL);
    assert(ca_video_hfov(88.0f, 4.0f) < m.hfov_deg);
    assert(ca_video_hfov(88.0f, NAN) == 0.0f);
    /* Lens-specific calibration vectors are checked for each target by tests/test_targets.py. */
    m.hfov_deg = INFINITY;
    assert(ca_video_metadata_json(&m, &utc, 0, json, sizeof(json)) > 0U);
    assert(strstr(json, "\"hfov_deg\":null") != NULL);
    test_sei(CA_VIDEO_H264);
    test_sei(CA_VIDEO_H265);
    for (unsigned hevc = 0U; hevc < 2U; hevc++) {
        enum ca_video_codec codec = hevc ? CA_VIDEO_H265 : CA_VIDEO_H264;
        uint8_t au[] = {0, 0, 0, 1, (uint8_t)(hevc ? 70 : 9), 1, 0x80,
                        0, 0, 1, (uint8_t)(hevc ? 38 : 5), 1, 0x80};
        uint8_t *annotated;
        size_t length;
        assert(ca_video_metadata_insert(codec, au, sizeof(au), 1234, 88.0f,
                                         &annotated, &length) == 1);
        assert(memcmp(annotated, au, 7U) == 0); /* AUD remains first */
        size_t offset = 0U, nal, nal_length;
        assert(ca_annexb_next(annotated, length, &offset, &nal, &nal_length));
        assert(ca_annexb_next(annotated, length, &offset, &nal, &nal_length));
        assert(annotated[nal] == (hevc ? 78U : 6U));
        assert(ca_annexb_next(annotated, length, &offset, &nal, &nal_length));
        assert(annotated[nal] == (hevc ? 38U : 5U));
        assert(!ca_annexb_next(annotated, length, &offset, &nal, &nal_length));
        free(annotated);
        assert(ca_video_metadata_insert(codec, au, 7U, 1234, 88.0f,
                                         &annotated, &length) == 0);
        assert(annotated == NULL);
    }
    // Transport age must survive insertion into frame metadata.
    struct timespec sampled;
    assert(clock_gettime(CLOCK_MONOTONIC, &sampled) == 0);
    uint64_t sample_ms = uint64_t(sampled.tv_sec) * 1000 + sampled.tv_nsec / 1000000 - 200;
    ca_metadata_set_position(-353632610, 1491652300, 600, 20, 0, sample_ms);
    ca_metadata_set_velocity(1, 2, 3, sample_ms);
    ca_metadata_set_vehicle_attitude_motion(0, 0, 0, .1f, sample_ms);
    ca_metadata_snapshot(&m);
    assert(m.have_position && m.position_age_ms >= 200);
    assert(m.have_velocity && m.velocity_age_ms >= 200);
    assert(m.have_vehicle_attitude && m.vehicle_attitude_age_ms >= 200);
    sample_ms -= CA_METADATA_MAX_AGE_MS;
    ca_metadata_set_position(-353632610, 1491652300, 600, 20, 0, sample_ms);
    ca_metadata_set_velocity(1, 2, 3, sample_ms);
    ca_metadata_set_vehicle_attitude_motion(0, 0, 0, .1f, sample_ms);
    ca_metadata_snapshot(&m);
    assert(!m.have_position && !m.have_velocity && !m.have_vehicle_attitude);
    puts("PASS video telemetry JSON, H.264/H.265 SEI escaping and access-unit insertion");
    return 0;
}
