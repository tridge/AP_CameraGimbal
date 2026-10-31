#include <new>
#include <vector>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/support_video.h"
#include "camera_app/video_metadata.h"
#include "camera_app/log.h"
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include "apcam/atomic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_QUEUED_BYTES (2U * 1024U * 1024U)
#define MAX_QUEUED_FRAMES 60U
#define RTP_PAYLOAD 1200U
struct frame {
    struct frame *next;
    size_t length;
    uint32_t timestamp;
    bool key;
    uint8_t data[];
};
struct ca_support_video {
    struct ca_support_config config;
    unsigned port, stream;
    enum ca_video_codec codec;
    char url[768], track_url[800], session[128];
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    atomic_bool stop;
    struct frame *first, *last;
    size_t queued_bytes;
    unsigned queued_frames;
    bool wait_key;
    unsigned dropped_frames;
    uint16_t sequence;
    uint32_t ssrc, cseq;
    int fd;
    std::vector<uint8_t> matroska_header;
};

static uint64_t milliseconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000U + now.tv_nsec / 1000000U;
}

static void clear_frames(struct ca_support_video *p)
{
    while (p->first) {
        struct frame *next = p->first->next;
        free(p->first);
        p->first = next;
    }
    p->last = NULL;
    p->queued_bytes = p->queued_frames = 0;
    p->wait_key = true;
}

static int ready(struct ca_support_video *p, short events, uint64_t deadline)
{
    while (!atomic_load(&p->stop) && milliseconds() < deadline) {
        struct pollfd f = {.fd = p->fd, .events = events};
        int result = poll(&f, 1, 100);
        if (result < 0 && errno != EINTR) return -1;
        if (result > 0) {
            if (f.revents & events) return 0;
            if (f.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                errno = ECONNRESET;
                return -1;
            }
        }
    }
    errno = ETIMEDOUT;
    return -1;
}

static int transfer(struct ca_support_video *p, void *data, size_t length,
                      bool writing, uint64_t deadline, bool progress_timeout = false)
{
    uint8_t *bytes = (uint8_t*)(data);
    while (length) {
        if (ready(p, writing ? POLLOUT : POLLIN, deadline) < 0) return -1;
        ssize_t count = writing ? send(p->fd, bytes, length, MSG_NOSIGNAL)
                                : recv(p->fd, bytes, length, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count == 0) errno = ECONNRESET;
        if (count <= 0) return -1;
        bytes += count;
        length -= count;
        if (progress_timeout) deadline = milliseconds() + 3000U;
    }
    return 0;
}

static int request(struct ca_support_video *p, const char *method,
                    const char *url, const char *headers, const char *body)
{
    char buffer[4096];
    char session[160] = "";
    if (p->session[0]) snprintf(session, sizeof(session), "Session: %s\r\n", p->session);
    int length = snprintf(buffer, sizeof(buffer),
        "%s %s RTSP/1.0\r\nCSeq: %u\r\nUser-Agent: ArduPilot-camera\r\n%s%sContent-Length: %zu\r\n\r\n%s",
        method, url, ++p->cseq, session, headers, strlen(body), body);
    uint64_t deadline = milliseconds() + 3000U;
    if (length < 0 || (size_t)length >= sizeof(buffer) ||
        transfer(p, buffer, length, true, deadline) < 0) return -1;
    size_t used = 0;
    for (;;) {
        uint8_t byte;
        if (transfer(p, &byte, 1, false, deadline) < 0) return -1;
        if (used == 0 && byte == '$') {
            // Drain interleaved RTCP before reading the next RTSP response.
            uint8_t header[3], discard[512];
            if (transfer(p, header, 3, false, deadline) < 0) return -1;
            size_t remaining = (unsigned)header[1] * 256U + header[2];
            while (remaining) {
                size_t chunk = remaining < sizeof(discard) ? remaining : sizeof(discard);
                if (transfer(p, discard, chunk, false, deadline) < 0) return -1;
                remaining -= chunk;
            }
            continue;
        }
        if (used + 1 >= sizeof(buffer)) return -1;
        buffer[used++] = byte;
        if (used >= 4 && memcmp(buffer + used - 4, "\r\n\r\n", 4) == 0) break;
    }
    buffer[used] = '\0';
    unsigned code;
    if (sscanf(buffer, "RTSP/1.0 %u", &code) != 1 || code != 200) {
        errno = EPROTO;
        return -1;
    }
    size_t content_length = 0;
    char *save = NULL;
    for (char *line = strtok_r(buffer, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (strncasecmp(line, "Session:", 8) == 0) {
            char *value = line + 8;
            while (*value == ' ') value++;
            size_t size = strcspn(value, "; \t");
            if (size == 0 || size >= sizeof(p->session)) return -1;
            memcpy(p->session, value, size); p->session[size] = '\0';
        } else if (strncasecmp(line, "Content-Length:", 15) == 0) {
            char *end;
            content_length = strtoul(line + 15, &end, 10);
            if (*end || content_length > 65536U) return -1;
        }
    }
    while (content_length) {
        size_t chunk = content_length < sizeof(buffer) ? content_length : sizeof(buffer);
        if (transfer(p, buffer, chunk, false, deadline) < 0) return -1;
        content_length -= chunk;
    }
    return 0;
}

static int send_chunk(struct ca_support_video *p, const uint8_t *data, size_t length,
                      uint64_t deadline)
{
    // Allow a whole raw frame to take longer on a slow uplink; time out
    // only when progress stops. The pending-frame queue remains bounded.
    char size[32];
    int n = snprintf(size, sizeof(size), "%zx\r\n", length);
    char end[] = "\r\n";
    return transfer(p, size, n, true, deadline) < 0 ||
        transfer(p, const_cast<uint8_t *>(data), length, true, deadline, true) < 0 ||
        transfer(p, end, 2, true, milliseconds()+3000U) < 0 ? -1 : 0;
}

static int connect_publisher(struct ca_support_video *p)
{
    char port[8];
    snprintf(port, sizeof(port), "%u", p->port);
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *addresses;
    if (getaddrinfo(p->config.host, port, &hints, &addresses) != 0) return -1;
    for (struct addrinfo *a = addresses; a; a = a->ai_next) {
        p->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (p->fd < 0) continue;
        // Leave SO_SNDBUF unset so TCP can autotune for the link's bandwidth
        // and RTT within tcp_wmem limits. A fixed 128 KiB budget throttles the
        // 4 Mbit/s RGB stream on a 225 ms link even after slow start completes.
        int result = connect(p->fd, a->ai_addr, a->ai_addrlen);
        if (result < 0 && errno == EINPROGRESS && ready(p, POLLOUT, milliseconds() + 2000U) == 0) {
            int error = 0;
            socklen_t size = sizeof(error);
            if (getsockopt(p->fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0) {
                if (!error) result = 0;
                else errno = error;
            }
        }
        if (result == 0) break;
        int error = errno;
        close(p->fd); p->fd = -1;
        errno = error;
    }
    freeaddrinfo(addresses);
    if (p->fd < 0) return -1;
    if (!p->matroska_header.empty()) {
        char request[1024], response[256];
        int n = snprintf(request, sizeof(request),
            "PUT %s HTTP/1.1\r\nHost: %s:%u\r\nContent-Type: video/x-matroska\r\n"
            "Transfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n",
            p->url, p->config.host, p->port);
        uint64_t deadline = milliseconds() + 3000;
        if (n < 0 || size_t(n) >= sizeof(request) || transfer(p, request, n, true, deadline) < 0) return -1;
        size_t used = 0;
        while (used+1 < sizeof(response)) {
            if (transfer(p, response+used, 1, false, deadline) < 0) return -1;
            used++;
            if (used >= 4 && memcmp(response+used-4, "\r\n\r\n", 4) == 0) break;
        }
        response[used] = 0;
        if (strcmp(response, "HTTP/1.1 100 Continue\r\n\r\n") != 0) { errno = EACCES; return -1; }
        if (send_chunk(p, p->matroska_header.data(), p->matroska_header.size(), deadline) < 0) return -1;
        ca_log("SupportProxy video3 publishing Matroska to %s:%u", p->config.host, p->port);
        return 0;
    }
    p->session[0] = '\0'; p->cseq = 0;
    char sdp[2048];
    snprintf(sdp, sizeof(sdp),
        "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=%s\r\n"
        "c=IN IP4 0.0.0.0\r\nt=0 0\r\na=control:*\r\n"
        "m=video 0 RTP/AVP 96\r\na=rtpmap:96 %s/90000\r\n%s"
        "a=control:%s\r\n",
        p->stream == 0 ? p->config.video1_name : p->config.video2_name,
        p->codec == CA_VIDEO_H265 ? "H265" : "H264",
        p->codec == CA_VIDEO_H264 ? "a=fmtp:96 packetization-mode=1\r\n" : "", p->track_url);
    if (request(p, "OPTIONS", p->url, "", "") < 0 ||
        request(p, "ANNOUNCE", p->url, "Content-Type: application/sdp\r\n", sdp) < 0 ||
        request(p, "SETUP", p->track_url,
                "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=record\r\n", "") < 0 ||
        request(p, "RECORD", p->url, "Range: npt=0.000-\r\n", "") < 0) return -1;
    ca_log("SupportProxy video%u publishing to %s:%u", p->stream + 1U, p->config.host, p->port);
    return 0;
}

static int rtp(struct ca_support_video *p, const uint8_t *payload, size_t length,
                uint32_t timestamp, bool marker, uint64_t deadline)
{
    uint8_t packet[16 + RTP_PAYLOAD + 3];
    size_t size = length + 12U;
    packet[0] = '$'; packet[1] = 0;
    packet[2] = size >> 8; packet[3] = size;
    packet[4] = 0x80; packet[5] = 96U | (marker ? 0x80U : 0U);
    packet[6] = p->sequence >> 8; packet[7] = p->sequence++;
    for (unsigned i = 0; i < 4; i++) {
        packet[8 + i] = timestamp >> (24U - 8U * i);
        packet[12 + i] = p->ssrc >> (24U - 8U * i);
    }
    memcpy(packet + 16, payload, length);
    return transfer(p, packet, length + 16U, true, deadline);
}

static int send_frame(struct ca_support_video *p, const struct frame *frame)
{
    if (!p->matroska_header.empty())
        return send_chunk(p, frame->data, frame->length, milliseconds() + 3000U);
    size_t offset = 0, nal, size;
    uint64_t deadline = milliseconds() + 1000U;
    while (ca_annexb_next(frame->data, frame->length, &offset, &nal, &size)) {
        if (!size) continue;
        size_t peek = offset, next, next_size;
        bool last = !ca_annexb_next(frame->data, frame->length, &peek, &next, &next_size);
        const uint8_t *data = frame->data + nal;
        if (size <= RTP_PAYLOAD) {
            if (rtp(p, data, size, frame->timestamp, last, deadline) < 0) return -1;
            continue;
        }
        bool hevc = p->codec == CA_VIDEO_H265;
        size_t header = hevc ? 2U : 1U;
        uint8_t fragment[RTP_PAYLOAD + 3];
        size_t fu_size = hevc ? 3U : 2U;
        if (hevc) {
            fragment[0] = (data[0] & 0x81U) | (49U << 1);
            fragment[1] = data[1];
        } else fragment[0] = (data[0] & 0xe0U) | 28U;
        uint8_t type = hevc ? (data[0] >> 1) & 63U : data[0] & 31U;
        for (size_t pos = header; pos < size;) {
            size_t chunk = size - pos < RTP_PAYLOAD ? size - pos : RTP_PAYLOAD;
            bool end = pos + chunk == size;
            fragment[fu_size - 1U] = type | (pos == header ? 0x80U : 0U) | (end ? 0x40U : 0U);
            memcpy(fragment + fu_size, data + pos, chunk);
            if (rtp(p, fragment, chunk + fu_size, frame->timestamp, end && last, deadline) < 0) return -1;
            pos += chunk;
        }
    }
    return 0;
}

static void *publish(void *opaque)
{
    struct ca_support_video *p = (struct ca_support_video*)(opaque);
    uint64_t keepalive = 0;
    while (!atomic_load(&p->stop)) {
        pthread_mutex_lock(&p->lock);
        while (!p->first && !atomic_load(&p->stop)) pthread_cond_wait(&p->changed, &p->lock);
        struct frame *frame = p->first;
        unsigned dropped = p->dropped_frames;
        p->dropped_frames = 0;
        if (frame) {
            p->first = frame->next;
            if (!p->first) p->last = NULL;
            p->queued_bytes -= frame->length; p->queued_frames--;
        }
        pthread_mutex_unlock(&p->lock);
        if (!frame) continue;
        if (dropped) ca_log("SupportProxy video%u dropped %u queued frames; resuming at a keyframe",
                            p->stream + 1U, dropped);
        // Dropping complete queued access units does not break RTSP framing.
        // Keep the TCP connection (and its congestion window) and recover at
        // the next keyframe instead of repeatedly starting TCP slow start.
        int result = 0;
        const char *stage = p->matroska_header.empty() ? "RTSP setup" : "Matroska setup";
        if (p->fd < 0) {
            result = frame->key ? connect_publisher(p) : -1;
            keepalive = milliseconds() + 15000U;
        }
        if (result == 0 && p->matroska_header.empty() && milliseconds() >= keepalive) {
            stage = "RTSP keepalive";
            result = request(p, "OPTIONS", p->url, "", "");
            keepalive = milliseconds() + 15000U;
        }
        if (result == 0) {
            stage = "video send";
            result = send_frame(p, frame);
        }
        free(frame);
        if (result < 0) {
            if (!atomic_load(&p->stop))
                ca_log("SupportProxy video%u %s failed: %s; reconnecting",
                       p->stream + 1U, stage, strerror(errno));
            if (p->fd >= 0) { close(p->fd); p->fd = -1; }
            pthread_mutex_lock(&p->lock);
            clear_frames(p);
            pthread_mutex_unlock(&p->lock);
            for (unsigned i = 0; i < 20 && !atomic_load(&p->stop); i++) usleep(100000);
        }
    }
    if (p->fd >= 0) close(p->fd);
    return NULL;
}

static void escape_url(char *output, const char *input)
{
    const char hex[] = "0123456789ABCDEF";
    for (; *input; input++) {
        unsigned char c = (unsigned char)*input;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') *output++ = c;
        else { *output++ = '%'; *output++ = hex[c >> 4]; *output++ = hex[c & 15]; }
    }
    *output = '\0';
}

static int open_publisher(struct ca_support_video **result,
                           const struct ca_support_config *config,
                           unsigned stream, enum ca_video_codec codec,
                           const uint8_t *header, size_t header_length)
{
    *result = NULL;
    if (!config->enabled) return 0;
    unsigned port = stream == 2 ? config->video3_port :
        (stream == 0 ? config->video1_port : config->video2_port);
    if (!port) return 0;
    struct ca_support_video *p = new (std::nothrow) ca_support_video{};
    if (!p) return -1;
    p->config = *config; p->port = port; p->stream = stream; p->codec = codec;
    p->fd = -1; p->wait_key = true;
    if (header_length) p->matroska_header.assign(header, header+header_length);
    p->ssrc = (uint32_t)milliseconds() ^ ((uint32_t)getpid() << 12) ^ stream;
    atomic_init(&p->stop, false);
    char name[3 * 64], password[3 * 128], query[3 * 128 + 5];
    escape_url(name, stream == 0 ? config->video1_name : config->video2_name);
    escape_url(password, config->publish_password);
    snprintf(query, sizeof(query), "%s%s", password[0] ? "?pw=" : "", password);
    snprintf(p->url, sizeof(p->url), "rtsp://%s:%u/%s%s", config->host, port, name, query);
    snprintf(p->track_url, sizeof(p->track_url), "rtsp://%s:%u/%s/streamid=0%s", config->host, port, name, query);
    if (stream == 2) snprintf(p->url, sizeof(p->url), "/v3.mkv%s", query);
    int error = pthread_mutex_init(&p->lock, NULL);
    if (error) { delete p; errno = error; return -1; }
    error = pthread_cond_init(&p->changed, NULL);
    if (error) { pthread_mutex_destroy(&p->lock); delete p; errno = error; return -1; }
    error = pthread_create(&p->thread, NULL, publish, p);
    if (error) {
        pthread_cond_destroy(&p->changed); pthread_mutex_destroy(&p->lock);
        delete p; errno = error; return -1;
    }
    *result = p;
    return 0;
}

int ca_support_video_open(struct ca_support_video **result,
                          const struct ca_support_config *config,
                          unsigned stream, enum ca_video_codec codec)
{
    return open_publisher(result, config, stream, codec, nullptr, 0);
}

int ca_support_video_open_matroska(struct ca_support_video **result,
                                 const struct ca_support_config *config,
                                 const uint8_t *header, size_t length)
{
    return open_publisher(result, config, 2, CA_VIDEO_H264, header, length);
}

void ca_support_video_push(struct ca_support_video *p, const uint8_t *data,
                            size_t length, uint32_t timestamp, bool key)
{
    if (!p || !length) return;
    pthread_mutex_lock(&p->lock);
    if ((!p->matroska_header.empty() && p->first) ||
        length > MAX_QUEUED_BYTES || p->queued_bytes + length > MAX_QUEUED_BYTES ||
        p->queued_frames == MAX_QUEUED_FRAMES) {
        p->dropped_frames += p->queued_frames;
        clear_frames(p);
    }
    if (length > MAX_QUEUED_BYTES || (p->wait_key && !key)) {
        pthread_mutex_unlock(&p->lock); return;
    }
    struct frame *frame = (struct frame*)(malloc(sizeof(*frame) + length));
    if (!frame) {
        p->dropped_frames += p->queued_frames;
        clear_frames(p);
        pthread_mutex_unlock(&p->lock); return;
    }
    frame->next = NULL; frame->length = length; frame->timestamp = timestamp; frame->key = key;
    memcpy(frame->data, data, length);
    if (p->last) p->last->next = frame;
    else p->first = frame;
    p->last = frame; p->queued_bytes += length; p->queued_frames++; p->wait_key = false;
    pthread_cond_signal(&p->changed);
    pthread_mutex_unlock(&p->lock);
}

void ca_support_video_close(struct ca_support_video *p)
{
    if (!p) return;
    atomic_store(&p->stop, true);
    pthread_mutex_lock(&p->lock);
    pthread_cond_signal(&p->changed);
    pthread_mutex_unlock(&p->lock);
    pthread_join(p->thread, NULL);
    clear_frames(p);
    pthread_cond_destroy(&p->changed); pthread_mutex_destroy(&p->lock);
    delete p;
}
