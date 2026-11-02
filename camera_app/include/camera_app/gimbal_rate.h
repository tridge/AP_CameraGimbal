#ifndef CAMERA_APP_GIMBAL_RATE_H
#define CAMERA_APP_GIMBAL_RATE_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* Invert measured signed command/rate pairs. Compare achievable integer
 * commands, including zero, so interpolation cannot hide a motor dead zone. */
static inline int8_t ca_gimbal_rate_command(float degrees_per_second,
                                           const float *curve, size_t count)
{
    int best = 0;
    float best_error = fabsf(degrees_per_second);
    size_t segment = 0;
    for (int command = -100; command <= 100; command++) {
        while (segment + 4 < count && command > curve[segment + 2]) segment += 2;
        float fraction = (command - curve[segment]) / (curve[segment + 2] - curve[segment]);
        float rate = curve[segment + 1] + fraction * (curve[segment + 3] - curve[segment + 1]);
        float error = fabsf(degrees_per_second - rate);
        if (error < best_error) {
            best_error = error;
            best = command;
        }
    }
    return (int8_t)best;
}
/* Tracking-only pulse-density conversion. `angle` stores demanded motion not
 * yet sent. Commands above the measured dead zone pass through; smaller rates
 * alternate zero and the minimum moving rate, preserving average motion.
 * Explicit SDK/MAVLink rate commands do not use this conversion. */
struct ca_tracking_rate_pulse { float angle; };
static inline float ca_tracking_rate_command(float demand, float minimum,
                                             float dt, struct ca_tracking_rate_pulse *pulse)
{
    if (!isfinite(demand) || !isfinite(dt) || dt <= 0 || demand == 0) {
        pulse->angle = 0;
        return 0;
    }
    if (!(minimum > 0) || fabsf(demand) >= minimum) {
        pulse->angle = 0;
        return demand;
    }
    if (pulse->angle * demand < 0) pulse->angle = 0;
    pulse->angle += demand * dt;
    const float quantum = minimum * dt;
    if (fabsf(pulse->angle) + quantum * 1.0e-6f >= quantum) {
        pulse->angle -= copysignf(quantum, demand);
        return copysignf(minimum, demand);
    }
    return 0;
}
#endif
