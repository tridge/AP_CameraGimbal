/* Receive the retained vendor ISP service's H.264 stream without decoding or
 * opening AX VIN/VENC. Media ownership stays in one process. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pipeline.h"
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t millis(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static unsigned be16(const uint8_t *p) { return (unsigned)p[0] * 256 + p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)be16(p) << 16 | be16(p + 2); }
static int append(struct ca_z1_rtp *s, const uint8_t *p, size_t n, bool start)
{
    if (n + (start ? 4 : 0) > sizeof(s->frame) - s->used) return -1;
    if (start) {
        memcpy(s->frame + s->used, "\0\0\0\1", 4); s->used += 4;
        if (n && (p[0] & 31) == 5) s->key = true;
    }
    memcpy(s->frame + s->used, p, n); s->used += n;
    return 0;
}
int ca_z1_rtp_packet(struct ca_z1_rtp *s, const uint8_t *p, size_t n)
{
    size_t off;
    if (n < 12 || p[0] >> 6 != 2) goto bad;
    off = 12 + 4 * (p[0] & 15);
    if (off > n) goto bad;
    {
    if (p[0] & 16) {
        if (n - off < 4) goto bad;
        off += 4 + 4 * be16(p + off + 2);
    }
    if (p[0] & 32) {
        unsigned pad = p[n - 1];
        if (!pad || pad > n) goto bad;
        n -= pad;
    }
    if (off >= n) goto bad;
    uint16_t seq = be16(p + 2);
    uint32_t ts = be32(p + 4);
    if (s->have_sequence && seq != (uint16_t)(s->sequence + 1)) {
        s->damaged = s->wait_key = true; s->fu = false;
    }
    s->sequence = seq; s->have_sequence = true;
    if (s->have_timestamp && ts != s->timestamp) {
        if (s->used) s->wait_key = true; /* missing previous marker */
        s->used = 0; s->fu = s->key = s->damaged = false;
    }
    s->timestamp = ts; s->have_timestamp = true;
    const uint8_t *v = p + off;
    size_t len = n - off;
    unsigned type = v[0] & 31;
    if (type >= 1 && type <= 23) {
        if (s->fu) goto bad;
        if (append(s, v, len, true)) goto bad;
    } else if (type == 24) {
        if (s->fu) goto bad;
        size_t pos = 1;
        while (pos < len) {
            if (len - pos < 2) goto bad;
            size_t k = be16(v + pos); pos += 2;
            if (!k || k > len - pos || !(v[pos] & 31) || (v[pos] & 31) > 23) goto bad;
            if (append(s, v + pos, k, true)) goto bad;
            pos += k;
        }
    } else if (type == 28 && len >= 3 && (v[1] & 31) >= 1 && (v[1] & 31) <= 23) {
        if ((v[1] & 0xc0) == 0xc0 || (v[1] & 0x20)) goto bad;
        if (v[1] & 128) {
            if (s->fu) goto bad;
            uint8_t header = (v[0] & 0xe0) | (v[1] & 31);
            if (append(s, &header, 1, true)) goto bad;
            s->fu = true;
        } else if (!s->fu) goto bad;
        if (append(s, v + 2, len - 2, false)) goto bad;
        if (v[1] & 64) s->fu = false;
    } else goto bad;
    if (p[1] & 128) {
        if (s->fu) goto bad;
        if (!s->damaged && s->used && (!s->wait_key || s->key)) {
            if (s->key && s->parameter_size) {
                if (s->parameter_size > sizeof(s->frame) - s->used) goto bad;
                memmove(s->frame + s->parameter_size, s->frame, s->used);
                memcpy(s->frame, s->parameters, s->parameter_size);
                s->used += s->parameter_size;
            }
            /* RTP clock is 90 kHz, independent of thermal 30 -> 5 fps changes. */
            if (s->emitted) s->ticks += (uint32_t)(ts - s->last_timestamp);
            s->last_timestamp = ts; s->emitted = true;
            s->wait_key = false;
            s->consume(s->opaque, s->frame, s->used, s->ticks * 1000000 / 90000, s->key);
        }
        s->used = 0; s->key = s->damaged = false;
    }
    return 0;
    }
bad:
    s->damaged = s->wait_key = true; s->fu = false;
    return -1;
}
static int exact(int fd, void *buffer, size_t n, atomic_bool *stop)
{
    uint8_t *p = (uint8_t*)(buffer);
    uint64_t deadline = millis() + 5000;
    while (n && !atomic_load(stop) && millis() < deadline) {
        struct pollfd f = {.fd = fd, .events = POLLIN};
        int ready = poll(&f, 1, 100);
        if (ready < 0 && errno != EINTR) return -1;
        if (ready <= 0) continue;
        ssize_t got = read(fd, p, n);
        if (got <= 0) return -1;
        p += got; n -= (size_t)got;
    }
    return n ? -1 : 0;
}
static int send_request(int fd, const char *method, const char *url, unsigned seq, const char *headers)
{
    char buffer[2048];
    int n = snprintf(buffer, sizeof(buffer), "%s %s RTSP/1.0\r\nCSeq: %u\r\nUser-Agent: AP-CameraGimbal\r\n%s\r\n", method, url, seq, headers);
    if (n <= 0 || n >= (int)sizeof(buffer)) return -1;
    size_t off = 0;
    while (off < (size_t)n) {
        ssize_t k = send(fd, buffer + off, (size_t)n - off, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        off += (size_t)k;
    }
    return 0;
}
/* Keep the header separate: never search an SDP body for RTSP header fields. */
static int response(int fd, char *out, size_t capacity, atomic_bool *stop, bool first_r)
{
    size_t n = 0;
    if (first_r) out[n++] = 'R';
    while (n + 1 < capacity) {
        if (exact(fd, out + n, 1, stop)) return -1;
        out[++n] = 0;
        if (n >= 4 && !memcmp(out + n - 4, "\r\n\r\n", 4)) break;
    }
    if (n + 1 >= capacity) return -1;
    unsigned long length = 0;
    char *cl = strcasestr(out, "\r\nContent-Length:");
    if (cl) {
        char *end;
        errno = 0;
        length = strtoul(cl + 17, &end, 10);
        if (errno || end == cl + 17 || (*end != '\r' && *end != ' ')) return -1;
    }
    if (length >= capacity - n || exact(fd, out + n, length, stop)) return -1;
    out[n + length] = 0;
    return strncmp(out, "RTSP/1.0 200 ", 13) ? -1 : 0;
}
static int sprop(struct ca_z1_rtp *s, const char *sdp)
{
    const char *p = strstr(sdp, "sprop-parameter-sets=");
    if (!p) return 0; /* some servers send SPS/PPS in-band */
    p += 20;
    while (*p && *p != ';' && *p != '\r' && *p != '\n') {
        uint8_t nal[1024]; size_t n = 0; unsigned bits = 0, value = 0;
        while (*p && *p != ',' && *p != ';' && *p != '\r' && *p != '\n') {
            if (*p == '=') { p++; continue; }
            const char *base = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            const char *q = strchr(base, *p++);
            if (!q) return -1;
            value = (value << 6) | (unsigned)(q - base); bits += 6;
            if (bits >= 8) { bits -= 8; if (n == sizeof(nal)) return -1; nal[n++] = value >> bits; }
        }
        if (!n || ((nal[0] & 31) != 7 && (nal[0] & 31) != 8) ||
            n + 4 > sizeof(s->parameters) - s->parameter_size) return -1;
        memcpy(s->parameters + s->parameter_size, "\0\0\0\1", 4); s->parameter_size += 4;
        memcpy(s->parameters + s->parameter_size, nal, n); s->parameter_size += n;
        if (*p == ',') p++; else break;
    }
    return 0;
}
int ca_z1_receive(atomic_bool *stop, ca_z1_frame_fn consume, void *opaque)
{
    struct ca_z1_rtp *s = (struct ca_z1_rtp*)(calloc(1, sizeof(*s)));
    if (!s) return -1;
    s->consume = consume; s->opaque = opaque; s->wait_key = true;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), result = -1;
    {
    if (fd < 0) goto done;
    struct timeval timeout = {.tv_sec = 2};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_port = htons(554),
                              .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) }};
#ifdef CA_Z1_TEST
    const char *port = getenv("CA_Z1_SOURCE_PORT");
    if (port) peer.sin_port = htons((unsigned)atoi(port));
#endif
    if (connect(fd, (struct sockaddr *)&peer, sizeof(peer))) goto done;
    const char *url = "rtsp://127.0.0.1/";
    char reply[16384], control[512], session[128], headers[256];
    if (send_request(fd, "DESCRIBE", url, 1, "Accept: application/sdp\r\n") ||
        response(fd, reply, sizeof(reply), stop, false) || !strstr(reply, "H264/90000") || sprop(s, reply)) goto done;
    /* The surveyed AX620A service advertises streamid=0. */
    char *p = strstr(reply, "a=control:streamid=");
    char track[128];
    if (!p || sscanf(p, "a=control:%127[^\r\n]", track) != 1) goto done;
    snprintf(control, sizeof(control), "%s%s", url, track);
    if (send_request(fd, "SETUP", control, 2, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n") ||
        response(fd, reply, sizeof(reply), stop, false)) goto done;
    p = strcasestr(reply, "\r\nSession:");
    if (!p || sscanf(p + 10, " %127[^;\r\n]", session) != 1) goto done;
    snprintf(headers, sizeof(headers), "Session: %s\r\n", session);
    if (send_request(fd, "PLAY", url, 3, headers) || response(fd, reply, sizeof(reply), stop, false)) goto done;
    uint64_t keepalive = millis(); unsigned seq = 4;
    while (!atomic_load(stop)) {
        uint8_t h[4], packet[65536];
        if (millis() - keepalive >= 20000) {
            if (send_request(fd, "OPTIONS", url, seq++, headers)) break;
            keepalive = millis();
        }
        if (exact(fd, h, 1, stop)) break;
        if (h[0] == 'R') { if (response(fd, reply, sizeof(reply), stop, true)) break; continue; }
        if (h[0] != '$' || exact(fd, h + 1, 3, stop)) break;
        size_t n = be16(h + 2);
        if (exact(fd, packet, n, stop)) break;
        if (h[1] == 0) (void)ca_z1_rtp_packet(s, packet, n);
    }
    (void)send_request(fd, "TEARDOWN", url, seq, headers);
    result = atomic_load(stop) ? 0 : -1;
    }
done:
    if (fd >= 0) close(fd);
    free(s);
    return result;
}
