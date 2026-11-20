/* XFRobot public A8 E5 / 8A 5E protocol, distinct from its internal MCU link.
 * Packet layout reference: ArduPilot libraries/AP_Mount/AP_Mount_XFRobot.h.
 * Unsupported orders return failure.
 * All I/O is bounded/nonblocking so a slow vendor client cannot stall control.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/xfrobot_server.h"
#include "camera_app/log.h"
#include "camera_app/binlog.h"
#include "apcam/gimbal_transform.h"
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLIENTS 4
#define MAX_PACKET 256U
struct client { int fd; struct sockaddr_in peer; uint8_t input[MAX_PACKET], output[73]; size_t used, sent, pending; };
struct ca_xfrobot_server {
    bool manual_control; int tcp, udp; bool inverted; struct client clients[CLIENTS]; };
static uint16_t crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0;
    while (length--) {
        crc ^= (uint16_t)*data++ << 8;
        for (unsigned i = 0; i < 8; i++) crc = crc & 0x8000 ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static int listener(int type, unsigned port)
{
    int fd = socket(AF_INET, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0), one = 1;
    if (fd < 0) return -1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr = { .s_addr = htonl(INADDR_ANY) }};
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || (type == SOCK_STREAM && listen(fd, CLIENTS) < 0)) {
        int saved = errno; close(fd); errno = saved; return -1;
    }
    return fd;
}
int ca_xfrobot_server_open(struct ca_xfrobot_server **out, unsigned port, bool inverted)
{
    struct ca_xfrobot_server *s = (struct ca_xfrobot_server*)(calloc(1, sizeof(*s)));
    if (!s) return -1;
    s->inverted = inverted;
    for (unsigned i = 0; i < CLIENTS; i++) s->clients[i].fd = -1;
    s->udp = listener(SOCK_DGRAM, port);
    if (s->udp < 0) ca_log("XFRobot UDP %u unavailable: %s", port, strerror(errno));
    unsigned tcp = port == APCAM_VENDOR_PORT ? APCAM_VENDOR_TCP_PORT : port;
    s->tcp = listener(SOCK_STREAM, tcp);
    if (s->tcp < 0) ca_log("XFRobot TCP %u unavailable: %s", tcp, strerror(errno));
    *out = s;
    return 0;
}
static bool reply(struct ca_xfrobot_server *server, struct ca_backend *backend, struct ca_media *media,
                  const uint8_t *data, size_t length, uint8_t output[73], uint8_t link, const sockaddr_in &peer)
{
    if (length < 72 || length > MAX_PACKET || data[0] != 0xa8 || data[1] != 0xe5 ||
        get16(data + 2) != length || data[4] != 2 || crc16(data, length)) return false;
    ca_binlog_packet(false, CA_PACKET_XFROBOT, link, ntohl(peer.sin_addr.s_addr), ntohs(peer.sin_port), data, length);
    int result = -1;
    float command[3] = {(int16_t)get16(data + 5) * 0.01f, (int16_t)get16(data + 7) * 0.01f, (int16_t)get16(data + 9) * 0.01f};
    apcam_inverse_transform(&apcam_angle_command[server->inverted], command, command, false);
    float rad = 0.00017453292519943296f;
    bool blocked=server->manual_control && (data[69]==0x03 || data[69]==0x10 || data[69]==0x13);
    if (!blocked) switch (data[69]) {
    case 0: result = 0; break;
    case 0x03: result = ca_backend_set_gimbal_neutral(backend); break;
    case 0x10:
        result = !(data[11] & 4U) ? 0 : ca_backend_set_gimbal_angles(backend,
                      command[1] * (100 * rad), command[2] * (100 * rad));
        break;
    case 0x13: {
        struct ca_gimbal_attitude attitude;
        if (ca_backend_gimbal_attitude(backend, &attitude))
            result = ca_backend_set_gimbal_angles(backend, -9000 * rad, attitude.yaw_rad);
        break;
    }
    case 0x21:
        if (length == 73 && data[70] <= 1) result = ca_media_set_recording(media, data[70] != 0);
        break;
    default: break;
    }
    memset(output, 0, 73);
    output[0] = 0x8a; output[1] = 0x5e; put16(output + 2, 73); output[4] = 2;
    output[5] = 0x10;
    struct ca_gimbal_attitude a;
    if (ca_backend_gimbal_attitude(backend, &a)) {
        float pose[3] = {a.roll_rad / (100 * rad), a.pitch_rad / (100 * rad), a.yaw_rad / (100 * rad)};
        apcam_inverse_transform(&apcam_feedback[server->inverted], pose, pose, false);
        a.roll_rad = pose[0] * (100 * rad); a.pitch_rad = pose[1] * (100 * rad); a.yaw_rad = pose[2] * (100 * rad);
        put16(output + 12, (uint16_t)(int16_t)lroundf(a.pitch_rad / rad));
        put16(output + 14, (uint16_t)(int16_t)lroundf(a.roll_rad / rad));
        put16(output + 16, (uint16_t)(int16_t)lroundf(a.yaw_rad / rad));
        put16(output + 18, (uint16_t)(int16_t)lroundf(a.roll_rad / rad));
        put16(output + 20, (uint16_t)(int16_t)lroundf(a.pitch_rad / rad));
    } else put16(output + 41, 1U << 7);
    output[37] = 1;
    put16(output + 59, (uint16_t)lroundf(ca_media_zoom(media) * 10));
    put16(output + 64, ca_media_recording(media) ? 16U : 0U);
    output[69] = data[69]; output[70] = result == 0 ? 0 : 1;
    uint16_t crc = crc16(output, 71); output[71] = crc >> 8; output[72] = crc;
    return true;
}
static void close_client(struct client *c)
{
    if (c->fd >= 0) close(c->fd);
    memset(c, 0, sizeof(*c)); c->fd = -1;
}
void ca_xfrobot_server_update(struct ca_xfrobot_server *s, struct ca_backend *backend, struct ca_media *media, bool manual_control)
{
    if (!s) return;
    s->manual_control=manual_control;
    if (s->tcp >= 0) {
        sockaddr_in peer {};
        socklen_t size=sizeof(peer);
        int fd = accept4(s->tcp, (sockaddr *)&peer, &size, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0) {
            unsigned i;
            for (i = 0; i < CLIENTS; i++) if (s->clients[i].fd < 0) { s->clients[i].fd = fd; s->clients[i].peer=peer; break; }
            if (i == CLIENTS) close(fd);
        }
    }
    for (unsigned i = 0; s->udp >= 0 && i < 8; i++) {
        uint8_t data[MAX_PACKET + 1], output[73];
        struct sockaddr_in peer; socklen_t size = sizeof(peer);
        ssize_t n = recvfrom(s->udp, data, sizeof(data), 0, (struct sockaddr *)&peer, &size);
        if (n < 0) break;
        if (reply(s, backend, media, data, (size_t)n, output, CA_PACKET_UDP, peer)) {
            /* Vendor SDK clients receive on 2338 independently of TX port.
             * Unicast to the sender avoids exposing telemetry to the subnet. */
            peer.sin_port = htons(APCAM_VENDOR_REPLY_PORT);
            ssize_t sent=sendto(s->udp, output, sizeof(output), MSG_NOSIGNAL, (struct sockaddr *)&peer, size);
            ca_binlog_packet(true, CA_PACKET_XFROBOT, CA_PACKET_UDP, ntohl(peer.sin_addr.s_addr), ntohs(peer.sin_port),
                             output, sizeof(output), sent == sizeof(output) ? 0 : sent < 0 ? -errno : -EIO);
        }
    }
    for (unsigned i = 0; i < CLIENTS; i++) {
        struct client *c = &s->clients[i];
        if (c->fd < 0) continue;
        if (c->pending) {
            ssize_t n = send(c->fd, c->output + c->sent, c->pending, MSG_NOSIGNAL);
            if (n < 0) { if (errno != EAGAIN && errno != EINTR) close_client(c); continue; }
            if (!n) { close_client(c); continue; }
            c->sent += (size_t)n; c->pending -= (size_t)n;
            if (c->pending) continue;
        }
        ssize_t n = recv(c->fd, c->input + c->used, sizeof(c->input) - c->used, 0);
        if (!n) { close_client(c); continue; }
        if (n < 0 && errno != EAGAIN && errno != EINTR) { close_client(c); continue; }
        if (n > 0) c->used += (size_t)n;
        while (c->used >= 4) {
            size_t length = get16(c->input + 2);
            if (c->input[0] != 0xa8 || c->input[1] != 0xe5 || length < 72 || length > MAX_PACKET) {
                memmove(c->input, c->input + 1, --c->used); continue;
            }
            if (c->used < length) break;
            bool valid = reply(s, backend, media, c->input, length, c->output, CA_PACKET_TCP, c->peer);
            c->used -= length; memmove(c->input, c->input + length, c->used);
            if (valid) {
                c->sent = 0; c->pending = sizeof(c->output);
                ca_binlog_packet(true, CA_PACKET_XFROBOT, CA_PACKET_TCP, ntohl(c->peer.sin_addr.s_addr), ntohs(c->peer.sin_port),
                                 c->output, sizeof(c->output));
                break;
            }
        }
    }
}
void ca_xfrobot_server_close(struct ca_xfrobot_server *s)
{
    if (!s) return;
    if (s->udp >= 0) close(s->udp);
    if (s->tcp >= 0) close(s->tcp);
    for (unsigned i = 0; i < CLIENTS; i++) close_client(&s->clients[i]);
    free(s);
}
