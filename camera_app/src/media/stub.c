#define _GNU_SOURCE
#include "camera_app/media_impl.h"
#include "camera_app/binlog.h"
#include "camera_app/video_fov.h"
#include "camera_app/recorder.h"
#include "apcam/lens.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef CAMERA_APP_SITL
#include "camera_app/live_video_server.h"
#include "camera_app/log.h"
#include "camera_app/rtsp.h"
#include "camera_app/mp4.h"
#include "camera_app/sitl_terrain.h"

#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;
#endif

#ifdef CAMERA_APP_SITL
struct sitl_video {
    enum ca_video_codec codec;
};
#endif

struct ca_media_impl {
    struct ca_recorder recorder;
    float zoom;
    bool has_thermal;
    float digital_ratio[2];
    float optical_ratio;
    _Atomic float visible_hfov_deg;
    _Atomic enum ca_media_lens lens;
    _Atomic bool thermal_main;
    unsigned thermal_captures;
    _Atomic uint8_t thermal_gain;
    _Atomic uint8_t thermal_palette;
    _Atomic float defocus;
    bool inverted;
    char *capture_root;
#ifdef CAMERA_APP_SITL
    struct ca_live_video_server *live_video;
    struct ca_rtsp *rtsp;
    unsigned rtsp_port;
    struct ca_support_config support;
    unsigned secondary_rtsp_stream;
    unsigned sitl_frame_rate;
    struct sitl_video videos[CA_SITL_STREAMS];
    struct ca_sitl_terrain *terrain;
    pthread_t video_thread;
    bool video_thread_started;
    atomic_bool video_stop;
    pthread_mutex_t record_lock;
    pthread_mutex_t image_lock;
    struct ca_config image_settings;
    struct ca_exposure exposure;
    pthread_cond_t capture_changed;
    unsigned capture_mask;
    unsigned capture_generation;
    float capture_fov[3];
    uint8_t *photos[3];
    size_t photo_length[3];
    bool capture_done;
    atomic_bool sitl_recording;
    struct ca_mp4 *mp4[2];
    bool wait_keyframe[2];
    char *record_root;
    char recording_path[2][4096];
    unsigned record_channels;
    unsigned rgb_record_source;
    unsigned video_width[CA_SITL_STREAMS], video_height[CA_SITL_STREAMS];
#endif
};

static int write_file_all(int fd, const uint8_t *data, size_t length)
{
    while (length != 0U) {
        ssize_t written = write(fd, data, length);
        if (written > 0) {
            data += (size_t)written;
            length -= (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            if (written == 0) errno = EIO;
            return -1;
        }
    }
    return 0;
}

#ifdef CAMERA_APP_SITL
static int close_sitl_recording(struct ca_media_impl *media)
{
    int result = 0;
    for (unsigned i = 0U; i < 2U; i++) {
        if (ca_mp4_close(media->mp4[i]) < 0) result = -1;
        media->mp4[i] = NULL;
    }
    atomic_store(&media->sitl_recording, false);
    if (ca_recorder_set(&media->recorder, false) < 0) result = -1;
    return result;
}

/* Two queued frames plus the frame awaiting presentation absorb a short
 * texture-upload stall without turning it into a visible pause. */
#define TERRAIN_QUEUE_SIZE 2U
struct terrain_frame {
    uint8_t *data[CA_SITL_STREAMS];
    size_t length[CA_SITL_STREAMS];
    bool key[CA_SITL_STREAMS];
    float fov[2];
    bool thermal_main;
    uint64_t pts;
    struct timespec due;
};
struct terrain_queue {
    struct ca_media_impl *media;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    struct terrain_frame *frames[TERRAIN_QUEUE_SIZE];
    unsigned head, count;
    bool stopped, done;
};

static void free_terrain_frame(struct terrain_frame *frame)
{
    if (!frame) return;
    for (unsigned i=0; i<CA_SITL_STREAMS; i++) free(frame->data[i]);
    free(frame);
}

static void advance_time(struct timespec *time, int64_t ns)
{
    time->tv_sec += (time->tv_nsec + ns) / INT64_C(1000000000);
    time->tv_nsec = (time->tv_nsec + ns) % INT64_C(1000000000);
}

static int64_t time_difference_ns(struct timespec a, struct timespec b)
{
    return (a.tv_sec - b.tv_sec) * INT64_C(1000000000) + a.tv_nsec - b.tv_nsec;
}

static void *render_terrain_frames(void *opaque)
{
    struct terrain_queue *queue = opaque;
    struct ca_media_impl *media = queue->media;
    int64_t interval = INT64_C(1000000000) / media->sitl_frame_rate;
    int64_t lead = (TERRAIN_QUEUE_SIZE + 1U) * interval;
    if (lead > INT64_C(250000000)) lead = INT64_C(250000000);
    struct timespec started, due;
    clock_gettime(CLOCK_MONOTONIC, &started);
    due = started;
    advance_time(&due, lead);
    for (;;) {
        pthread_mutex_lock(&queue->lock);
        while (!queue->stopped && queue->count == TERRAIN_QUEUE_SIZE)
            pthread_cond_wait(&queue->changed, &queue->lock);
        bool stopped = queue->stopped;
        pthread_mutex_unlock(&queue->lock);
        if (stopped || atomic_load(&media->video_stop)) break;
        struct terrain_frame *frame = calloc(1, sizeof(*frame));
        if (!frame) break;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (time_difference_ns(now, due) > 0) {
            due = now;
            advance_time(&due, lead); /* recover after initial loading or a long stall */
        }
        frame->due = due;
        frame->pts = (uint64_t)(time_difference_ns(due, started) * 9 / 100000);
        frame->thermal_main = media->thermal_main;
        frame->fov[0] = ca_media_impl_hfov(media, false);
        frame->fov[1] = ca_media_impl_hfov(media, media->has_thermal);
        struct ca_sitl_image image = {0};
        pthread_mutex_lock(&media->image_lock);
        image.settings = media->image_settings;
        image.capture_mask = media->capture_mask;
        image.capture_generation = media->capture_generation;
        memcpy(image.capture_fov, media->capture_fov, sizeof(image.capture_fov));
        media->capture_mask = 0;
        pthread_mutex_unlock(&media->image_lock);
        image.thermal_gain = media->thermal_gain;
        image.thermal_palette = media->thermal_palette;
        image.defocus = media->defocus;
        uint8_t *photos[3] = {0};
        size_t photo_length[3] = {0};
        struct ca_exposure exposure;
        unsigned exposure_lens=media->lens;
        int result = ca_sitl_terrain_frame(media->terrain, frame->pts,
                (uint64_t)due.tv_sec * 1000U + (uint64_t)due.tv_nsec / 1000000U,
                frame->fov, frame->thermal_main, media->has_thermal,
                atomic_load(&media->sitl_recording),
                &image, frame->data, frame->length, frame->key, photos, photo_length, &exposure);
        if (result==0) {
            exposure.time_us=ca_binlog_time_us();
            exposure.lens=exposure_lens;
            pthread_mutex_lock(&media->image_lock);
            media->exposure=exposure;
            pthread_mutex_unlock(&media->image_lock);
        }
        if (image.capture_mask) {
            pthread_mutex_lock(&media->image_lock);
            for (unsigned i = 0; i < 3; i++) {
                if (image.capture_generation == media->capture_generation) {
                    free(media->photos[i]);
                    media->photos[i] = photos[i];
                    media->photo_length[i] = result < 0 ? 0 : photo_length[i];
                } else free(photos[i]);
            }
            if (image.capture_generation == media->capture_generation) media->capture_done = true;
            pthread_cond_signal(&media->capture_changed);
            pthread_mutex_unlock(&media->image_lock);
        }
        if (result < 0) {
            if (!atomic_load(&media->video_stop))
                ca_log("SITL terrain renderer failed: %s", strerror(errno));
            free_terrain_frame(frame);
            break;
        }
        pthread_mutex_lock(&queue->lock);
        if (queue->stopped) {
            pthread_mutex_unlock(&queue->lock);
            free_terrain_frame(frame);
            break;
        }
        queue->frames[(queue->head + queue->count) % TERRAIN_QUEUE_SIZE] = frame;
        queue->count++;
        pthread_cond_broadcast(&queue->changed);
        pthread_mutex_unlock(&queue->lock);
        advance_time(&due, interval);
    }
    pthread_mutex_lock(&queue->lock);
    queue->done = true;
    pthread_cond_broadcast(&queue->changed);
    pthread_mutex_unlock(&queue->lock);
    return NULL;
}

static struct terrain_queue *start_terrain_queue(struct ca_media_impl *media)
{
    struct terrain_queue *queue = calloc(1, sizeof(*queue));
    if (!queue) return NULL;
    queue->media = media;
    int error = pthread_mutex_init(&queue->lock, NULL);
    if (error) goto fail;
    error = pthread_cond_init(&queue->changed, NULL);
    if (error) { pthread_mutex_destroy(&queue->lock); goto fail; }
    error = pthread_create(&queue->thread, NULL, render_terrain_frames, queue);
    if (error) {
        pthread_cond_destroy(&queue->changed);
        pthread_mutex_destroy(&queue->lock);
        goto fail;
    }
    return queue;
fail:
    free(queue); errno = error; return NULL;
}

static struct terrain_frame *next_terrain_frame(struct terrain_queue *queue)
{
    pthread_mutex_lock(&queue->lock);
    while (!queue->count && !queue->done)
        pthread_cond_wait(&queue->changed, &queue->lock);
    struct terrain_frame *frame = NULL;
    if (queue->count) {
        frame = queue->frames[queue->head];
        queue->head = (queue->head + 1U) % TERRAIN_QUEUE_SIZE;
        queue->count--;
        pthread_cond_broadcast(&queue->changed);
    }
    pthread_mutex_unlock(&queue->lock);
    return frame;
}

static void stop_terrain_queue(struct terrain_queue *queue)
{
    if (!queue) return;
    pthread_mutex_lock(&queue->lock);
    queue->stopped = true;
    pthread_cond_broadcast(&queue->changed);
    pthread_mutex_unlock(&queue->lock);
    ca_sitl_terrain_interrupt(queue->media->terrain);
    pthread_join(queue->thread, NULL);
    for (unsigned i = 0; i < queue->count; i++)
        free_terrain_frame(queue->frames[(queue->head + i) % TERRAIN_QUEUE_SIZE]);
    pthread_cond_destroy(&queue->changed);
    pthread_mutex_destroy(&queue->lock);
    free(queue);
}

static void *video_thread(void *opaque)
{
    struct ca_media_impl *media = opaque;
    uint64_t pts = 0U;

    struct terrain_queue *queue = media->terrain ? start_terrain_queue(media) : NULL;
    if (media->terrain && !queue) {
        ca_log("cannot start SITL render queue: %s", strerror(errno));
        return NULL;
    }
    bool previous_source = false;
    enum ca_video_codec rtsp_codecs[2] = {media->videos[0].codec, media->videos[1].codec};
    while (!atomic_load(&media->video_stop)) {
        uint8_t *rendered[CA_SITL_STREAMS] = {0};
        size_t rendered_length[CA_SITL_STREAMS];
        bool rendered_key[CA_SITL_STREAMS];
        bool thermal_main = media->thermal_main;
        float fov[2] = {
            ca_media_impl_hfov(media, false),
            ca_media_impl_hfov(media, media->has_thermal),
        };
        if (media->terrain) {
            struct terrain_frame *frame = next_terrain_frame(queue);
            if (!frame) break;
            memcpy(rendered, frame->data, sizeof(rendered));
            memcpy(rendered_length, frame->length, sizeof(rendered_length));
            memcpy(rendered_key, frame->key, sizeof(rendered_key));
            memcpy(fov, frame->fov, sizeof(fov));
            thermal_main = frame->thermal_main;
            pts = frame->pts;
            /* Keep each frame's source/FOV with its pixels across queued zoom
             * and source changes. The deadline is on the same prediction clock. */
            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &frame->due, NULL) == EINTR) {}
            free(frame); /* payload ownership moved to rendered[] */
        }
        if (media->has_thermal && thermal_main != previous_source) {
            unsigned indices[2] = {thermal_main ? 1U : 0U, thermal_main ? 2U : 1U};
            enum ca_video_codec codecs[2] = {media->videos[indices[0]].codec, media->videos[indices[1]].codec};
            if (codecs[0] != rtsp_codecs[0] || codecs[1] != rtsp_codecs[1]) {
                /* SDP and RTP packetisation must match the newly routed lens.
                 * A codec change requires RTSP clients to reconnect. */
                ca_rtsp_close(media->rtsp);
                media->rtsp = NULL;
                if (ca_rtsp_open(&media->rtsp, media->rtsp_port, "video1", codecs[0], media->sitl_frame_rate) < 0 ||
                    ca_rtsp_add_video(media->rtsp, "video2", codecs[1], media->sitl_frame_rate,
                                       &media->secondary_rtsp_stream) < 0) {
                    ca_log("cannot reconfigure SITL RTSP after source codec change");
                    for (unsigned i = 0; i < CA_SITL_STREAMS; i++) free(rendered[i]);
                    break;
                }
                if (ca_rtsp_support_proxy(media->rtsp, &media->support) < 0)
                    ca_log("SupportProxy video restart failed: %s", strerror(errno));
                memcpy(rtsp_codecs, codecs, sizeof(codecs));
            }
            for (unsigned i = 0; i < 2; i++) {
                unsigned source = indices[i];
                (void)ca_live_video_server_configure(media->live_video, i,
                    media->video_width[source], media->video_height[source],
                    media->sitl_frame_rate, media->videos[source].codec == CA_VIDEO_H264);
            }
            previous_source = thermal_main;
        }
        for (unsigned stream = 0U; stream < CA_SITL_STREAMS; stream++) {
            const uint8_t *data;
            size_t length;
            bool key_frame;
            bool have_frame;
            if (media->terrain) {
                data = rendered[stream]; length = rendered_length[stream];
                key_frame = rendered_key[stream]; have_frame = length != 0;
            } else break;
            if (have_frame) {
                float record_hfov = ca_media_impl_hfov(media, media->has_thermal && (stream == 1U || stream == 4U));
                float hfov_deg = record_hfov;
                if (media->terrain) record_hfov = hfov_deg = fov[(stream == 1U || stream == 4U) ? 1U : 0U];
                pthread_mutex_lock(&media->record_lock);
                unsigned record = stream == media->rgb_record_source ? 0U :
                                  (media->has_thermal && stream == 4U ? 1U : 2U);
                if (record < media->record_channels && media->mp4[record] != NULL) {
                    if (key_frame) media->wait_keyframe[record] = false;
                    if (!media->wait_keyframe[record] &&
                        ca_mp4_write_h264(media->mp4[record], data, length,
                                          pts, key_frame, record_hfov) < 0) {
                        ca_log("SITL recording write failed: %s", strerror(errno));
                        (void)close_sitl_recording(media);
                    }
                }
                pthread_mutex_unlock(&media->record_lock);
                bool publish = media->has_thermal ? (stream == 1U ||
                    (thermal_main ? stream == 2U : stream == 0U)) : stream < 2U;
                if (!publish) { free(rendered[stream]); continue; }
                unsigned live_stream = media->has_thermal ?
                    (stream == 1U ? (thermal_main ? 0U : 1U) : (thermal_main ? 1U : 0U)) : stream;
                (void)ca_live_video_server_publish(media->live_video, live_stream,
                                                    data, length, pts,
                                                    key_frame, hfov_deg);
                (void)ca_rtsp_push_h264_stream(media->rtsp, live_stream, data,
                                                length, key_frame, hfov_deg);
            }
            free(rendered[stream]);
        }
        pts += 90000U / media->sitl_frame_rate;
    }
    stop_terrain_queue(queue);
    return NULL;
}

static int open_sitl_video(struct ca_media_impl *media,
                           const struct ca_media_config *config)
{
    const char *video1 = getenv("CAMERA_APP_SITL_VIDEO1");
    const char *video2 = getenv("CAMERA_APP_SITL_VIDEO2");
    const char *terrain = getenv("CAMERA_APP_SITL_TERRAIN");
    unsigned width1, height1, width2, height2, width3, height3, width4, height4;
    unsigned frame_rate = APCAM_FRAME_RATE;

    if (video1 == NULL && video2 == NULL && terrain == NULL) return 0;
    ca_video_resolution_size(config->settings.main_resolution, &width1, &height1);
    ca_video_resolution_size(config->settings.sub_resolution, &width2, &height2);
    width3 = width2; height3 = height2;
    ca_video_resolution_size(config->settings.recording_resolution, &width4, &height4);
    media->rgb_record_source = 3U; /* separate overlays and H.264 recording */
    if (media->has_thermal) {
        width2 = APCAM_THERMAL_STREAM_WIDTH;
        height2 = APCAM_THERMAL_STREAM_HEIGHT;
    }
    {
        if (terrain) frame_rate = 20U;
        const char *rate = getenv("CAMERA_GIMBAL_SITL_FPS");
        if (rate) {
            char *end;
            unsigned long value = strtoul(rate, &end, 10);
            if (!*rate || *end || value < 1 || value > 60) { errno = EINVAL; return -1; }
            frame_rate = (unsigned)value;
        }
        const unsigned widths[CA_SITL_STREAMS] = {width1, width2, width3, width4, width2}, heights[CA_SITL_STREAMS] = {height1, height2, height3, height4, height2};
        const enum ca_video_codec codecs[CA_SITL_STREAMS] = {config->settings.main_codec,
            media->has_thermal ? CA_VIDEO_H264 : config->settings.sub_codec,
            config->settings.sub_codec, CA_VIDEO_H264, CA_VIDEO_H264};
        for (unsigned i = 0; i < CA_SITL_STREAMS; i++) media->videos[i].codec = codecs[i];
        const char *renderer = terrain ? terrain : getenv("CAMERA_APP_SITL_RENDERER");
        if (!renderer || !*renderer) renderer = CA_SITL_VIDEO_SCRIPT;
        if (ca_sitl_terrain_open(&media->terrain, renderer, widths, heights, frame_rate, codecs) < 0) return -1;
    }
    if (config->rtsp_port == UINT16_MAX) { errno = EINVAL; return -1; }
    media->rtsp_port = config->rtsp_port;
    media->support = config->settings.support;
    if (ca_rtsp_open(&media->rtsp, config->rtsp_port, "video1",
                     media->videos[0].codec, frame_rate) < 0) {
        ca_log("cannot start SITL RTSP on port %u: %s", config->rtsp_port, strerror(errno));
        return -1;
    }
    if (ca_rtsp_add_video(media->rtsp, "video2", media->videos[1].codec, frame_rate,
                          &media->secondary_rtsp_stream) < 0 ||
        ca_live_video_server_open(&media->live_video,
                                  config->rtsp_port + 1U) < 0 ||
        ca_live_video_server_configure(media->live_video, 0U, width1, height1,
                                       frame_rate, media->videos[0].codec == CA_VIDEO_H264) < 0 ||
        ca_live_video_server_configure(media->live_video, 1U, width2, height2,
                                       frame_rate, media->videos[1].codec == CA_VIDEO_H264) < 0) {
        ca_log("cannot configure SITL live video: %s", strerror(errno));
        return -1;
    }
    if (ca_rtsp_support_proxy(media->rtsp, &config->settings.support) < 0)
        ca_log("SupportProxy video startup failed: %s", strerror(errno));
    media->sitl_frame_rate = frame_rate;
    media->video_width[0] = width1; media->video_height[0] = height1;
    media->video_width[1] = width2; media->video_height[1] = height2;
    media->video_width[2] = width3; media->video_height[2] = height3;
    media->video_width[3] = width4; media->video_height[3] = height4;
    media->video_width[4] = width2; media->video_height[4] = height2;
    media->record_channels = APCAM_NUM_RECORDING_CHANNELS;
    int code = pthread_create(&media->video_thread, NULL, video_thread, media);
    if (code != 0) {
        errno = code;
        return -1;
    }
    media->video_thread_started = true;
    ca_log("SITL video sources video1=%s (%ux%u) video2=%s (%ux%u) at %u fps",
           terrain ? "3D terrain" : video1, width1, height1,
           terrain ? "3D terrain" : video2, width2, height2, frame_rate);
    return 0;
}
#endif

int ca_media_impl_open(struct ca_media_impl **result, const struct ca_media_config *config)
{
    const char *state_path = getenv("CAMERA_APP_RECORD_STATE");
    if (result == NULL || config == NULL) {
        errno = EINVAL;
        return -1;
    }
    struct ca_media_impl *media = calloc(1, sizeof(*media));
    if (media == NULL) return -1;
    ca_recorder_init(&media->recorder, state_path);
    media->zoom = 1.0f;
    media->has_thermal = APCAM_HAVE_THERMAL;
    media->digital_ratio[0] = media->digital_ratio[1] = 1.0f;
    media->optical_ratio = 1.0f;
    atomic_store(&media->visible_hfov_deg,
                 ca_lens1_hfov(media->zoom));
    media->lens = CA_MEDIA_LENS_WIDE;
    media->thermal_gain = 1U;
    media->thermal_palette = config->settings.thermal_palette;
    media->capture_root = strdup(config->capture_root);
    if (media->capture_root == NULL) {
        ca_recorder_close(&media->recorder);
        free(media);
        return -1;
    }
#ifdef CAMERA_APP_SITL
    pthread_mutex_init(&media->record_lock, NULL);
    pthread_mutex_init(&media->image_lock, NULL);
    pthread_cond_init(&media->capture_changed, NULL);
    media->image_settings = config->settings;
    media->record_root = strdup(config->record_root);
    if (media->record_root == NULL || open_sitl_video(media, config) < 0) {
        int saved_errno = errno;
        ca_media_impl_close(media);
        errno = saved_errno;
        return -1;
    }
#endif
    *result = media;
    return 0;
}

int ca_media_impl_set_recording(struct ca_media_impl *media, bool active)
{
#ifdef CAMERA_APP_SITL
    if (media->video_thread_started) {
        int result = 0;
        pthread_mutex_lock(&media->record_lock);
        if (active == atomic_load(&media->sitl_recording)) goto done;
        if (!active) { result = close_sitl_recording(media); goto done; }
        if (mkdir(media->record_root, 0755) < 0 && errno != EEXIST) { result = -1; goto done; }
        /* Recording encoders always produce H.264, independently of live codecs. */
        for (unsigned i = 0U; i < media->record_channels; i++) {
            if (media->videos[i == 0U ? media->rgb_record_source : 4U].codec != CA_VIDEO_H264) { errno = ENOTSUP; result = -1; goto done; }
        }
        /* Reserve unique names even after a simulator restart. */
        for (unsigned i = 0U; i < media->record_channels; i++) {
            int n = snprintf(media->recording_path[i], sizeof(media->recording_path[i]),
                             "%s/SITL_%u_XXXXXX.mp4", media->record_root, i);
            if (n < 0 || (size_t)n >= sizeof(media->recording_path[i])) { result = -1; break; }
            int fd = mkstemps(media->recording_path[i], 4);
            if (fd < 0) { result = -1; break; }
            close(fd);
            unlink(media->recording_path[i]);
            if (ca_mp4_open(&media->mp4[i], media->recording_path[i],
                            media->video_width[i == 0U ? media->rgb_record_source : 4U], media->video_height[i == 0U ? media->rgb_record_source : 4U],
                            media->sitl_frame_rate) < 0) { result = -1; break; }
            media->wait_keyframe[i] = true;
        }
        if (result == 0) result = ca_recorder_set(&media->recorder, true);
        if (result < 0) (void)close_sitl_recording(media);
        else atomic_store(&media->sitl_recording, true);
    done:
        pthread_mutex_unlock(&media->record_lock);
        return result;
    }
#endif
    return ca_recorder_set(&media->recorder, active);
}

bool ca_media_impl_recording(const struct ca_media_impl *media)
{
#ifdef CAMERA_APP_SITL
    if (media->video_thread_started) return atomic_load(&media->sitl_recording);
#endif
    return ca_recorder_active(&media->recorder);
}

const char *ca_media_impl_recording_path(const struct ca_media_impl *media)
{
#ifdef CAMERA_APP_SITL
    if (media->video_thread_started) return media->recording_path[0];
#endif
    (void)media;
    return "test-recording.mp4";
}

static void update_hfov(struct ca_media_impl *media)
{
    float hfov;
    if (APCAM_HAVE_ZOOM_LENS && media->lens == CA_MEDIA_LENS_ZOOM)
        hfov = ca_zoom_lens_hfov(media->optical_ratio, media->digital_ratio[media->lens]);
    else
        hfov = ca_lens1_hfov(APCAM_HAVE_ZOOM_LENS ? media->digital_ratio[media->lens] : media->zoom);
    atomic_store(&media->visible_hfov_deg, hfov);
}

float ca_media_impl_hfov(const struct ca_media_impl *media, bool thermal)
{
    if (media == NULL) return 0.0f;
    if (thermal) return media->has_thermal ? CA_THERMAL_HFOV_DEG : 0.0f;
    return atomic_load(&media->visible_hfov_deg);
}

int ca_media_impl_set_zoom(struct ca_media_impl *media, float zoom)
{
    if (media == NULL || !isfinite(zoom) || zoom < 1.0f ||
        zoom > APCAM_ZOOM_MAX) {
        errno = EINVAL;
        return -1;
    }
    media->zoom = zoom;
    media->lens = apcam_uses_zoom_lens(zoom) ? CA_MEDIA_LENS_ZOOM
                                                  : CA_MEDIA_LENS_WIDE;
    media->thermal_main = false;
    media->optical_ratio = apcam_zoom_lens_optical(zoom);
    media->digital_ratio[media->lens] = media->lens == CA_MEDIA_LENS_WIDE ? zoom : 1.0f;
    update_hfov(media);
    return 0;
}

float ca_media_impl_zoom(const struct ca_media_impl *media)
{
    return media != NULL ? media->zoom : 1.0f;
}

enum ca_media_lens ca_media_impl_lens(const struct ca_media_impl *media)
{
    return media != NULL ? media->lens : CA_MEDIA_LENS_WIDE;
}

int ca_media_impl_autofocus(struct ca_media_impl *media, uint16_t x, uint16_t y)
{
    (void)x;
    (void)y;
    if (media == NULL) {
        errno = EINVAL;
        return -1;
    }
    media->defocus = 0;
    return 0;
}

int ca_media_impl_manual_focus(struct ca_media_impl *media, int direction)
{
    if (media == NULL || direction < -1 || direction > 1) {
        errno = EINVAL;
        return -1;
    }
    media->defocus = fmaxf(0, fminf(1, media->defocus + direction * 0.1f));
    return 0;
}

int ca_media_impl_set_focus_percent(struct ca_media_impl *media, float percent)
{
    if (media == NULL || !isfinite(percent) || percent < 0.0f || percent > 100.0f) {
        errno = EINVAL;
        return -1;
    }
    media->defocus = percent * 0.01f;
    return 0;
}

bool ca_media_impl_thermal_range(struct ca_media_impl *media,
                            struct ca_thermal_range *range)
{
    (void)media;
    if (range == NULL) return false;
    *range = (struct ca_thermal_range) {
        .maximum_centi_c = 4200,
        .minimum_centi_c = 1200,
        .maximum_x = 100,
        .maximum_y = 200,
        .minimum_x = 10,
        .minimum_y = 20,
        .frame_sequence = 1,
    };
    return true;
}

#ifdef CAMERA_APP_SITL
static int capture_sitl_photo(struct ca_media_impl *media, enum ca_photo_scope scope)
{
    unsigned mask = media->has_thermal && scope == CA_PHOTO_SCOPE_THERMAL ? 4U :
                    (1U | (APCAM_HAVE_ZOOM_LENS ? 2U : 0U) | (media->has_thermal ? 4U : 0U));
    if (mkdir(media->capture_root, 0755) < 0 && errno != EEXIST) return -1;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    pthread_mutex_lock(&media->image_lock);
    media->capture_done = false;
    media->capture_generation++;
    media->capture_mask = mask;
    media->capture_fov[0] = ca_lens1_hfov(APCAM_HAVE_ZOOM_LENS ? media->digital_ratio[0] : media->zoom);
#if APCAM_HAVE_ZOOM_LENS
    media->capture_fov[1] = ca_zoom_lens_hfov(media->optical_ratio, media->digital_ratio[1]);
#endif
    media->capture_fov[2] = ca_media_impl_hfov(media, true);
    int error = 0;
    while (!media->capture_done && !error)
        error = pthread_cond_timedwait(&media->capture_changed, &media->image_lock, &deadline);
    if (error) {
        media->capture_mask = 0;
        media->capture_generation++; /* discard any late response */
    }
    const char *suffix[] = {"_C.jpg", "_Z.jpg", "_I.jpg"};
    for (unsigned i = 0; i < 3 && !error; i++) {
        if (!(mask & (1U << i))) continue;
        if (!media->photo_length[i]) { error = EIO; break; }
        char path[4096];
        int n = snprintf(path, sizeof(path), "%s/SITL_XXXXXX%s", media->capture_root, suffix[i]);
        if (n < 0 || (size_t)n >= sizeof(path)) { error = ENAMETOOLONG; break; }
        int fd = mkstemps(path, 6);
        if (fd < 0) { error = errno; break; }
        int written = write_file_all(fd, media->photos[i], media->photo_length[i]);
        if (written < 0) error = errno;
        if (close(fd) < 0 && !error) error = errno;
        if (error) unlink(path);
    }
    pthread_mutex_unlock(&media->image_lock);
    if (error) { errno = error; return -1; }
    return 0;
}
#endif

int ca_media_impl_capture_photo(struct ca_media_impl *media, enum ca_photo_scope scope)
{
#ifdef CAMERA_APP_SITL
    if (media && media->terrain) return capture_sitl_photo(media, scope);
#endif
    const char *source;
    const char *suffixes[3] = {"_C.jpg", "_Z.jpg", "_I.jpg"};
    unsigned first;

    if (media == NULL || (scope != CA_PHOTO_SCOPE_THERMAL &&
                          scope != CA_PHOTO_SCOPE_ALL)) {
        errno = EINVAL;
        return -1;
    }
    media->thermal_captures++;
    source = getenv("CAMERA_APP_SITL_PHOTO");
    if (source == NULL || media->capture_root == NULL) return 0;
    if (mkdir(media->capture_root, 0755) < 0 && errno != EEXIST) return -1;
    first = scope == CA_PHOTO_SCOPE_ALL ? 0U : 2U;
    for (unsigned index = first; index < 3U; index++) {
        char path[4096];
        int input;
        int output;
        uint8_t buffer[16384];
        int length = snprintf(path, sizeof(path), "%s/SITL_%06u%s",
                              media->capture_root,
                              media->thermal_captures, suffixes[index]);
        if (length <= 0 || (size_t)length >= sizeof(path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        input = open(source, O_RDONLY | O_CLOEXEC);
        output = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (input < 0 || output < 0) {
            if (input >= 0) close(input);
            if (output >= 0) close(output);
            return -1;
        }
        for (;;) {
            ssize_t got = read(input, buffer, sizeof(buffer));
            if (got == 0) break;
            if (got < 0 && errno == EINTR) continue;
            if (got < 0 || write_file_all(output, buffer, (size_t)got) < 0) {
                close(input);
                close(output);
                unlink(path);
                return -1;
            }
        }
        close(input);
        if (close(output) < 0) return -1;
    }
    return 0;
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
    media->inverted = inverted;
    return 0;
}

int ca_media_impl_set_lens(struct ca_media_impl *media, enum ca_media_lens lens)
{
    if (media == NULL || (lens != CA_MEDIA_LENS_WIDE &&
                          lens != CA_MEDIA_LENS_ZOOM)) {
        errno = EINVAL;
        return -1;
    }
    media->lens = lens;
    update_hfov(media);
    media->thermal_main = false;
    return 0;
}

int ca_media_impl_set_thermal_main(struct ca_media_impl *media, bool thermal_main)
{
    if (media == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (thermal_main) {
        media->lens = CA_MEDIA_LENS_ZOOM;
        update_hfov(media);
    }
    media->thermal_main = thermal_main;
    return 0;
}

bool ca_media_impl_thermal_main(const struct ca_media_impl *media)
{
    return media != NULL && media->thermal_main;
}

void ca_media_impl_close(struct ca_media_impl *media)
{
    if (media == NULL) return;
#ifdef CAMERA_APP_SITL
    atomic_store(&media->video_stop, true);
    ca_sitl_terrain_interrupt(media->terrain);
    if (media->video_thread_started) pthread_join(media->video_thread, NULL);
    ca_sitl_terrain_close(media->terrain);
    (void)close_sitl_recording(media);
    pthread_mutex_destroy(&media->record_lock);
    pthread_mutex_destroy(&media->image_lock);
    pthread_cond_destroy(&media->capture_changed);
    for (unsigned i = 0; i < 3; i++) free(media->photos[i]);
    free(media->record_root);
    ca_live_video_server_close(media->live_video);
    ca_rtsp_close(media->rtsp);
#endif
    ca_recorder_close(&media->recorder);
    free(media->capture_root);
    free(media);
}

unsigned ca_media_impl_frame_rate(const struct ca_media_impl *media, bool thermal)
{
#ifdef CAMERA_APP_SITL
    if (media && media->sitl_frame_rate) return media->sitl_frame_rate;
#else
    (void)media;
#endif
    return thermal ? APCAM_THERMAL_FRAME_RATE : APCAM_FRAME_RATE;
}

int ca_media_impl_apply_image(struct ca_media_impl *media, const struct ca_config *settings)
{
    if (!media || !settings) { errno = EINVAL; return -1; }
#ifdef CAMERA_APP_SITL
    pthread_mutex_lock(&media->image_lock);
    ca_config_copy_image(&media->image_settings, settings);
    pthread_mutex_unlock(&media->image_lock);
#endif
    return 0;
}

bool ca_media_impl_ready(const struct ca_media_impl *media) { return media != NULL; }

int ca_media_impl_exposure(struct ca_media_impl *media, unsigned lens, struct ca_exposure *s)
{
    s->source=1;
#ifdef CAMERA_APP_SITL
    pthread_mutex_lock(&media->image_lock);
    struct ca_exposure cached=media->exposure;
    pthread_mutex_unlock(&media->image_lock);
    if (!cached.time_us || !cached.valid || cached.lens!=lens) return -ENODATA;
    uint64_t now=ca_binlog_time_us();
    if (now<cached.time_us || now-cached.time_us>1000000U) return -ETIMEDOUT;
    *s=cached;
    return s->result;
#else
    (void)media; (void)lens;
    return -ENOTSUP;
#endif
}

int ca_media_impl_apply_overlay(struct ca_media_impl *media, const struct ca_config *settings)
{
    if (!media || !settings) { errno=EINVAL; return -1; }
#ifdef CAMERA_APP_SITL
    pthread_mutex_lock(&media->image_lock);
    media->image_settings.osd_cross = settings->osd_cross;
    media->image_settings.osd_recording = settings->osd_recording;
    media->image_settings.osd_thermal_fov = settings->osd_thermal_fov;
    pthread_mutex_unlock(&media->image_lock);
#endif
    return 0;
}
