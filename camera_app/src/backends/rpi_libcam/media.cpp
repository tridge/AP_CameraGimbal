#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <new>
#include "pipeline.h"
#include "jpeg.h"
#include "apcam/atomic.h"
#include "apcam/target.h"
#include "camera_app/APC_Media_Backend.h"
#include "camera_app/live_video_server.h"
#include "camera_app/rtsp.h"
#include "camera_app/mp4.h"
#include "camera_app/video_metadata.h"
#include "camera_app/log.h"
#include "camera_app/still.h"
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

#define RPI_HFOV_DEG APCAM_LENS1_FOV_H

class APC_Media_RPiLibcam;
struct APC_Media_RPiLibcam_State {
    struct ca_media_config config;
    atomic_bool ready, recording;
    unsigned width, height; /* the main resolution; recordings use it too */
    bool wait_key, have_pts;
    pthread_mutex_t lock;
    struct ca_rtsp *rtsp;
    struct ca_live_video_server *live;
    struct ca_mp4 *mp4;
    struct ca_rpi_pipeline *pipeline;
    char path[PATH_MAX];
    uint64_t pts_offset, last_pts;
    struct ca_exposure exposure;
    pthread_mutex_t photo_lock;
    atomic_int photo_resolution; /* enum ca_photo_resolution, from PHOTO_RES */
};

#define RPI_JPEG_QUALITY 90
#define RPI_STILL_TIMEOUT_MS 1000U
/* A sensor mode switch normally captures within 400 ms (configuration and a
 * few settling frames at the slower full-resolution rate). The main loop
 * waits for it, so keep the failure case short. */
#define RPI_MODE_SWITCH_TIMEOUT_MS 1500U

// One H.264 stream from libcamera and the Pi 4 / CM4 encoder, at the main
// resolution, serves the RTSP path, the web live view and recording. The
// pipeline's encoder thread is joined before any of those outputs are released.
class APC_Media_RPiLibcam final : public APC_Media_Backend {
public:
    ~APC_Media_RPiLibcam() override { shutdown(); }
    static std::unique_ptr<APC_Media_Backend> create(const ca_media_config &config)
    {
        auto *driver = new (std::nothrow) APC_Media_RPiLibcam();
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
    APC_Media_RPiLibcam() = default;
    int init(const ca_media_config *config);
    void shutdown();
    APC_Media_RPiLibcam_State *_state = nullptr;
};

static uint64_t monotonic_us(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void consume(void *opaque, const uint8_t *data, size_t n, uint64_t pts, bool key)
{
    auto *m = static_cast<APC_Media_RPiLibcam_State *>(opaque);
    if (!m->have_pts) { m->pts_offset = monotonic_us() - pts; m->have_pts = true; }
    pts += m->pts_offset;
    if (pts <= m->last_pts) pts = m->last_pts + 1;
    m->last_pts = pts;
    (void)ca_rtsp_push_video_timed(m->rtsp, 0, data, n, key, RPI_HFOV_DEG, pts);
    (void)ca_live_video_server_publish(m->live, 0, data, n, pts, key, RPI_HFOV_DEG);
    pthread_mutex_lock(&m->lock);
    if (m->mp4 && key) m->wait_key = false;
    if (m->mp4 && !m->wait_key && ca_mp4_write_h264(m->mp4, data, n, pts, key, RPI_HFOV_DEG) < 0) {
        ca_log("RPi recording write failed: %s", strerror(errno));
        (void)ca_mp4_close(m->mp4); m->mp4 = NULL;
        atomic_store(&m->recording, false);
    }
    pthread_mutex_unlock(&m->lock);
    atomic_store(&m->ready, true);
}

static void consume_exposure(void *opaque, const struct ca_exposure *sample)
{
    auto *m = static_cast<APC_Media_RPiLibcam_State *>(opaque);
    pthread_mutex_lock(&m->lock);
    m->exposure = *sample;
    pthread_mutex_unlock(&m->lock);
}

int APC_Media_RPiLibcam::init(const struct ca_media_config *c)
{
    if (!c || !c->backend || strcmp(c->backend, APCAM_NAME) ||
        !c->record_root || !c->capture_root) { errno = EINVAL; return -1; }
    if (c->settings.main_codec != CA_VIDEO_H264) {
        ca_log("RPi media requires an H264 stream");
        errno = ENOTSUP; return -1;
    }
    auto *m = new (std::nothrow) APC_Media_RPiLibcam_State{};
    if (!m) { errno = ENOMEM; return -1; }
    _state = m;
    m->config = *c;
    ca_video_resolution_size(c->settings.main_resolution, &m->width, &m->height);
    pthread_mutex_init(&m->lock, NULL);
    pthread_mutex_init(&m->photo_lock, NULL);
    atomic_init(&m->photo_resolution, (int)c->settings.photo_resolution);
    /* Read the running settings, not the startup values in c: this also runs when
     * the resolution changes, and a bitrate changed since startup must stay. */
    const unsigned fps = c->settings.main_frame_rate;
    m->config.frame_rate = fps;
    struct ca_rpi_pipeline_config pipeline = {
        .width = m->width, .height = m->height, .frame_rate = fps,
        .bit_rate_kbps = c->settings.main_bitrate_kbps,
        .inverted = c->settings.orientation == CA_MOUNT_INVERTED,
        .image = c->settings,
        .frame = consume, .exposure = consume_exposure, .opaque = m,
    };
    if (ca_rtsp_open(&m->rtsp, c->rtsp_port, "video1", CA_VIDEO_H264, fps) < 0 ||
        ca_live_video_server_open(&m->live, c->rtsp_port + 1U) < 0 ||
        ca_live_video_server_configure(m->live, 0, m->width, m->height, fps, true) < 0) goto fail;
    ca_rtsp_add_config_aliases(m->rtsp, &c->settings);
    if (ca_rtsp_support_proxy(m->rtsp, &c->settings.support) < 0)
        ca_log("RPi SupportProxy startup failed: %s", strerror(errno));
    if (ca_rpi_pipeline_open(&m->pipeline, &pipeline) < 0) goto fail;
    for (unsigned i = 0; i < 50 && !atomic_load(&m->ready); i++) usleep(100000);
    if (!atomic_load(&m->ready)) { ca_log("RPi no encoded video within 5s"); errno = ETIMEDOUT; goto fail; }
    ca_log("RPi media ready: libcamera -> %ux%u@%u H264", m->width, m->height, fps);
    return 0;
fail:;
    int saved = errno; shutdown(); errno = saved; return -1;
}

int APC_Media_RPiLibcam::set_recording(bool active)
{
    auto *m = _state;
    if (!m) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&m->lock);
    int result = 0;
    if (active == atomic_load(&m->recording)) goto done;
    if (active) {
        if (!atomic_load(&m->ready)) { errno = EAGAIN; result = -1; goto done; }
        struct stat st;
        if (stat(m->config.record_root, &st) || !S_ISDIR(st.st_mode)) { result = -1; goto done; }
        struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
        int n = snprintf(m->path, sizeof(m->path), "%s/RPI_%lld_%09ld.%s",
                         m->config.record_root, (long long)now.tv_sec, now.tv_nsec, ca_video_recording_extension());
        if (n < 0 || n >= (int)sizeof(m->path)) { errno = ENAMETOOLONG; result = -1; goto done; }
        if (ca_mp4_open(&m->mp4, m->path, m->width, m->height, frame_rate(false)) < 0) { result = -1; goto done; }
        m->wait_key = true;
        atomic_store(&m->recording, true);
    } else {
        result = ca_mp4_close(m->mp4); m->mp4 = NULL;
        atomic_store(&m->recording, false);
    }
done:
    pthread_mutex_unlock(&m->lock); return result;
}

bool APC_Media_RPiLibcam::ready() const
{
    const auto *m = _state; return m && atomic_load(&m->ready); }
bool APC_Media_RPiLibcam::recording() const
{
    const auto *m = _state; return m && atomic_load(&m->recording); }
const char * APC_Media_RPiLibcam::recording_path() const
{
    const auto *m = _state; return m ? m->path : ""; }
static int unsupported(void) { errno = ENOTSUP; return -1; }
int APC_Media_RPiLibcam::set_zoom(float z) { return z == 1 ? 0 : unsupported(); }
float APC_Media_RPiLibcam::zoom() const { return 1; }
float APC_Media_RPiLibcam::hfov(bool thermal) const { return thermal ? NAN : RPI_HFOV_DEG; }
unsigned APC_Media_RPiLibcam::frame_rate(bool thermal) const
{
    const auto *m = _state;
    if (thermal) return 0;
    return m && m->config.frame_rate ? m->config.frame_rate : APCAM_FRAME_RATE;
}
int APC_Media_RPiLibcam::set_lens(enum ca_media_lens l) { return l == CA_MEDIA_LENS_WIDE ? 0 : unsupported(); }
enum ca_media_lens APC_Media_RPiLibcam::lens() const { return CA_MEDIA_LENS_WIDE; }
int APC_Media_RPiLibcam::set_thermal_main(bool t) { return t ? unsupported() : 0; }
bool APC_Media_RPiLibcam::thermal_main() const { return false; }
int APC_Media_RPiLibcam::autofocus(uint16_t x, uint16_t y) { (void)x; (void)y; return unsupported(); }
int APC_Media_RPiLibcam::manual_focus(int d) { (void)d; return unsupported(); }
int APC_Media_RPiLibcam::set_focus_percent(float p) { (void)p; return unsupported(); }
bool APC_Media_RPiLibcam::thermal_range(struct ca_thermal_range *r) { (void)r; return false; }
/* PHOTO_RES "video" copies the next video frame: no interruption, at the
 * video resolution. Larger sizes switch the sensor mode, pausing video and
 * recording for about a second. */
int APC_Media_RPiLibcam::capture_photo(enum ca_photo_scope scope)
{
    auto *m = _state;
    if (!m || !m->pipeline || (scope != CA_PHOTO_SCOPE_ALL && scope != CA_PHOTO_SCOPE_THERMAL)) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&m->photo_lock);
    struct timespec captured_at;
    struct ca_rpi_still_frame frame;
    uint8_t *jpeg = NULL;
    size_t length = 0;
    char path[PATH_MAX];
    int result = clock_gettime(CLOCK_REALTIME, &captured_at);
    const int resolution = atomic_load(&m->photo_resolution);
    if (result == 0 && resolution == CA_PHOTO_RES_BINNED)
        result = ca_rpi_pipeline_capture_still(m->pipeline, APCAM_PHOTO_BINNED_WIDTH, APCAM_PHOTO_BINNED_HEIGHT,
                                               &frame, RPI_MODE_SWITCH_TIMEOUT_MS);
    else if (result == 0 && resolution == CA_PHOTO_RES_FULL)
        result = ca_rpi_pipeline_capture_still(m->pipeline, APCAM_PHOTO_FULL_WIDTH, APCAM_PHOTO_FULL_HEIGHT,
                                               &frame, RPI_MODE_SWITCH_TIMEOUT_MS);
    else if (result == 0)
        result = ca_rpi_pipeline_grab_still(m->pipeline, &frame, RPI_STILL_TIMEOUT_MS);
    if (result == 0) {
        if (!frame.jpeg_colour) ca_rpi_jpeg_convert(&frame);
        result = ca_rpi_jpeg_encode(&frame, RPI_JPEG_QUALITY, &jpeg, &length);
        free(frame.data);
    }
    if (result == 0) {
        result = ca_still_write_jpeg(m->config.capture_root, 'C', jpeg, length, &captured_at,
                                     path, sizeof(path));
        if (result == 0) ca_log("RPi JPEG capture saved: %s (%zu bytes)", path, length);
        else ca_log("RPi JPEG write failed: %s", strerror(errno));
    } else {
        ca_log("RPi JPEG capture failed: %s", strerror(errno));
    }
    free(jpeg);
    pthread_mutex_unlock(&m->photo_lock);
    return result;
}
int APC_Media_RPiLibcam::get_thermal_gain(uint8_t *g) { (void)g; return unsupported(); }
int APC_Media_RPiLibcam::set_thermal_gain(uint8_t g) { (void)g; return unsupported(); }
int APC_Media_RPiLibcam::get_thermal_palette(uint8_t *p) { (void)p; return unsupported(); }
int APC_Media_RPiLibcam::set_thermal_palette(uint8_t p) { (void)p; return unsupported(); }
int APC_Media_RPiLibcam::set_inverted(bool inverted)
{
    auto *m = _state;
    /* Applied when the camera is configured; MOUNT_ORIENT requires a restart. */
    return inverted == (m->config.settings.orientation == CA_MOUNT_INVERTED) ? 0 : unsupported();
}

int APC_Media_RPiLibcam::exposure(unsigned lens, struct ca_exposure *s)
{
    auto *m = _state;
    if (lens) return -ENOTSUP;
    pthread_mutex_lock(&m->lock);
    struct ca_exposure cached = m->exposure;
    pthread_mutex_unlock(&m->lock);
    if (!cached.time_us) return -ENODATA;
    uint64_t now = monotonic_us();
    if (now < cached.time_us || now - cached.time_us > 1000000U) return -ETIMEDOUT;
    *s = cached;
    return s->result;
}

int APC_Media_RPiLibcam::apply_overlay(const struct ca_config *settings)
{
    static atomic_bool warned;
    if (settings->osd_cross && !atomic_exchange(&warned, true))
        ca_log("OSD_CROSS is configured but overlays are not yet supported on Raspberry Pi");
    return 0;
}

int APC_Media_RPiLibcam::apply_image(const struct ca_config *settings)
{
    auto *m = _state;
    if (ca_rpi_pipeline_set_image(m->pipeline, settings) < 0) return -1;
#if APCAM_HAVE_VIDEO_BITRATE
    if (ca_rpi_pipeline_set_bitrate(m->pipeline, settings->main_bitrate_kbps) < 0) return -1;
#endif
    atomic_store(&m->photo_resolution, (int)settings->photo_resolution);
    ca_log("RPi image brightness=%d saturation=%d contrast=%d ev=%d iso=%d shutter=%d "
           "metering=%d white_balance=%d", settings->brightness, settings->saturation,
           settings->contrast, settings->exposure_compensation, settings->iso,
           settings->shutter, settings->metering, settings->white_balance);
    return 0;
}

void APC_Media_RPiLibcam::shutdown()
{
    auto *m = _state;
    if (!m) return;
    ca_rpi_pipeline_close(m->pipeline);
    if (m->mp4) (void)ca_mp4_close(m->mp4);
    ca_live_video_server_close(m->live); ca_rtsp_close(m->rtsp);
    pthread_mutex_destroy(&m->photo_lock);
    pthread_mutex_destroy(&m->lock); _state = nullptr; delete m;
}

std::unique_ptr<APC_Media_Backend> APC_Media_Backend::create(const ca_media_config &config)
{
    return APC_Media_RPiLibcam::create(config);
}
