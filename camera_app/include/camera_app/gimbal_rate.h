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
#endif
