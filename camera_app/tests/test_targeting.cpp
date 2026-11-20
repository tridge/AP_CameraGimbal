#include "camera_app/targeting.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#define PI_F 3.14159265358979323846f

static float degrees(float radians)
{
    return radians * (180.0f / PI_F);
}

static void expect_angles(int32_t vehicle_lat, int32_t vehicle_lon,
                          float vehicle_alt, int32_t target_lat,
                          int32_t target_lon, float target_alt,
                          float expected_pitch, float expected_yaw,
                          float tolerance)
{
    float pitch;
    float yaw;
    assert(ca_targeting_global_angles(vehicle_lat, vehicle_lon, vehicle_alt,
                                      target_lat, target_lon, target_alt,
                                      &pitch, &yaw));
    assert(fabsf(degrees(pitch) - expected_pitch) < tolerance);
    assert(fabsf(degrees(yaw) - expected_yaw) < tolerance);
}

int main(void)
{
    const int32_t latitude = -353632620;
    const int32_t longitude = 1491652370;
    const int32_t north_100m = latitude + 8983;
    const int32_t east_100m = longitude + 10996;

    expect_angles(latitude, longitude, 600.0f,
                  north_100m, longitude, 550.0f,
                  -26.565f, 0.0f, 0.1f);
    expect_angles(latitude, longitude, 600.0f,
                  latitude, east_100m, 600.0f,
                  0.0f, 90.0f, 0.1f);
    expect_angles(latitude, 1799999500, 100.0f,
                  latitude, -1799999500, 100.0f,
                  0.0f, 90.0f, 0.1f);
    expect_angles(latitude, longitude, 100.0f,
                  latitude, longitude, 50.0f,
                  -90.0f, 0.0f, 0.1f);

    float pitch = 1.0f;
    float yaw = 2.0f;
    assert(!ca_targeting_global_angles(900000001, longitude, 100.0f,
                                       latitude, longitude, 100.0f,
                                       &pitch, &yaw));
    assert(pitch == 1.0f && yaw == 2.0f);
    assert(!ca_targeting_global_angles(latitude, longitude, NAN,
                                       latitude, longitude, 100.0f,
                                       &pitch, &yaw));

    /* A circling aircraft keeps a centre ROI at a fixed relative bearing.
     * Position telemetry arrives at 4 Hz while control runs at 20 Hz. */
    float worst = 0, old_worst = 0;
    for (unsigned i = 0; i < 400; i++) {
        double now = i * 0.05, sampled = (i / 5) * 0.25;
        double phase = sampled * 0.25, current_phase = now * 0.25;
        double north = 100 * cos(phase), east = 100 * sin(phase);
        double metres_per_e7 = 6378137.0 * (PI_F / 180.0) * 1.0e-7;
        int32_t lat = latitude + (int32_t)llround(north / metres_per_e7);
        int32_t lon = longitude + (int32_t)llround(east / (metres_per_e7 * cos(latitude * 1.0e-7 * PI_F / 180.0)));
        float alt = 600;
        assert(ca_targeting_global_angles(lat, lon, alt, latitude, longitude, 500, &pitch, &yaw));
        old_worst = fmaxf(old_worst, fabsf(remainderf(yaw - current_phase - PI_F, 2 * PI_F)));
        assert(ca_targeting_predict_position(&lat, &lon, &alt,
                                             -25 * sin(phase), 25 * cos(phase), 2,
                                             now - sampled));
        assert(fabs(alt - (600 - 2 * (now - sampled))) < 0.001);
        assert(ca_targeting_global_angles(lat, lon, alt, latitude, longitude, 500, &pitch, &yaw));
        worst = fmaxf(worst, fabsf(remainderf(yaw - current_phase - PI_F, 2 * PI_F)));
    }
    assert(degrees(old_worst) > 2.8f);
    assert(degrees(worst) < 0.03f);
    int32_t lat = latitude, lon = 1799999990;
    float alt = 100;
    assert(ca_targeting_predict_position(&lat, &lon, &alt, 0, 25, 0, 0.25));
    assert(lon < -1799990000); /* wrap correctly across the date line */
    assert(!ca_targeting_predict_position(&lat, &lon, &alt, NAN, 0, 0, 0.1));
    assert(!ca_targeting_predict_position(&lat, &lon, &alt, 0, 0, 0, -1));
    printf("circling ROI: peak bearing error %.3f -> %.3f degrees\n",
           degrees(old_worst), degrees(worst));

    float pr, yr;
    // Stationary target north, aircraft translating east: clockwise bearing
    // decreases. Northward motion towards an elevated target raises pitch.
    assert(ca_targeting_global_rates(latitude, longitude, 600, north_100m, longitude, 650,
                                     0, 10, 0, &pr, &yr));
    assert(fabsf(yr + .1f) < .0001f && fabsf(pr) < .0001f);
    assert(ca_targeting_global_rates(latitude, longitude, 600, north_100m, longitude, 650,
                                     10, 0, 0, &pr, &yr));
    assert(fabsf(pr - .04f) < .0001f && fabsf(yr) < .0001f);
    assert(ca_targeting_global_rates(latitude, longitude, 600, north_100m, longitude, 650,
                                     0, 0, 5, &pr, &yr));
    assert(fabsf(pr - .04f) < .0001f);
    assert(ca_targeting_global_rates(latitude, longitude, 600, north_100m, longitude, 650,
                                     0, 0, 0, &pr, &yr));
    assert(pr == 0 && yr == 0);
    // Date-line crossing is a short eastward LOS, not a nearly-global vector.
    assert(ca_targeting_global_rates(latitude, 1799999500, 600, latitude, -1799999500, 600,
                                     1, 0, 0, &pr, &yr));
    assert(yr > .1f && yr < .12f && fabsf(pr) < .0001f);
    assert(!ca_targeting_global_rates(latitude, longitude, 600, latitude, longitude, 500,
                                      1, 0, 0, &pr, &yr));
    assert(!ca_targeting_global_rates(latitude, longitude, 600, north_100m, longitude, 500,
                                      NAN, 0, 0, &pr, &yr));
    puts("targeting position, LOS angle and analytic rate tests passed");
    return 0;
}
