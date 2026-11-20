#ifndef CAMERA_APP_EXPOSURE_H
#define CAMERA_APP_EXPOSURE_H
#include "apcam/compiler.h"
#include <stdint.h>
#include <math.h>

/* AE BIN record and local capture IPC. Lens is the zero-based target lens;
 * source 0 = ISP, 1 = SITL image model. Invalid floats are NaN, never zero.
 * Gains are multipliers and shutter is microseconds. Luminance uses the
 * backend's reported scale; SigmaStar AE values are not 8-bit pixel values. */
enum ca_exposure_valid {
    CA_AE_SHUTTER=1, CA_AE_AGAIN=2, CA_AE_DGAIN=4, CA_AE_IGAIN=8,
    CA_AE_LUMA=16, CA_AE_TARGET=32, CA_AE_ERROR=64, CA_AE_MODE=128,
    CA_AE_STABLE=256, CA_AE_LIMIT=512
};
enum ca_exposure_mode {
    CA_AE_AUTO=0, CA_AE_MANUAL=1, CA_AE_GAIN_PRIORITY=2,
    CA_AE_SHUTTER_PRIORITY=3, CA_AE_UNKNOWN=255
};
/* State flags are meaningful only when the corresponding Valid bit is set. */
#define CA_AE_STATE_STABLE 1U
#define CA_AE_STATE_LIMIT 2U
struct __attribute__((packed)) ca_exposure {
    uint64_t time_us;
    uint8_t lens, source;
    uint16_t valid;
    uint8_t mode, state;
    int32_t result;
    float shutter_us, analog_gain, digital_gain, isp_gain, luma, target, error;
};
APC_STATIC_ASSERT(sizeof(struct ca_exposure)==46, "AE log/IPC layout");
static inline struct ca_exposure ca_exposure_empty(unsigned lens, uint64_t time_us)
{
    struct ca_exposure result = {};
    result.time_us = time_us;
    result.lens = (uint8_t)lens;
    result.mode = CA_AE_UNKNOWN;
    result.shutter_us = result.analog_gain = result.digital_gain = result.isp_gain = NAN;
    result.luma = result.target = result.error = NAN;
    return result;
}
#endif
