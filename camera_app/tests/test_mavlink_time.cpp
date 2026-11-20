/* Exercise the real message handler with clocks that cannot alter host time.
 * Unrelated server functions are discarded by --gc-sections. */
#define clock_gettime test_clock_gettime
#define clock_settime test_clock_settime
#include "../src/protocol/mavlink_server.cpp"
#undef clock_gettime
#undef clock_settime

#include <assert.h>

static struct timespec fake_now;
static unsigned set_calls;
static bool fail_set;

int test_clock_gettime(clockid_t clock, struct timespec *value)
{
    assert(clock == CLOCK_REALTIME);
    *value = fake_now;
    return 0;
}

int test_clock_settime(clockid_t clock, const struct timespec *value)
{
    assert(clock == CLOCK_REALTIME);
    set_calls++;
    if (fail_set) {
        errno = EPERM;
        return -1;
    }
    fake_now = *value;
    return 0;
}

void ca_log(const char *format, ...) { (void)format; }

static void send_time(struct ca_mavlink_server *server, uint8_t system,
                      uint8_t component, uint64_t unix_us)
{
    mavlink_message_t message;
    mavlink_msg_system_time_pack(system, component, &message, unix_us, 1000);
    handle_system_time(server, &message);
}

int main(void)
{
    struct ca_mavlink_server server = {
        .autopilot_system_id = 42, .autopilot_component_id = 1,
    };
    /* 2026-09-01T00:00:00Z, including the fractional microsecond conversion. */
    const uint64_t valid_us = UINT64_C(1788220800123456);
    const time_t old_dates[] = {-1, 0, 1600000000, 1788220799};
    for (unsigned i = 0; i < sizeof(old_dates) / sizeof(old_dates[0]); i++) {
        fake_now.tv_sec = old_dates[i];
        set_calls = 0;
        send_time(&server, 43, 1, valid_us);
        send_time(&server, 42, 100, valid_us);
        send_time(&server, 42, 1, 0);
        send_time(&server, 42, 1, UINT64_C(1788220799999999));
        assert(set_calls == 0);
        send_time(&server, 42, 1, valid_us);
        assert(set_calls == 1);
        assert(fake_now.tv_sec == 1788220800);
        assert(fake_now.tv_nsec == 123456000);
        send_time(&server, 42, 1, valid_us + UINT64_C(10000000));
        assert(set_calls == 1); /* A valid clock is not repeatedly stepped. */
    }
    fake_now.tv_sec = 1788220800;
    fake_now.tv_nsec = 0;
    set_calls = 0;
    send_time(&server, 42, 1, valid_us);
    assert(set_calls == 0); /* The cutoff itself is already valid. */
    fake_now.tv_sec = 1800000000;
    send_time(&server, 42, 1, valid_us);
    assert(set_calls == 0); /* Do not move a valid clock backwards. */

    fake_now.tv_sec = 0;
    fail_set = true;
    send_time(&server, 42, 1, valid_us);
    assert(set_calls == 1 && fake_now.tv_sec == 0);
    fail_set = false;
    send_time(&server, 42, 1, valid_us);
    assert(set_calls == 2 && fake_now.tv_sec == 1788220800);
    puts("MAVLink SYSTEM_TIME source, cutoff, conversion and retry tests passed");
    return 0;
}
