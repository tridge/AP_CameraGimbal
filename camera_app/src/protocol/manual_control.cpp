#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/manual_control.h"
#include "camera_app/backend.h"
#include "camera_app/mavlink_server.h"
#include "camera_app/log.h"
#include "apcam/target.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000U + t.tv_nsec / 1000000U;
}
static int stop_motion(struct ca_manual_control *c)
{
    c->executing = true;
    int result = ca_backend_set_gimbal_rates(c->backend, 0, 0);
    c->executing = false;
    /* Retry on transport errors, rather than forgetting a moving gimbal. */
    c->stop_ms = result ? now_ms() + 20U : 0;
    return result;
}
static int token_create(uint8_t token[16])
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t used = 0;
    while (used < 16) {
        ssize_t n = read(fd, token + used, 16 - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(fd);
            return -1;
        }
        used += (size_t)n;
    }
    close(fd);
    return 0;
}
static int command(struct ca_manual_control *c, struct apcam_manual_packet *p)
{
    uint64_t now = now_ms();
    if (ca_mavlink_server_survey_owns_gimbal(c->mavlink) &&
        (p->action == APCAM_MANUAL_ACQUIRE || p->action == APCAM_MANUAL_CENTER ||
         (p->action >= APCAM_MANUAL_LEFT && p->action <= APCAM_MANUAL_DOWN))) return -EBUSY;
    if (p->action == APCAM_MANUAL_ACQUIRE) {
        if (c->active)
            return -EBUSY;
        if (token_create(c->token) < 0)
            return -EIO;
        if (stop_motion(c) < 0)
            return -EIO;
        ca_mavlink_server_suspend_gimbal(c->mavlink);
        c->active = true;
        c->expires_ms = now + APCAM_MANUAL_LEASE_MS;
        memcpy(p->token, c->token, sizeof(p->token));
        ca_log("web manual gimbal control acquired");
        return 0;
    }
    if (!c->active || memcmp(c->token, p->token, sizeof(c->token)))
        return -EACCES;
    if (p->action == APCAM_MANUAL_RELEASE) {
        if (stop_motion(c) < 0)
            return -EIO;
        c->active = false;
        memset(c->token, 0, sizeof(c->token));
        ca_log("web manual gimbal control released");
        return 0;
    }
    if (p->action == APCAM_MANUAL_RENEW) {
        c->expires_ms = now + APCAM_MANUAL_LEASE_MS;
        return 0;
    }
    bool pulse = p->action >= APCAM_MANUAL_LEFT && p->action <= APCAM_MANUAL_DOWN;
    if (pulse && (!isfinite(p->value) || p->value < 5 || p->value > 60))
        return -EINVAL;
    if (p->action == APCAM_MANUAL_ZOOM &&
        (!isfinite(p->value) || p->value < 1 || p->value > APCAM_ZOOM_CONTROL_MAX))
        return -EINVAL;
    // Optical lens movement can block the control loop. Stop a preceding
    // bounded pan/tilt pulse before entering that operation.
    if (p->action == APCAM_MANUAL_ZOOM && c->stop_ms && stop_motion(c) < 0)
        return -EIO;
    int result;
    c->executing = true;
    if (pulse) {
        float rate = p->value * 0.01745329252f;
        float pitch = p->action == APCAM_MANUAL_UP     ? rate
                      : p->action == APCAM_MANUAL_DOWN ? -rate
                                                       : 0;
        float yaw = p->action == APCAM_MANUAL_RIGHT  ? rate
                    : p->action == APCAM_MANUAL_LEFT ? -rate
                                                     : 0;
        result = ca_backend_set_gimbal_rates(c->backend, pitch, yaw);
        /* Stop even when a transport reports a partially transmitted command. */
        c->stop_ms = now + APCAM_MANUAL_PULSE_MS;
    } else if (p->action == APCAM_MANUAL_CENTER) {
        result = ca_backend_set_gimbal_neutral(c->backend);
        if (!result)
            c->stop_ms = 0;
    } else if (p->action == APCAM_MANUAL_ZOOM) {
        result = ca_backend_set_zoom(c->backend, p->value);
    } else
        result = -1;
    c->executing = false;
    return result ? -EIO : 0;
}
int ca_manual_control_open(struct ca_manual_control *c, struct ca_backend *backend,
                           struct ca_mavlink_server *mavlink)
{
    c->backend = backend;
    c->mavlink = mavlink;
    c->fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (c->fd < 0)
        return -1;
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    if (bind(c->fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        getsockname(c->fd, (struct sockaddr *)&address, &length) < 0) {
        close(c->fd);
        c->fd = -1;
        return -1;
    }
    c->port = ntohs(address.sin_port);
    return 0;
}
void ca_manual_control_update(struct ca_manual_control *c)
{
    uint64_t now = now_ms();
    if (c->active && now >= c->expires_ms) {
        (void)stop_motion(c);
        c->active = false;
        memset(c->token, 0, sizeof(c->token));
        ca_log("web manual gimbal control expired");
    }
    if (c->stop_ms && now >= c->stop_ms)
        (void)stop_motion(c);
    for (unsigned i = 0; c->fd >= 0 && i < 16; i++) {
        uint8_t data[sizeof(struct apcam_manual_packet) + 1];
        struct sockaddr_in peer;
        socklen_t length = sizeof(peer);
        ssize_t n = recvfrom(c->fd, data, sizeof(data), 0, (struct sockaddr *)&peer, &length);
        if (n < 0)
            break;
        if (n != sizeof(struct apcam_manual_packet) ||
            peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
            continue;
        struct apcam_manual_packet packet;
        memcpy(&packet, data, sizeof(packet));
        if (packet.magic != APCAM_MANUAL_MAGIC)
            continue;
        packet.result = command(c, &packet);
        (void)sendto(c->fd, &packet, sizeof(packet), 0, (struct sockaddr *)&peer, length);
    }
}
void ca_manual_control_close(struct ca_manual_control *c)
{
    if (c->active || c->stop_ms)
        (void)stop_motion(c);
    c->active = false;
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
}
