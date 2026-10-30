#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <new>
#include "pipeline.h"
#include "camera_app/overlay.h"

#include "camera_app/live_video_server.h"
#include "camera_app/log.h"
#include "camera_app/APC_Media_Backend.h"
#include "camera_app/sigmastar_exposure.h"
#include <dlfcn.h>
#include "camera_app/zr10_fov.h"
#include "camera_app/mp4.h"
#include "camera_app/video_metadata.h"
#include "camera_app/rtsp.h"
#include "camera_app/still.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include "apcam/atomic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CA_ZR10_LIVE_VIDEO_PORT 8555U
#define CA_ZR10_JPEG_QUALITY 90U
#define CA_ZR10_ISP_BIN "/customer/day_gc4663-2-aec-AWB.bin"
/* frames seen on the main channel before the ISP tuning file is loaded;
 * loading it earlier blocks inside the vendor library */
#define CA_ZR10_ISP_BIN_AFTER_FRAMES 10U

class APC_Media_ZR10;
struct APC_Media_ZR10_State {
    struct ca_overlay_hw *overlay;
    ca_tracking_status tracking;
    struct ca_media_config config;
    struct ca_zr10_pipeline_config pipeline;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_mutex_t photo_lock;
    bool pipeline_open;
    bool thread_started;
    atomic_bool stop;
    atomic_bool recording;
    bool recording_wait_keyframe;
    struct ca_rtsp *rtsp;
    unsigned sub_rtsp_stream;
    uint64_t last_pts[2];
    struct ca_live_video_server *live_video;
    struct ca_mp4 *mp4;
    char recording_path[PATH_MAX];
    atomic_bool encoded_frame_logged[CA_ZR10_VENC_COUNT];
    unsigned main_frames;
    atomic_bool isp_bin_loaded;
    APC_ATOMIC(float) zoom;
    APC_ATOMIC(float) visible_hfov_deg;
    uint8_t thermal_gain;
    uint8_t thermal_palette;
    atomic_bool inverted;

    APC_Media_ZR10 *owner = nullptr;
};

// Driver state is private to this translation unit. The owner survives all
// capture callbacks and joins them before releasing SDK resources.
class APC_Media_ZR10 final : public APC_Media_Backend {
public:
    ~APC_Media_ZR10() override { shutdown(); }
    static std::unique_ptr<APC_Media_Backend> create(const ca_media_config &config)
    {
        auto *driver = new (std::nothrow) APC_Media_ZR10();
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
    bool tracking_available() const override { return ca_zr10_tracking_available(); }
    bool tracking_frame(ca_tracking_frame &frame) override {
        pthread_mutex_lock(&_state->lock);
        const bool ok=ca_zr10_tracking_frame(frame);
        pthread_mutex_unlock(&_state->lock);
        return ok;
    }
    void tracking_overlay(const ca_tracking_status *status) override {
        _state->tracking=status?*status:ca_tracking_status{};
        (void)apply_overlay(&_state->config.settings);
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
    APC_Media_ZR10() = default;
    int init(const ca_media_config *config);
    void shutdown();
    APC_Media_ZR10_State *_state = nullptr;
};


static bool video_key_frame(enum ca_video_codec codec, const uint8_t *data,
                            size_t length)
{
    for (size_t i = 0; i + 4U < length; i++) {
        size_t start = 0;
        if (data[i] == 0U && data[i + 1U] == 0U && data[i + 2U] == 1U) {
            start = i + 3U;
        } else if (data[i] == 0U && data[i + 1U] == 0U &&
                   data[i + 2U] == 0U && data[i + 3U] == 1U) {
            start = i + 4U;
        }
        if (start != 0U && start < length) {
            unsigned type = codec == CA_VIDEO_H265
                                ? (data[start] >> 1U) & 0x3fU
                                : data[start] & 0x1fU;
            if ((codec == CA_VIDEO_H264 && type == 5U) ||
                (codec == CA_VIDEO_H265 && type >= 19U && type <= 21U)) {
                return true;
            }
        }
    }
    return false;
}

static unsigned stream_bitrate(unsigned width, unsigned height)
{
    if (width >= 2560U || height >= 1440U) return 12000U;
    if (width >= 1920U || height >= 1080U) return 4096U;
    return 2048U;
}

static void consume_frame(struct APC_Media_ZR10_State *media, unsigned channel,
                          uint8_t *data, size_t length, uint64_t pts)
{
    enum ca_video_codec codec = media->pipeline.streams[channel].codec;
    bool key_frame = video_key_frame(codec, data, length);
    float hfov_deg = media->owner->hfov(false);

    if (!atomic_exchange(&media->encoded_frame_logged[channel], true)) {
        ca_log("VENC channel %u first encoded frame bytes=%zu key=%u", channel,
               length, key_frame ? 1U : 0U);
    }
    if (channel == CA_ZR10_MAIN_VENC || channel == CA_ZR10_SUB_VENC) {
        unsigned stream = channel == CA_ZR10_MAIN_VENC ? 0U
                                                     : media->sub_rtsp_stream;
        if (pts <= media->last_pts[channel]) pts = media->last_pts[channel] + 1;
        media->last_pts[channel] = pts;
        (void)ca_rtsp_push_video_timed(media->rtsp, stream, data, length, key_frame, hfov_deg, pts);
        (void)ca_live_video_server_publish(media->live_video, channel, data,
                                            length, pts, key_frame, hfov_deg);
        if (channel == CA_ZR10_MAIN_VENC && !media->isp_bin_loaded &&
            ++media->main_frames >= CA_ZR10_ISP_BIN_AFTER_FRAMES) {
            if (ca_zr10_load_isp_bin(CA_ZR10_ISP_BIN) == 0) {
                ca_log("ISP tuning loaded from %s", CA_ZR10_ISP_BIN);
                atomic_store(&media->isp_bin_loaded, true);
            } else {
                ca_log("factory ISP tuning failed");
                atomic_store(&media->stop, true);
            }
            /* Factory IQ is retained; manual image controls need SDK validation. */
        }
        if(channel==CA_ZR10_MAIN_VENC && media->isp_bin_loaded)ca_zr10_sample_focus();
        return;
    }
    pthread_mutex_lock(&media->lock);
    if (atomic_load(&media->recording) && media->recording_wait_keyframe &&
        key_frame) {
        media->recording_wait_keyframe = false;
    }
    if (atomic_load(&media->recording) && !media->recording_wait_keyframe &&
        ca_mp4_write_h264(media->mp4, data, length, pts, key_frame, hfov_deg) < 0) {
        ca_log("recording stopped after MP4 write failure: %s",
               strerror(errno));
        (void)ca_mp4_close(media->mp4);
        media->mp4 = NULL;
        atomic_store(&media->recording, false);
    }
    pthread_mutex_unlock(&media->lock);
}

static void *capture_thread(void *opaque)
{
    struct APC_Media_ZR10_State *media = (struct APC_Media_ZR10_State*)(opaque);
    struct pollfd items[CA_ZR10_VENC_COUNT];

    for (unsigned channel = 0; channel < CA_ZR10_VENC_COUNT; channel++) {
        items[channel].fd = ca_zr10_venc_fd(channel);
        items[channel].events = POLLIN;
    }
    while (!atomic_load(&media->stop)) {
        int ready = poll(items, CA_ZR10_VENC_COUNT, 100);
        if (ready < 0) {
            if (errno == EINTR) continue;
            ca_log("encoder poll failed: %s", strerror(errno));
            break;
        }
        for (unsigned channel = 0; channel < CA_ZR10_VENC_COUNT; channel++) {
            uint8_t *frame = NULL;
            size_t length = 0;
            uint64_t pts = 0;
            if ((items[channel].revents & POLLIN) == 0) continue;
            if (ca_zr10_venc_get(channel, &frame, &length, &pts) == 1) {
                consume_frame(media, channel, frame, length, pts);
                free(frame);
            }
        }
    }
    return NULL;
}

static int make_recording_path(struct APC_Media_ZR10_State *media)
{
    time_t now = time(NULL);
    struct tm local;
    char directory[PATH_MAX];
    char day[16];

    localtime_r(&now, &local);
    strftime(day, sizeof(day), "%Y-%m-%d", &local);
    if (mkdir(media->config.record_root, 0700) < 0 && errno != EEXIST) return -1;
    if (snprintf(directory, sizeof(directory), "%s/%s",
                 media->config.record_root, day) >= (int)sizeof(directory)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) return -1;
    for (unsigned suffix = 0; suffix < 1000U; suffix++) {
        int length = snprintf(media->recording_path,
                              sizeof(media->recording_path),
                              "%s/%s_%lld_A%s.%s", directory, day,
                              (long long)now, suffix == 0U ? "" : "_next", ca_video_recording_extension());
        if (length < 0 || (size_t)length >= sizeof(media->recording_path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (access(media->recording_path, F_OK) < 0 && errno == ENOENT) {
            return 0;
        }
        now++;
    }
    errno = EEXIST;
    return -1;
}

static void media_cleanup(struct APC_Media_ZR10_State *media)
{
    if (media->thread_started) {
        atomic_store(&media->stop, true);
        pthread_join(media->thread, NULL);
        media->thread_started = false;
    }
    if (media->mp4 != NULL) {
        (void)ca_mp4_close(media->mp4);
        media->mp4 = NULL;
    }
    ca_live_video_server_close(media->live_video);
    media->live_video = NULL;
    ca_rtsp_close(media->rtsp);
    media->rtsp = NULL;
    ca_overlay_hw_close(media->overlay);
    media->overlay=NULL;
    if (media->pipeline_open) {
        ca_zr10_pipeline_close();
        media->pipeline_open = false;
    }
}

int APC_Media_ZR10::init(const struct ca_media_config *config)
{
    struct APC_Media_ZR10_State *media;
    struct ca_zr10_stream *streams;
    const char *stage = "argument validation";

    if (config == NULL || config->backend == NULL ||
        strcmp(config->backend, "zr10") != 0 || config->capture_root == NULL ||
        config->record_root == NULL) {
        errno = EINVAL;
        return -1;
    }
    bool frames_ready;
    media = new (std::nothrow) APC_Media_ZR10_State{};
    if (media == NULL) { errno = ENOMEM; return -1; }
    _state = media;
    media->owner = this;
    media->config = *config;
    media->zoom = 1.0f;
    atomic_store(&media->visible_hfov_deg, ca_zr10_hfov(media->zoom));
    media->thermal_gain = 1U;
    media->thermal_palette = (uint8_t)config->settings.thermal_palette;
    pthread_mutex_init(&media->lock, NULL);
    pthread_mutex_init(&media->photo_lock, NULL);
    streams = media->pipeline.streams;
    ca_video_resolution_size(config->settings.main_resolution,
                             &streams[CA_ZR10_MAIN_VENC].width,
                             &streams[CA_ZR10_MAIN_VENC].height);
    streams[CA_ZR10_MAIN_VENC].codec = config->settings.main_codec;
    ca_video_resolution_size(config->settings.sub_resolution,
                             &streams[CA_ZR10_SUB_VENC].width,
                             &streams[CA_ZR10_SUB_VENC].height);
    streams[CA_ZR10_SUB_VENC].codec = config->settings.sub_codec;
    ca_video_resolution_size(config->settings.recording_resolution,
                             &streams[CA_ZR10_RECORD_VENC].width,
                             &streams[CA_ZR10_RECORD_VENC].height);
    streams[CA_ZR10_RECORD_VENC].codec = CA_VIDEO_H264;
    for (unsigned i = 0; i < CA_ZR10_VENC_COUNT; i++) {
        streams[i].bit_rate_kbps = stream_bitrate(streams[i].width,
                                                  streams[i].height);
    }
    media->pipeline.jpeg_quality = CA_ZR10_JPEG_QUALITY;
    stage = "MI pipeline startup";
    if (ca_zr10_pipeline_open(&media->pipeline) < 0) goto fail;
    media->pipeline_open = true;
    if (config->settings.orientation == CA_MOUNT_INVERTED) {
        stage = "inverted output configuration";
        if (media->owner->set_inverted(true) < 0) goto fail;
    }
    stage = "RTSP startup";
    if (ca_rtsp_open(&media->rtsp, config->rtsp_port, "video1",
                     streams[CA_ZR10_MAIN_VENC].codec, CA_ZR10_FRAME_RATE) < 0 ||
        ca_rtsp_add_video(media->rtsp, "video2", streams[CA_ZR10_SUB_VENC].codec,
                          CA_ZR10_FRAME_RATE, &media->sub_rtsp_stream) < 0) {
        goto fail;
    }
    ca_rtsp_add_config_aliases(media->rtsp, &config->settings);
    if (ca_rtsp_support_proxy(media->rtsp, &config->settings.support) < 0)
        ca_log("SupportProxy video startup failed: %s", strerror(errno));
    stage = "native live-video startup";
    if (ca_live_video_server_open(&media->live_video,
                                  CA_ZR10_LIVE_VIDEO_PORT) < 0 ||
        ca_live_video_server_configure(
            media->live_video, 0U, streams[CA_ZR10_MAIN_VENC].width,
            streams[CA_ZR10_MAIN_VENC].height, CA_ZR10_FRAME_RATE,
            streams[CA_ZR10_MAIN_VENC].codec == CA_VIDEO_H264) < 0 ||
        ca_live_video_server_configure(
            media->live_video, 1U, streams[CA_ZR10_SUB_VENC].width,
            streams[CA_ZR10_SUB_VENC].height, CA_ZR10_FRAME_RATE,
            streams[CA_ZR10_SUB_VENC].codec == CA_VIDEO_H264) < 0) {
        goto fail;
    }
    stage = "capture-thread startup";
    if (pthread_create(&media->thread, NULL, capture_thread, media) != 0) goto fail;
    media->thread_started = true;
    stage = "first frames and factory ISP tuning";
    frames_ready=false;
    for (unsigned attempt=0;attempt<120U;attempt++) {
        frames_ready=atomic_load(&media->isp_bin_loaded);
        for (unsigned ch=0;ch<CA_ZR10_VENC_COUNT;ch++)
            if(!atomic_load(&media->encoded_frame_logged[ch]))frames_ready=false;
        if(frames_ready || atomic_load(&media->stop))break;
        usleep(100000);
    }
    if(!frames_ready) { errno=atomic_load(&media->stop) ? EIO:ETIMEDOUT;goto fail; }
    (void)ca_zr10_venc_request_idr(CA_ZR10_MAIN_VENC);
    (void)ca_zr10_venc_request_idr(CA_ZR10_SUB_VENC);
    ca_log("media ready: /video1 %ux%u/%u %s; /video2 %ux%u/%u %s; "
           "recording %ux%u H.264; RTSP port %u",
           streams[CA_ZR10_MAIN_VENC].width, streams[CA_ZR10_MAIN_VENC].height,
           CA_ZR10_FRAME_RATE, ca_video_codec_name(streams[CA_ZR10_MAIN_VENC].codec),
           streams[CA_ZR10_SUB_VENC].width, streams[CA_ZR10_SUB_VENC].height,
           CA_ZR10_FRAME_RATE, ca_video_codec_name(streams[CA_ZR10_SUB_VENC].codec),
           streams[CA_ZR10_RECORD_VENC].width, streams[CA_ZR10_RECORD_VENC].height,
           config->rtsp_port);

    return 0;
fail:
    {
        int saved_errno = errno;
        ca_log("media startup failed at %s: %s", stage, strerror(saved_errno));
        media_cleanup(media);
        pthread_mutex_destroy(&media->photo_lock);
        pthread_mutex_destroy(&media->lock);
        _state = nullptr; delete media;
        errno = saved_errno != 0 ? saved_errno : EIO;
    }
    return -1;
}

int APC_Media_ZR10::set_recording(bool active)
{
    auto *media = _state;
    int result = 0;

    if (media == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&media->lock);
    if (active == atomic_load(&media->recording)) {
        pthread_mutex_unlock(&media->lock);
        return 0;
    }
    if (active) {
        const struct ca_zr10_stream *stream =
            &media->pipeline.streams[CA_ZR10_RECORD_VENC];
        if (make_recording_path(media) < 0 ||
            ca_mp4_open(&media->mp4, media->recording_path, stream->width,
                        stream->height, CA_ZR10_FRAME_RATE) < 0) {
            pthread_mutex_unlock(&media->lock);
            return -1;
        }
        atomic_store(&media->recording, true);
        media->recording_wait_keyframe = true;
        (void)ca_zr10_venc_request_idr(CA_ZR10_RECORD_VENC);
        ca_log("recording started %s", media->recording_path);
    } else {
        if (ca_mp4_close(media->mp4) < 0) result = -1;
        media->mp4 = NULL;
        atomic_store(&media->recording, false);
        media->recording_wait_keyframe = false;
        ca_log("recording stopped %s", media->recording_path);
    }
    pthread_mutex_unlock(&media->lock);
    return result;
}

bool APC_Media_ZR10::recording() const
{
    const auto *media = _state;
    return media != NULL && atomic_load(&media->recording);
}

const char * APC_Media_ZR10::recording_path() const
{
    const auto *media = _state;
    return media != NULL ? media->recording_path : "";
}

int APC_Media_ZR10::capture_photo(enum ca_photo_scope scope)
{
    auto *media = _state;
    uint8_t *jpeg = NULL;
    size_t length = 0;
    struct timespec captured_at;
    char path[PATH_MAX];
    int result;

    if (media == NULL || (scope != CA_PHOTO_SCOPE_THERMAL &&
                          scope != CA_PHOTO_SCOPE_ALL)) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&media->photo_lock);
    if (clock_gettime(CLOCK_REALTIME, &captured_at) < 0) {
        pthread_mutex_unlock(&media->photo_lock);
        return -1;
    }
    result = ca_zr10_capture_jpeg(&jpeg, &length);
    if (result == 0) {
        result = ca_still_write_jpeg(media->config.capture_root, 'C', jpeg,
                                     length, &captured_at, path, sizeof(path));
        if (result == 0) {
            ca_log("C JPEG capture saved: %s (%zu bytes)", path, length);
        } else {
            ca_log("C JPEG write failed: %s", strerror(errno));
        }
    } else {
        ca_log("C JPEG capture failed: %s", strerror(errno));
    }
    free(jpeg);
    pthread_mutex_unlock(&media->photo_lock);
    return result;
}

int APC_Media_ZR10::set_zoom(float zoom)
{
    auto *media = _state;
    if (media == NULL || !isfinite(zoom) || zoom < 1.0f || zoom > CA_ZR10_MAX_ZOOM) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&media->lock);
    media->zoom = zoom;
    atomic_store(&media->visible_hfov_deg, ca_zr10_hfov(zoom));
    pthread_mutex_unlock(&media->lock);
    return 0;
}

float APC_Media_ZR10::hfov(bool thermal) const
{
    const auto *media = _state;
    return media != NULL && !thermal ? atomic_load(&media->visible_hfov_deg) : 0.0f;
}

float APC_Media_ZR10::zoom() const
{
    const auto *media = _state;
    return media != NULL ? atomic_load(&media->zoom) : 1.0f;
}

int APC_Media_ZR10::set_lens(enum ca_media_lens lens)
{
    auto *media = _state;
    /* one lens; the SIYI image-slot commands still need to succeed */
    if (media == NULL || (lens != CA_MEDIA_LENS_WIDE &&
                          lens != CA_MEDIA_LENS_ZOOM)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

enum ca_media_lens APC_Media_ZR10::lens() const
{
    const auto *media = _state;
    (void)media;
    return CA_MEDIA_LENS_WIDE;
}

int APC_Media_ZR10::set_thermal_main(bool thermal_main)
{
    auto *media = _state;
    if (media == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (thermal_main) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

bool APC_Media_ZR10::thermal_main() const
{
    const auto *media = _state;
    (void)media;
    return false;
}

int APC_Media_ZR10::autofocus(uint16_t x, uint16_t y)
{
    auto *media = _state;
    (void)x;
    (void)y;
    if (media == NULL) {
        errno = EINVAL;
        return -1;
    }
    errno = ENOTSUP;
    return -1;
}

int APC_Media_ZR10::manual_focus(int direction)
{
    auto *media = _state;
    if (media == NULL || direction < -1 || direction > 1) {
        errno = EINVAL;
        return -1;
    }
    errno = ENOTSUP;
    return -1;
}

int APC_Media_ZR10::set_focus_percent(float percent)
{
    auto *media = _state;
    if (media == NULL || percent < 0.0f || percent > 100.0f) {
        errno = EINVAL;
        return -1;
    }
    errno = ENOTSUP;
    return -1;
}

bool APC_Media_ZR10::thermal_range(struct ca_thermal_range *range)
{
    auto *media = _state;
    (void)media;
    (void)range;
    return false;
}

int APC_Media_ZR10::get_thermal_gain(uint8_t *gain)
{
    auto *media = _state;
    if (media == NULL || gain == NULL) {
        errno = EINVAL;
        return -1;
    }
    *gain = media->thermal_gain;
    return 0;
}

int APC_Media_ZR10::set_thermal_gain(uint8_t gain)
{
    auto *media = _state;
    if (media == NULL || gain > 1U) {
        errno = EINVAL;
        return -1;
    }
    media->thermal_gain = gain;
    return 0;
}

int APC_Media_ZR10::get_thermal_palette(uint8_t *palette)
{
    auto *media = _state;
    if (media == NULL || palette == NULL) {
        errno = EINVAL;
        return -1;
    }
    *palette = media->thermal_palette;
    return 0;
}

int APC_Media_ZR10::set_thermal_palette(uint8_t palette)
{
    auto *media = _state;
    if (media == NULL || palette > 11U || palette == 1U) {
        errno = EINVAL;
        return -1;
    }
    media->thermal_palette = palette;
    return 0;
}

int APC_Media_ZR10::set_inverted(bool inverted)
{
    auto *media = _state;
    if (media == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (atomic_load(&media->inverted) == inverted) return 0;
    pthread_mutex_lock(&media->photo_lock);
    if (ca_zr10_set_inverted(inverted) < 0) {
        pthread_mutex_unlock(&media->photo_lock);
        ca_log("output orientation change failed: %s", strerror(errno));
        return -1;
    }
    atomic_store(&media->inverted, inverted);
    pthread_mutex_unlock(&media->photo_lock);
    ca_log("output orientation=%s", inverted ? "inverted" : "upright");
    return 0;
}

void APC_Media_ZR10::shutdown()
{
    auto *media = _state;
    if (media == NULL) return;
    media_cleanup(media);
    pthread_mutex_destroy(&media->photo_lock);
    pthread_mutex_destroy(&media->lock);
    _state = nullptr; delete media;
}

unsigned APC_Media_ZR10::frame_rate(bool thermal) const
{
    const auto *media = _state;
    (void)media;
    return thermal ? APCAM_THERMAL_FRAME_RATE : APCAM_FRAME_RATE;
}

int APC_Media_ZR10::apply_image(const struct ca_config *settings)
{
    auto *media = _state;
    (void)media; (void)settings;
    errno = ENOTSUP; return -1;
}

bool APC_Media_ZR10::ready() const
{
    const auto *media = _state; return media != NULL; }

/* Diagnostics are optional: unavailable SDK symbols must not stop capture. */
int APC_Media_ZR10::exposure(unsigned lens, struct ca_exposure *s)
{
    auto *media = _state;
    if (lens!=0) return -ENOTSUP;
    pthread_mutex_lock(&media->lock);
    if (!media->isp_bin_loaded) { pthread_mutex_unlock(&media->lock); return -EAGAIN; }
    typedef int (*query_fn)(uint32_t, void *);
    query_fn query=(query_fn)dlsym(RTLD_DEFAULT,"MI_ISP_AE_QueryExposureInfo");
    query_fn mode=(query_fn)dlsym(RTLD_DEFAULT,"MI_ISP_AE_GetExpoMode");
    /* Room for SDK extensions, while decoding only the documented prefix. */
    union { struct ca_sstar_exposure_info info; uint64_t space[512]; } q= {};
    int result=query ? query(0,&q) : -ENOTSUP;
    if (!result) ca_sstar_exposure_decode(s,&q.info);
    uint32_t value=0;
    int mode_result=mode ? mode(0,&value) : -ENOTSUP;
    if (!mode_result) ca_sstar_exposure_mode(s,value);
    pthread_mutex_unlock(&media->lock);
    return result ? result : mode_result;
}

int APC_Media_ZR10::apply_overlay(const struct ca_config *settings)
{
    auto *media = _state;
    struct ca_overlay_channel channels[3]= {};
    enum ca_video_resolution resolutions[3]={settings->main_resolution,settings->sub_resolution,settings->recording_resolution};
    pthread_mutex_lock(&media->lock);
    media->config.settings.osd_cross=settings->osd_cross;
    media->config.settings.osd_recording=settings->osd_recording;
    media->config.settings.osd_thermal_fov=settings->osd_thermal_fov;
    for (unsigned c=0;c<3;c++) {
        ca_video_resolution_size(resolutions[c],&channels[c].width,&channels[c].height);
        channels[c].cross=settings->osd_cross && (c<2 || settings->osd_recording);
        if(c<2 && (media->tracking.state==CA_TRACK_ACTIVE || media->tracking.state==CA_TRACK_COASTING)) {
            channels[c].tracking=true;
            memcpy(channels[c].rect,&media->tracking.rect,sizeof(channels[c].rect));
        }
    }
    int result=ca_overlay_hw_set(&media->overlay,channels,3);
    pthread_mutex_unlock(&media->lock);
    return result;
}

std::unique_ptr<APC_Media_Backend> APC_Media_Backend::create(const ca_media_config &config)
{
    return APC_Media_ZR10::create(config);
}
