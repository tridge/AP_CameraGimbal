/* Exercise the lease protocol without hardware, including stale browser tokens. */
#define clock_gettime manual_test_clock
#include "../src/protocol/manual_control.cpp"
#undef clock_gettime
#include <assert.h>
#include <stdio.h>

static uint64_t clock_ms = 1000;
static float pitch_rate, yaw_rate;
static unsigned suspends;
static bool survey_mode;
bool ca_mavlink_server_survey_owns_gimbal(const ca_mavlink_server *) { return survey_mode; }
int manual_test_clock(clockid_t id, struct timespec *t) noexcept
{
    assert(id == CLOCK_MONOTONIC);
    t->tv_sec = clock_ms / 1000;
    t->tv_nsec = (clock_ms % 1000) * 1000000;
    return 0;
}
int ca_backend_set_gimbal_rates(struct ca_backend *b, float p, float y)
{
    (void)b;
    pitch_rate = p;
    yaw_rate = y;
    return 0;
}
int ca_backend_set_gimbal_neutral(struct ca_backend *b)
{
    (void)b;
    return 0;
}
int ca_backend_set_zoom(struct ca_backend *b, float z)
{
    (void)b;
    (void)z;
    assert(pitch_rate == 0 && yaw_rate == 0);
    return 0;
}
void ca_mavlink_server_suspend_gimbal(struct ca_mavlink_server *s)
{
    (void)s;
    suspends++;
}
void ca_log(const char *format, ...) { (void)format; }

static struct apcam_manual_packet exchange(struct ca_manual_control *c, int fd,
                                           struct apcam_manual_packet p)
{
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_port = htons(c->port),
                                  .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
    assert(sendto(fd, &p, sizeof(p), 0, (struct sockaddr *)&address, sizeof(address)) == sizeof(p));
    ca_manual_control_update(c);
    assert(recv(fd, &p, sizeof(p), 0) == sizeof(p));
    return p;
}
int main(void)
{
    struct ca_manual_control c = {.fd = -1};
    assert(ca_manual_control_open(&c, NULL, NULL) == 0);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    struct timeval timeout = {.tv_sec = 1};
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    struct apcam_manual_packet p = {.magic = APCAM_MANUAL_MAGIC, .action = APCAM_MANUAL_ACQUIRE};
    survey_mode = true;
    assert(exchange(&c, fd, p).result == -EBUSY && !c.active && suspends == 0);
    survey_mode = false;
    p = exchange(&c, fd, p);
    assert(p.result == 0 && c.active && suspends == 1);
    assert(exchange(&c, fd, p).result == -EBUSY);
    p.action = APCAM_MANUAL_RIGHT;
    p.value = 30;
    struct apcam_manual_packet forged = p;
    forged.token[0] ^= 1;
    assert(exchange(&c, fd, forged).result == -EACCES);
    survey_mode = true;
    assert(exchange(&c, fd, p).result == -EBUSY && yaw_rate == 0 && pitch_rate == 0);
    survey_mode = false;
    assert(exchange(&c, fd, p).result == 0 && yaw_rate > 0.5f && pitch_rate == 0);
    clock_ms += APCAM_MANUAL_PULSE_MS + 1;
    ca_manual_control_update(&c);
    assert(yaw_rate == 0 && c.active);
    p.action = APCAM_MANUAL_RIGHT;
    assert(exchange(&c, fd, p).result == 0 && yaw_rate > .5f);
    p.action = APCAM_MANUAL_ZOOM; p.value = 10;
    assert(exchange(&c, fd, p).result == 0 && yaw_rate == 0 && c.stop_ms == 0 && c.active);
    for (unsigned i = 0; i < 10; i++) {
        clock_ms += 1000;
        p.action = APCAM_MANUAL_RENEW;
        assert(exchange(&c, fd, p).result == 0 && c.active);
    }
    bool internal = false;
    assert(ca_backend_manual_blocked(&c.active, &internal, 0x07, NULL, 0));
    assert(!ca_backend_manual_blocked(&c.active, &internal, 0x0d, NULL, 0));
    internal = true;
    assert(!ca_backend_manual_blocked(&c.active, &internal, 0x07, NULL, 0));
    clock_ms += APCAM_MANUAL_LEASE_MS + 1;
    assert(exchange(&c, fd, p).result == -EACCES && !c.active);
    p.action = APCAM_MANUAL_ACQUIRE;
    p = exchange(&c, fd, p);
    assert(p.result == 0 && c.active);
    p.action = APCAM_MANUAL_RELEASE;
    assert(exchange(&c, fd, p).result == 0 && !c.active);
    p.action = APCAM_MANUAL_RENEW;
    assert(exchange(&c, fd, p).result == -EACCES);
    p.action = APCAM_MANUAL_ACQUIRE;
    p = exchange(&c, fd, p);
    assert(p.result == 0 && c.active);
    ca_manual_control_close(&c);
    c = (struct ca_manual_control){.fd = -1};
    assert(ca_manual_control_open(&c, NULL, NULL) == 0);
    p.action = APCAM_MANUAL_RENEW;
    assert(exchange(&c, fd, p).result == -EACCES && !c.active);
    ca_manual_control_close(&c);
    close(fd);
    puts("Manual control: ownership, renewal, bounded motion, expiry, release and restart passed");
    return 0;
}
