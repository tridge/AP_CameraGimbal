#pragma once
#include <cmath>

// Tracking-only pulse-density conversion. These plain fields also support
// the zero-initialized MAVLink server; explicit rate commands bypass it.
struct ca_tracking_rate_pulse {
    float angle;   // demanded minus delivered motion through the last tick
    float rate;    // demand held during the interval which just ended
    float command; // command actually held during that interval
};

static inline float ca_tracking_rate_command(float rate, float minimum, float dt,
                                             ca_tracking_rate_pulse *pulse)
{
    if (!std::isfinite(rate) || !std::isfinite(dt) || dt <= 0 || dt > .1f) {
        *pulse = {};
        return 0;
    }
    if (rate == 0 || !(minimum > 0) || std::fabs(rate) >= minimum) {
        *pulse = {};
        return rate;
    }
    // Direction changes discard the previous direction's credit/debt. Do not
    // debit a new pulse using a past interval: it will be held until next tick.
    if (pulse->rate * rate <= 0) *pulse = {};
    pulse->angle += (pulse->rate - pulse->command) * dt;
    const float direction = std::copysign(1.f, rate);
    // Use the last period only to predict the next pulse length. Any timing
    // error is corrected by accounting for its actual held time on the next call.
    const float quantum = minimum * dt;
    const bool moving = direction * pulse->angle + std::fabs(rate) * dt >= quantum * .5f;
    pulse->rate = rate;
    pulse->command = moving ? direction * minimum : 0;
    return pulse->command;
}

class ca_tracking_rate_pulses {
public:
    void reset() { _pulse = {}; }
    float apply(float rate, float minimum, float dt) {
        return ca_tracking_rate_command(rate, minimum, dt, &_pulse);
    }
private:
    ca_tracking_rate_pulse _pulse{};
};
