#define _GNU_SOURCE
#include "camera_app/overlay.h"
#include "e5739.h"
#include "isp.h"
#include "pipeline.h"
#include "thermal.h"

#include "camera_app/log.h"
#include "camera_app/autofocus.h"
#include "camera_app/live_video_server.h"
#include "camera_app/media_impl.h"
#include "camera_app/video_fov.h"
#include "camera_app/mp4.h"
#include "camera_app/raw_thermal.h"
#include "camera_app/raw_thermal_server.h"
#include "camera_app/rtsp.h"
#include "camera_app/still.h"

#include "ss_mpi_vpss.h"
#include "ss_mpi_isp.h"

#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct ca_media_impl {
    struct ca_media_config config;
    struct ca_overlay_hw *overlay;
    sample_vi_cfg vi_cfg[2];
    ot_venc_chn_attr venc_attr[4];
    pthread_t thread;
    pthread_t autofocus_thread;
    pthread_t manual_focus_thread;
    pthread_mutex_t lock;
    pthread_mutex_t venc_lock;
    pthread_mutex_t actuator_lock;
    pthread_mutex_t photo_lock;
    td_bool sys_started;
    td_bool vi_started[2];
    td_bool vi_vpss_bound[2];
    td_bool vpss_started[2];
    td_bool thermal_vpss_started;
    td_bool scene_started;
    td_bool venc_created[4];
    td_bool rtsp_main_bound;
    td_bool rtsp_sub_bound;
    td_bool record_rgb_bound;
    td_bool record_thermal_bound;
    td_bool venc_started[4];
    bool thread_started;
    bool autofocus_thread_started;
    bool manual_focus_thread_started;
    atomic_bool stop;
    atomic_bool autofocus_cancel;
    atomic_bool autofocus_running;
    atomic_int manual_focus_direction;
    atomic_bool recording;
    atomic_bool thermal_main;
    atomic_bool inverted;
    bool recording_wait_keyframe[2];
    struct ca_rtsp *rtsp;
    struct ca_live_video_server *live_video;
    struct ca_mp4 *mp4[2];
    char recording_path[2][PATH_MAX];
    unsigned thermal_rtsp_stream;
    struct ca_mt11_thermal *thermal;
    struct ca_raw_thermal *raw_thermal;
    struct ca_raw_thermal_server *raw_thermal_server;
    uint16_t *thermal_latest;
    bool thermal_latest_valid;
    struct timespec thermal_latest_timestamp;
    atomic_bool thermal_send_error_logged;
    atomic_bool thermal_frame_logged;
    atomic_uint thermal_frames;
    bool thermal_range_valid;
    struct ca_thermal_range thermal_range;
    atomic_bool encoded_frame_logged[4];
    enum ca_media_lens lens;
    float zoom;
    _Atomic float visible_hfov_deg;
    float digital_ratio[2];
    ot_vpss_grp selected_group;
    struct ca_e5739 *e5739;
    uint16_t autofocus_x;
    uint16_t autofocus_y;
};

#define CA_MT11_VENC_COUNT 4U
#define CA_MT11_MAIN_VENC 0U
#define CA_MT11_SUB_VENC 1U
#define CA_MT11_RGB_RECORD_VENC 2U
#define CA_MT11_THERMAL_RECORD_VENC 3U
#define CA_MT11_STILL_VENC 4
#define CA_MT11_LIVE_VIDEO_PORT 8555U

#define MT11_FACTORY_SCENE_DIR \
    "/app/cfg/sensor_auto/sensor_imx586_imx678_scene"

static int switch_rtsp_sources_locked(struct ca_media_impl *media,
                                      bool thermal_main);

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

static void consume_frame(struct ca_media_impl *media, unsigned channel,
                          uint8_t *data, size_t length, uint64_t pts)
{
    bool is_rtsp = channel <= CA_MT11_SUB_VENC;
    enum ca_video_codec codec = is_rtsp
        ? (channel == CA_MT11_MAIN_VENC ? media->config.settings.main_codec
                                        : media->config.settings.sub_codec)
        : CA_VIDEO_H264;
    bool key_frame = video_key_frame(codec, data, length);
    bool thermal = is_rtsp
        ? ((channel == CA_MT11_MAIN_VENC) == atomic_load(&media->thermal_main))
        : channel == CA_MT11_THERMAL_RECORD_VENC;
    float hfov_deg = ca_media_impl_hfov(media, thermal);
    if (!atomic_exchange(&media->encoded_frame_logged[channel], true)) {
        ca_log("VENC channel %u first encoded frame bytes=%zu key=%u", channel,
               length, key_frame ? 1U : 0U);
    }
    if (is_rtsp) {
        unsigned stream = channel == CA_MT11_MAIN_VENC
                              ? 0U : media->thermal_rtsp_stream;
        (void)ca_rtsp_push_video(media->rtsp, stream, data, length, key_frame, hfov_deg);
        (void)ca_live_video_server_publish(media->live_video, channel, data,
                                            length, pts, key_frame, hfov_deg);
        return;
    }
    unsigned recording_channel = channel - CA_MT11_RGB_RECORD_VENC;
    pthread_mutex_lock(&media->lock);
    if (atomic_load(&media->recording) &&
        media->recording_wait_keyframe[recording_channel] &&
        key_frame) {
        media->recording_wait_keyframe[recording_channel] = false;
    }
    if (atomic_load(&media->recording) &&
        !media->recording_wait_keyframe[recording_channel] &&
        ca_mp4_write_h264(media->mp4[recording_channel], data, length, pts,
                          key_frame, hfov_deg) < 0) {
        ca_log("recording stopped after channel %u MP4 write failure: %s",
               channel, strerror(errno));
        for (unsigned i = 0; i < 2U; i++) {
            (void)ca_mp4_close(media->mp4[i]);
            media->mp4[i] = NULL;
        }
        atomic_store(&media->recording, false);
    }
    pthread_mutex_unlock(&media->lock);
}

static void *capture_thread(void *opaque)
{
    struct ca_media_impl *media = opaque;
    while (!atomic_load(&media->stop)) {
        bool consumed = false;
        for (unsigned channel = 0; channel < CA_MT11_VENC_COUNT; channel++) {
            ot_venc_chn_status status = {0};
            ot_venc_stream stream = {0};
            uint8_t *frame = NULL;
            size_t frame_length = 0;
            uint64_t pts = 0;
            pthread_mutex_lock(&media->venc_lock);
            td_s32 ret = ss_mpi_venc_query_status(channel, &status);
            if (ret != TD_SUCCESS || status.cur_packs == 0U) {
                pthread_mutex_unlock(&media->venc_lock);
                continue;
            }
            if (status.cur_packs > 128U) {
                ca_log("VENC channel %u returned invalid pack count=%u",
                       channel, status.cur_packs);
                pthread_mutex_unlock(&media->venc_lock);
                continue;
            }
            stream.pack = calloc(status.cur_packs, sizeof(*stream.pack));
            if (stream.pack == NULL) {
                pthread_mutex_unlock(&media->venc_lock);
                continue;
            }
            stream.pack_cnt = status.cur_packs;
            ret = ss_mpi_venc_get_stream(channel, &stream, 100);
            if (ret == TD_SUCCESS) {
                size_t total = 0;
                for (td_u32 i = 0; i < stream.pack_cnt; i++) {
                    if (stream.pack[i].offset <= stream.pack[i].len) {
                        total += stream.pack[i].len - stream.pack[i].offset;
                        pts = stream.pack[i].pts;
                    }
                }
                frame = total != 0U ? malloc(total) : NULL;
                if (frame != NULL) {
                    size_t offset = 0;
                    for (td_u32 i = 0; i < stream.pack_cnt; i++) {
                        ot_venc_pack *pack = &stream.pack[i];
                        if (pack->offset > pack->len) continue;
                        size_t bytes = pack->len - pack->offset;
                        memcpy(frame + offset, pack->addr + pack->offset,
                               bytes);
                        offset += bytes;
                    }
                    frame_length = offset;
                }
                (void)ss_mpi_venc_release_stream(channel, &stream);
                consumed = true;
            }
            free(stream.pack);
            pthread_mutex_unlock(&media->venc_lock);
            if (frame != NULL) {
                consume_frame(media, channel, frame, frame_length, pts);
                free(frame);
            }
        }
        if (!consumed) {
            const struct timespec delay = {.tv_sec = 0, .tv_nsec = 5000000};
            (void)nanosleep(&delay, NULL);
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
        int rgb_length = snprintf(media->recording_path[0],
                                  sizeof(media->recording_path[0]),
                                  "%s/%s_%lld_A%s.mp4", directory, day,
                                  (long long)now,
                                  suffix == 0U ? "" : "_next");
        int thermal_length = snprintf(media->recording_path[1],
                                      sizeof(media->recording_path[1]),
                                      "%s/%s_%lld_I%s.mp4", directory, day,
                                      (long long)now,
                                      suffix == 0U ? "" : "_next");
        if (rgb_length < 0 || thermal_length < 0 ||
            (size_t)rgb_length >= sizeof(media->recording_path[0]) ||
            (size_t)thermal_length >= sizeof(media->recording_path[1])) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (access(media->recording_path[0], F_OK) < 0 && errno == ENOENT &&
            access(media->recording_path[1], F_OK) < 0 && errno == ENOENT) {
            return 0;
        }
        now++;
    }
    errno = EEXIST;
    return -1;
}

static void thermal_frame(const uint8_t *display_yuyv,
                          const uint16_t *radiometric_y16,
                          const struct timespec *captured_at, void *opaque)
{
    struct ca_media_impl *media = opaque;
    struct ca_thermal_range range = {0};
    unsigned frame_number;
    td_s32 result;
    if (ca_mt11_thermal_range_from_y16(
            radiometric_y16, CA_MT11_THERMAL_WIDTH,
            CA_MT11_THERMAL_HEIGHT, &range)) {
        pthread_mutex_lock(&media->lock);
        range.frame_sequence = media->thermal_range.frame_sequence + 1U;
        media->thermal_range = range;
        media->thermal_range_valid = true;
        memcpy(media->thermal_latest, radiometric_y16,
               CA_MT11_THERMAL_WIDTH * CA_MT11_THERMAL_HEIGHT *
                   sizeof(*radiometric_y16));
        media->thermal_latest_timestamp = *captured_at;
        media->thermal_latest_valid = true;
        pthread_mutex_unlock(&media->lock);
        if (ca_raw_thermal_server_publish(media->raw_thermal_server,
                                          radiometric_y16,
                                          captured_at) < 0) {
            ca_log("raw thermal TCP publish failed: %s", strerror(errno));
        }
    }
    if (!atomic_exchange(&media->thermal_frame_logged, true)) {
        ca_log("thermal USB first complete frame received");
    }
    frame_number = atomic_fetch_add(&media->thermal_frames, 1U) + 1U;
    result = ca_mt11_thermal_send_yuyv(display_yuyv);
    if (result != TD_SUCCESS) {
        if (!atomic_exchange(&media->thermal_send_error_logged, true)) {
            ca_log("thermal frame submission to VPSS failed: 0x%x; further failures suppressed",
                   (unsigned)result);
        }
    } else if (frame_number == CA_MT11_THERMAL_FRAME_RATE) {
        ca_log("thermal capture sustained through frame %u", frame_number);
    }
}

static void media_cleanup(struct ca_media_impl *media)
{
    atomic_store(&media->manual_focus_direction, 0);
    if (media->autofocus_thread_started) {
        atomic_store(&media->autofocus_cancel, true);
        pthread_join(media->autofocus_thread, NULL);
        media->autofocus_thread_started = false;
    }
    if (media->manual_focus_thread_started) {
        pthread_join(media->manual_focus_thread, NULL);
        media->manual_focus_thread_started = false;
    }
    ca_overlay_hw_close(media->overlay);
    media->overlay=NULL;
    ca_e5739_close(media->e5739);
    media->e5739 = NULL;
    ca_mt11_thermal_close(media->thermal);
    media->thermal = NULL;
    ca_raw_thermal_server_close(media->raw_thermal_server);
    media->raw_thermal_server = NULL;
    ca_raw_thermal_close(media->raw_thermal);
    media->raw_thermal = NULL;
    free(media->thermal_latest);
    media->thermal_latest = NULL;
    if (media->thread_started) {
        atomic_store(&media->stop, true);
        pthread_join(media->thread, NULL);
        media->thread_started = false;
    }
    ca_live_video_server_close(media->live_video);
    media->live_video = NULL;
    (void)ca_media_impl_set_recording(media, false);
    ca_rtsp_close(media->rtsp);
    media->rtsp = NULL;
    for (int channel = (int)CA_MT11_VENC_COUNT - 1; channel >= 0; channel--) {
        if (media->venc_started[channel]) {
            (void)ss_mpi_venc_stop_chn(channel);
        }
    }
    bool thermal_main = atomic_load(&media->thermal_main);
    if (media->record_thermal_bound) {
        (void)sample_comm_vpss_un_bind_venc(CA_MT11_THERMAL_GROUP,
                                             CA_MT11_RTSP_SUB_CHN,
                                             CA_MT11_THERMAL_RECORD_VENC);
    }
    if (media->record_rgb_bound) {
        (void)sample_comm_vpss_un_bind_venc(media->selected_group,
                                             CA_MT11_RECORD_CHN,
                                             CA_MT11_RGB_RECORD_VENC);
    }
    if (media->rtsp_sub_bound) {
        (void)sample_comm_vpss_un_bind_venc(
            thermal_main ? media->selected_group : CA_MT11_THERMAL_GROUP,
            CA_MT11_RTSP_SUB_CHN, CA_MT11_SUB_VENC);
    }
    if (media->rtsp_main_bound) {
        (void)sample_comm_vpss_un_bind_venc(
            thermal_main ? CA_MT11_THERMAL_GROUP : media->selected_group,
            thermal_main ? CA_MT11_RTSP_SUB_CHN : CA_MT11_RTSP_MAIN_CHN,
            CA_MT11_MAIN_VENC);
    }
    for (int channel = (int)CA_MT11_VENC_COUNT - 1; channel >= 0; channel--) {
        if (media->venc_created[channel]) {
            (void)ss_mpi_venc_destroy_chn(channel);
        }
    }
    if (media->scene_started) ca_mt11_scene_stop();
    if (media->thermal_vpss_started) ca_mt11_thermal_vpss_stop();
    for (int path = 1; path >= 0; path--) {
        if (media->vpss_started[path]) ca_mt11_vpss_stop(path);
    }
    for (int path = 1; path >= 0; path--) {
        if (media->vi_vpss_bound[path]) {
            (void)sample_comm_vi_un_bind_vpss(path, 0, path, 0);
        }
    }
    for (int path = 1; path >= 0; path--) {
        if (media->vi_started[path]) sample_comm_vi_stop_vi(&media->vi_cfg[path]);
    }
    if (media->sys_started) sample_comm_sys_exit();
}

static unsigned stream_bitrate(unsigned width, unsigned height)
{
    if (width >= 3840U || height >= 2160U) return 12000U;
    if (width >= 1920U || height >= 1080U) return 4096U;
    return 2048U;
}

int ca_media_impl_open(struct ca_media_impl **result, const struct ca_media_config *config)
{
    struct ca_media_impl *media;
    struct ca_mt11_output_sizes sizes = {0};
    unsigned main_width, main_height, sub_width, sub_height;
    unsigned record_width, record_height;
    ot_venc_start_param start = {.recv_pic_num = -1};
    const char *stage = "argument validation";
    if (result == NULL || config == NULL || config->backend == NULL ||
        strcmp(config->backend, "mt11") != 0 ||
        config->capture_root == NULL || config->record_root == NULL ||
        config->frame_rate != 30U) {
        errno = EINVAL;
        return -1;
    }
    media = calloc(1, sizeof(*media));
    if (media == NULL) return -1;
    media->config = *config;
    ca_video_resolution_size(config->settings.main_resolution,
                             &main_width, &main_height);
    ca_video_resolution_size(config->settings.sub_resolution,
                             &sub_width, &sub_height);
    ca_video_resolution_size(config->settings.recording_resolution,
                             &record_width, &record_height);
    sizes.width[CA_MT11_RTSP_MAIN_CHN] = main_width;
    sizes.height[CA_MT11_RTSP_MAIN_CHN] = main_height;
    sizes.width[CA_MT11_RTSP_SUB_CHN] = sub_width;
    sizes.height[CA_MT11_RTSP_SUB_CHN] = sub_height;
    sizes.width[CA_MT11_RECORD_CHN] = record_width;
    sizes.height[CA_MT11_RECORD_CHN] = record_height;
    pthread_mutex_init(&media->lock, NULL);
    pthread_mutex_init(&media->venc_lock, NULL);
    pthread_mutex_init(&media->actuator_lock, NULL);
    pthread_mutex_init(&media->photo_lock, NULL);
    stage = "raw thermal capture allocation";
    media->thermal_latest = malloc(CA_MT11_THERMAL_WIDTH *
                                   CA_MT11_THERMAL_HEIGHT *
                                   sizeof(*media->thermal_latest));
    if (media->thermal_latest == NULL ||
        ca_raw_thermal_open(&media->raw_thermal, config->capture_root) < 0) {
        goto fail;
    }
    stage = "raw thermal TCP startup";
    if (ca_raw_thermal_server_open(&media->raw_thermal_server,
                                   config->capture_root, 7345U,
                                   CA_MT11_THERMAL_WIDTH,
                                   CA_MT11_THERMAL_HEIGHT) < 0) {
        goto fail;
    }
    ca_mt11_configure_vi(&media->vi_cfg[CA_MT11_ZOOM_PIPE], TD_FALSE);
    ca_mt11_configure_vi(&media->vi_cfg[CA_MT11_WIDE_PIPE], TD_TRUE);
    stage = "SYS/VB initialization";
    if (ca_mt11_system_init() != TD_SUCCESS) goto fail;
    media->sys_started = TD_TRUE;
    stage = "MIPI/VI/ISP startup";
    for (int path = 0; path < 2; path++) {
        if (sample_comm_vi_start_vi(&media->vi_cfg[path]) != TD_SUCCESS) goto fail;
        media->vi_started[path] = TD_TRUE;
    }
    stage = "VI-to-VPSS binding";
    for (int path = 0; path < 2; path++) {
        if (sample_comm_vi_bind_vpss(path, 0, path, 0) != TD_SUCCESS) goto fail;
        media->vi_vpss_bound[path] = TD_TRUE;
    }
    stage = "VPSS startup";
    for (int path = 0; path < 2; path++) {
        if (ca_mt11_vpss_start(path, &sizes) != TD_SUCCESS) goto fail;
        media->vpss_started[path] = TD_TRUE;
    }
    stage = "factory ISP scene startup";
    if (ca_mt11_scene_start(MT11_FACTORY_SCENE_DIR) != TD_SUCCESS) goto fail;
    media->scene_started = TD_TRUE;
    stage = "ISP configuration";
    if (ca_mt11_apply_isp_config(&config->settings, false) != TD_SUCCESS) goto fail;
    stage = "thermal VPSS startup";
    if (ca_mt11_thermal_vpss_start(&sizes) != TD_SUCCESS) goto fail;
    media->thermal_vpss_started = TD_TRUE;
    ca_mt11_video_attributes(&media->venc_attr[CA_MT11_MAIN_VENC],
                             config->settings.main_codec, main_width,
                             main_height, 30U,
                             stream_bitrate(main_width, main_height));
    ca_mt11_video_attributes(&media->venc_attr[CA_MT11_SUB_VENC],
                             config->settings.sub_codec,
                             CA_MT11_THERMAL_ENCODE_WIDTH,
                             CA_MT11_THERMAL_ENCODE_HEIGHT,
                             CA_MT11_THERMAL_FRAME_RATE,
                             stream_bitrate(CA_MT11_THERMAL_ENCODE_WIDTH,
                                            CA_MT11_THERMAL_ENCODE_HEIGHT));
    /* The sub URL starts on thermal, but can later be switched to its
     * configured RGB size.  SS928 fixes these maxima and the stream buffer
     * when the VENC channel is created. */
    if (sub_width > media->venc_attr[CA_MT11_SUB_VENC].venc_attr.max_pic_width) {
        media->venc_attr[CA_MT11_SUB_VENC].venc_attr.max_pic_width = sub_width;
    }
    if (sub_height > media->venc_attr[CA_MT11_SUB_VENC].venc_attr.max_pic_height) {
        media->venc_attr[CA_MT11_SUB_VENC].venc_attr.max_pic_height = sub_height;
    }
    media->venc_attr[CA_MT11_SUB_VENC].venc_attr.buf_size = OT_ALIGN_UP(
        media->venc_attr[CA_MT11_SUB_VENC].venc_attr.max_pic_width *
        media->venc_attr[CA_MT11_SUB_VENC].venc_attr.max_pic_height * 3U / 4U,
        64U);
    ca_mt11_video_attributes(&media->venc_attr[CA_MT11_RGB_RECORD_VENC],
                             CA_VIDEO_H264, record_width, record_height, 30U,
                             stream_bitrate(record_width, record_height));
    ca_mt11_video_attributes(&media->venc_attr[CA_MT11_THERMAL_RECORD_VENC],
                             CA_VIDEO_H264, CA_MT11_THERMAL_ENCODE_WIDTH,
                             CA_MT11_THERMAL_ENCODE_HEIGHT,
                             CA_MT11_THERMAL_FRAME_RATE,
                             stream_bitrate(CA_MT11_THERMAL_ENCODE_WIDTH,
                                            CA_MT11_THERMAL_ENCODE_HEIGHT));
    stage = "VENC creation";
    for (unsigned channel = 0; channel < CA_MT11_VENC_COUNT; channel++) {
        if (ss_mpi_venc_create_chn(channel, &media->venc_attr[channel]) !=
            TD_SUCCESS) goto fail;
        media->venc_created[channel] = TD_TRUE;
    }
    media->selected_group = CA_MT11_WIDE_GROUP;
    media->lens = CA_MEDIA_LENS_WIDE;
    atomic_store(&media->thermal_main, false);
    media->zoom = 1.0f;
    atomic_store(&media->visible_hfov_deg, CA_VISIBLE_HFOV_DEG);
    media->digital_ratio[CA_MT11_ZOOM_GROUP] = 1.0f;
    media->digital_ratio[CA_MT11_WIDE_GROUP] = 1.0f;
    if (ca_e5739_open(&media->e5739,
                      "/app/cfg/zoom/E5739_focus.txt") < 0) {
        ca_log("E5739 optical zoom unavailable; retaining VPSS digital-crop "
               "fallback (implementation gap; this gap will only be logged "
               "once)");
    }
    stage = "main RTSP VPSS-to-VENC binding";
    if (sample_comm_vpss_bind_venc(media->selected_group,
                                   CA_MT11_RTSP_MAIN_CHN,
                                   CA_MT11_MAIN_VENC) != TD_SUCCESS) {
        goto fail;
    }
    media->rtsp_main_bound = TD_TRUE;
    stage = "sub RTSP VPSS-to-VENC binding";
    if (sample_comm_vpss_bind_venc(CA_MT11_THERMAL_GROUP,
                                   CA_MT11_RTSP_SUB_CHN,
                                   CA_MT11_SUB_VENC) != TD_SUCCESS) {
        goto fail;
    }
    media->rtsp_sub_bound = TD_TRUE;
    stage = "RGB recording VPSS-to-VENC binding";
    if (sample_comm_vpss_bind_venc(media->selected_group,
                                   CA_MT11_RECORD_CHN,
                                   CA_MT11_RGB_RECORD_VENC) != TD_SUCCESS) {
        goto fail;
    }
    media->record_rgb_bound = TD_TRUE;
    stage = "thermal recording VPSS-to-VENC binding";
    if (sample_comm_vpss_bind_venc(CA_MT11_THERMAL_GROUP,
                                   CA_MT11_RTSP_SUB_CHN,
                                   CA_MT11_THERMAL_RECORD_VENC) != TD_SUCCESS) {
        goto fail;
    }
    media->record_thermal_bound = TD_TRUE;
    stage = "VENC startup";
    for (unsigned channel = 0; channel < CA_MT11_VENC_COUNT; channel++) {
        if (ss_mpi_venc_start_chn(channel, &start) != TD_SUCCESS) goto fail;
        media->venc_started[channel] = TD_TRUE;
    }
    stage = "RTSP startup";
    if (ca_rtsp_open(&media->rtsp, config->rtsp_port, "video1",
                     config->settings.main_codec, 30U) < 0) goto fail;
    if (ca_rtsp_add_video(media->rtsp, "video2",
                          config->settings.sub_codec,
                          CA_MT11_THERMAL_FRAME_RATE,
                          &media->thermal_rtsp_stream) < 0) goto fail;
    if (ca_rtsp_support_proxy(media->rtsp, &config->settings.support) < 0)
        ca_log("SupportProxy video startup failed: %s", strerror(errno));
    stage = "native live-video startup";
    if (ca_live_video_server_open(&media->live_video,
                                  CA_MT11_LIVE_VIDEO_PORT) < 0 ||
        ca_live_video_server_configure(
            media->live_video, 0U, main_width, main_height, 30U,
            config->settings.main_codec == CA_VIDEO_H264) < 0 ||
        ca_live_video_server_configure(
            media->live_video, 1U, CA_MT11_THERMAL_ENCODE_WIDTH,
            CA_MT11_THERMAL_ENCODE_HEIGHT, CA_MT11_THERMAL_FRAME_RATE,
            config->settings.sub_codec == CA_VIDEO_H264) < 0) goto fail;
    stage = "thermal USB startup";
    if (ca_mt11_thermal_open(&media->thermal, thermal_frame, media) < 0) {
        goto fail;
    }
    stage = "thermal palette configuration";
    if (ca_mt11_thermal_set_palette(
            media->thermal, (uint8_t)config->settings.thermal_palette) < 0) {
        goto fail;
    }
    if (config->settings.orientation == CA_MOUNT_INVERTED &&
        ca_media_impl_set_inverted(media, true) < 0) {
        stage = "inverted output configuration";
        goto fail;
    }
    stage = "capture-thread startup";
    if (pthread_create(&media->thread, NULL, capture_thread, media) != 0) goto fail;
    media->thread_started = true;
    (void)ss_mpi_venc_request_idr(0, TD_TRUE);
    (void)ss_mpi_venc_request_idr(CA_MT11_SUB_VENC, TD_TRUE);
    ca_log("media ready: /video1 %ux%u/30 %s; /video2 %ux%u/25 %s; "
           "recording %ux%u H.264; RTSP port %u", main_width, main_height,
           ca_video_codec_name(config->settings.main_codec),
           CA_MT11_THERMAL_ENCODE_WIDTH, CA_MT11_THERMAL_ENCODE_HEIGHT,
           ca_video_codec_name(config->settings.sub_codec),
           record_width, record_height, config->rtsp_port);
    *result = media;
    return 0;
fail:
    ca_log("media startup failed at %s", stage);
    media_cleanup(media);
    pthread_mutex_destroy(&media->photo_lock);
    pthread_mutex_destroy(&media->actuator_lock);
    pthread_mutex_destroy(&media->venc_lock);
    pthread_mutex_destroy(&media->lock);
    free(media);
    errno = EIO;
    return -1;
}

int ca_media_impl_set_recording(struct ca_media_impl *media, bool active)
{
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
        unsigned record_width, record_height;
        ca_video_resolution_size(media->config.settings.recording_resolution,
                                 &record_width, &record_height);
        if (make_recording_path(media) < 0 ||
            ca_mp4_open(&media->mp4[0], media->recording_path[0],
                        record_width, record_height,
                        media->config.frame_rate) < 0) {
            pthread_mutex_unlock(&media->lock);
            return -1;
        }
        if (ca_mp4_open(&media->mp4[1], media->recording_path[1],
                        CA_MT11_THERMAL_ENCODE_WIDTH,
                        CA_MT11_THERMAL_ENCODE_HEIGHT,
                        CA_MT11_THERMAL_FRAME_RATE) < 0) {
            int saved_errno = errno;
            (void)ca_mp4_close(media->mp4[0]);
            media->mp4[0] = NULL;
            (void)unlink(media->recording_path[0]);
            pthread_mutex_unlock(&media->lock);
            errno = saved_errno;
            return -1;
        }
        atomic_store(&media->recording, true);
        media->recording_wait_keyframe[0] = true;
        media->recording_wait_keyframe[1] = true;
        (void)ss_mpi_venc_request_idr(CA_MT11_RGB_RECORD_VENC, TD_TRUE);
        (void)ss_mpi_venc_request_idr(CA_MT11_THERMAL_RECORD_VENC, TD_TRUE);
        ca_log("recording started RGB=%s thermal=%s",
               media->recording_path[0], media->recording_path[1]);
    } else {
        int result = 0;
        for (unsigned channel = 0; channel < 2U; channel++) {
            if (ca_mp4_close(media->mp4[channel]) < 0) result = -1;
            media->mp4[channel] = NULL;
        }
        atomic_store(&media->recording, false);
        media->recording_wait_keyframe[0] = false;
        media->recording_wait_keyframe[1] = false;
        ca_log("recording stopped RGB=%s thermal=%s",
               media->recording_path[0], media->recording_path[1]);
        pthread_mutex_unlock(&media->lock);
        return result;
    }
    pthread_mutex_unlock(&media->lock);
    return 0;
}

bool ca_media_impl_recording(const struct ca_media_impl *media)
{
    return media != NULL && atomic_load(&media->recording);
}

const char *ca_media_impl_recording_path(const struct ca_media_impl *media)
{
    return media != NULL ? media->recording_path[0] : "";
}

bool ca_media_impl_thermal_range(struct ca_media_impl *media,
                            struct ca_thermal_range *range)
{
    bool valid;

    if (media == NULL || range == NULL) return false;
    pthread_mutex_lock(&media->lock);
    valid = media->thermal_range_valid;
    if (valid) *range = media->thermal_range;
    pthread_mutex_unlock(&media->lock);
    return valid;
}

static void jpeg_attributes(ot_venc_chn_attr *attr, td_u32 width,
                            td_u32 height)
{
    memset(attr, 0, sizeof(*attr));
    attr->venc_attr.type = OT_PT_JPEG;
    attr->venc_attr.max_pic_width = width;
    attr->venc_attr.max_pic_height = height;
    attr->venc_attr.pic_width = width;
    attr->venc_attr.pic_height = height;
    attr->venc_attr.buf_size = OT_ALIGN_UP(width, 16U) *
                               OT_ALIGN_UP(height, 16U) * 4U;
    attr->venc_attr.is_by_frame = TD_TRUE;
    attr->venc_attr.jpeg_attr.dcf_en = TD_FALSE;
    attr->venc_attr.jpeg_attr.mpf_cfg.large_thumbnail_num = 0;
    attr->venc_attr.jpeg_attr.recv_mode = OT_VENC_PIC_RECV_SINGLE;
    attr->gop_attr.gop_mode = OT_VENC_GOP_MODE_NORMAL_P;
}

static int capture_group_jpeg(struct ca_media_impl *media, ot_vpss_grp group,
                              ot_vpss_chn channel, td_u32 width,
                              td_u32 height, char suffix)
{
    ot_venc_chn_attr attr;
    ot_venc_start_param start = {.recv_pic_num = 1};
    ot_video_frame_info frame = {0};
    ot_venc_chn_status status = {0};
    ot_venc_stream stream = {0};
    struct timespec captured_at;
    char path[PATH_MAX];
    unsigned char *jpeg = NULL;
    size_t jpeg_length = 0;
    bool created = false;
    bool started = false;
    bool frame_acquired = false;
    bool stream_acquired = false;
    const char *stage = "VENC creation";
    td_s32 code;
    int result = -1;

    jpeg_attributes(&attr, width, height);
    code = ss_mpi_venc_create_chn(CA_MT11_STILL_VENC, &attr);
    if (code != TD_SUCCESS) goto mpi_fail;
    created = true;
    stage = "VENC startup";
    code = ss_mpi_venc_start_chn(CA_MT11_STILL_VENC, &start);
    if (code != TD_SUCCESS) goto mpi_fail;
    started = true;
    stage = "VPSS frame acquisition";
    code = ss_mpi_vpss_get_chn_frame(group, channel, &frame, 1000);
    if (code != TD_SUCCESS) goto mpi_fail;
    frame_acquired = true;
    if (clock_gettime(CLOCK_REALTIME, &captured_at) < 0) goto done;
    stage = "JPEG frame submission";
    code = ss_mpi_venc_send_frame(CA_MT11_STILL_VENC, &frame, 1000);
    if (code != TD_SUCCESS) goto mpi_fail;
    (void)ss_mpi_vpss_release_chn_frame(group, channel, &frame);
    frame_acquired = false;

    stage = "JPEG completion";
    for (unsigned attempt = 0; attempt < 50U; attempt++) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 20000000L};
        code = ss_mpi_venc_query_status(CA_MT11_STILL_VENC, &status);
        if (code != TD_SUCCESS) goto mpi_fail;
        if (status.cur_packs != 0U) break;
        (void)nanosleep(&delay, NULL);
    }
    if (status.cur_packs == 0U || status.cur_packs > 128U) {
        code = TD_FAILURE;
        goto mpi_fail;
    }
    stream.pack = calloc(status.cur_packs, sizeof(*stream.pack));
    if (stream.pack == NULL) goto done;
    stream.pack_cnt = status.cur_packs;
    stage = "JPEG stream retrieval";
    code = ss_mpi_venc_get_stream(CA_MT11_STILL_VENC, &stream, 1000);
    if (code != TD_SUCCESS) goto mpi_fail;
    stream_acquired = true;
    for (td_u32 i = 0; i < stream.pack_cnt; i++) {
        const ot_venc_pack *pack = &stream.pack[i];
        if (pack->offset > pack->len ||
            jpeg_length > SIZE_MAX - (pack->len - pack->offset)) {
            errno = EOVERFLOW;
            goto done;
        }
        jpeg_length += pack->len - pack->offset;
    }
    jpeg = malloc(jpeg_length);
    if (jpeg == NULL) goto done;
    for (td_u32 i = 0, offset = 0; i < stream.pack_cnt; i++) {
        const ot_venc_pack *pack = &stream.pack[i];
        td_u32 bytes = pack->len - pack->offset;
        memcpy(jpeg + offset, pack->addr + pack->offset, bytes);
        offset += bytes;
    }
    if (ca_still_write_jpeg(media->config.capture_root, suffix, jpeg,
                            jpeg_length, &captured_at, path,
                            sizeof(path)) < 0) {
        ca_log("%c JPEG write failed: %s", suffix, strerror(errno));
        goto done;
    }
    ca_log("%c JPEG capture saved: %s (%zu bytes)", suffix, path,
           jpeg_length);
    result = 0;
    goto done;

mpi_fail:
    ca_log("%c JPEG capture failed at %s: 0x%x", suffix, stage,
           (unsigned)code);
    errno = EIO;
done:
    {
        int saved_errno = errno;
        free(jpeg);
        if (stream_acquired) {
            (void)ss_mpi_venc_release_stream(CA_MT11_STILL_VENC, &stream);
        }
        free(stream.pack);
        if (frame_acquired) {
            (void)ss_mpi_vpss_release_chn_frame(group, channel, &frame);
        }
        if (started) (void)ss_mpi_venc_stop_chn(CA_MT11_STILL_VENC);
        if (created) (void)ss_mpi_venc_destroy_chn(CA_MT11_STILL_VENC);
        errno = saved_errno;
    }
    return result;
}

static int capture_raw_thermal(struct ca_media_impl *media)
{
    const size_t bytes = CA_MT11_THERMAL_WIDTH * CA_MT11_THERMAL_HEIGHT *
                         sizeof(*media->thermal_latest);
    uint16_t *snapshot;
    struct timespec captured_at;
    unsigned count;

    snapshot = malloc(bytes);
    if (snapshot == NULL) return -1;
    pthread_mutex_lock(&media->lock);
    if (!media->thermal_latest_valid) {
        pthread_mutex_unlock(&media->lock);
        free(snapshot);
        errno = ENODATA;
        return -1;
    }
    memcpy(snapshot, media->thermal_latest, bytes);
    captured_at = media->thermal_latest_timestamp;
    pthread_mutex_unlock(&media->lock);
    if (ca_raw_thermal_write_at(media->raw_thermal, snapshot,
                                CA_MT11_THERMAL_WIDTH,
                                CA_MT11_THERMAL_HEIGHT, &captured_at) < 0) {
        free(snapshot);
        return -1;
    }
    free(snapshot);
    count = ca_raw_thermal_count(media->raw_thermal);
    if (count == 1U || count % 100U == 0U) {
        ca_log("raw thermal capture %u saved: %s", count,
               ca_raw_thermal_path(media->raw_thermal));
    }
    return 0;
}

int ca_media_impl_capture_photo(struct ca_media_impl *media, enum ca_photo_scope scope)
{
    int result = 0;
    int failure_errno = 0;
    unsigned width, height;

    if (media == NULL || (scope != CA_PHOTO_SCOPE_THERMAL &&
                          scope != CA_PHOTO_SCOPE_ALL)) {
        errno = EINVAL;
        return -1;
    }
    ca_video_resolution_size(media->config.settings.main_resolution,
                             &width, &height);
    pthread_mutex_lock(&media->photo_lock);
    if (scope == CA_PHOTO_SCOPE_ALL) {
        if (capture_group_jpeg(media, CA_MT11_WIDE_GROUP,
                               CA_MT11_RTSP_MAIN_CHN,
                               width, height, 'C') < 0) {
            result = -1;
            failure_errno = errno;
        }
        if (capture_group_jpeg(media, CA_MT11_ZOOM_GROUP,
                               CA_MT11_RTSP_MAIN_CHN,
                               width, height, 'Z') < 0) {
            result = -1;
            if (failure_errno == 0) failure_errno = errno;
        }
        if (capture_group_jpeg(media, CA_MT11_THERMAL_GROUP,
                               CA_MT11_RTSP_SUB_CHN,
                               CA_MT11_THERMAL_ENCODE_WIDTH,
                               CA_MT11_THERMAL_ENCODE_HEIGHT, 'I') < 0) {
            result = -1;
            if (failure_errno == 0) failure_errno = errno;
        }
    }
    if (capture_raw_thermal(media) < 0) {
        result = -1;
        if (failure_errno == 0) failure_errno = errno;
    }
    pthread_mutex_unlock(&media->photo_lock);
    if (failure_errno != 0) errno = failure_errno;
    return result;
}

int ca_media_impl_get_thermal_gain(struct ca_media_impl *media, uint8_t *gain)
{
    if (media == NULL || gain == NULL) {
        errno = EINVAL;
        return -1;
    }
    return ca_mt11_thermal_get_gain(media->thermal, gain);
}

int ca_media_impl_set_thermal_gain(struct ca_media_impl *media, uint8_t gain)
{
    if (media == NULL) {
        errno = EINVAL;
        return -1;
    }
    return ca_mt11_thermal_set_gain(media->thermal, gain);
}

int ca_media_impl_get_thermal_palette(struct ca_media_impl *media, uint8_t *palette)
{
    if (media == NULL || palette == NULL) {
        errno = EINVAL;
        return -1;
    }
    return ca_mt11_thermal_get_palette(media->thermal, palette);
}

int ca_media_impl_set_thermal_palette(struct ca_media_impl *media, uint8_t palette)
{
    if (media == NULL) {
        errno = EINVAL;
        return -1;
    }
    return ca_mt11_thermal_set_palette(media->thermal, palette);
}

int ca_media_impl_set_inverted(struct ca_media_impl *media, bool inverted)
{
    td_s32 result;
    if (media == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (atomic_load(&media->inverted) == inverted) return 0;
    result = ca_mt11_set_inverted(inverted ? TD_TRUE : TD_FALSE);
    if (result != TD_SUCCESS) {
        ca_log("output orientation change failed: 0x%x", (unsigned)result);
        errno = EIO;
        return -1;
    }
    atomic_store(&media->inverted, inverted);
    ca_log("output orientation=%s", inverted ? "inverted" : "upright");
    return 0;
}

static int select_lens_locked(struct ca_media_impl *media, enum ca_media_lens lens)
{
    ot_vpss_grp group = lens == CA_MEDIA_LENS_WIDE ? CA_MT11_WIDE_GROUP
                                                    : CA_MT11_ZOOM_GROUP;

    if (group != media->selected_group) {
        ot_vpss_grp old_group = media->selected_group;
        bool thermal_main = atomic_load(&media->thermal_main);
        ot_vpss_chn rtsp_chn = thermal_main ? CA_MT11_RTSP_SUB_CHN
                                             : CA_MT11_RTSP_MAIN_CHN;
        ot_venc_chn rtsp_venc = thermal_main ? CA_MT11_SUB_VENC
                                              : CA_MT11_MAIN_VENC;
        td_s32 unbind_rtsp = sample_comm_vpss_un_bind_venc(
            old_group, rtsp_chn, rtsp_venc);
        td_s32 unbind_record = sample_comm_vpss_un_bind_venc(
            old_group, CA_MT11_RECORD_CHN, CA_MT11_RGB_RECORD_VENC);
        td_s32 bind_rtsp = TD_FAILURE;
        td_s32 bind_record = TD_FAILURE;
        if (unbind_rtsp == TD_SUCCESS && unbind_record == TD_SUCCESS) {
            bind_rtsp = sample_comm_vpss_bind_venc(group, rtsp_chn,
                                                    rtsp_venc);
            if (bind_rtsp == TD_SUCCESS) {
                bind_record = sample_comm_vpss_bind_venc(
                    group, CA_MT11_RECORD_CHN, CA_MT11_RGB_RECORD_VENC);
            }
        }
        if (unbind_rtsp != TD_SUCCESS || unbind_record != TD_SUCCESS ||
            bind_rtsp != TD_SUCCESS || bind_record != TD_SUCCESS) {
            ca_log("RGB lens switch VPSS %d->%d failed: rtsp unbind=0x%x "
                   "record unbind=0x%x rtsp bind=0x%x record bind=0x%x",
                   old_group, group, (unsigned)unbind_rtsp,
                   (unsigned)unbind_record, (unsigned)bind_rtsp,
                   (unsigned)bind_record);
            if (bind_rtsp == TD_SUCCESS) {
                (void)sample_comm_vpss_un_bind_venc(group, rtsp_chn,
                                                     rtsp_venc);
            }
            if (bind_record == TD_SUCCESS) {
                (void)sample_comm_vpss_un_bind_venc(
                    group, CA_MT11_RECORD_CHN, CA_MT11_RGB_RECORD_VENC);
            }
            (void)sample_comm_vpss_bind_venc(old_group, rtsp_chn, rtsp_venc);
            (void)sample_comm_vpss_bind_venc(
                old_group, CA_MT11_RECORD_CHN, CA_MT11_RGB_RECORD_VENC);
            errno = EIO;
            return -1;
        }
        media->selected_group = group;
    }
    media->lens = lens;
    float hfov = lens == CA_MEDIA_LENS_ZOOM
        ? ca_zoom_lens_hfov(media->e5739 != NULL ? ca_e5739_zoom(media->e5739) : 1.0f,
                            media->digital_ratio[group])
        : ca_lens1_hfov(media->digital_ratio[group]);
    atomic_store(&media->visible_hfov_deg, hfov);
    return 0;
}

int ca_media_impl_set_zoom(struct ca_media_impl *media, float zoom)
{
    enum ca_media_lens lens;
    ot_vpss_grp group;
    float requested_optical;
    float optical_ratio = 1.0f;
    float digital_ratio;

    if (media == NULL || !isfinite(zoom) || zoom < 1.0f || zoom > 10.0f) {
        errno = EINVAL;
        return -1;
    }
    lens = ca_e5739_system_uses_tele(zoom) ? CA_MEDIA_LENS_ZOOM
                                           : CA_MEDIA_LENS_WIDE;
    group = lens == CA_MEDIA_LENS_WIDE ? CA_MT11_WIDE_GROUP
                                        : CA_MT11_ZOOM_GROUP;
    requested_optical = ca_e5739_system_to_optical(zoom);
    digital_ratio = lens == CA_MEDIA_LENS_WIDE
                        ? zoom
                        : (media->e5739 != NULL
                               ? 1.0f
                               : zoom / CA_E5739_SYSTEM_CROSSOVER);
    pthread_mutex_lock(&media->actuator_lock);
    pthread_mutex_lock(&media->lock);
    if (media->e5739 != NULL &&
        ca_e5739_set_zoom(media->e5739, requested_optical,
                          &optical_ratio) < 0) {
        ca_log("E5739 optical zoom command %.2fx for system %.2fx failed: %s",
               requested_optical, zoom,
               strerror(errno));
        pthread_mutex_unlock(&media->lock);
        pthread_mutex_unlock(&media->actuator_lock);
        return -1;
    }
    if (fabsf(digital_ratio - media->digital_ratio[group]) > 0.001f) {
        td_s32 crop_result = ca_mt11_set_digital_zoom(group, digital_ratio);
        if (crop_result != TD_SUCCESS) {
            float rollback_optical;
            ca_log("VPSS group %d digital crop %.2fx failed: 0x%x", group,
                   digital_ratio, (unsigned)crop_result);
            if (media->e5739 != NULL &&
                ca_e5739_set_zoom(
                    media->e5739,
                    ca_e5739_system_to_optical(media->zoom),
                                  &rollback_optical) < 0) {
                ca_log("E5739 rollback to %.2fx also failed: %s", media->zoom,
                       strerror(errno));
            }
            pthread_mutex_unlock(&media->lock);
            pthread_mutex_unlock(&media->actuator_lock);
            errno = EIO;
            return -1;
        }
        media->digital_ratio[group] = digital_ratio;
    }
    if (select_lens_locked(media, lens) < 0) {
        pthread_mutex_unlock(&media->lock);
        pthread_mutex_unlock(&media->actuator_lock);
        return -1;
    }
    if (switch_rtsp_sources_locked(media, false) < 0) {
        pthread_mutex_unlock(&media->lock);
        pthread_mutex_unlock(&media->actuator_lock);
        return -1;
    }
    media->zoom = zoom;
    (void)ss_mpi_venc_request_idr(0, TD_TRUE);
    ca_log("RGB zoom=%.1fx lens=%s optical=%.2fx digital_crop=%.2fx",
           zoom, media->lens == CA_MEDIA_LENS_WIDE ? "wide" : "zoom",
           optical_ratio, digital_ratio);
    pthread_mutex_unlock(&media->lock);
    pthread_mutex_unlock(&media->actuator_lock);
    return 0;
}

float ca_media_impl_hfov(const struct ca_media_impl *media, bool thermal)
{
    if (media == NULL) return 0.0f;
    return thermal ? CA_THERMAL_HFOV_DEG : atomic_load(&media->visible_hfov_deg);
}

float ca_media_impl_zoom(const struct ca_media_impl *media)
{
    return media != NULL ? media->zoom : 1.0f;
}

int ca_media_impl_set_lens(struct ca_media_impl *media, enum ca_media_lens lens)
{
    if (media == NULL || (lens != CA_MEDIA_LENS_WIDE &&
                          lens != CA_MEDIA_LENS_ZOOM)) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&media->lock);
    if (select_lens_locked(media, lens) < 0) {
        pthread_mutex_unlock(&media->lock);
        return -1;
    }
    if (switch_rtsp_sources_locked(media, false) < 0) {
        pthread_mutex_unlock(&media->lock);
        return -1;
    }
    (void)ss_mpi_venc_request_idr(0, TD_TRUE);
    ca_log("RGB lens=%s zoom=%.1fx zoom-sensor digital_crop=%.2fx",
           lens == CA_MEDIA_LENS_WIDE ? "wide" : "zoom", media->zoom,
           media->digital_ratio[CA_MT11_ZOOM_GROUP]);
    pthread_mutex_unlock(&media->lock);
    return 0;
}

enum ca_media_lens ca_media_impl_lens(const struct ca_media_impl *media)
{
    return media != NULL ? media->lens : CA_MEDIA_LENS_WIDE;
}

static td_s32 reconfigure_rtsp_venc(struct ca_media_impl *media,
                                    unsigned channel,
                                    const ot_venc_chn_attr *new_attr)
{
    const ot_venc_start_param start = {.recv_pic_num = -1};
    ot_venc_chn_attr old_attr = media->venc_attr[channel];
    td_s32 result;

    pthread_mutex_lock(&media->venc_lock);
    result = ss_mpi_venc_stop_chn(channel);
    if (result == TD_SUCCESS) {
        result = ss_mpi_venc_set_chn_attr(channel, new_attr);
    }
    if (result == TD_SUCCESS) {
        result = ss_mpi_venc_start_chn(channel, &start);
    }
    if (result == TD_SUCCESS) {
        media->venc_attr[channel] = *new_attr;
    } else {
        (void)ss_mpi_venc_set_chn_attr(channel, &old_attr);
        (void)ss_mpi_venc_start_chn(channel, &start);
    }
    pthread_mutex_unlock(&media->venc_lock);
    return result;
}

static int switch_rtsp_sources_locked(struct ca_media_impl *media,
                                      bool thermal_main)
{
    bool current = atomic_load(&media->thermal_main);
    ot_vpss_grp old_main = current ? CA_MT11_THERMAL_GROUP
                                   : media->selected_group;
    ot_vpss_grp old_sub = current ? media->selected_group
                                  : CA_MT11_THERMAL_GROUP;
    ot_vpss_grp new_main = thermal_main ? CA_MT11_THERMAL_GROUP
                                        : media->selected_group;
    ot_vpss_grp new_sub = thermal_main ? media->selected_group
                                       : CA_MT11_THERMAL_GROUP;
    ot_vpss_chn old_main_chn = current ? CA_MT11_RTSP_SUB_CHN
                                       : CA_MT11_RTSP_MAIN_CHN;
    ot_vpss_chn old_sub_chn = CA_MT11_RTSP_SUB_CHN;
    ot_vpss_chn new_main_chn = thermal_main ? CA_MT11_RTSP_SUB_CHN
                                             : CA_MT11_RTSP_MAIN_CHN;
    ot_vpss_chn new_sub_chn = CA_MT11_RTSP_SUB_CHN;
    ot_venc_chn_attr old_attr[2] = {
        media->venc_attr[CA_MT11_MAIN_VENC],
        media->venc_attr[CA_MT11_SUB_VENC],
    };
    ot_venc_chn_attr new_attr[2];
    unsigned main_width, main_height, sub_width, sub_height;
    td_s32 result;

    if (current == thermal_main) return 0;
    ca_video_resolution_size(media->config.settings.main_resolution,
                             &main_width, &main_height);
    ca_video_resolution_size(media->config.settings.sub_resolution,
                             &sub_width, &sub_height);
    if (thermal_main) {
        main_width = CA_MT11_THERMAL_ENCODE_WIDTH;
        main_height = CA_MT11_THERMAL_ENCODE_HEIGHT;
    } else {
        sub_width = CA_MT11_THERMAL_ENCODE_WIDTH;
        sub_height = CA_MT11_THERMAL_ENCODE_HEIGHT;
    }
    ca_mt11_video_attributes(&new_attr[CA_MT11_MAIN_VENC],
                             media->config.settings.main_codec,
                             main_width, main_height,
                             thermal_main ? CA_MT11_THERMAL_FRAME_RATE
                                          : media->config.frame_rate,
                             stream_bitrate(main_width, main_height));
    ca_mt11_video_attributes(&new_attr[CA_MT11_SUB_VENC],
                             media->config.settings.sub_codec,
                             sub_width, sub_height,
                             thermal_main ? media->config.frame_rate
                                          : CA_MT11_THERMAL_FRAME_RATE,
                             stream_bitrate(sub_width, sub_height));
    /* Regions must not retain positions outside the resized encoder frame.
     * The public media wrapper restores them after success or rollback. */
    ca_overlay_hw_close(media->overlay);
    media->overlay=NULL;
    /* VENC's maximum geometry and allocation are creation-time properties;
     * only the active picture geometry may change while the channel exists. */
    for (unsigned channel = 0; channel < 2U; channel++) {
        new_attr[channel].venc_attr.max_pic_width =
            old_attr[channel].venc_attr.max_pic_width;
        new_attr[channel].venc_attr.max_pic_height =
            old_attr[channel].venc_attr.max_pic_height;
        new_attr[channel].venc_attr.buf_size =
            old_attr[channel].venc_attr.buf_size;
    }
    result = sample_comm_vpss_un_bind_venc(old_main, old_main_chn,
                                            CA_MT11_MAIN_VENC);
    if (result != TD_SUCCESS) goto fail;
    result = sample_comm_vpss_un_bind_venc(old_sub, old_sub_chn,
                                            CA_MT11_SUB_VENC);
    if (result != TD_SUCCESS) {
        (void)sample_comm_vpss_bind_venc(old_main, old_main_chn,
                                          CA_MT11_MAIN_VENC);
        goto fail;
    }
    result = reconfigure_rtsp_venc(media, CA_MT11_MAIN_VENC,
                                   &new_attr[CA_MT11_MAIN_VENC]);
    if (result != TD_SUCCESS) goto rollback;
    result = reconfigure_rtsp_venc(media, CA_MT11_SUB_VENC,
                                   &new_attr[CA_MT11_SUB_VENC]);
    if (result != TD_SUCCESS) goto restore_encoders;
    result = sample_comm_vpss_bind_venc(new_main, new_main_chn,
                                         CA_MT11_MAIN_VENC);
    if (result != TD_SUCCESS) goto restore_encoders;
    result = sample_comm_vpss_bind_venc(new_sub, new_sub_chn,
                                         CA_MT11_SUB_VENC);
    if (result == TD_SUCCESS) {
        atomic_store(&media->thermal_main, thermal_main);
        (void)ca_live_video_server_configure(
            media->live_video, 0U, main_width, main_height,
            thermal_main ? CA_MT11_THERMAL_FRAME_RATE
                         : media->config.frame_rate,
            media->config.settings.main_codec == CA_VIDEO_H264);
        (void)ca_live_video_server_configure(
            media->live_video, 1U, sub_width, sub_height,
            thermal_main ? media->config.frame_rate
                         : CA_MT11_THERMAL_FRAME_RATE,
            media->config.settings.sub_codec == CA_VIDEO_H264);
        return 0;
    }
    (void)sample_comm_vpss_un_bind_venc(new_main, new_main_chn,
                                         CA_MT11_MAIN_VENC);
restore_encoders:
    (void)reconfigure_rtsp_venc(media, CA_MT11_MAIN_VENC,
                                &old_attr[CA_MT11_MAIN_VENC]);
    (void)reconfigure_rtsp_venc(media, CA_MT11_SUB_VENC,
                                &old_attr[CA_MT11_SUB_VENC]);
rollback:
    (void)sample_comm_vpss_bind_venc(old_main, old_main_chn,
                                      CA_MT11_MAIN_VENC);
    (void)sample_comm_vpss_bind_venc(old_sub, old_sub_chn,
                                      CA_MT11_SUB_VENC);
fail:
    ca_log("RTSP image-slot source switch to %s failed: 0x%x",
           thermal_main ? "thermal-main" : "RGB-main", (unsigned)result);
    errno = EIO;
    return -1;
}

int ca_media_impl_set_thermal_main(struct ca_media_impl *media, bool thermal_main)
{
    if (media == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&media->lock);
    if (thermal_main &&
        select_lens_locked(media, CA_MEDIA_LENS_ZOOM) < 0) {
        pthread_mutex_unlock(&media->lock);
        return -1;
    }
    if (switch_rtsp_sources_locked(media, thermal_main) < 0) {
        pthread_mutex_unlock(&media->lock);
        return -1;
    }
    (void)ca_rtsp_set_frame_rate(media->rtsp, 0U,
                                 thermal_main ? CA_MT11_THERMAL_FRAME_RATE
                                              : media->config.frame_rate);
    (void)ca_rtsp_set_frame_rate(media->rtsp, media->thermal_rtsp_stream,
                                 thermal_main ? media->config.frame_rate
                                              : CA_MT11_THERMAL_FRAME_RATE);
    (void)ss_mpi_venc_request_idr(0, TD_TRUE);
    (void)ss_mpi_venc_request_idr(CA_MT11_SUB_VENC, TD_TRUE);
    ca_log("RTSP image slots: main=%s secondary=%s",
           thermal_main ? "thermal" :
                          (media->lens == CA_MEDIA_LENS_WIDE ? "wide" : "zoom"),
           thermal_main ? "zoom" : "thermal");
    pthread_mutex_unlock(&media->lock);
    return 0;
}

bool ca_media_impl_thermal_main(const struct ca_media_impl *media)
{
    return media != NULL && atomic_load(&media->thermal_main);
}

struct focus_measurement {
    struct ca_media_impl *media;
    uint16_t x;
    uint16_t y;
    uint64_t last_pts;
    int position;
};

struct focus_components {
    uint64_t h1;
    uint64_t h2;
    uint64_t v1;
    uint64_t v2;
};

static struct focus_components focus_score(const ot_isp_af_stats *stats,
                                           uint16_t x, uint16_t y)
{
    unsigned centre_column;
    unsigned centre_row;
    unsigned radius;
    unsigned column_min;
    unsigned column_max;
    unsigned row_min;
    unsigned row_max;
    struct focus_components score = {0};

    if (x == 0U && y == 0U) {
        centre_column = OT_ISP_AF_ZONE_COLUMN / 2U;
        centre_row = OT_ISP_AF_ZONE_ROW / 2U;
        radius = 1U;
    } else {
        centre_column = ((uint32_t)x * OT_ISP_AF_ZONE_COLUMN) /
                        CA_MT11_ENCODE_WIDTH;
        centre_row = ((uint32_t)y * OT_ISP_AF_ZONE_ROW) /
                     CA_MT11_ENCODE_HEIGHT;
        if (centre_column >= OT_ISP_AF_ZONE_COLUMN) {
            centre_column = OT_ISP_AF_ZONE_COLUMN - 1U;
        }
        if (centre_row >= OT_ISP_AF_ZONE_ROW) {
            centre_row = OT_ISP_AF_ZONE_ROW - 1U;
        }
        radius = 1U;
    }
    column_min = centre_column > radius ? centre_column - radius : 0U;
    row_min = centre_row > radius ? centre_row - radius : 0U;
    column_max = centre_column + radius;
    row_max = centre_row + radius;
    if (column_max >= OT_ISP_AF_ZONE_COLUMN) {
        column_max = OT_ISP_AF_ZONE_COLUMN - 1U;
    }
    if (row_max >= OT_ISP_AF_ZONE_ROW) row_max = OT_ISP_AF_ZONE_ROW - 1U;
    for (unsigned row = row_min; row <= row_max; row++) {
        for (unsigned column = column_min; column <= column_max; column++) {
            const ot_isp_focus_zone *zone =
                &stats->be_af_stat.zone_metrics[row][column];
            score.h1 += zone->h1;
            score.h2 += zone->h2;
            score.v1 += zone->v1;
            score.v2 += zone->v2;
        }
    }
    return score;
}

static int move_focus(void *opaque, int position)
{
    struct focus_measurement *measurement = opaque;
    const struct timespec settle = {.tv_sec = 0, .tv_nsec = 50000000L};

    if (atomic_load(&measurement->media->autofocus_cancel)) {
        errno = ECANCELED;
        return -1;
    }
    if (ca_e5739_set_focus_position(measurement->media->e5739, position) < 0) {
        return -1;
    }
    measurement->position = position;
    (void)nanosleep(&settle, NULL);
    return 0;
}

static int measure_focus(void *opaque, uint64_t *score)
{
    struct focus_measurement *measurement = opaque;
    struct focus_components total = {0};
    unsigned fresh_frames = 0;

    while (fresh_frames < 4U) {
        ot_isp_af_stats stats = {0};
        bool got_frame = false;

        for (unsigned attempt = 0; attempt < 50U; attempt++) {
            const struct timespec delay = {.tv_sec = 0,
                                           .tv_nsec = 5000000L};
            td_s32 result;
            if (atomic_load(&measurement->media->autofocus_cancel)) {
                errno = ECANCELED;
                return -1;
            }
            result = ss_mpi_isp_get_focus_stats(CA_MT11_ZOOM_PIPE, &stats);
            if (result != TD_SUCCESS) {
                errno = EIO;
                return -1;
            }
            if (stats.be_af_grid_info.status != 0U &&
                stats.pts != measurement->last_pts) {
                got_frame = true;
                break;
            }
            (void)nanosleep(&delay, NULL);
        }
        if (!got_frame) {
            errno = ETIMEDOUT;
            return -1;
        }
        measurement->last_pts = stats.pts;
        /* Discard the first post-move frame, then average settled frames. */
        if (fresh_frames != 0U) {
            struct focus_components frame =
                focus_score(&stats, measurement->x, measurement->y);
            total.h1 += frame.h1;
            total.h2 += frame.h2;
            total.v1 += frame.v1;
            total.v2 += frame.v2;
        }
        fresh_frames++;
    }
    total.h1 /= 3U;
    total.h2 /= 3U;
    total.v1 /= 3U;
    total.v2 /= 3U;
    *score = total.h1 + total.h2 + total.v1 + total.v2;
    ca_log("autofocus sample position=%d score=%llu h1=%llu h2=%llu "
           "v1=%llu v2=%llu", measurement->position,
           (unsigned long long)*score, (unsigned long long)total.h1,
           (unsigned long long)total.h2, (unsigned long long)total.v1,
           (unsigned long long)total.v2);
    return 0;
}

static void *autofocus_thread(void *opaque)
{
    struct ca_media_impl *media = opaque;
    struct focus_measurement measurement = {
        .media = media,
        .x = media->autofocus_x,
        .y = media->autofocus_y,
    };
    struct ca_autofocus_ops ops = {
        .move = move_focus,
        .measure = measure_focus,
        .opaque = &measurement,
    };
    struct ca_autofocus_result result;
    int minimum;
    int maximum;
    int initial;

    pthread_mutex_lock(&media->actuator_lock);
    if (ca_e5739_focus_range(media->e5739, &minimum, &maximum, &initial) < 0 ||
        ca_autofocus_search(minimum, maximum, initial, 3U, &ops,
                            &result) < 0) {
        ca_log("autofocus failed at point=%u,%u: %s", measurement.x,
               measurement.y, strerror(errno));
    } else {
        ca_log("autofocus complete point=%u,%u range=%d..%d position=%d "
               "score=%llu samples=%u", measurement.x, measurement.y,
               minimum, maximum, result.position,
               (unsigned long long)result.score, result.samples);
    }
    pthread_mutex_unlock(&media->actuator_lock);
    atomic_store(&media->autofocus_running, false);
    return NULL;
}

static void *manual_focus_thread(void *opaque)
{
    struct ca_media_impl *media = opaque;
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 50000000L};
    int current = -1;

    for (;;) {
        int direction = atomic_load(&media->manual_focus_direction);
        int minimum;
        int maximum;
        int target;

        if (direction == 0) break;
        pthread_mutex_lock(&media->actuator_lock);
        direction = atomic_load(&media->manual_focus_direction);
        if (direction == 0) {
            pthread_mutex_unlock(&media->actuator_lock);
            break;
        }
        if (ca_e5739_focus_range(media->e5739, &minimum, &maximum,
                                 &current) < 0) {
            ca_log("manual focus failed reading range: %s", strerror(errno));
            pthread_mutex_unlock(&media->actuator_lock);
            atomic_store(&media->manual_focus_direction, 0);
            break;
        }
        /* SIYI +1 means farther (toward the lower/infinity position). */
        target = current - direction;
        if (target < minimum || target > maximum) {
            ca_log("manual focus reached %s limit position=%d range=%d..%d",
                   direction > 0 ? "far" : "near", current, minimum,
                   maximum);
            pthread_mutex_unlock(&media->actuator_lock);
            atomic_store(&media->manual_focus_direction, 0);
            break;
        }
        if (ca_e5739_set_focus_position(media->e5739, target) < 0) {
            ca_log("manual focus move to position=%d failed: %s", target,
                   strerror(errno));
            pthread_mutex_unlock(&media->actuator_lock);
            atomic_store(&media->manual_focus_direction, 0);
            break;
        }
        current = target;
        pthread_mutex_unlock(&media->actuator_lock);
        (void)nanosleep(&delay, NULL);
    }
    if (current >= 0) ca_log("manual focus stopped position=%d", current);
    else ca_log("manual focus stopped before first step");
    return NULL;
}

int ca_media_impl_manual_focus(struct ca_media_impl *media, int direction)
{
    int thread_result;

    if (media == NULL || direction < -1 || direction > 1) {
        errno = EINVAL;
        return -1;
    }
    if (media->e5739 == NULL) {
        errno = ENOTSUP;
        return -1;
    }
    if (direction == 0) {
        atomic_store(&media->manual_focus_direction, 0);
        if (media->manual_focus_thread_started) {
            pthread_join(media->manual_focus_thread, NULL);
            media->manual_focus_thread_started = false;
        }
        return 0;
    }
    if (atomic_load(&media->autofocus_running)) {
        errno = EBUSY;
        return -1;
    }
    if (media->manual_focus_thread_started &&
        atomic_load(&media->manual_focus_direction) != 0) {
        atomic_store(&media->manual_focus_direction, direction);
        ca_log("manual focus direction changed to %s",
               direction > 0 ? "far" : "near");
        return 0;
    }
    if (media->manual_focus_thread_started) {
        pthread_join(media->manual_focus_thread, NULL);
        media->manual_focus_thread_started = false;
    }
    atomic_store(&media->manual_focus_direction, direction);
    thread_result = pthread_create(&media->manual_focus_thread, NULL,
                                   manual_focus_thread, media);
    if (thread_result != 0) {
        atomic_store(&media->manual_focus_direction, 0);
        errno = thread_result;
        return -1;
    }
    media->manual_focus_thread_started = true;
    ca_log("manual focus started direction=%s",
           direction > 0 ? "far" : "near");
    return 0;
}

int ca_media_impl_set_focus_percent(struct ca_media_impl *media, float percent)
{
    int minimum;
    int maximum;
    int current;
    int target;
    int result;

    if (media == NULL || !isfinite(percent) ||
        percent < 0.0f || percent > 100.0f) {
        errno = EINVAL;
        return -1;
    }
    if (media->e5739 == NULL) {
        errno = ENOTSUP;
        return -1;
    }
    atomic_store(&media->manual_focus_direction, 0);
    if (media->manual_focus_thread_started) {
        pthread_join(media->manual_focus_thread, NULL);
        media->manual_focus_thread_started = false;
    }
    if (atomic_load(&media->autofocus_running)) {
        errno = EBUSY;
        return -1;
    }
    if (media->autofocus_thread_started) {
        pthread_join(media->autofocus_thread, NULL);
        media->autofocus_thread_started = false;
    }
    pthread_mutex_lock(&media->actuator_lock);
    result = ca_e5739_focus_range(media->e5739, &minimum, &maximum,
                                  &current);
    if (result == 0) {
        target = minimum +
                 (int)lroundf((maximum - minimum) * percent / 100.0f);
        result = ca_e5739_set_focus_position(media->e5739, target);
    }
    pthread_mutex_unlock(&media->actuator_lock);
    if (result == 0) {
        ca_log("focus set to %.1f%% position=%d range=%d..%d", percent,
               target, minimum, maximum);
    }
    return result;
}

int ca_media_impl_autofocus(struct ca_media_impl *media, uint16_t x, uint16_t y)
{
    bool expected = false;
    int thread_result;

    if (media == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (media->e5739 == NULL) {
        errno = ENOTSUP;
        return -1;
    }
    if (atomic_load(&media->manual_focus_direction) != 0) {
        errno = EBUSY;
        return -1;
    }
    if (media->manual_focus_thread_started) {
        pthread_join(media->manual_focus_thread, NULL);
        media->manual_focus_thread_started = false;
    }
    if (!atomic_compare_exchange_strong(&media->autofocus_running, &expected,
                                        true)) {
        errno = EBUSY;
        return -1;
    }
    if (media->autofocus_thread_started) {
        pthread_join(media->autofocus_thread, NULL);
        media->autofocus_thread_started = false;
    }
    media->autofocus_x = x;
    media->autofocus_y = y;
    atomic_store(&media->autofocus_cancel, false);
    thread_result = pthread_create(&media->autofocus_thread, NULL,
                                   autofocus_thread, media);
    if (thread_result != 0) {
        atomic_store(&media->autofocus_running, false);
        errno = thread_result;
        return -1;
    }
    media->autofocus_thread_started = true;
    ca_log("autofocus started point=%u,%u", x, y);
    return 0;
}

void ca_media_impl_close(struct ca_media_impl *media)
{
    if (media == NULL) return;
    media_cleanup(media);
    pthread_mutex_destroy(&media->photo_lock);
    pthread_mutex_destroy(&media->actuator_lock);
    pthread_mutex_destroy(&media->venc_lock);
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
    pthread_mutex_lock(&media->lock);
    int result = ca_mt11_apply_isp_config(settings, true);
    if (result == 0) ca_config_copy_image(&media->config.settings, settings);
    pthread_mutex_unlock(&media->lock);
    if (result != 0) { errno = EIO; return -1; }
    return 0;
}

bool ca_media_impl_ready(const struct ca_media_impl *media) { return media != NULL; }

int ca_media_impl_exposure(struct ca_media_impl *media, unsigned lens, struct ca_exposure *s)
{
    pthread_mutex_lock(&media->lock);
    int result=ca_mt11_exposure(lens,s);
    pthread_mutex_unlock(&media->lock);
    return result;
}

int ca_media_impl_apply_overlay(struct ca_media_impl *media, const struct ca_config *settings)
{
    struct ca_overlay_channel channels[4]={0};
    pthread_mutex_lock(&media->lock);
    pthread_mutex_lock(&media->venc_lock);
    bool swap=atomic_load(&media->thermal_main);
    for (unsigned c=0;c<4;c++) {
        bool thermal=c<2 ? ((c==0)==swap) : c==3;
        channels[c]=(struct ca_overlay_channel){
            .width=media->venc_attr[c].venc_attr.pic_width,
            .height=media->venc_attr[c].venc_attr.pic_height,
            .cross=settings->osd_cross && (c<2 || settings->osd_recording),
            .thermal_box=settings->osd_thermal_fov && !thermal && (c<2 || settings->osd_recording),
            .hfov=ca_media_impl_hfov(media,false)};
    }
    int result=ca_overlay_hw_set(&media->overlay,channels,4);
    pthread_mutex_unlock(&media->venc_lock);
    pthread_mutex_unlock(&media->lock);
    return result;
}
