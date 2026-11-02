#include "apcam/target.h"
#include "camera_app/gimbal_rate.h"
#include <assert.h>
#include <stdio.h>
#include <initializer_list>

static void tracking_pulses()
{
    // Integrated motion stays within one minimum-speed pulse, including
    // variable update periods, signed low rates and the continuous region.
    for (float demand : {-20.0f, -6.0f, -5.9f, -1.0f, -0.05f, 0.05f, 1.0f, 5.9f, 6.0f, 20.0f}) {
        ca_tracking_rate_pulse pulse{};
        float desired = 0, actual = 0;
        for (unsigned i = 0; i < 2000; i++) {
            float dt = .03f + (i % 5) * .01f;
            float out = ca_tracking_rate_command(demand, 6, dt, &pulse);
            assert(out == 0 || fabsf(out) >= 6);
            assert(out == 0 || out * demand > 0);
            desired += demand * dt; actual += out * dt;
            assert(fabsf(actual - desired) <= 6 * .071f);
        }
    }
    ca_tracking_rate_pulse pulse{};
    assert(ca_tracking_rate_command(1, 6, .05f, &pulse) == 0);
    assert(ca_tracking_rate_command(-1, 6, .05f, &pulse) == 0);
    assert(pulse.angle < 0); // Opposite-direction credit was discarded.
    assert(ca_tracking_rate_command(0, 6, .05f, &pulse) == 0 && pulse.angle == 0);
    assert(ca_tracking_rate_command(NAN, 6, .05f, &pulse) == 0 && pulse.angle == 0);
    assert(ca_tracking_rate_command(1, 6, 0, &pulse) == 0 && pulse.angle == 0);
    assert(ca_tracking_rate_command(1.25f, 0, .05f, &pulse) == 1.25f);
    // A small stationary pointing error must close despite the measured MT11
    // dead zone and actuator lag. This was previously a non-moving equilibrium.
    float angle = -3, velocity = 0;
    for (unsigned i = 0; i < 300; i++) {
        float demand = 1.2f * fmaxf(0, -angle - .2f);
        float out = ca_tracking_rate_command(demand, 6, .05f, &pulse);
        velocity += (.05f / (.04f + .05f)) * (out - velocity);
        angle += velocity * .05f;
    }
    assert(fabsf(angle) < .4f);
}

int main(void)
{
    const float pitch[] = APCAM_VENDOR_PITCH_RATE_CURVE;
    const float yaw[] = APCAM_VENDOR_YAW_RATE_CURVE;
#define COMMAND(rate, curve) ca_gimbal_rate_command(rate, curve, sizeof(curve)/sizeof(curve[0]))
    assert(COMMAND(0, pitch) == 0);
    assert(COMMAND(2, pitch) == 0);
    assert(COMMAND(-2, pitch) == 0);
    assert(COMMAND(3, pitch) == 6);
    assert(COMMAND(-3, pitch) == -6);
    assert(COMMAND(10, pitch) == 14);
    assert(COMMAND(-10, pitch) == -14);
    assert(COMMAND(10, yaw) == 12);
    assert(COMMAND(-10, yaw) == -14);
    assert(COMMAND(200, yaw) == 100);
    assert(COMMAND(-200, yaw) == -100);
    tracking_pulses();
    puts("A8 rate conversion and tracking low-rate pulse/stop/reversal tests passed");
    return 0;
}
