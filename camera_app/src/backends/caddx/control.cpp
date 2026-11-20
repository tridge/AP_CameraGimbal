/* CADDX gimbal (same as XFRobot C20-T), 115200 8N1 UART, or UDP for a
 * flight controller's serial passthrough (udp://IPv4:PORT), or TCP
 * (tcp://IPv4:PORT, reconnected if the link drops).
 * The protocol matches ArduPilot's AP_Mount_CADDX driver: one 10-byte angle
 * command, A5 5A, lock mode, roll/pitch/yaw as 12-bit fractions of a turn
 * and a big-endian CRC16-CCITT. The gimbal sends no feedback, so the reported
 * attitude is the commanded target, as in ArduPilot. Requested rates are
 * integrated into angle targets. Nothing is sent until the first command so
 * starting the camera app does not move the gimbal. Without a configured
 * device the gimbal is unsupported and camera functions remain available.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "apcam/target.h"
#include "apcam/gimbal_transform.h"
#include "camera_app/backend.h"
#include "camera_app/binlog.h"
#include "camera_app/log.h"
#include "camera_app/tcp_transport.h"
#include "caddx.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CADDX_PACKET_SIZE 10U
#define CADDX_AXIS_COUNTS 4096U
#define CADDX_PITCH_LOCK 0x01U
#define CADDX_ROLL_LOCK 0x02U
#define CADDX_SEND_MS 20U
#define CADDX_RATE_TIMEOUT_MS 300U
#define CADDX_RECONNECT_MS 1000U
#define CADDX_CONNECT_TIMEOUT_MS 5000U
#define CADDX_RATE_MAX (60.0f * DEG_RAD)
#define DEG_RAD (3.14159265358979323846f / 180.0f)

struct ca_backend {
    struct ca_backend_config config;
    int fd;
    bool linked, udp, tcp, connected, reported, commanded;
    const char *device;
    float pitch, yaw, pitch_rate, yaw_rate;
    uint64_t last_tx, rate_time, last_connect;
    uint8_t tx[CADDX_PACKET_SIZE];
    size_t tx_pending;
};

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U;
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

static float clamp(float v, float low, float high)
{
    return fminf(high, fmaxf(low, v));
}

/* Angle as a 12-bit fraction of a turn, 0 at neutral, increasing clockwise. */
static uint16_t axis_counts(float radians)
{
    float turns = radians / (2.0f * 3.14159265358979323846f);
    turns -= floorf(turns);
    return (uint16_t)(lroundf(turns * CADDX_AXIS_COUNTS) % CADDX_AXIS_COUNTS);
}

void ca_caddx_pack(uint8_t out[CADDX_PACKET_SIZE], float roll, float pitch, float yaw)
{
    const uint16_t r = axis_counts(roll), p = axis_counts(pitch), y = axis_counts(yaw);
    out[0] = 0xa5;
    out[1] = 0x5a;
    /* Earth-frame roll and pitch; yaw follows the vehicle. Sensitivity 0. */
    out[2] = CADDX_PITCH_LOCK | CADDX_ROLL_LOCK;
    out[3] = (uint8_t)((r << 4) & 0xf0);
    out[4] = (uint8_t)(r >> 4);
    out[5] = (uint8_t)p;
    out[6] = (uint8_t)(((p >> 8) & 0x0f) | ((y << 4) & 0xf0));
    out[7] = (uint8_t)(y >> 4);
    const uint16_t crc = crc16(out, CADDX_PACKET_SIZE - 2U);
    out[8] = (uint8_t)(crc >> 8);
    out[9] = (uint8_t)crc;
}

int ca_backend_open(struct ca_backend **out, const struct ca_backend_config *c)
{
    if (!out || !c) { errno = EINVAL; return -1; }
    struct ca_backend *b = (struct ca_backend *)calloc(1, sizeof(*b));
    if (!b) return -1;
    b->config = *c;
    b->fd = -1;
    const char *device = c->uart_device;
    if (!device || !*device) {
        ca_log("CADDX gimbal: no UART configured; gimbal control unavailable");
        *out = b;
        return 0;
    }
    b->device = device;
    b->udp = strncmp(device, "udp://", 6) == 0;
    b->tcp = strncmp(device, "tcp://", 6) == 0;
    b->last_connect = now_ms();
    b->fd = b->udp ? ca_open_udp_transport(device)
          : b->tcp ? ca_open_tcp_transport(device)
          : open(device, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (b->fd < 0) {
        /* A TCP peer that cannot be reached yet (for example no route to the
         * flight controller during boot) is retried by tcp_service(). */
        if (!b->tcp || errno == EINVAL) goto fail;
        ca_log("CADDX gimbal: cannot open %s: %s; retrying", device, strerror(errno));
        b->reported = true;
    }
    b->linked = true;
    if (!b->udp && !b->tcp) {
        struct termios t;
        if (flock(b->fd, LOCK_EX | LOCK_NB) || tcgetattr(b->fd, &t)) goto fail;
        cfmakeraw(&t);
        t.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS | CSIZE);
        t.c_cflag |= CS8 | CLOCAL | CREAD;
        t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
        if (cfsetispeed(&t, B115200) || cfsetospeed(&t, B115200) ||
            tcsetattr(b->fd, TCSANOW, &t)) goto fail;
    }
    ca_log("CADDX gimbal on %s; waiting for the first command", device);
    *out = b;
    return 0;
fail:;
    int saved = errno;
    ca_log("CADDX gimbal: cannot open %s: %s", device, strerror(saved));
    if (b->fd >= 0) close(b->fd);
    free(b);
    errno = saved;
    return -1;
}

/* A network link is not polled by the main loop: an ICMP error from an
 * unreachable passthrough (for example while the flight controller reboots),
 * or a dropped TCP connection, would report POLLERR/POLLHUP there and stop the
 * camera app. periodic() services it instead, which also consumes any pending
 * socket error and reconnects TCP. */
int ca_backend_fd(const struct ca_backend *b) { return b && !b->udp && !b->tcp ? b->fd : -1; }

static uint8_t mcu_link(const struct ca_backend *b)
{
    return b->tcp ? CA_PACKET_MCU_TCP : b->udp ? CA_PACKET_MCU_UDP : CA_PACKET_MCU_UART;
}

/* The gimbal is not known to reply; log anything received for analysis. */
int ca_backend_handle_fd(struct ca_backend *b)
{
    uint8_t data[256];
    if (!b || b->fd < 0) return 0;
    ssize_t n = read(b->fd, data, sizeof(data));
    if (n < 0) return errno == EAGAIN || errno == EINTR || errno == ECONNREFUSED ? 0 : -1;
    if (n > 0)
        ca_binlog_packet(false, CA_PACKET_CADDX, mcu_link(b),
                         0, 0, data, (size_t)n);
    return 0;
}

static void drain_udp(struct ca_backend *b)
{
    for (unsigned i = 0; i < 16U; i++) {
        uint8_t data[256];
        ssize_t n = recv(b->fd, data, sizeof(data), MSG_DONTWAIT);
        if (n < 0) return; /* EAGAIN, or a pending error now consumed */
        if (n > 0)
            ca_binlog_packet(false, CA_PACKET_CADDX, mcu_link(b), 0, 0, data, (size_t)n);
    }
}

static void tcp_drop(struct ca_backend *b, const char *why)
{
    if (!b->reported) ca_log("CADDX gimbal: %s %s; retrying", b->device, why);
    b->reported = true; /* once until it connects again, not every second */
    if (b->fd >= 0) close(b->fd);
    b->fd = -1;
    b->connected = false;
    b->tx_pending = 0;
    b->last_connect = now_ms();
}

/* Connects, and reconnects after a drop; true when data can be sent. */
static bool tcp_service(struct ca_backend *b)
{
    const uint64_t now = now_ms();
    if (b->fd < 0) {
        if (now - b->last_connect < CADDX_RECONNECT_MS) return false;
        b->last_connect = now;
        b->fd = ca_open_tcp_transport(b->device);
        return false;
    }
    if (!b->connected) {
        struct pollfd item = {.fd = b->fd, .events = POLLOUT};
        if (poll(&item, 1, 0) <= 0) {
            if (now - b->last_connect > CADDX_CONNECT_TIMEOUT_MS) tcp_drop(b, "connect timed out");
            return false;
        }
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(b->fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error) {
            tcp_drop(b, "refused the connection");
            return false;
        }
        b->connected = true;
        b->reported = false;
        ca_log("CADDX gimbal: connected to %s", b->device);
    }
    for (unsigned i = 0; i < 16U; i++) {
        uint8_t data[256];
        ssize_t n = recv(b->fd, data, sizeof(data), MSG_DONTWAIT);
        if (n > 0) {
            ca_binlog_packet(false, CA_PACKET_CADDX, mcu_link(b), 0, 0, data, (size_t)n);
        } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            tcp_drop(b, "closed the connection");
            return false;
        } else {
            break;
        }
    }
    return true;
}

static ssize_t link_write(struct ca_backend *b, const uint8_t *data, size_t size)
{
    ssize_t n = b->tcp ? send(b->fd, data, size, MSG_NOSIGNAL) : write(b->fd, data, size);
    if (n < 0 && b->tcp && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        tcp_drop(b, "write failed");
    return n;
}

void ca_backend_periodic(struct ca_backend *b)
{
    if (!b || !b->linked) return;
    if (b->udp) drain_udp(b);
    if (b->tcp && !tcp_service(b)) return;
    if (!b->commanded) return;
    if (b->tx_pending) {
        ssize_t n = link_write(b, b->tx + sizeof(b->tx) - b->tx_pending, b->tx_pending);
        if (n > 0) b->tx_pending -= (size_t)n;
        return;
    }
    const uint64_t now = now_ms();
    if (now - b->last_tx < CADDX_SEND_MS) return;
    const float dt = fminf((float)(now - b->last_tx) * 0.001f, 0.05f);
    b->last_tx = now;
    if (now - b->rate_time > CADDX_RATE_TIMEOUT_MS) b->pitch_rate = b->yaw_rate = 0;
    if (b->pitch_rate != 0)
        b->pitch = clamp(b->pitch + b->pitch_rate * dt, APCAM_GIMBAL_PITCH_MIN * DEG_RAD, APCAM_GIMBAL_PITCH_MAX * DEG_RAD);
    if (b->yaw_rate != 0)
        b->yaw = clamp(b->yaw + b->yaw_rate * dt, APCAM_GIMBAL_YAW_MIN * DEG_RAD, APCAM_GIMBAL_YAW_MAX * DEG_RAD);
    float command[3] = {0, b->pitch / DEG_RAD, b->yaw / DEG_RAD};
    apcam_transform(&apcam_angle_command[b->config.orientation == CA_MOUNT_INVERTED], command, command, false);
    ca_caddx_pack(b->tx, command[0] * DEG_RAD, command[1] * DEG_RAD, command[2] * DEG_RAD);
    b->tx_pending = sizeof(b->tx);
    ca_binlog_packet(true, CA_PACKET_CADDX, mcu_link(b),
                     0, 0, b->tx, sizeof(b->tx));
    ssize_t n = link_write(b, b->tx, b->tx_pending);
    if (n > 0) b->tx_pending -= (size_t)n;
    else if (n < 0 && b->udp) b->tx_pending = 0; /* datagrams are not resumable */
}

int ca_backend_request_gimbal_attitude(struct ca_backend *b)
{
    if (!b || !b->linked) { errno = ENOTSUP; return -1; }
    return 0;
}

bool ca_backend_gimbal_attitude(const struct ca_backend *b, struct ca_gimbal_attitude *a)
{
    if (!b || !a || !b->linked || !b->commanded) return false;
    *a = {};
    a->pitch_rad = b->pitch;
    a->yaw_rad = b->yaw;
    a->roll_rate_rad_s = a->pitch_rate_rad_s = a->yaw_rate_rad_s = NAN;
    a->timestamp_ms = now_ms();
    return true;
}

int ca_backend_set_gimbal_angles(struct ca_backend *b, float pitch, float yaw)
{
    if (!b || !b->linked) { errno = ENOTSUP; return -1; }
    if (!isfinite(pitch) || !isfinite(yaw)) { errno = EINVAL; return -1; }
    b->pitch = clamp(pitch, APCAM_GIMBAL_PITCH_MIN * DEG_RAD, APCAM_GIMBAL_PITCH_MAX * DEG_RAD);
    b->yaw = clamp(yaw, APCAM_GIMBAL_YAW_MIN * DEG_RAD, APCAM_GIMBAL_YAW_MAX * DEG_RAD);
    b->pitch_rate = b->yaw_rate = 0;
    b->commanded = true;
    CA_BINLOG(CA_LOG_GCMD, ca_log_gcmd, .mode=1, .pitch=b->pitch / DEG_RAD,
              .yaw=b->yaw / DEG_RAD, .wirep=NAN, .wirey=NAN, .result=0);
    return 0;
}

int ca_backend_set_gimbal_rates(struct ca_backend *b, float pitch, float yaw)
{
    if (!b || !b->linked) { errno = ENOTSUP; return -1; }
    if (!isfinite(pitch) || !isfinite(yaw)) { errno = EINVAL; return -1; }
    b->pitch_rate = clamp(pitch, -CADDX_RATE_MAX, CADDX_RATE_MAX);
    b->yaw_rate = clamp(yaw, -CADDX_RATE_MAX, CADDX_RATE_MAX);
    b->rate_time = now_ms();
    b->commanded = true;
    CA_BINLOG(CA_LOG_GCMD, ca_log_gcmd, .mode=2, .pitch=b->pitch_rate / DEG_RAD,
              .yaw=b->yaw_rate / DEG_RAD, .wirep=NAN, .wirey=NAN, .result=0);
    return 0;
}

int ca_backend_set_gimbal_neutral(struct ca_backend *b) { return ca_backend_set_gimbal_angles(b, 0, 0); }

bool ca_backend_recording(const struct ca_backend *b)
{
    return b && b->config.recording_get && b->config.recording_get(b->config.recording_opaque);
}

const char *ca_backend_name(const struct ca_backend *b) { (void)b; return APCAM_NAME; }

int ca_backend_handle_siyi(struct ca_backend *b, const uint8_t *p, size_t n)
{
    (void)b; (void)p; (void)n; errno = ENOTSUP; return -1;
}

bool ca_backend_has_gimbal(const struct ca_backend *b) { return b && b->linked; }

void ca_backend_close(struct ca_backend *b)
{
    if (!b) return;
    if (b->fd >= 0) close(b->fd);
    free(b);
}

int ca_backend_set_zoom(struct ca_backend *b, float ratio)
{
    (void)b; (void)ratio; errno = ENOTSUP; return -1;
}

int ca_backend_set_zoom_rate(struct ca_backend *b, float rate)
{
    (void)b; (void)rate; errno = ENOTSUP; return -1;
}

int ca_backend_write_external_uart(struct ca_backend *b, const uint8_t *data, size_t length)
{
    (void)b; (void)data; (void)length; errno = ENOTSUP; return -1;
}
