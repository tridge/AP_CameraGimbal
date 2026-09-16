#define _GNU_SOURCE
#include "pipeline.h"
#include "camera_app/overlay.h"

#include "camera_app/live_video_server.h"
#include "camera_app/log.h"
#include "camera_app/media_impl.h"
#include "camera_app/sigmastar_exposure.h"
#include <dlfcn.h>
#include "camera_app/zr10_fov.h"
#include "camera_app/mp4.h"
#include "camera_app/rtsp.h"
#include "camera_app/still.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
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

struct ca_media_impl {
    struct ca_overlay_hw *overlay;
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
    struct ca_live_video_server *live_video;
    struct ca_mp4 *mp4;
    char recording_path[PATH_MAX];
    atomic_bool encoded_frame_logged[CA_ZR10_VENC_COUNT];
    unsigned main_frames;
    atomic_bool isp_bin_loaded;
    _Atomic float zoom;
    _Atomic float visible_hfov_deg;
    uint8_t thermal_gain;
    uint8_t thermal_palette;
    atomic_bool inverted;
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

static void consume_frame(struct ca_media_impl *media, unsigned channel,
                          uint8_t *data, size_t length, uint64_t pts)
{
    enum ca_video_codec codec = media->pipeline.streams[channel].codec;
    bool key_frame = video_key_frame(codec, data, length);
    float hfov_deg = ca_media_impl_hfov(media, false);

    if (!atomic_exchange(&media->encoded_frame_logged[channel], true)) {
        ca_log("VENC channel %u first encoded frame bytes=%zu key=%u", channel,
               length, key_frame ? 1U : 0U);
    }
    if (channel == CA_ZR10_MAIN_VENC || channel == CA_ZR10_SUB_VENC) {
        unsigned stream = channel == CA_ZR10_MAIN_VENC ? 0U
                                                     : media->sub_rtsp_stream;
        (void)ca_rtsp_push_video(media->rtsp, stream, data, length, key_frame, hfov_deg);
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
    struct ca_media_impl *media = opaque;
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

static int make_recording_path(struct ca_media_impl *media)
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
                              "%s/%s_%lld_A%s.mp4", directory, day,
                              (long long)now, suffix == 0U ? "" : "_next");
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

static void media_cleanup(struct ca_media_impl *media)
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

int ca_media_impl_open(struct ca_media_impl **result, const struct ca_media_config *config)
{
    struct ca_media_impl *media;
    struct ca_zr10_stream *streams;
    const char *stage = "argument validation";

    if (result == NULL || config == NULL || config->backend == NULL ||
        strcmp(config->backend, "zr10") != 0 || config->capture_root == NULL ||
        config->record_root == NULL) {
        errno = EINVAL;
        return -1;
    }
    media = calloc(1, sizeof(*media));
    if (media == NULL) return -1;
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
        if (ca_media_impl_set_inverted(media, true) < 0) goto fail;
    }
    stage = "RTSP startup";
    if (ca_rtsp_open(&media->rtsp, config->rtsp_port, "video1",
                     streams[CA_ZR10_MAIN_VENC].codec, CA_ZR10_FRAME_RATE) < 0 ||
        ca_rtsp_add_video(media->rtsp, "video2", streams[CA_ZR10_SUB_VENC].codec,
                          CA_ZR10_FRAME_RATE, &media->sub_rtsp_stream) < 0) {
        goto fail;
    }
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
    bool frames_ready=false;
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
    *result = media;
    return 0;
fail:
    {
        int saved_errno = errno;
        ca_log("media startup failed at %s: %s", stage, strerror(saved_errno));
        media_cleanup(media);
        pthread_mutex_destroy(&media->photo_lock);
        pthread_mutex_destroy(&media->lock);
        free(media);
        errno = saved_errno != 0 ? saved_errno : EIO;
    }
    return -1;
}

int ca_media_impl_set_recording(struct ca_media_impl *media, bool active)
{
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

bool ca_media_impl_recording(const struct ca_media_impl *media)
{
    return media != NULL && atomic_load(&media->recording);
}

const char *ca_media_impl_recording_path(const struct ca_media_impl *media)
{
    return media != NULL ? media->recording_path : "";
}

int ca_media_impl_capture_photo(struct ca_media_impl *media, enum ca_photo_scope scope)
{
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

int ca_media_impl_set_zoom(struct ca_media_impl *media, float zoom)
{
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

float ca_media_impl_hfov(const struct ca_media_impl *media, bool thermal)
{
    return media != NULL && !thermal ? atomic_load(&media->visible_hfov_deg) : 0.0f;
}

float ca_media_impl_zoom(const struct ca_media_impl *media)
{
    return media != NULL ? media->zoom : 1.0f;
}

int ca_media_impl_set_lens(struct ca_media_impl *media, enum ca_media_lens lens)
{
    /* one lens; the SIYI image-slot commands still need to succeed */
    if (media == NULL || (lens != CA_MEDIA_LENS_WIDE &&
                          lens != CA_MEDIA_LENS_ZOOM)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

enum ca_media_lens ca_media_impl_lens(const struct ca_media_impl *media)
{
    (void)media;
    return CA_MEDIA_LENS_WIDE;
}

int ca_media_impl_set_thermal_main(struct ca_media_impl *media, bool thermal_main)
{
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

bool ca_media_impl_thermal_main(const struct ca_media_impl *media)
{
    (void)media;
    return false;
}

int ca_media_impl_autofocus(struct ca_media_impl *media, uint16_t x, uint16_t y)
{
    (void)x;
    (void)y;
    if (media == NULL) {
        errno = EINVAL;
        return -1;
    }
    errno = ENOTSUP;
    return -1;
}

int ca_media_impl_manual_focus(struct ca_media_impl *media, int direction)
{
    if (media == NULL || direction < -1 || direction > 1) {
        errno = EINVAL;
        return -1;
    }
    errno = ENOTSUP;
    return -1;
}

int ca_media_impl_set_focus_percent(struct ca_media_impl *media, float percent)
{
    if (media == NULL || percent < 0.0f || percent > 100.0f) {
        errno = EINVAL;
        return -1;
    }
    errno = ENOTSUP;
    return -1;
}

bool ca_media_impl_thermal_range(struct ca_media_impl *media,
                            struct ca_thermal_range *range)
{
    (void)media;
    (void)range;
    return false;
}

int ca_media_impl_get_thermal_gain(struct ca_media_impl *media, uint8_t *gain)
{
    if (media == NULL || gain == NULL) {
        errno = EINVAL;
        return -1;
    }
    *gain = media->thermal_gain;
    return 0;
}

int ca_media_impl_set_thermal_gain(struct ca_media_impl *media, uint8_t gain)
{
    if (media == NULL || gain > 1U) {
        errno = EINVAL;
        return -1;
    }
    media->thermal_gain = gain;
    return 0;
}

int ca_media_impl_get_thermal_palette(struct ca_media_impl *media, uint8_t *palette)
{
    if (media == NULL || palette == NULL) {
        errno = EINVAL;
        return -1;
    }
    *palette = media->thermal_palette;
    return 0;
}

int ca_media_impl_set_thermal_palette(struct ca_media_impl *media, uint8_t palette)
{
    if (media == NULL || palette > 11U || palette == 1U) {
        errno = EINVAL;
        return -1;
    }
    media->thermal_palette = palette;
    return 0;
}

int ca_media_impl_set_inverted(struct ca_media_impl *media, bool inverted)
{
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

void ca_media_impl_close(struct ca_media_impl *media)
{
    if (media == NULL) return;
    media_cleanup(media);
    pthread_mutex_destroy(&media->photo_lock);
    pthread_mutex_destroy(&media->lock);
    free(media);
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

bool ca_media_impl_ready(const struct ca_media_impl *media) { return media != NULL; }

/* Diagnostics are optional: unavailable SDK symbols must not stop capture. */
int ca_media_impl_exposure(struct ca_media_impl *media, unsigned lens, struct ca_exposure *s)
{
    if (lens!=0) return -ENOTSUP;
    pthread_mutex_lock(&media->lock);
    if (!media->isp_bin_loaded) { pthread_mutex_unlock(&media->lock); return -EAGAIN; }
    typedef int (*query_fn)(uint32_t, void *);
    query_fn query=(query_fn)dlsym(RTLD_DEFAULT,"MI_ISP_AE_QueryExposureInfo");
    query_fn mode=(query_fn)dlsym(RTLD_DEFAULT,"MI_ISP_AE_GetExpoMode");
    /* Room for SDK extensions, while decoding only the documented prefix. */
    union { struct ca_sstar_exposure_info info; uint64_t space[512]; } q={0};
    int result=query ? query(0,&q) : -ENOTSUP;
    if (!result) ca_sstar_exposure_decode(s,&q.info);
    uint32_t value=0;
    int mode_result=mode ? mode(0,&value) : -ENOTSUP;
    if (!mode_result) ca_sstar_exposure_mode(s,value);
    pthread_mutex_unlock(&media->lock);
    return result ? result : mode_result;
}

int ca_media_impl_apply_overlay(struct ca_media_impl *media, const struct ca_config *settings)
{
    struct ca_overlay_channel channels[3]={0};
    enum ca_video_resolution resolutions[3]={settings->main_resolution,settings->sub_resolution,settings->recording_resolution};
    pthread_mutex_lock(&media->lock);
    for (unsigned c=0;c<3;c++) {
        ca_video_resolution_size(resolutions[c],&channels[c].width,&channels[c].height);
        channels[c].cross=settings->osd_cross && (c<2 || settings->osd_recording);
    }
    int result=ca_overlay_hw_set(&media->overlay,channels,3);
    pthread_mutex_unlock(&media->lock);
    return result;
}
