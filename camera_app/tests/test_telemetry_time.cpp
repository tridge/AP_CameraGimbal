/* Exercise the actual MAVLink handlers with a deterministic local clock. */
#define clock_gettime test_clock_gettime
#include "../src/protocol/mavlink_server.cpp"
#undef clock_gettime
#include <assert.h>
#include <initializer_list>

static uint64_t now_ms = 100000;
static uint64_t metadata_attitude_ms, metadata_position_ms;
static float metadata_pitch, metadata_yaw_rate;
int test_clock_gettime(clockid_t clock, struct timespec *value)
{
    assert(clock == CLOCK_MONOTONIC);
    value->tv_sec = now_ms / 1000;
    value->tv_nsec = (now_ms % 1000) * 1000000;
    return 0;
}
void ca_log(const char *, ...) {}
uint64_t ca_binlog_time_us() { return now_ms * 1000; }
void ca_binlog_emit(uint8_t, const void *, size_t) {}
void ca_metadata_set_vehicle_attitude_motion(float, float pitch, float, float rate, uint64_t stamp)
{ metadata_attitude_ms = stamp; metadata_pitch = pitch; metadata_yaw_rate = rate; }
void ca_metadata_set_position(int32_t, int32_t, float, float, float, uint64_t stamp)
{ metadata_position_ms = stamp; }
void ca_metadata_set_velocity(float, float, float, uint64_t) {}

static void attitude(ca_mavlink_server &server, uint64_t remote_us, float yaw, float rate)
{
    float q[4];
    euler_to_quaternion(0, 0, yaw, q);
    mavlink_message_t msg;
    mavlink_msg_autopilot_state_for_gimbal_device_pack(42, 1, &msg, 42, 154,
        remote_us, q, 0, 0, 0, 0, 0, rate, 0, 0, rate);
    handle_autopilot_state_for_gimbal(&server, &msg);
}

static void position(ca_mavlink_server &server, uint32_t remote_ms, int32_t lat)
{
    mavlink_message_t msg;
    mavlink_msg_global_position_int_pack(42, 1, &msg, remote_ms, lat, 1490000000,
                                         600000, 100000, 1000, 0, 0, 0);
    handle_global_position_int(&server, &msg);
}

static void clock_tests()
{
    ca_telemetry_clock clock {};
    uint64_t stamp;
    bool reset;
    // Different boot epochs, transport delays 0..80 ms, multiple drift windows.
    for (unsigned i = 0; i < 350; i++) {
        uint64_t remote = 2000000 + i * 100000;
        unsigned jitter = (i % 5) * 20000;
        assert(clock.sample(remote, 1, remote + 98000000 + jitter, stamp, reset));
        assert(stamp == (remote + 98000000) / 1000 && !reset);
    }
    assert(!clock.sample(clock.last_raw, 1, clock.received_us + 10000, stamp, reset));
    assert(!clock.sample(clock.last_raw - 100000, 1, clock.received_us + 20000, stamp, reset));
    // A remote restart after an outage reacquires the offset.
    assert(clock.sample(1000000, 1, clock.received_us + 2000000, stamp, reset) && reset);
    assert(stamp == clock.received_us / 1000);
    // uint32 micros carried in uint64 time_boot_us, and uint32 milliseconds.
    for (uint32_t tick : {1U, 1000U}) {
        clock = {};
        assert(clock.sample(UINT32_MAX - 999, tick, 100000000, stamp, reset));
        assert(clock.sample(1000, tick, 100000000 + 2000ULL * tick, stamp, reset));
        assert(stamp == (100000000 + 2000ULL * tick) / 1000 && !reset);
        assert(!clock.sample(UINT32_MAX - 499, tick, clock.received_us + 1000, stamp, reset));
    }
    // The AP_RTC maximum-lag bound and clock drift recovery.
    clock = {};
    assert(clock.sample(1000000, 1, 100000000, stamp, reset));
    assert(clock.sample(1100000, 1, 101000000, stamp, reset));
    assert(stamp == 100500);
}

static void history_burst_tests()
{
    ca_telemetry_clock clock {};
    ca_yaw_history history {};
    uint64_t stamp;
    bool reset;
    assert(clock.sample(990000, 1, 1070000, stamp, reset));
    history.add(stamp, 0, 1);
    assert(clock.sample(1000000, 1, 1080000, stamp, reset));
    history.add(stamp, .01f, 1);
    // Buffered packets can map to the same millisecond as the offset improves.
    assert(clock.sample(1010000, 1, 1080000, stamp, reset));
    history.add(stamp, .02f, 2);
    float yaw, rate;
    assert(history.count == 2);
    assert(history.at(1075, yaw, rate));
    assert(fabsf(yaw - .01f) < 1e-6 && rate == 1.5f);
    assert(history.at(1080, yaw, rate) && yaw == .02f && rate == 2);
    history.add(1079, 9, 9); // a regressing estimate must not destroy history
    assert(history.at(1075, yaw, rate) && fabsf(yaw - .01f) < 1e-6);

    ca_mavlink_server server {};
    server.vehicle_yaw_history = history;
    save_vehicle_attitude(&server, 0, 0, 1, 2, 1090, false, true);
    // Feedback just before the first sample extrapolates backwards from it.
    assert(server.vehicle_yaw_history.at(1075, yaw, rate) && rate == 2);
    assert(fabsf(yaw - (1 - 2 * .015f)) < 1e-6);
    assert(!server.vehicle_yaw_history.at(839, yaw, rate));
    assert(server.vehicle_yaw_history.at(1090, yaw, rate) && yaw == 1);
}

static void fallback_tests()
{
    ca_mavlink_server server {};
    server.autopilot_system_id = 42;
    server.autopilot_component_id = 1;
    server.system_id = 42;
    server.gimbal_component_id = 154;
    now_ms = 200000;
    attitude(server, 1000000, 0, 1);
    mavlink_message_t msg;
    mavlink_msg_attitude_pack(42, 1, &msg, 1000, 0, 0, .5f, 0, 0, .5f);
    handle_attitude(&server, &msg); // establish fallback clock while primary is fresh
    float yaw, rate;
    now_ms = 200250;
    mavlink_msg_attitude_pack(42, 1, &msg, 1250, 0, 0, .5f, 0, 0, .5f);
    handle_attitude(&server, &msg);
    assert(server.vehicle_attitude_primary && current_vehicle_attitude(&server, &yaw, &rate));
    now_ms++;
    mavlink_msg_attitude_pack(42, 1, &msg, 1251, 0, 0, .5f, 0, 0, .5f);
    handle_attitude(&server, &msg);
    assert(!server.vehicle_attitude_primary);
    assert(current_vehicle_attitude(&server, &yaw, &rate) && yaw == .5f && rate == .5f);
    now_ms = 200260;
    attitude(server, 1260000, .26f, 1);
    assert(server.vehicle_attitude_primary && current_vehicle_attitude(&server, &yaw, &rate));

    // Euler yaw rate is undefined at vertical pitch, but the attitude still
    // belongs in metadata. It must not supply a NaN to control prediction.
    now_ms = 200600;
    for (float pitch : {PI_F / 2, -PI_F / 2}) {
        mavlink_msg_attitude_pack(42, 1, &msg, now_ms - 199000,
                                  .1f, pitch, .8f, 0, 0, .5f);
        handle_attitude(&server, &msg);
        assert(metadata_attitude_ms == now_ms && metadata_pitch == pitch);
        assert(isnan(metadata_yaw_rate));
        // The held primary sample still serves control, never the NaN rate.
        assert(current_vehicle_attitude(&server, &yaw, &rate) && rate == 1);
        now_ms += 100;
    }
    mavlink_msg_attitude_pack(42, 1, &msg, 1800, 0, 0, .8f, 0, 0, .5f);
    handle_attitude(&server, &msg);
    assert(current_vehicle_attitude(&server, &yaw, &rate) && rate == .5f);
    now_ms = 100000;
}

int main()
{
    clock_tests();
    history_burst_tests();
    fallback_tests();
    ca_mavlink_server server {};
    server.autopilot_system_id = 42;
    server.autopilot_component_id = 1;
    server.system_id = 42;
    server.gimbal_component_id = 154;
    server.started_ms = 99000;
    // Steady 1 rad/s turn. Late packets retain their generation time, and the
    // control prediction reaches the current yaw instead of lagging reception.
    attitude(server, 2000000, 0, 1);
    position(server, 2000, -350000000);
    now_ms = 100180;
    attitude(server, 2100000, .1f, 1);
    position(server, 2100, -349999910);
    assert(metadata_attitude_ms == 100100 && metadata_position_ms == 100100);
    float yaw, rate;
    assert(current_vehicle_attitude(&server, &yaw, &rate));
    assert(fabsf(yaw - .18f) < 1e-5 && rate == 1);
    int32_t lat;
    assert(current_vehicle_position(&server, &lat, nullptr, nullptr));
    assert(lat > server.vehicle_lat_e7 + 70); // includes the 80 ms transport age
    // Duplicate/out-of-order samples cannot replace state or refresh its age.
    attitude(server, 2100000, 2, 9);
    position(server, 2000, 0);
    assert(metadata_attitude_ms == 100100 && metadata_position_ms == 100100);
    assert(server.vehicle_lat_e7 == -349999910);

    // An unrelated source and the fallback cannot replace fresh primary data.
    mavlink_message_t fallback;
    mavlink_msg_attitude_pack(42, 1, &fallback, 2180, 0, 0, 2, 0, 0, 9);
    handle_attitude(&server, &fallback);
    assert(server.vehicle_attitude_primary && metadata_attitude_ms == 100100);
    mavlink_msg_global_position_int_pack(43, 1, &fallback, 2180, 0, 0, 0, 0, 0, 0, 0, 0);
    handle_global_position_int(&server, &fallback);
    assert(server.vehicle_lat_e7 == -349999910);

    // Rate changes after a gimbal sample must not contaminate that sample.
    now_ms = 100200;
    attitude(server, 2200000, .3f, 3);
    ca_gimbal_attitude gimbal {};
    gimbal.timestamp_ms = 100100;
    gimbal.yaw_rad = -.1f;
    gimbal.yaw_rate_rad_s = -1;
    server.yaw_lock = true;
    mavlink_message_t msg;
    pack_gimbal_status(&server, &msg, &gimbal);
    mavlink_gimbal_device_attitude_status_t status;
    mavlink_msg_gimbal_device_attitude_status_decode(&msg, &status);
    assert(fabsf(status.angular_velocity_z) < 1e-6);
    float roll, pitch;
    quaternion_to_euler(status.q, &roll, &pitch, &yaw);
    assert(fabsf(yaw) < 1e-6);
    assert(status.time_boot_ms == 1100);
    assert(status.flags & GIMBAL_DEVICE_FLAGS_YAW_IN_EARTH_FRAME);
    now_ms += 50;
    pack_gimbal_status(&server, &msg, &gimbal);
    mavlink_msg_gimbal_device_attitude_status_decode(&msg, &status);
    assert(status.time_boot_ms == 1100 && fabsf(status.angular_velocity_z) < 1e-6);

    // Interpolation crosses +/-pi by the short path, including ring wrap.
    ca_yaw_history history {};
    for (unsigned i = 0; i < 200; i++) history.add(i * 10, 0, 0);
    history.add(2000, 179 * PI_F / 180, 1);
    history.add(2100, -179 * PI_F / 180, 3);
    assert(history.at(2050, yaw, rate));
    assert(fabsf(fabsf(yaw) - PI_F) < 1e-5 && rate == 2);
    assert(!history.at(0, yaw, rate));
    // Beyond the newest sample extrapolation stops at 250 ms and the value
    // is held for up to hold_ms.
    assert(history.at(2400, yaw, rate) && rate == 3);
    assert(fabsf(remainderf(yaw - (-179 * PI_F / 180 + 3 * .25f), 2 * PI_F)) < 1e-5);
    assert(history.at(3100, yaw, rate));
    assert(!history.at(3101, yaw, rate));
    gimbal.timestamp_ms = 90000; // outside the available vehicle history
    now_ms = 101300;             // and the current vehicle yaw has expired
    pack_gimbal_status(&server, &msg, &gimbal);
    mavlink_msg_gimbal_device_attitude_status_decode(&msg, &status);
    assert(status.flags & GIMBAL_DEVICE_FLAGS_YAW_IN_VEHICLE_FRAME);
    assert(!(status.flags & GIMBAL_DEVICE_FLAGS_YAW_IN_EARTH_FRAME));
    assert(status.angular_velocity_z == -1);
    now_ms = 101700; // position held for 1.5 s after its 100100 sample
    assert(!current_vehicle_position(&server, &lat, nullptr, nullptr));
    now_ms = 101400; // attitude held for 1 s after its 100200 sample
    assert(!current_vehicle_attitude(&server, &yaw, &rate));
    // ATTITUDE takes over only after primary expiry and becomes usable for ROI.
    mavlink_msg_attitude_pack(42, 1, &msg, 3400, 0, 0, .8f, 0, 0, .5f);
    handle_attitude(&server, &msg);
    assert(current_vehicle_attitude(&server, &yaw, &rate));
    assert(fabsf(yaw - .8f) < 1e-6 && rate == .5f);
    assert(!server.vehicle_attitude_primary);
    puts("telemetry clock mapping, rollover, restart, history and handler tests passed");
}
