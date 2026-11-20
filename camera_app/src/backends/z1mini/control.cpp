/* XFRobot Z1-Mini AX620A -> gimbal MCU, 115200 8N1.
 * The internal A9 5B/B5 9A link is distinct from the public A8 E5 SDK.
 * Commands use verified angle mode after a zero-rate telemetry wake-up.
 * Requested rates are integrated into angle targets; relative yaw is an angle.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/binlog.h"
#include "camera_app/backend.h"
#include "camera_app/log.h"
#include "camera_app/udp_transport.h"
#include "apcam/gimbal_transform.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CD_RAD (3.14159265358979323846f / 18000.0f)
#define FRESH_MS 250U
#define RATE_TIMEOUT_MS 300U
struct ca_backend {
    struct ca_backend_config config;
    int fd;
    uint8_t rx[26];
    size_t used;
    struct ca_gimbal_attitude attitude;
    float pitch, yaw, roll, pitch_rate, yaw_rate;
    uint64_t last_tx, rate_time;
    bool initialized, datagram;
    uint8_t tx[40];
    size_t tx_pending;
};
static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000U + t.tv_nsec / 1000000U;
}
static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    while (n--) {
        crc ^= (uint16_t)*p++ << 8;
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}
static float angle(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8) * CD_RAD;
}
static void put_angle(uint8_t *p, float radians)
{
    int16_t cd = (int16_t)lroundf(radians / CD_RAD);
    p[0] = (uint8_t)cd;
    p[1] = (uint8_t)((uint16_t)cd >> 8);
}
static float clamp(float v, float low, float high)
{
    return fminf(high, fmaxf(low, v));
}
static bool fresh(const struct ca_backend *b)
{
    return b && b->initialized && now_ms() - b->attitude.timestamp_ms <= FRESH_MS;
}
int ca_backend_open(struct ca_backend **out, const struct ca_backend_config *c)
{
    if (!out || !c || !c->name || strcmp(c->name, "z1mini")) { errno = EINVAL; return -1; }
    struct ca_backend *b = (struct ca_backend*)(calloc(1, sizeof(*b)));
    if (!b) return -1;
    b->config = *c;
    const char *device = c->uart_device ? c->uart_device : "/dev/ttyS3";
    bool datagram = strncmp(device, "udp://", 6) == 0;
    b->datagram = datagram;
    b->fd = datagram ? ca_open_udp_transport(device) : open(device,
                 O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    struct termios t;
    if (b->fd < 0) goto fail;
    if (!datagram) {
    if (flock(b->fd, LOCK_EX | LOCK_NB) || tcgetattr(b->fd, &t)) goto fail;
    cfmakeraw(&t);
    t.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS | CSIZE);
    t.c_cflag |= CS8 | CLOCAL | CREAD;
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    if (cfsetispeed(&t, B115200) || cfsetospeed(&t, B115200) ||
        tcsetattr(b->fd, TCSANOW, &t)) goto fail;
    }
    *out = b;
    ca_log("Z1-Mini gimbal: requesting MCU feedback with zero pitch/yaw rates");
    return 0;
fail:;
    int saved = errno;
    if (b->fd >= 0) close(b->fd);
    free(b); errno = saved; return -1;
}
int ca_backend_fd(const struct ca_backend *b) { return b ? b->fd : -1; }
int ca_backend_handle_fd(struct ca_backend *b)
{
    uint8_t data[512];
    ssize_t n = read(b->fd, data, sizeof(data));
    if (n < 0) return errno == EAGAIN || errno == EINTR ? 0 : -1;
    for (ssize_t i = 0; i < n; i++) {
        b->rx[b->used++] = data[i];
        while (b->used && (b->rx[0] != 0xb5 ||
               (b->used > 1 && b->rx[1] != 0x9a) ||
               (b->used == sizeof(b->rx) && crc16(b->rx, sizeof(b->rx))))) {
            memmove(b->rx, b->rx + 1, --b->used);
        }
        if (b->used != sizeof(b->rx)) continue;
        ca_binlog_packet(false, CA_PACKET_XFROBOT_MCU, b->datagram ? CA_PACKET_MCU_UDP : CA_PACKET_MCU_UART,
                         0, 0, b->rx, sizeof(b->rx));
        bool resync = !fresh(b);
        float pose[3] = {angle(b->rx + 12) * (180.0f / 3.14159265358979323846f),
                         angle(b->rx + 14) * (180.0f / 3.14159265358979323846f),
                         angle(b->rx + 22) * (180.0f / 3.14159265358979323846f)};
        apcam_transform(&apcam_feedback[b->config.orientation == CA_MOUNT_INVERTED], pose, pose, false);
        b->attitude.roll_rad = pose[0] * (100.0f * CD_RAD);
        b->attitude.pitch_rad = pose[1] * (100.0f * CD_RAD);
        b->attitude.yaw_rad = pose[2] * (100.0f * CD_RAD);
        /* Gyro axis/sign calibration is still pending; do not report guesses. */
        b->attitude.roll_rate_rad_s = NAN;
        b->attitude.pitch_rate_rad_s = NAN;
        b->attitude.yaw_rate_rad_s = NAN;
        b->attitude.timestamp_ms = now_ms();
        ca_binlog_feedback(&b->attitude);
        if (resync) {
            b->tx_pending = 0; /* discard an old partial command after link loss */
            b->roll = b->attitude.roll_rad;
            b->pitch = b->attitude.pitch_rad;
            b->yaw = b->attitude.yaw_rad;
            b->pitch_rate = b->yaw_rate = 0;
            b->initialized = true;
        }
        b->used = 0;
    }
    return 0;
}
void ca_backend_periodic(struct ca_backend *b)
{
    uint64_t now = now_ms();
    bool wake = !fresh(b);
    if (wake) b->pitch_rate = b->yaw_rate = 0;
    if (b->tx_pending) {
        ssize_t n = write(b->fd, b->tx + sizeof(b->tx) - b->tx_pending, b->tx_pending);
        if (n > 0) b->tx_pending -= (size_t)n;
        return;
    }
    if (now - b->last_tx < 20) return;
    float dt = fminf((now - b->last_tx) * 0.001f, 0.05f);
    b->last_tx = now;
    if (now - b->rate_time > RATE_TIMEOUT_MS) b->pitch_rate = b->yaw_rate = 0;
    if (b->pitch_rate != 0) b->pitch = clamp(b->pitch + b->pitch_rate * dt, APCAM_GIMBAL_PITCH_MIN * 100 * CD_RAD, APCAM_GIMBAL_PITCH_MAX * 100 * CD_RAD);
    if (b->yaw_rate != 0) b->yaw = clamp(b->yaw + b->yaw_rate * dt, APCAM_GIMBAL_YAW_MIN * 100 * CD_RAD, APCAM_GIMBAL_YAW_MAX * 100 * CD_RAD);
    memset(b->tx, 0, sizeof(b->tx));
    b->tx[0] = 0xa9; b->tx[1] = 0x5b; b->tx[2] = 0x20; b->tx[3] = 0xc0;
    b->tx[4] = b->tx[7] = 0x10; /* absolute roll/pitch, relative yaw */
    float command[3] = {b->roll / (100.0f * CD_RAD), b->pitch / (100.0f * CD_RAD), b->yaw / (100.0f * CD_RAD)};
    apcam_transform(&apcam_angle_command[b->config.orientation == CA_MOUNT_INVERTED], command, command, false);
    put_angle(b->tx + 5, command[0] * (100.0f * CD_RAD));
    if (wake) {
        /* The MCU replies only after a command. The vendor head-follow form
         * with zero pitch/yaw rates wakes telemetry without selecting a new
         * pitch/yaw pose. Verified on Z1-Mini; hold feedback angles afterward. */
        b->tx[7] = 0x50;
        b->tx[10] = 0x40;
    } else {
        put_angle(b->tx + 8, command[1] * (100.0f * CD_RAD));
        put_angle(b->tx + 11, command[2] * (100.0f * CD_RAD));
    }
    /* INS-valid stays clear: no fabricated carrier attitude/acceleration. */
    b->tx[26] = 30; b->tx[27] = 0xf4; b->tx[28] = 1; /* stock optical constants */
    uint16_t crc = crc16(b->tx, 38);
    b->tx[38] = crc >> 8; b->tx[39] = crc;
    b->tx_pending = sizeof(b->tx);
    // Log submission once; retries of a partial nonblocking write are not new packets.
    ca_binlog_packet(true, CA_PACKET_XFROBOT_MCU, b->datagram ? CA_PACKET_MCU_UDP : CA_PACKET_MCU_UART,
                     0, 0, b->tx, sizeof(b->tx));
    ssize_t n = write(b->fd, b->tx, b->tx_pending);
    if (n > 0) b->tx_pending -= (size_t)n;
}
int ca_backend_request_gimbal_attitude(struct ca_backend *b) { return b ? 0 : -1; }
bool ca_backend_gimbal_attitude(const struct ca_backend *b, struct ca_gimbal_attitude *a)
{
    if (!a || !fresh(b)) return false;
    *a = b->attitude; return true;
}
int ca_backend_set_gimbal_angles(struct ca_backend *b, float pitch, float yaw)
{
    if (!fresh(b)) { errno = EAGAIN; return -1; }
    if (!isfinite(pitch) || !isfinite(yaw)) { errno = EINVAL; return -1; }
    b->pitch = clamp(pitch, APCAM_GIMBAL_PITCH_MIN * 100 * CD_RAD, APCAM_GIMBAL_PITCH_MAX * 100 * CD_RAD);
    b->yaw = clamp(yaw, APCAM_GIMBAL_YAW_MIN * 100 * CD_RAD, APCAM_GIMBAL_YAW_MAX * 100 * CD_RAD);
    b->pitch_rate = b->yaw_rate = 0;
    CA_BINLOG(CA_LOG_GCMD,ca_log_gcmd,.mode=1,.pitch=b->pitch*57.295779513f,
        .yaw=b->yaw*57.295779513f,.wirep=NAN,.wirey=NAN,.result=0);
    return 0;
}
int ca_backend_set_gimbal_rates(struct ca_backend *b, float pitch, float yaw)
{
    if (!fresh(b)) { errno = EAGAIN; return -1; }
    if (!isfinite(pitch) || !isfinite(yaw)) { errno = EINVAL; return -1; }
    b->pitch_rate = clamp(pitch, -6000 * CD_RAD, 6000 * CD_RAD);
    b->yaw_rate = clamp(yaw, -6000 * CD_RAD, 6000 * CD_RAD);
    b->rate_time = now_ms();
    CA_BINLOG(CA_LOG_GCMD,ca_log_gcmd,.mode=2,.pitch=b->pitch_rate*57.295779513f,
        .yaw=b->yaw_rate*57.295779513f,.wirep=NAN,.wirey=NAN,.result=0);
    return 0;
}
int ca_backend_set_gimbal_neutral(struct ca_backend *b) { return ca_backend_set_gimbal_angles(b, 0, 0); }
bool ca_backend_recording(const struct ca_backend *b)
{
    return b && b->config.recording_get && b->config.recording_get(b->config.recording_opaque);
}
const char *ca_backend_name(const struct ca_backend *b) { (void)b; return "z1mini"; }
int ca_backend_handle_siyi(struct ca_backend *b, const uint8_t *p, size_t n)
{
    (void)b; (void)p; (void)n; errno = ENOTSUP; return -1;
}
void ca_backend_close(struct ca_backend *b) { if (b) { close(b->fd); free(b); } }

int ca_backend_set_zoom(struct ca_backend *backend, float ratio)
{ (void)backend; (void)ratio; errno = ENOTSUP; return -1; }
int ca_backend_set_zoom_rate(struct ca_backend *backend, float rate)
{ (void)backend; (void)rate; errno = ENOTSUP; return -1; }

int ca_backend_write_external_uart(struct ca_backend *backend, const uint8_t *data, size_t length)
{
    (void)backend; (void)data; (void)length; errno = ENOTSUP; return -1;
}
