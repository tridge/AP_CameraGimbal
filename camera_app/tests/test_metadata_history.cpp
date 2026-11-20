// Deterministic frame-time interpolation and rejection of stale telemetry.
#include <time.h>
#include <stdint.h>
static uint64_t test_ms;
static int history_clock(clockid_t, timespec *t);
#define clock_gettime history_clock
#include "../src/recording/metadata.cpp"
#undef clock_gettime
#include <assert.h>
static int history_clock(clockid_t, timespec *t)
{
    t->tv_sec=test_ms/1000; t->tv_nsec=(test_ms%1000)*1000000;
    return 0;
}
static void sample(uint64_t ms, int32_t lat, float yaw)
{
    test_ms=ms;
    ca_metadata_set_position(lat,1490000000,1000,400,0,ms);
    ca_metadata_set_velocity(25,0,0,ms);
    ca_metadata_set_vehicle_attitude_motion(0,0,yaw,0,ms);
    ca_metadata_set_gimbal_attitude_motion(0,-1,0,0,ms);
    ca_metadata_record_sample(ms);
}
int main()
{
    sample(10000,-350000000,3.13f);
    sample(10100,-349999775,-3.13f);
    ca_metadata m;
    assert(ca_metadata_at(10050,&m));
    assert(abs(m.lat_e7-(-349999885))<4);
    assert(fabs(fabs(m.vehicle_yaw_rad)-M_PI)<.001);
    assert(!ca_metadata_at(9999,&m));
    assert(!ca_metadata_at(10301,&m));
    test_ms=11000; ca_metadata_record_sample(test_ms);
    assert(!ca_metadata_at(11000,&m));
    puts("PASS exposure metadata interpolation, yaw wrap and stale rejection");
}
