#define _GNU_SOURCE
#include "camera_app/sitl_terrain.h"
#include "camera_app/video_metadata.h"
#include "apcam/target.h"
#include "camera_app/overlay.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>

extern char **environ;
struct ca_sitl_terrain { int fd; pid_t pid; unsigned width[CA_SITL_STREAMS], height[CA_SITL_STREAMS]; };

static int transfer(int fd, void *buffer, size_t length, bool writing)
{
    uint8_t *p = buffer;
    while (length) {
        struct pollfd f = {.fd = fd, .events = writing ? POLLOUT : POLLIN};
        int ready = poll(&f, 1, 10000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) { if (!ready) errno = ETIMEDOUT; return -1; }
        ssize_t n = writing ? send(fd, p, length, MSG_NOSIGNAL) : recv(fd, p, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = ECONNRESET; return -1; }
        p += n; length -= (size_t)n;
    }
    return 0;
}

int ca_sitl_terrain_open(struct ca_sitl_terrain **out, const char *script,
                         const unsigned width[CA_SITL_STREAMS], const unsigned height[CA_SITL_STREAMS], unsigned fps,
                         const enum ca_video_codec codecs[CA_SITL_STREAMS])
{
    int pair[2] = {-1, -1};
    const char *link_option = "--fd", *link_value = "3";
    char token[33] = "";
#ifdef __CYGWIN__
    /* Native Windows Python cannot inherit a Cygwin socket descriptor. */
    char port[16];
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t address_size = sizeof(address);
    pair[0] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (pair[0] < 0) return -1;
    if (bind(pair[0], (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(pair[0], 1) < 0 ||
        getsockname(pair[0], (struct sockaddr *)&address, &address_size) < 0) {
        close(pair[0]); return -1;
    }
    unsigned char random[16];
    arc4random_buf(random, sizeof(random));
    for (unsigned i = 0; i < sizeof(random); i++) snprintf(token + 2*i, 3, "%02x", random[i]);
    snprintf(port, sizeof(port), "%u", ntohs(address.sin_port));
    link_option = "--connect"; link_value = port;
#else
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0) return -1;
#endif
    struct ca_sitl_terrain *t = calloc(1, sizeof(*t));
    if (!t) { close(pair[0]); close(pair[1]); return -1; }
    t->fd = pair[0];
    memcpy(t->width, width, sizeof(t->width));
    memcpy(t->height, height, sizeof(t->height));
    char w1[16], h1[16], w2[16], h2[16], w3[16], h3[16], w4[16], h4[16], w5[16], h5[16], rate[16];
    snprintf(w1, sizeof(w1), "%u", width[0]); snprintf(h1, sizeof(h1), "%u", height[0]);
    snprintf(w2, sizeof(w2), "%u", width[1]); snprintf(h2, sizeof(h2), "%u", height[1]);
    snprintf(w3, sizeof(w3), "%u", width[2]); snprintf(h3, sizeof(h3), "%u", height[2]);
    snprintf(w4, sizeof(w4), "%u", width[3]); snprintf(h4, sizeof(h4), "%u", height[3]);
    snprintf(w5, sizeof(w5), "%u", width[4]); snprintf(h5, sizeof(h5), "%u", height[4]);
    snprintf(rate, sizeof(rate), "%u", fps);
    const char *python = getenv("CAMERA_GIMBAL_SITL_PYTHON");
    if (!python || !*python) python = "python3";
    char *argv[] = {(char *)python, (char *)script, (char *)link_option, (char *)link_value, "--width1", w1,
                    "--height1", h1, "--width2", w2, "--height2", h2, "--width3", w3, "--height3", h3, "--width4", w4, "--height4", h4, "--width5", w5, "--height5", h5, "--fps", rate, "--token", token,
                    "--codec1", (char *)ca_video_codec_name(codecs[0]),
                    "--codec2", (char *)ca_video_codec_name(codecs[1]),
                    "--codec3", (char *)ca_video_codec_name(codecs[2]),
                    "--codec4", (char *)ca_video_codec_name(codecs[3]),
                    "--codec5", (char *)ca_video_codec_name(codecs[4]), NULL};
    posix_spawn_file_actions_t actions;
    int code = posix_spawn_file_actions_init(&actions);
    if (!code) {
        code = posix_spawn_file_actions_addclose(&actions, pair[0]);
#ifndef __CYGWIN__
        if (!code) code = posix_spawn_file_actions_adddup2(&actions, pair[1], 3);
        if (!code && pair[1] != 3) code = posix_spawn_file_actions_addclose(&actions, pair[1]);
#endif
        if (!code) code = posix_spawnp(&t->pid, python, &actions, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
    }
    close(pair[1]);
    if (code) { close(t->fd); free(t); errno = code; return -1; }
#ifdef __CYGWIN__
    struct pollfd listener = {.fd = t->fd, .events = POLLIN};
    int connected = poll(&listener, 1, 30000) > 0 ? accept4(t->fd, NULL, NULL, SOCK_CLOEXEC) : -1;
    close(t->fd);
    t->fd = connected;
    char received[32];
    if (connected < 0 || transfer(connected, received, sizeof(received), false) < 0 ||
        memcmp(received, token, sizeof(received)) != 0) {
        ca_sitl_terrain_close(t); errno = EPROTO; return -1;
    }
#endif
    char ready;
    if (transfer(t->fd, &ready, 1, false) < 0 || ready != 'R') {
        int saved = errno ? errno : EPROTO;
        ca_sitl_terrain_close(t);
        errno = saved;
        return -1;
    }
    *out = t;
    return 0;
}

int ca_sitl_terrain_frame(struct ca_sitl_terrain *t, uint64_t pts, uint64_t presentation_ms,
                          const float hfov[2], bool thermal_main, bool has_thermal, bool separate_recording,
                          const struct ca_sitl_image *image,
                          uint8_t *data[CA_SITL_STREAMS], size_t length[CA_SITL_STREAMS], bool key[CA_SITL_STREAMS],
                          uint8_t *photos[3], size_t photo_length[3], struct ca_exposure *exposure)
{
    struct ca_metadata metadata;
    struct timespec utc, now;
    ca_metadata_snapshot(&metadata);
    clock_gettime(CLOCK_REALTIME, &utc);
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_ms = (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
    unsigned lead_ms = presentation_ms > now_ms ? (unsigned)(presentation_ms - now_ms) : 0U;
    if (lead_ms > 250U) lead_ms = 250U;
    double thermal_aspect=1;
#if APCAM_HAVE_THERMAL
    thermal_aspect=(double)APCAM_LENS3_WIDTH/APCAM_LENS3_HEIGHT;
#endif
    char request[CA_VIDEO_METADATA_JSON_MAX + 3000];
    size_t n = ca_video_metadata_json(&metadata, &utc, pts, request, sizeof(request));
    if (!n) { errno = EINVAL; return -1; }
    int extra = snprintf(request + n - 1, sizeof(request) - n + 1,
                          ",\"fov\":[%.4f,%.4f],\"thermal\":[false,%s],\"swap\":%s,\"recording\":%s,\"prediction_ms\":%u,"
                          "\"thermal_aspect\":%.4f,\"base_fov\":%.4f,\"image\":{\"brightness\":%d,\"saturation\":%d,\"contrast\":%d,\"exposure\":%d,"
                          "\"iso\":%d,\"shutter\":%d,\"metering\":%d,\"white_balance\":%d,"
                          "\"thermal_gain\":%u,\"thermal_palette\":%u,\"defocus\":%.4f},"
                          "\"capture\":%u,\"capture_fov\":[%.4f,%.4f,%.4f]}\n",
                          (double)hfov[0], (double)hfov[1],
                          has_thermal ? "true" : "false", thermal_main ? "true" : "false",
                          separate_recording ? "true" : "false", lead_ms, thermal_aspect, (double)APCAM_LENS1_FOV_H,
                          image->settings.brightness, image->settings.saturation, image->settings.contrast,
                          image->settings.exposure_compensation, image->settings.iso, image->settings.shutter,
                          image->settings.metering, image->settings.white_balance,
                          image->thermal_gain, image->thermal_palette, (double)image->defocus,
                          image->capture_mask, (double)image->capture_fov[0],
                          (double)image->capture_fov[1], (double)image->capture_fov[2]);
    if (extra < 0 || (size_t)extra >= sizeof(request) - n + 1) { errno = EINVAL; return -1; }
    size_t used = n - 1 + (size_t)extra - 2; /* replace final } and newline */
    int added = snprintf(request + used, sizeof(request) - used, ",\"overlays\":[");
    if (added < 0 || (size_t)added >= sizeof(request)-used) { errno=EOVERFLOW; return -1; }
    used += added;
    for (unsigned stream=0; stream<CA_SITL_STREAMS; stream++) {
        struct ca_overlay_geometry geometry;
        ca_overlay_geometry(&geometry, t->width[stream], t->height[stream],
                            image->settings.osd_cross && (stream<3 || image->settings.osd_recording),
                            image->settings.osd_thermal_fov && (stream<3 || image->settings.osd_recording) &&
                            !(has_thermal && (stream==1 || stream==4)), hfov[0]);
        added=snprintf(request+used,sizeof(request)-used,"%s{\"scale\":%u,\"lines\":[",stream?",":"",geometry.scale);
        if (added<0 || (size_t)added>=sizeof(request)-used) { errno=EOVERFLOW; return -1; }
        used+=added;
        for (unsigned i=0; i<geometry.count; i++) {
            const struct ca_overlay_line *l=&geometry.lines[i];
            added=snprintf(request+used,sizeof(request)-used,"%s[%d,%d,%d,%d,%u]",
                           i?",":"",l->x0,l->y0,l->x1,l->y1,l->dashed);
            if (added<0 || (size_t)added>=sizeof(request)-used) { errno=EOVERFLOW; return -1; }
            used+=added;
        }
        added=snprintf(request+used,sizeof(request)-used,"]}");
        if (added<0 || (size_t)added>=sizeof(request)-used) { errno=EOVERFLOW; return -1; }
        used+=added;
    }
    added=snprintf(request+used,sizeof(request)-used,"]}\n");
    if (added<0 || (size_t)added>=sizeof(request)-used) { errno=EOVERFLOW; return -1; }
    used+=added;
    if (transfer(t->fd, request, used, true) < 0) return -1;
    for (unsigned i = 0; i < CA_SITL_STREAMS; i++) {
        uint32_t header[2];
        if (transfer(t->fd, header, sizeof(header), false) < 0) return -1;
        length[i] = ntohl(header[0]); key[i] = ntohl(header[1]) != 0;
        if (i >= 2 && length[i] == 0) continue;
        if (!length[i] || length[i] > 8U * 1024U * 1024U) { errno = EPROTO; return -1; }
        data[i] = malloc(length[i]);
        if (!data[i] || transfer(t->fd, data[i], length[i], false) < 0) return -1;
    }
    if (transfer(t->fd, exposure, sizeof(*exposure), false) < 0) return -1;
    for (unsigned i = 0; i < 3; i++) {
        if (!(image->capture_mask & (1U << i))) continue;
        uint32_t length;
        if (transfer(t->fd, &length, sizeof(length), false) < 0) return -1;
        photo_length[i] = ntohl(length);
        if (!photo_length[i] || photo_length[i] > 16U * 1024U * 1024U) { errno = EPROTO; return -1; }
        photos[i] = malloc(photo_length[i]);
        if (!photos[i] || transfer(t->fd, photos[i], photo_length[i], false) < 0) return -1;
    }
    return 0;
}

void ca_sitl_terrain_interrupt(struct ca_sitl_terrain *t)
{
    if (t) shutdown(t->fd, SHUT_RDWR);
}

void ca_sitl_terrain_close(struct ca_sitl_terrain *t)
{
    if (!t) return;
    close(t->fd);
    /* Renderer owns only this camera's resources; never leave it behind on restart. */
    kill(t->pid, SIGTERM);
    while (waitpid(t->pid, NULL, 0) < 0 && errno == EINTR) {}
    free(t);
}
