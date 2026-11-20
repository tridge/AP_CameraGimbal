#ifndef CAMERA_APP_TARGETING_H
#define CAMERA_APP_TARGETING_H

#include <stdbool.h>
#include <stdint.h>

/* Advance a position using NED velocity; callers bound the prediction time. */
bool ca_targeting_predict_position(int32_t *lat_e7, int32_t *lon_e7, float *alt_m,
                                   float vn, float ve, float vd, float seconds);

bool ca_targeting_global_angles(int32_t vehicle_lat_e7,
                                int32_t vehicle_lon_e7,
                                float vehicle_alt_amsl_m,
                                int32_t target_lat_e7,
                                int32_t target_lon_e7,
                                float target_alt_amsl_m,
                                float *pitch_rad,
                                float *yaw_earth_rad);

/* Instantaneous earth-frame LOS rates to a stationary ROI, using vehicle NED
 * velocity. Returns false at undefined bearings (within 10 cm or at a pole). */
bool ca_targeting_global_rates(int32_t vehicle_lat_e7, int32_t vehicle_lon_e7,
                               float vehicle_alt_amsl_m, int32_t target_lat_e7,
                               int32_t target_lon_e7, float target_alt_amsl_m,
                               float vn, float ve, float vd,
                               float *pitch_rate, float *yaw_rate);

#endif
