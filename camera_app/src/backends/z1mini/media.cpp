#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <new>
#include "pipeline.h"
#include "native.h"
#include "camera_app/APC_Media_Backend.h"
#include "camera_app/live_video_server.h"
#include "camera_app/rtsp.h"
#include "camera_app/mp4.h"
#include "camera_app/video_metadata.h"
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

class APC_Media_Z1Mini;
struct APC_Media_Z1Mini_State {
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
    ca_tracking_frame tracking;

    APC_Media_Z1Mini *owner = nullptr;
};

// Driver state is private to this translation unit. The owner survives all
// capture callbacks and joins them before releasing SDK resources.
class APC_Media_Z1Mini final : public APC_Media_Backend {
public:
    ~APC_Media_Z1Mini() override { shutdown(); }
    static std::unique_ptr<APC_Media_Backend> create(const ca_media_config &config)
    {
        auto *driver = new (std::nothrow) APC_Media_Z1Mini();
        if (!driver) { errno = ENOMEM; return nullptr; }
        if (driver->init(&config) < 0) {
            const int saved = errno;
            delete driver;
            errno = saved;
            return nullptr;
        }
        return std::unique_ptr<APC_Media_Backend>(driver);
    }
    bool ready() const override;
    bool tracking_available() const override {
        if(!_state) return false;
        pthread_mutex_lock(&_state->lock);
        const bool ok=_state->tracking.timestamp_ms!=0;
        pthread_mutex_unlock(&_state->lock); return ok;
    }
    bool tracking_frame(ca_tracking_frame &frame) override {
        pthread_mutex_lock(&_state->lock); frame=_state->tracking;
        pthread_mutex_unlock(&_state->lock); return frame.timestamp_ms!=0;
    }
    int set_recording(bool active) override;
    bool recording() const override;
    const char * recording_path() const override;
    int set_zoom(float zoom) override;
    float zoom() const override;
    float hfov(bool thermal) const override;
    unsigned frame_rate(bool thermal) const override;
    int set_lens(enum ca_media_lens lens) override;
    enum ca_media_lens lens() const override;
    int set_thermal_main(bool thermal_main) override;
    bool thermal_main() const override;
    int autofocus(uint16_t x, uint16_t y) override;
    int manual_focus(int direction) override;
    int set_focus_percent(float percent) override;
    bool thermal_range(struct ca_thermal_range *range) override;
    int capture_photo(enum ca_photo_scope scope) override;
    int get_thermal_gain(uint8_t *gain) override;
    int set_thermal_gain(uint8_t gain) override;
    int get_thermal_palette(uint8_t *palette) override;
    int set_thermal_palette(uint8_t palette) override;
    int set_inverted(bool inverted) override;
    int exposure(unsigned lens, struct ca_exposure *sample) override;
    int apply_overlay(const struct ca_config *settings) override;
    int apply_image(const struct ca_config *settings) override;
private:
    APC_Media_Z1Mini() = default;
    int init(const ca_media_config *config);
    void shutdown();
    APC_Media_Z1Mini_State *_state = nullptr;
};

static uint64_t monotonic_us(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static void consume_native(void *opaque, const uint8_t *data, size_t n, uint64_t pts, bool key, unsigned stream)
{
    struct APC_Media_Z1Mini_State *m = (struct APC_Media_Z1Mini_State*)(opaque);
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
    struct APC_Media_Z1Mini_State *m=(struct APC_Media_Z1Mini_State*)(opaque);
    pthread_mutex_lock(&m->lock);
    m->exposure=*sample;
    pthread_mutex_unlock(&m->lock);
}
static void *receiver(void *opaque)
{
    struct APC_Media_Z1Mini_State *m = (struct APC_Media_Z1Mini_State*)(opaque);
    /* Explicit opt-in during AX bring-up. Keep SDK global state and callbacks
     * in an exclusive child process, separate from the direct MCU controller. */
    const char *native_path = getenv("CAMERA_APP_Z1_NATIVE_HELPER");
    if (native_path && *native_path) {
        ca_log("Z1 native AX capture starting; exclusive media ownership required");
        int result = ca_z1_native_receive(native_path, &m->stop, consume_native, consume_exposure, m, &m->overlay,
            m->config.settings.orientation == CA_MOUNT_INVERTED,
            [](void *opaque,const ca_tracking_frame &frame) {
                auto *m=static_cast<APC_Media_Z1Mini_State *>(opaque);
                pthread_mutex_lock(&m->lock); m->tracking=frame; pthread_mutex_unlock(&m->lock);
            });
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
int APC_Media_Z1Mini::init(const struct ca_media_config *c)
{
    if (!c || !c->backend || strcmp(c->backend, "z1mini") ||
        !c->record_root || !c->capture_root) { errno = EINVAL; return -1; }
    const char *helper = getenv("CAMERA_APP_Z1_NATIVE_HELPER");
    bool native = helper && *helper;
    /* Do not silently advertise settings which the retained ISP never applied. */
    if (c->settings.main_resolution != CA_VIDEO_1080P ||
        c->settings.sub_resolution != CA_VIDEO_1080P ||
        (c->settings.recording_resolution != CA_VIDEO_1080P &&
         !(native && c->settings.recording_resolution == CA_VIDEO_2160P)) ||
        c->settings.main_codec != CA_VIDEO_H264 || c->settings.sub_codec != CA_VIDEO_H264) {
        ca_log("Z1 media requires 1080p H264 streams and 1080p recording (or 4K with native capture)");
        errno = ENOTSUP; return -1;
    }
    if (c->settings.orientation == CA_MOUNT_INVERTED && !native) {
        ca_log("Z1 inverted mounting requires the native capture helper");
        errno = ENOTSUP; return -1;
    }
    struct APC_Media_Z1Mini_State *m = new (std::nothrow) APC_Media_Z1Mini_State{};
    if (!m) { errno = ENOMEM; return -1; }
    _state = m;
    m->owner = this;
    m->config = *c;
    pthread_mutex_init(&m->lock, NULL);
    int error;
    if (ca_rtsp_open(&m->rtsp, c->rtsp_port, "video1", CA_VIDEO_H264, 30) < 0 ||
        ca_rtsp_add_video(m->rtsp, "video2", CA_VIDEO_H264, 30, &m->sub) < 0 ||
        ca_live_video_server_open(&m->live, c->rtsp_port + 1U) < 0 ||
        ca_live_video_server_configure(m->live, 0, 1920, 1080, 30, true) < 0 ||
        ca_live_video_server_configure(m->live, 1, 1920, 1080, 30, true) < 0) goto fail;
    ca_rtsp_add_config_aliases(m->rtsp, &c->settings);
    if (ca_rtsp_support_proxy(m->rtsp, &c->settings.support) < 0)
        ca_log("Z1 SupportProxy startup failed: %s", strerror(errno));
    error = pthread_create(&m->thread, NULL, receiver, m);
    if (error) { errno = error; goto fail; }
    m->started = true;
    for (unsigned i = 0; i < 150 && !atomic_load(&m->ready); i++) usleep(100000);
    if (!atomic_load(&m->ready)) { errno = ETIMEDOUT; goto fail; }
    ca_log("Z1 media ready: %s -> 1080p live H264, %s recording",
           native ? "native AX ISP" : "retained AX ISP",
           c->settings.recording_resolution == CA_VIDEO_2160P ? "4K" : "1080p");
     return 0;
fail:;
    int saved = errno; m->owner->shutdown(); errno = saved; return -1;
}
int APC_Media_Z1Mini::set_recording(bool active)
{
    auto *m = _state;
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
        int n = snprintf(m->path, sizeof(m->path), "%s/Z1_%lld_%09ld.%s",
                         m->config.record_root, (long long)now.tv_sec, now.tv_nsec, ca_video_recording_extension());
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
bool APC_Media_Z1Mini::ready() const
{
    const auto *m = _state; return m && atomic_load(&m->ready); }
bool APC_Media_Z1Mini::recording() const
{
    const auto *m = _state; return m && atomic_load(&m->recording); }
const char * APC_Media_Z1Mini::recording_path() const
{
    const auto *m = _state; return m ? m->path : ""; }
static int unsupported(void) { errno = ENOTSUP; return -1; }
int APC_Media_Z1Mini::set_zoom(float z)
{
    auto *m = _state; (void)m; return z == 1 ? 0 : unsupported(); }
float APC_Media_Z1Mini::zoom() const
{
    const auto *m = _state; (void)m; return 1; }
float APC_Media_Z1Mini::hfov(bool thermal) const
{
    const auto *m = _state; (void)m; return thermal ? NAN : Z1_HFOV_DEG; }
int APC_Media_Z1Mini::set_lens(enum ca_media_lens l)
{
    auto *m = _state; (void)m; return l == CA_MEDIA_LENS_WIDE ? 0 : unsupported(); }
enum ca_media_lens APC_Media_Z1Mini::lens() const
{
    const auto *m = _state; (void)m; return CA_MEDIA_LENS_WIDE; }
int APC_Media_Z1Mini::set_thermal_main(bool t)
{
    auto *m = _state; (void)m; return t ? unsupported() : 0; }
bool APC_Media_Z1Mini::thermal_main() const
{
    const auto *m = _state; (void)m; return false; }
int APC_Media_Z1Mini::autofocus(uint16_t x, uint16_t y)
{
    auto *m = _state; (void)m; (void)x; (void)y; return unsupported(); }
int APC_Media_Z1Mini::manual_focus(int d)
{
    auto *m = _state; (void)m; (void)d; return unsupported(); }
int APC_Media_Z1Mini::set_focus_percent(float p)
{
    auto *m = _state; (void)m; (void)p; return unsupported(); }
bool APC_Media_Z1Mini::thermal_range(struct ca_thermal_range *r)
{
    auto *m = _state; (void)m; (void)r; return false; }
int APC_Media_Z1Mini::capture_photo(enum ca_photo_scope s)
{
    auto *m = _state; (void)m; (void)s; return unsupported(); }
int APC_Media_Z1Mini::get_thermal_gain(uint8_t *g)
{
    auto *m = _state; (void)m; (void)g; return unsupported(); }
int APC_Media_Z1Mini::set_thermal_gain(uint8_t g)
{
    auto *m = _state; (void)m; (void)g; return unsupported(); }
int APC_Media_Z1Mini::get_thermal_palette(uint8_t *p)
{
    auto *m = _state; (void)m; (void)p; return unsupported(); }
int APC_Media_Z1Mini::set_thermal_palette(uint8_t p)
{
    auto *m = _state; (void)m; (void)p; return unsupported(); }
int APC_Media_Z1Mini::set_inverted(bool inverted)
{
    auto *m = _state;
    /* Applied by the helper before the first frame. MOUNT_ORIENT requires an
     * app restart; reapplying the active value during media reopen is harmless. */
    return inverted == (m->config.settings.orientation == CA_MOUNT_INVERTED) ? 0 : unsupported();
}
void APC_Media_Z1Mini::shutdown()
{
    auto *m = _state;
    if (!m) return;
    atomic_store(&m->stop, true);
    if (m->started) pthread_join(m->thread, NULL);
    if (m->mp4) (void)ca_mp4_close(m->mp4);
    ca_live_video_server_close(m->live); ca_rtsp_close(m->rtsp);
    pthread_mutex_destroy(&m->lock); _state = nullptr; delete m;
}

unsigned APC_Media_Z1Mini::frame_rate(bool thermal) const
{
    const auto *media = _state;
    (void)media;
    return thermal ? APCAM_THERMAL_FRAME_RATE : APCAM_FRAME_RATE;
}

int APC_Media_Z1Mini::apply_image(const struct ca_config *settings)
{
    auto *media = _state;
    (void)media; (void)settings;
    errno = ENOTSUP; return -1;
}

int APC_Media_Z1Mini::exposure(unsigned lens, struct ca_exposure *s)
{
    auto *m = _state;
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

int APC_Media_Z1Mini::apply_overlay(const struct ca_config *settings)
{
    auto *m = _state;
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

std::unique_ptr<APC_Media_Backend> APC_Media_Backend::create(const ca_media_config &config)
{
    return APC_Media_Z1Mini::create(config);
}
