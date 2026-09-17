#define _GNU_SOURCE
#include "pipeline.h"
#include "native.h"
#include "camera_app/media_impl.h"
#include "camera_app/live_video_server.h"
#include "camera_app/rtsp.h"
#include "camera_app/mp4.h"
#include "camera_app/log.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Nominal lens HFOV supplied for this camera; both VIN outputs use the full crop. */
#include "apcam/target.h"
#define Z1_HFOV_DEG APCAM_LENS1_FOV_H

struct ca_media_impl {
    struct ca_media_config config;
    struct ca_z1_overlay_control overlay;
    atomic_bool stop, ready, recording;
    pthread_t thread;
    bool started, wait_key, have_pts;
    unsigned streams_seen;
    pthread_mutex_t lock;
    struct ca_rtsp *rtsp;
    struct ca_live_video_server *live;
    unsigned sub;
    struct ca_mp4 *mp4;
    char path[PATH_MAX];
    uint64_t pts_offset, last_pts[2];
    struct ca_exposure exposure;
};
static uint64_t monotonic_us(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static void consume_native(void *opaque, const uint8_t *data, size_t n, uint64_t pts, bool key, unsigned stream)
{
    struct ca_media_impl *m = opaque;
    if (!m->have_pts) { m->pts_offset = monotonic_us() - pts; m->have_pts = true; }
    pts += m->pts_offset;
    if (stream > 1) return;
    if (pts <= m->last_pts[stream]) pts = m->last_pts[stream] + 1;
    m->last_pts[stream] = pts;
    /* Both advertised paths expose the same fixed 1080p stream for now. */
    if (stream == 0) for (unsigned i = 0; i < 2; i++) {
        (void)ca_rtsp_push_video_timed(m->rtsp, i ? m->sub : 0, data, n, key, Z1_HFOV_DEG, pts);
        (void)ca_live_video_server_publish(m->live, i, data, n, pts, key, Z1_HFOV_DEG);
    }
    unsigned recording_stream = m->config.settings.recording_resolution == CA_VIDEO_2160P ? 1 : 0;
    if (stream == recording_stream) {
        pthread_mutex_lock(&m->lock);
        if (m->mp4 && key) m->wait_key = false;
        if (m->mp4 && !m->wait_key && ca_mp4_write_h264(m->mp4, data, n, pts, key, Z1_HFOV_DEG) < 0) {
            ca_log("Z1 recording write failed: %s", strerror(errno));
            (void)ca_mp4_close(m->mp4); m->mp4 = NULL;
            atomic_store(&m->recording, false);
        }
        pthread_mutex_unlock(&m->lock);
    }
    m->streams_seen |= 1U << stream;
    atomic_store(&m->ready, (m->streams_seen & (1U | (1U << recording_stream))) ==
                           (1U | (1U << recording_stream)));
}
static void consume(void *opaque, const uint8_t *data, size_t n, uint64_t pts, bool key)
{
    consume_native(opaque, data, n, pts, key, 0);
}
static void consume_exposure(void *opaque, const struct ca_exposure *sample)
{
    struct ca_media_impl *m=opaque;
    pthread_mutex_lock(&m->lock);
    m->exposure=*sample;
    pthread_mutex_unlock(&m->lock);
}
static void *receiver(void *opaque)
{
    struct ca_media_impl *m = opaque;
    /* Explicit opt-in during AX bring-up. Keep SDK global state and callbacks
     * in an exclusive child process, separate from the direct MCU controller. */
    const char *native_path = getenv("CAMERA_APP_Z1_NATIVE_HELPER");
    if (native_path && *native_path) {
        ca_log("Z1 native AX capture starting; exclusive media ownership required");
        int result = ca_z1_native_receive(native_path, &m->stop, consume_native, consume_exposure, m, &m->overlay);
        ca_log("Z1 native AX capture stopped result=%d", result);
        atomic_store(&m->ready, false);
        pthread_mutex_lock(&m->lock);
        if (m->mp4) { (void)ca_mp4_close(m->mp4); m->mp4 = NULL; }
        atomic_store(&m->recording, false);
        pthread_mutex_unlock(&m->lock);
        return NULL;
    }
    while (!atomic_load(&m->stop)) {
        m->have_pts = false;
        if (ca_z1_receive(&m->stop, consume, m) < 0 && !atomic_load(&m->stop)) {
            ca_log("Z1 vendor RTSP unavailable; reconnecting");
            atomic_store(&m->ready, false);
            pthread_mutex_lock(&m->lock);
            m->wait_key = true;
            pthread_mutex_unlock(&m->lock);
            for (unsigned i = 0; i < 10 && !atomic_load(&m->stop); i++) usleep(100000);
        }
    }
    return NULL;
}
int ca_media_impl_open(struct ca_media_impl **out, const struct ca_media_config *c)
{
    if (!out || !c || !c->backend || strcmp(c->backend, "z1mini") ||
        !c->record_root || !c->capture_root) { errno = EINVAL; return -1; }
    const char *helper = getenv("CAMERA_APP_Z1_NATIVE_HELPER");
    bool native = helper && *helper;
    /* Do not silently advertise settings which the retained ISP never applied. */
    if (c->settings.main_resolution != CA_VIDEO_1080P ||
        c->settings.sub_resolution != CA_VIDEO_1080P ||
        (c->settings.recording_resolution != CA_VIDEO_1080P &&
         !(native && c->settings.recording_resolution == CA_VIDEO_2160P)) ||
        c->settings.main_codec != CA_VIDEO_H264 || c->settings.sub_codec != CA_VIDEO_H264 ||
        c->settings.orientation == CA_MOUNT_INVERTED) { errno = ENOTSUP; return -1; }
    struct ca_media_impl *m = calloc(1, sizeof(*m));
    if (!m) return -1;
    m->config = *c;
    pthread_mutex_init(&m->lock, NULL);
    if (ca_rtsp_open(&m->rtsp, c->rtsp_port, "video1", CA_VIDEO_H264, 30) < 0 ||
        ca_rtsp_add_video(m->rtsp, "video2", CA_VIDEO_H264, 30, &m->sub) < 0 ||
        ca_live_video_server_open(&m->live, c->rtsp_port + 1U) < 0 ||
        ca_live_video_server_configure(m->live, 0, 1920, 1080, 30, true) < 0 ||
        ca_live_video_server_configure(m->live, 1, 1920, 1080, 30, true) < 0) goto fail;
    if (ca_rtsp_support_proxy(m->rtsp, &c->settings.support) < 0)
        ca_log("Z1 SupportProxy startup failed: %s", strerror(errno));
    int error = pthread_create(&m->thread, NULL, receiver, m);
    if (error) { errno = error; goto fail; }
    m->started = true;
    for (unsigned i = 0; i < 150 && !atomic_load(&m->ready); i++) usleep(100000);
    if (!atomic_load(&m->ready)) { errno = ETIMEDOUT; goto fail; }
    ca_log("Z1 media ready: %s -> 1080p live H264, %s recording",
           native ? "native AX ISP" : "retained AX ISP",
           c->settings.recording_resolution == CA_VIDEO_2160P ? "4K" : "1080p");
    *out = m; return 0;
fail:;
    int saved = errno; ca_media_impl_close(m); errno = saved; return -1;
}
int ca_media_impl_set_recording(struct ca_media_impl *m, bool active)
{
    if (!m) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&m->lock);
    int result = 0;
    if (active == atomic_load(&m->recording)) goto done;
    if (active) {
        if (!atomic_load(&m->ready)) { errno = EAGAIN; result = -1; goto done; }
        /* Launcher supplies a directory on a mounted card. Never fill /opt. */
#ifndef CA_Z1_TEST
        FILE *mounts = fopen("/proc/mounts", "r");
        bool mounted = false;
        char line[1024], device[256], path[256];
        if (mounts) {
            while (fgets(line, sizeof(line), mounts)) {
                if (sscanf(line, "%255s %255s", device, path) == 2 &&
                    !strncmp(device, "/dev/mmcblk", 11) && !strcmp(path, "/mnt/mmc")) mounted = true;
            }
            fclose(mounts);
        }
        if (!mounted || strncmp(m->config.record_root, "/mnt/mmc/", 9)) {
            errno = ENODEV; result = -1; goto done;
        }
#endif
        struct stat st;
        if (stat(m->config.record_root, &st) || !S_ISDIR(st.st_mode)) { result = -1; goto done; }
        struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
        int n = snprintf(m->path, sizeof(m->path), "%s/Z1_%lld_%09ld.mp4",
                         m->config.record_root, (long long)now.tv_sec, now.tv_nsec);
        if (n < 0 || n >= (int)sizeof(m->path)) { errno = ENAMETOOLONG; result = -1; goto done; }
        unsigned width, height;
        ca_video_resolution_size(m->config.settings.recording_resolution, &width, &height);
        if (ca_mp4_open(&m->mp4, m->path, width, height, 30) < 0) { result = -1; goto done; }
        m->wait_key = true;
        atomic_store(&m->recording, true);
    } else {
        result = ca_mp4_close(m->mp4); m->mp4 = NULL;
        atomic_store(&m->recording, false);
    }
done:
    pthread_mutex_unlock(&m->lock); return result;
}
bool ca_media_impl_ready(const struct ca_media_impl *m) { return m && atomic_load(&m->ready); }
bool ca_media_impl_recording(const struct ca_media_impl *m) { return m && atomic_load(&m->recording); }
const char *ca_media_impl_recording_path(const struct ca_media_impl *m) { return m ? m->path : ""; }
static int unsupported(void) { errno = ENOTSUP; return -1; }
int ca_media_impl_set_zoom(struct ca_media_impl *m, float z) { (void)m; return z == 1 ? 0 : unsupported(); }
float ca_media_impl_zoom(const struct ca_media_impl *m) { (void)m; return 1; }
float ca_media_impl_hfov(const struct ca_media_impl *m, bool thermal) { (void)m; return thermal ? NAN : Z1_HFOV_DEG; }
int ca_media_impl_set_lens(struct ca_media_impl *m, enum ca_media_lens l) { (void)m; return l == CA_MEDIA_LENS_WIDE ? 0 : unsupported(); }
enum ca_media_lens ca_media_impl_lens(const struct ca_media_impl *m) { (void)m; return CA_MEDIA_LENS_WIDE; }
int ca_media_impl_set_thermal_main(struct ca_media_impl *m, bool t) { (void)m; return t ? unsupported() : 0; }
bool ca_media_impl_thermal_main(const struct ca_media_impl *m) { (void)m; return false; }
int ca_media_impl_autofocus(struct ca_media_impl *m, uint16_t x, uint16_t y) { (void)m; (void)x; (void)y; return unsupported(); }
int ca_media_impl_manual_focus(struct ca_media_impl *m, int d) { (void)m; (void)d; return unsupported(); }
int ca_media_impl_set_focus_percent(struct ca_media_impl *m, float p) { (void)m; (void)p; return unsupported(); }
bool ca_media_impl_thermal_range(struct ca_media_impl *m, struct ca_thermal_range *r) { (void)m; (void)r; return false; }
int ca_media_impl_capture_photo(struct ca_media_impl *m, enum ca_photo_scope s) { (void)m; (void)s; return unsupported(); }
int ca_media_impl_get_thermal_gain(struct ca_media_impl *m, uint8_t *g) { (void)m; (void)g; return unsupported(); }
int ca_media_impl_set_thermal_gain(struct ca_media_impl *m, uint8_t g) { (void)m; (void)g; return unsupported(); }
int ca_media_impl_get_thermal_palette(struct ca_media_impl *m, uint8_t *p) { (void)m; (void)p; return unsupported(); }
int ca_media_impl_set_thermal_palette(struct ca_media_impl *m, uint8_t p) { (void)m; (void)p; return unsupported(); }
int ca_media_impl_set_inverted(struct ca_media_impl *m, bool i) { (void)m; return i ? unsupported() : 0; }
void ca_media_impl_close(struct ca_media_impl *m)
{
    if (!m) return;
    atomic_store(&m->stop, true);
    if (m->started) pthread_join(m->thread, NULL);
    if (m->mp4) (void)ca_mp4_close(m->mp4);
    ca_live_video_server_close(m->live); ca_rtsp_close(m->rtsp);
    pthread_mutex_destroy(&m->lock); free(m);
}

unsigned ca_media_impl_frame_rate(const struct ca_media_impl *media, bool thermal)
{
    (void)media;
    return thermal ? APCAM_THERMAL_FRAME_RATE : APCAM_FRAME_RATE;
}

int ca_media_impl_apply_image(struct ca_media_impl *media, const struct ca_config *settings)
{
    (void)media; (void)settings;
    errno = ENOTSUP; return -1;
}

int ca_media_impl_exposure(struct ca_media_impl *m, unsigned lens, struct ca_exposure *s)
{
    if (lens) return -ENOTSUP;
    pthread_mutex_lock(&m->lock);
    struct ca_exposure cached=m->exposure;
    pthread_mutex_unlock(&m->lock);
    if (!cached.time_us) return -ENODATA; /* Also identifies legacy RTSP-only mode. */
    uint64_t now=monotonic_us();
    if (now<cached.time_us || now-cached.time_us>1000000U) return -ETIMEDOUT;
    *s=cached;
    return s->result;
}

int ca_media_impl_apply_overlay(struct ca_media_impl *m, const struct ca_config *settings)
{
    int desired=settings->osd_cross;
    if (atomic_load(&m->overlay.applied)==desired) return 0;
    const char *helper=getenv("CAMERA_APP_Z1_NATIVE_HELPER");
    /* The retained vendor ISP has no overlay control channel.  Keep the
     * setting harmless so it cannot prevent the camera app from starting. */
    if (!helper || !*helper) {
        static atomic_bool warned;
        if (desired && !atomic_exchange(&warned, true))
            ca_log("OSD_CROSS is configured but unavailable with the retained vendor ISP");
        return 0;
    }
    atomic_store(&m->overlay.desired,desired);
    atomic_store(&m->overlay.applied,-EINPROGRESS);
    /* The native receiver acknowledges on its video thread. Waiting here
     * blocks the MAVLink event loop; the receiver retries after three seconds
     * of video if no acknowledgement arrives. */
    return 0;
}
