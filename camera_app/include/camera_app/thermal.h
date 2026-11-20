#ifndef CAMERA_APP_THERMAL_H
#define CAMERA_APP_THERMAL_H

#include <stdint.h>
#include <stdbool.h>

struct ca_thermal_range {
    int32_t maximum_centi_c;
    int32_t minimum_centi_c;
    uint16_t maximum_x;
    uint16_t maximum_y;
    uint16_t minimum_x;
    uint16_t minimum_y;
    uint64_t frame_sequence;
    uint64_t sampled_us; /* CLOCK_MONOTONIC; zero means no measurement. */
    bool rotated_180;   /* Native sensor coordinates need this display rotation. */
};

#define CA_THERMAL_MAX_AGE_US UINT64_C(250000)
static inline bool ca_thermal_range_fresh(const struct ca_thermal_range *range, uint64_t now)
{
    return range->sampled_us && now >= range->sampled_us &&
           now - range->sampled_us <= CA_THERMAL_MAX_AGE_US;
}
/* SIYI's legacy unsigned wire fields cannot represent the full sensor range.
 * Preserve their saturating encoding; MAVLink uses the full signed values. */
static inline uint16_t ca_thermal_legacy_centi_c(int32_t value)
{
    return value < 0 ? 0 : value > UINT16_MAX ? UINT16_MAX : (uint16_t)value;
}
static inline unsigned ca_thermal_display_pixel(unsigned pixel, unsigned size, bool rotated)
{
    return rotated && pixel < size ? size - 1U - pixel : pixel;
}
#endif
