#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <pty.h>
#include <stdio.h>
#include "../src/backends/z1mini/control.cpp"
#include "../src/backends/z1mini/pipeline.h"

uint64_t ca_binlog_time_us(void) { return 0; }
void ca_binlog_emit(uint8_t id, const void *data, size_t size)
{
    (void)id; (void)data; (void)size;
}
void ca_binlog_feedback(const struct ca_gimbal_attitude *attitude)
{
    (void)attitude;
}
// This standalone framing test does not open a BIN log. Packet logging is
// exercised separately by vendor-binlog-test with the real logger.
void ca_binlog_packet(bool, uint8_t, uint8_t, uint32_t, uint16_t,
                      const uint8_t *, size_t, int32_t)
{
}

static unsigned frames;
static uint64_t received_pts;
static void frame(void *opaque, const uint8_t *p, size_t n, uint64_t pts, bool key)
{
    (void)opaque;
    assert(n >= 5 && !memcmp(p, "\0\0\0\1", 4));
    if (!frames) assert(key);
    received_pts = pts; frames++;
}
static void feedback(int fd, struct ca_backend *b, uint8_t *p, size_t n)
{
    assert(write(fd, p, n) == (ssize_t)n);
    usleep(1000);
    assert(ca_backend_handle_fd(b) == 0);
}
static void test_gimbal(void)
{
    int master, slave; char path[128];
    assert(openpty(&master, &slave, path, NULL, NULL) == 0);
    struct ca_backend_config c = {.name = "z1mini", .uart_device = path};
    struct ca_backend *b;
    assert(!ca_backend_open(&b, &c));
    assert(ca_backend_set_gimbal_angles(b, 0, 0) < 0);
    ca_backend_periodic(b);
    uint8_t wake[40]; assert(read(master, wake, 40) == 40);
    assert(crc16(wake, 40) == 0 && wake[7] == 0x50 && wake[10] == 0x40);
    assert(angle(wake + 8) == 0 && angle(wake + 11) == 0);
    uint8_t rx[26] = {0xb5, 0x9a};
    put_angle(rx + 12, 50 * CD_RAD);
    put_angle(rx + 14, -1234 * CD_RAD);
    put_angle(rx + 16, -11114 * CD_RAD); /* earth yaw must NOT be used */
    put_angle(rx + 22, 250 * CD_RAD);
    uint16_t crc = crc16(rx, 24); rx[24] = crc >> 8; rx[25] = crc;
    uint8_t noise[] = {0, 0xb5, 0xb5, 0x9a, 3, 9, 0};
    feedback(master, b, noise, sizeof(noise));
    feedback(master, b, rx, 11); assert(!b->initialized);
    feedback(master, b, rx + 11, 15); assert(b->initialized);
    assert(fabsf(b->yaw - 250 * CD_RAD) < 1e-6f);
    b->last_tx -= 20;
    ca_backend_periodic(b);
    uint8_t tx[40]; assert(read(master, tx, 40) == 40);
    assert(crc16(tx, 40) == 0 && tx[0] == 0xa9 && tx[1] == 0x5b);
    assert(tx[4] == 0x10 && tx[7] == 0x10 && tx[10] == 0);
    assert(tx[13] == 0 && fabsf(angle(tx + 8) + 1234 * CD_RAD) < 1e-6);
    assert(fabsf(angle(tx + 11) - 250 * CD_RAD) < 1e-6);
    assert(ca_backend_set_gimbal_angles(b, NAN, 0) < 0);
    assert(!ca_backend_set_gimbal_angles(b, 100, -100));
    assert(b->pitch == 3000 * CD_RAD && b->yaw == -18000 * CD_RAD);
    assert(!ca_backend_set_gimbal_rates(b, 0.1, 0.2));
    b->rate_time -= RATE_TIMEOUT_MS + 1; b->last_tx -= 20;
    ca_backend_periodic(b); assert(b->pitch_rate == 0 && b->yaw_rate == 0);
    b->attitude.timestamp_ms -= FRESH_MS + 1;
    struct ca_gimbal_attitude a; assert(!ca_backend_gimbal_attitude(b, &a));
    assert(ca_backend_set_gimbal_neutral(b) < 0);
    rx[25] ^= 1; feedback(master, b, rx, 26); assert(!fresh(b));
    rx[25] ^= 1; feedback(master, b, rx, 26); assert(fresh(b));
    assert(fabsf(b->pitch + 1234 * CD_RAD) < 1e-6);
    assert(isnan(b->attitude.pitch_rate_rad_s));
    ca_backend_close(b); close(master); close(slave);
}
static int packet(struct ca_z1_rtp *s, uint16_t seq, uint32_t ts, bool marker, const uint8_t *data, size_t n)
{
    uint8_t p[256] = {0x80, 96};
    p[1] |= marker ? 0x80 : 0; p[2] = seq >> 8; p[3] = seq;
    p[4] = ts >> 24; p[5] = ts >> 16; p[6] = ts >> 8; p[7] = ts;
    memcpy(p + 12, data, n);
    return ca_z1_rtp_packet(s, p, n + 12);
}
static void test_rtp(void)
{
    struct ca_z1_rtp *s = static_cast<ca_z1_rtp *>(calloc(1, sizeof(*s))); assert(s);
    s->wait_key = true; s->consume = frame;
    const uint8_t idr[] = {0x65, 1, 2}, delta[] = {0x41, 3};
    assert(!packet(s, 65534, 0xfffff000, true, delta, sizeof(delta))); assert(!frames);
    assert(!packet(s, 65535, 0xfffff000, true, idr, sizeof(idr))); assert(frames == 1);
    assert(!packet(s, 0, 0x3650, true, delta, sizeof(delta))); /* wrap + 18000 ticks (5 fps) */
    assert(frames == 2 && received_pts == 200000);
    const uint8_t start[] = {0x7c, 0x85, 1, 2}, end[] = {0x7c, 0x45, 3, 4};
    assert(!packet(s, 1, 0x8000, false, start, sizeof(start)));
    assert(packet(s, 3, 0x8000, true, end, sizeof(end)) < 0); assert(frames == 2);
    assert(!packet(s, 4, 0x9000, true, delta, sizeof(delta))); assert(frames == 2);
    assert(!packet(s, 5, 0xa000, false, start, sizeof(start)));
    assert(!packet(s, 6, 0xa000, true, end, sizeof(end))); assert(frames == 3);
    const uint8_t bad_stap[] = {24, 0, 20, 0x65};
    assert(packet(s, 7, 0xb000, true, bad_stap, sizeof(bad_stap)) < 0);
    const uint8_t stap[] = {24, 0, 2, 0x67, 1, 0, 2, 0x65, 2};
    assert(!packet(s, 8, 0xc000, true, stap, sizeof(stap))); assert(frames == 4);
    s->used = CA_Z1_FRAME_MAX - 1;
    assert(packet(s, 9, 0xc000, true, idr, sizeof(idr)) < 0);
    free(s);
}
int main(void) { test_gimbal(); test_rtp(); puts("Z1 MCU framing/CRC/freshness/rate timeout and RTP assembly tests passed"); }
