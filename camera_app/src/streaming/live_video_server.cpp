#include <new>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/live_video_server.h"

#include "camera_app/log.h"
#include "camera_app/mp4.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include "apcam/atomic.h"
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define LIVE_STREAMS 2U

struct live_stream {
    uint8_t *frame;
    size_t length;
    uint64_t pts;
    float hfov_deg;
    uint64_t generation;
    uint64_t config_generation;
    unsigned width;
    unsigned height;
    unsigned frame_rate;
    bool key_frame;
    bool h264;
    bool configured;
};

struct ca_live_video_server {
    int listen_fd;
    pthread_t thread;
    bool thread_started;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool lock_initialized;
    bool cond_initialized;
    atomic_bool stop;
    struct live_stream streams[LIVE_STREAMS];
    unsigned port;
    unsigned clients;
};

struct live_client {
    struct ca_live_video_server *server;
    int fd;
};

static int socket_write(void *opaque, const void *buffer, size_t length)
{
    struct live_client *client = (live_client*)(opaque);
    const uint8_t *data = (const uint8_t*)(buffer);

    while (length != 0U && !atomic_load(&client->server->stop)) {
        ssize_t sent = send(client->fd, data, length, MSG_NOSIGNAL);
        if (sent > 0) {
            data += (size_t)sent;
            length -= (size_t)sent;
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return length == 0U ? 0 : -1;
}

static void client_finished(struct live_client *client)
{
    struct ca_live_video_server *server = client->server;

    close(client->fd);
    free(client);
    pthread_mutex_lock(&server->lock);
    server->clients--;
    pthread_cond_broadcast(&server->changed);
    pthread_mutex_unlock(&server->lock);
}

static void *client_thread(void *opaque)
{
    struct live_client *client = (live_client*)(opaque);
    struct ca_live_video_server *server = client->server;
    struct ca_fmp4 *writer = NULL;
    uint64_t generation = 0;
    uint64_t config_generation;
    unsigned width, height, frame_rate;
    uint8_t selection;
    const uint8_t success = 0U;
    bool started = false;
    struct timeval timeout = {.tv_sec = 3, .tv_usec = 0};
    int no_delay = 1;

    /* A small final MP4 chunk must not wait for a delayed TCP ACK. */
    (void)setsockopt(client->fd, IPPROTO_TCP, TCP_NODELAY, &no_delay,
                     sizeof(no_delay));
    (void)setsockopt(client->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout));
    (void)setsockopt(client->fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout));
    if (recv(client->fd, &selection, 1U, MSG_WAITALL) != 1 ||
        selection >= LIVE_STREAMS) goto done;

    pthread_mutex_lock(&server->lock);
    if (!server->streams[selection].configured ||
        !server->streams[selection].h264) {
        pthread_mutex_unlock(&server->lock);
        selection = 1U;
        (void)send(client->fd, &selection, 1U, MSG_NOSIGNAL);
        goto done;
    }
    config_generation = server->streams[selection].config_generation;
    width = server->streams[selection].width;
    height = server->streams[selection].height;
    frame_rate = server->streams[selection].frame_rate;
    pthread_mutex_unlock(&server->lock);

    if (send(client->fd, &success, 1U, MSG_NOSIGNAL) != 1) goto done;

    if (ca_fmp4_open(&writer, width, height, frame_rate, socket_write,
                      client) < 0) goto done;
    while (!atomic_load(&server->stop)) {
        uint8_t *frame;
        size_t length;
        uint64_t pts;
        bool key_frame;
        float hfov_deg;

        pthread_mutex_lock(&server->lock);
        while (!atomic_load(&server->stop) &&
               server->streams[selection].config_generation ==
                   config_generation &&
               server->streams[selection].generation == generation) {
            pthread_cond_wait(&server->changed, &server->lock);
        }
        if (atomic_load(&server->stop) ||
            server->streams[selection].config_generation !=
                config_generation) {
            pthread_mutex_unlock(&server->lock);
            break;
        }
        generation = server->streams[selection].generation;
        length = server->streams[selection].length;
        pts = server->streams[selection].pts;
        hfov_deg = server->streams[selection].hfov_deg;
        key_frame = server->streams[selection].key_frame;
        frame = (uint8_t*)(malloc(length));
        if (frame != NULL) memcpy(frame, server->streams[selection].frame,
                                  length);
        pthread_mutex_unlock(&server->lock);
        if (frame == NULL) break;
        /* minimp4 needs SPS/PPS from an IDR access unit before it can emit the
         * initialization segment.  Never begin a viewer on a P-frame. */
        if (key_frame) started = true;
        if (writer != NULL && started) {
            if (ca_fmp4_write_h264(writer, frame, length, pts,
                                    key_frame, hfov_deg) < 0) {
                free(frame);
                break;
            }
        }
        free(frame);
    }
done:
    (void)ca_fmp4_close(writer);
    client_finished(client);
    return NULL;
}

static void *server_thread(void *opaque)
{
    struct ca_live_video_server *server = (struct ca_live_video_server*)(opaque);

    while (!atomic_load(&server->stop)) {
        struct live_client *client;
        pthread_attr_t attr;
        pthread_t thread;
        int fd = accept4(server->listen_fd, NULL, NULL, SOCK_CLOEXEC);

        if (fd < 0) {
            if (errno == EINTR) continue;
            if (atomic_load(&server->stop) || errno == EBADF ||
                errno == EINVAL) break;
            ca_log("live video accept failed: %s", strerror(errno));
            continue;
        }
        client = (live_client*)(calloc(1, sizeof(*client)));
        if (client == NULL) {
            close(fd);
            continue;
        }
        client->server = server;
        client->fd = fd;
        pthread_mutex_lock(&server->lock);
        server->clients++;
        pthread_mutex_unlock(&server->lock);
        if (pthread_attr_init(&attr) != 0) {
            client_finished(client);
            continue;
        }
        (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&thread, &attr, client_thread, client) != 0) {
            (void)pthread_attr_destroy(&attr);
            client_finished(client);
            continue;
        }
        (void)pthread_attr_destroy(&attr);
    }
    return NULL;
}

int ca_live_video_server_open(struct ca_live_video_server **result,
                              unsigned port)
{
    struct ca_live_video_server *server;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) },
    };
    socklen_t address_length = sizeof(address);
    int one = 1;
    int code;
    int fd = -1;

    if (result == NULL || port > 65535U) {
        errno = EINVAL;
        return -1;
    }
    server = new (std::nothrow) ca_live_video_server{};
    if (server == NULL) return -1;
    server->listen_fd = -1;
    code = pthread_mutex_init(&server->lock, NULL);
    if (code != 0) {
        errno = code;
        goto fail;
    }
    server->lock_initialized = true;
    code = pthread_cond_init(&server->changed, NULL);
    if (code != 0) {
        errno = code;
        goto fail;
    }
    server->cond_initialized = true;
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one,
                   sizeof(one)) < 0 ||
        bind(fd, (const struct sockaddr *)&address,
             sizeof(address)) < 0 ||
        listen(fd, 8) < 0 ||
        getsockname(fd, (struct sockaddr *)&address,
                    &address_length) < 0) goto fail;
    server->port = ntohs(address.sin_port);
    server->listen_fd = fd;
    fd = -1;
    code = pthread_create(&server->thread, NULL, server_thread, server);
    if (code != 0) {
        errno = code;
        close(server->listen_fd);
        server->listen_fd = -1;
        goto fail;
    }
    server->thread_started = true;
    ca_log("native live video ready on loopback port %u", server->port);
    *result = server;
    return 0;
fail:
    {
        int saved_errno = errno;
        if (fd >= 0) close(fd);
        ca_live_video_server_close(server);
        errno = saved_errno;
    }
    return -1;
}

int ca_live_video_server_configure(struct ca_live_video_server *server,
                                   unsigned stream, unsigned width,
                                   unsigned height, unsigned frame_rate,
                                   bool h264)
{
    struct live_stream *state;

    if (server == NULL || stream >= LIVE_STREAMS || width == 0U ||
        height == 0U || frame_rate == 0U) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&server->lock);
    state = &server->streams[stream];
    state->width = width;
    state->height = height;
    state->frame_rate = frame_rate;
    state->h264 = h264;
    state->configured = true;
    state->config_generation++;
    state->generation = 0;
    state->length = 0;
    pthread_cond_broadcast(&server->changed);
    pthread_mutex_unlock(&server->lock);
    return 0;
}

int ca_live_video_server_publish(struct ca_live_video_server *server,
                                 unsigned stream, const uint8_t *annex_b,
                                 size_t length, uint64_t pts, bool key_frame, float hfov_deg)
{
    struct live_stream *state;
    uint8_t *copy;

    if (server == NULL || stream >= LIVE_STREAMS || annex_b == NULL ||
        length == 0U) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&server->lock);
    if (server->clients == 0U || !server->streams[stream].configured ||
        !server->streams[stream].h264) {
        pthread_mutex_unlock(&server->lock);
        return 0;
    }
    pthread_mutex_unlock(&server->lock);
    copy = (uint8_t*)(malloc(length));
    if (copy == NULL) return -1;
    memcpy(copy, annex_b, length);
    pthread_mutex_lock(&server->lock);
    state = &server->streams[stream];
    if (!state->configured || !state->h264) {
        pthread_mutex_unlock(&server->lock);
        free(copy);
        return 0;
    }
    free(state->frame);
    state->frame = copy;
    state->length = length;
    state->pts = pts;
    state->hfov_deg = hfov_deg;
    state->key_frame = key_frame;
    state->generation++;
    pthread_cond_broadcast(&server->changed);
    pthread_mutex_unlock(&server->lock);
    return 0;
}

unsigned ca_live_video_server_port(const struct ca_live_video_server *server)
{
    return server != NULL ? server->port : 0U;
}

void ca_live_video_server_close(struct ca_live_video_server *server)
{
    if (server == NULL) return;
    atomic_store(&server->stop, true);
    if (server->listen_fd >= 0) {
        (void)shutdown(server->listen_fd, SHUT_RDWR);
        close(server->listen_fd);
        server->listen_fd = -1;
    }
    if (server->lock_initialized) {
        pthread_mutex_lock(&server->lock);
        if (server->cond_initialized) pthread_cond_broadcast(&server->changed);
        pthread_mutex_unlock(&server->lock);
    }
    if (server->thread_started) pthread_join(server->thread, NULL);
    if (server->lock_initialized) {
        pthread_mutex_lock(&server->lock);
        while (server->clients != 0U) {
            pthread_cond_wait(&server->changed, &server->lock);
        }
        pthread_mutex_unlock(&server->lock);
    }
    for (unsigned i = 0; i < LIVE_STREAMS; i++) free(server->streams[i].frame);
    if (server->cond_initialized) pthread_cond_destroy(&server->changed);
    if (server->lock_initialized) pthread_mutex_destroy(&server->lock);
    delete server;
}
