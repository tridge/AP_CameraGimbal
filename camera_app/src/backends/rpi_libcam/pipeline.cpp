/* Raspberry Pi libcamera -> V4L2 M2M H.264 pipeline (Pi 4 / CM4 encoder).
 *
 * libcamera fills YUV420 dmabufs which are queued, without copying, to the
 * bcm2835 encoder's OUTPUT queue. A frame's request is requeued to the camera
 * once the encoder releases its buffer; any pending image controls are
 * attached to that request. The encoder thread delivers Annex B access units
 * from the CAPTURE queue. Sequence headers repeat on every IDR frame so RTSP
 * clients can join at any key frame.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pipeline.h"
#include "camera_app/log.h"

#include <libcamera/libcamera.h>
#include <libcamera/control_ids.h>
#include <libcamera/version.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include "apcam/atomic.h"
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <stdlib.h>

#include <memory>
#include <new>
#include <vector>

using namespace libcamera;

#define RPI_ENCODER_DEVICE "/dev/video11"
#define RPI_CAMERA_BUFFERS 6U
#define RPI_ENCODED_BUFFERS 12U
#define RPI_ENCODED_BUFFER_SIZE (512U * 1024U)

/* Requires libcamera 0.2 or later. 0.5 replaced the RPi convention of zero
 * meaning automatic with explicit exposure/gain modes. */
#define RPI_LIBCAMERA_AT_LEAST(major, minor) \
    (LIBCAMERA_VERSION_MAJOR > (major) || \
     (LIBCAMERA_VERSION_MAJOR == (major) && LIBCAMERA_VERSION_MINOR >= (minor)))

struct ca_rpi_pipeline {
    struct ca_rpi_pipeline_config config;
    std::unique_ptr<CameraManager> manager;
    std::shared_ptr<Camera> camera;
    std::unique_ptr<CameraConfiguration> camera_config;
    std::unique_ptr<FrameBufferAllocator> allocator;
    Stream *stream = nullptr;
    std::vector<std::unique_ptr<Request>> requests;
    bool acquired = false, camera_started = false;

    int encoder = -1;
    void *encoded[RPI_ENCODED_BUFFERS] = {};
    size_t encoded_length[RPI_ENCODED_BUFFERS] = {};
    bool encoder_started = false;

    pthread_t thread {};
    bool thread_started = false;
    atomic_bool stop;

    pthread_mutex_t lock;
    ControlList pending {controls::controls};
    bool have_pending = false;
    struct ca_rpi_image_controls image {};

    /* Stills: camera buffers mapped read-only, and a frame copy requested by
     * ca_rpi_pipeline_grab_still() and filled by request_complete(). */
    std::vector<const uint8_t *> mapped;
    std::vector<size_t> mapped_length;
    pthread_cond_t still_ready;
    struct ca_rpi_still_frame *still = nullptr;
    bool still_done = false;
    atomic_bool still_wanted; /* lock-free check on every frame */

    /* Sensor mode switch for larger stills: video frames stop going to the
     * encoder while paused; still_mode routes completed requests to the
     * still capture. The encoder must return every camera buffer before
     * those buffers are freed. */
    atomic_bool paused, still_mode;
    /* Serialises handing frames to the encoder and requeueing them to the
     * camera with pausing, so a pause cannot overlap either. */
    pthread_mutex_t video_lock;
    atomic_int encoder_queued;
    int32_t last_exposure_us = 0;
    float last_gain = 0.0f, last_colour[2] = {0.0f, 0.0f};
    unsigned still_frames = 0;
};

static int xioctl(int fd, unsigned long request, void *arg)
{
    int result;
    do result = ioctl(fd, request, arg); while (result < 0 && errno == EINTR);
    return result;
}

static uint64_t monotonic_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000U + (uint64_t)t.tv_nsec / 1000U;
}

static void image_control_list(const struct ca_config *settings, ControlList &list,
                               struct ca_rpi_image_controls *mapped)
{
    struct ca_rpi_image_controls c;
    ca_rpi_image_controls(settings, &c);
    list.set(controls::Brightness, c.brightness);
    list.set(controls::Contrast, c.contrast);
    list.set(controls::Saturation, c.saturation);
    list.set(controls::ExposureValue, c.exposure_value);
#if RPI_LIBCAMERA_AT_LEAST(0, 5)
    list.set(controls::ExposureTimeMode, c.exposure_us > 0 ? controls::ExposureTimeModeManual
                                                           : controls::ExposureTimeModeAuto);
    list.set(controls::AnalogueGainMode, c.analogue_gain > 0.0f ? controls::AnalogueGainModeManual
                                                                : controls::AnalogueGainModeAuto);
    if (c.exposure_us > 0) list.set(controls::ExposureTime, c.exposure_us);
    if (c.analogue_gain > 0.0f) list.set(controls::AnalogueGain, c.analogue_gain);
#else
    /* The Raspberry Pi IPA treats zero as "return to automatic". */
    list.set(controls::ExposureTime, c.exposure_us);
    list.set(controls::AnalogueGain, c.analogue_gain);
#endif
    switch (settings->metering) {
    case CA_METERING_CENTER: list.set(controls::AeMeteringMode, controls::MeteringCentreWeighted); break;
    case CA_METERING_SPOT: list.set(controls::AeMeteringMode, controls::MeteringSpot); break;
    default: list.set(controls::AeMeteringMode, controls::MeteringMatrix); break;
    }
    switch (settings->white_balance) {
    case CA_WB_DAYLIGHT: list.set(controls::AwbMode, controls::AwbDaylight); break;
    case CA_WB_CLOUDY: list.set(controls::AwbMode, controls::AwbCloudy); break;
    case CA_WB_FLUORESCENT: list.set(controls::AwbMode, controls::AwbFluorescent); break;
    case CA_WB_INCANDESCENT: list.set(controls::AwbMode, controls::AwbIncandescent); break;
    default: list.set(controls::AwbMode, controls::AwbAuto); break;
    }
    if (mapped) *mapped = c;
}

static void report_exposure(struct ca_rpi_pipeline *p, const ControlList &metadata)
{
    struct ca_exposure s = ca_exposure_empty(0, monotonic_us());
    const auto shutter = metadata.get(controls::ExposureTime);
    const auto again = metadata.get(controls::AnalogueGain);
    const auto dgain = metadata.get(controls::DigitalGain);
    const auto colour = metadata.get(controls::ColourGains);
    if (shutter) { s.shutter_us = (float)*shutter; s.valid |= CA_AE_SHUTTER; }
    if (again) { s.analog_gain = *again; s.valid |= CA_AE_AGAIN; }
    if (dgain) { s.digital_gain = *dgain; s.valid |= CA_AE_DGAIN; }
    pthread_mutex_lock(&p->lock);
    s.mode = (uint8_t)p->image.mode;
    /* Exposure and white balance to hold during a sensor mode switch. */
    if (shutter) p->last_exposure_us = *shutter;
    if (again) p->last_gain = *again;
    if (colour) { p->last_colour[0] = (*colour)[0]; p->last_colour[1] = (*colour)[1]; }
    pthread_mutex_unlock(&p->lock);
    s.valid |= CA_AE_MODE;
    if (p->config.exposure) p->config.exposure(p->config.opaque, &s);
}

static void requeue_locked(struct ca_rpi_pipeline *p, unsigned index);

static void dmabuf_sync(int fd, uint64_t flags)
{
    struct dma_buf_sync sync = {.flags = flags};
    (void)xioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

/* libcamera's thread: copy the frame for a waiting still capture. */
static void copy_still(struct ca_rpi_pipeline *p, unsigned index, FrameBuffer *buffer, uint64_t timestamp_us)
{
    pthread_mutex_lock(&p->lock);
    struct ca_rpi_still_frame *frame = p->still;
    const auto planes = buffer->planes();
    if (frame && !p->still_done && index < p->mapped.size() && p->mapped[index] && planes.size() == 3) {
        const unsigned chroma_stride = frame->stride / 2U, chroma_height = (frame->height + 1U) / 2U;
        const size_t sizes[3] = {(size_t)frame->stride * frame->height,
                                 (size_t)chroma_stride * chroma_height, (size_t)chroma_stride * chroma_height};
        uint8_t *out = frame->data;
        const int fd = planes[0].fd.get();
        dmabuf_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        for (unsigned i = 0; i < 3U; i++) {
            const size_t n = sizes[i] < planes[i].length ? sizes[i] : planes[i].length;
            if (planes[i].offset + n <= p->mapped_length[index])
                memcpy(out, p->mapped[index] + planes[i].offset, n);
            out += sizes[i];
        }
        dmabuf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        frame->timestamp_us = timestamp_us;
        p->still_done = true;
        pthread_cond_signal(&p->still_ready);
    }
    pthread_mutex_unlock(&p->lock);
}

/* Frames delivered after a still configuration starts with locked controls:
 * take the first whose exposure matches, or the last if it never settles. */
#define RPI_STILL_SETTLE_FRAMES 6U

static bool near(double value, double target)
{
    return target <= 0.0 || (value > target * 0.9 && value < target * 1.1);
}

/* libcamera's thread, still mode: copy one settled frame, recycle the rest. */
static void still_complete(struct ca_rpi_pipeline *p, Request *request)
{
    FrameBuffer *buffer = request->findBuffer(p->stream);
    if (!buffer) return;
    if (buffer->metadata().status != FrameMetadata::FrameSuccess) {
        /* A flagged frame is neither saved nor counted towards settling. */
        request->reuse(Request::ReuseBuffers);
        (void)p->camera->queueRequest(request);
        return;
    }
    const auto &metadata = request->metadata();
    const auto shutter = metadata.get(controls::ExposureTime);
    const auto gain = metadata.get(controls::AnalogueGain);
    const auto sensor_ns = metadata.get(controls::SensorTimestamp);
    pthread_mutex_lock(&p->lock);
    const bool settled = ++p->still_frames >= RPI_STILL_SETTLE_FRAMES ||
        (shutter && gain && near(*shutter, p->last_exposure_us) && near(*gain, p->last_gain));
    pthread_mutex_unlock(&p->lock);
    if (settled) {
        copy_still(p, (unsigned)request->cookie(), buffer,
                   sensor_ns ? (uint64_t)*sensor_ns / 1000U : monotonic_us());
        return;
    }
    request->reuse(Request::ReuseBuffers);
    (void)p->camera->queueRequest(request);
}

/* libcamera's thread: hand the completed frame to the encoder. */
static void request_complete(struct ca_rpi_pipeline *p, Request *request)
{
    if (request->status() == Request::RequestCancelled || atomic_load(&p->stop)) return;
    if (atomic_load(&p->still_mode)) { still_complete(p, request); return; }
    if (atomic_load(&p->paused)) return;
    FrameBuffer *buffer = request->findBuffer(p->stream);
    if (!buffer) return;
    /* After a restart the IPA flags its first frames as start-up: keep them
     * out of the video and out of the exposure the next photo locks to. */
    if (buffer->metadata().status != FrameMetadata::FrameSuccess) {
        pthread_mutex_lock(&p->video_lock);
        requeue_locked(p, (unsigned)request->cookie());
        pthread_mutex_unlock(&p->video_lock);
        return;
    }
    report_exposure(p, request->metadata());
    const auto sensor_ns = request->metadata().get(controls::SensorTimestamp);
    const uint64_t timestamp_us = sensor_ns ? (uint64_t)*sensor_ns / 1000U : monotonic_us();
    if (atomic_load(&p->still_wanted)) copy_still(p, (unsigned)request->cookie(), buffer, timestamp_us);

    struct v4l2_plane plane = {};
    struct v4l2_buffer v = {};
    v.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    v.memory = V4L2_MEMORY_DMABUF;
    v.index = (unsigned)request->cookie();
    v.field = V4L2_FIELD_NONE;
    v.length = 1;
    v.m.planes = &plane;
    v.timestamp.tv_sec = (time_t)(timestamp_us / 1000000U);
    v.timestamp.tv_usec = (suseconds_t)(timestamp_us % 1000000U);
    /* All YUV420 planes share one dmabuf at the plane offsets. Pass its full
     * extent: the encoder may expect height padding beyond frameSize. */
    const auto planes = buffer->planes();
    plane.m.fd = planes[0].fd.get();
    plane.bytesused = planes.back().offset + planes.back().length;
    plane.length = plane.bytesused;
    pthread_mutex_lock(&p->video_lock);
    if (atomic_load(&p->paused)) {
        /* Paused after the check above: the buffer stays with the camera. */
        pthread_mutex_unlock(&p->video_lock);
        return;
    }
    atomic_fetch_add(&p->encoder_queued, 1);
    if (xioctl(p->encoder, VIDIOC_QBUF, &v) < 0) {
        atomic_fetch_add(&p->encoder_queued, -1);
        ca_log("RPi encoder input queue failed: %s", strerror(errno));
        /* Drop the frame but return its buffer, or capture stalls. */
        requeue_locked(p, v.index);
    }
    pthread_mutex_unlock(&p->video_lock);
}

/* Return a request to the camera; the caller holds video_lock. */
static void requeue_locked(struct ca_rpi_pipeline *p, unsigned index)
{
    if (index >= p->requests.size() || atomic_load(&p->stop) || atomic_load(&p->paused)) return;
    Request *request = p->requests[index].get();
    request->reuse(Request::ReuseBuffers);
    pthread_mutex_lock(&p->lock);
    if (p->have_pending) {
        /* Controls persist in the IPA, so one request carries each change. */
        request->controls().merge(p->pending);
        p->pending.clear();
        p->have_pending = false;
    }
    pthread_mutex_unlock(&p->lock);
    if (p->camera->queueRequest(request) < 0 && !atomic_load(&p->stop))
        ca_log("RPi camera request queue failed");
}

static void *encoder_thread(void *opaque)
{
    auto *p = static_cast<struct ca_rpi_pipeline *>(opaque);
    while (!atomic_load(&p->stop)) {
        struct pollfd item = {.fd = p->encoder, .events = POLLIN | POLLOUT};
        int ready = poll(&item, 1, 100);
        if (ready < 0 && errno != EINTR) { ca_log("RPi encoder poll failed: %s", strerror(errno)); break; }
        if (ready <= 0) continue;
        for (;;) {
            struct v4l2_plane plane = {};
            struct v4l2_buffer v = {};
            v.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            v.memory = V4L2_MEMORY_DMABUF;
            v.length = 1;
            v.m.planes = &plane;
            /* Dequeue, requeue and decrement as one step under video_lock, so
             * a still capture's encoder reset never lands part-way through.
             * The count drops only once the request is handed back: a still
             * capture frees requests at zero. */
            pthread_mutex_lock(&p->video_lock);
            if (xioctl(p->encoder, VIDIOC_DQBUF, &v) < 0) {
                pthread_mutex_unlock(&p->video_lock);
                break;
            }
            requeue_locked(p, v.index);
            atomic_fetch_add(&p->encoder_queued, -1);
            pthread_mutex_unlock(&p->video_lock);
        }
        for (;;) {
            struct v4l2_plane plane = {};
            struct v4l2_buffer v = {};
            v.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            v.memory = V4L2_MEMORY_MMAP;
            v.length = 1;
            v.m.planes = &plane;
            if (xioctl(p->encoder, VIDIOC_DQBUF, &v) < 0) break;
            if (v.index < RPI_ENCODED_BUFFERS && plane.bytesused && p->config.frame) {
                const uint64_t pts = (uint64_t)v.timestamp.tv_sec * 1000000U + (uint64_t)v.timestamp.tv_usec;
                p->config.frame(p->config.opaque, static_cast<const uint8_t *>(p->encoded[v.index]),
                                plane.bytesused, pts, (v.flags & V4L2_BUF_FLAG_KEYFRAME) != 0);
            }
            plane.bytesused = 0;
            if (xioctl(p->encoder, VIDIOC_QBUF, &v) < 0) {
                ca_log("RPi encoder output requeue failed: %s", strerror(errno));
                break;
            }
        }
    }
    return NULL;
}

static int set_encoder_control(int fd, uint32_t id, int32_t value, const char *name)
{
    struct v4l2_control control = {.id = id, .value = value};
    if (xioctl(fd, VIDIOC_S_CTRL, &control) < 0) {
        ca_log("RPi encoder %s=%d failed: %s", name, value, strerror(errno));
        return -1;
    }
    return 0;
}

static int open_encoder(struct ca_rpi_pipeline *p, const StreamConfiguration &stream)
{
    const char *device = getenv("CAMERA_APP_RPI_ENCODER");
    p->encoder = open(device && *device ? device : RPI_ENCODER_DEVICE, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (p->encoder < 0) {
        ca_log("RPi cannot open H.264 encoder: %s", strerror(errno));
        return -1;
    }
    const int fd = p->encoder;
    if (set_encoder_control(fd, V4L2_CID_MPEG_VIDEO_BITRATE, (int32_t)p->config.bit_rate_kbps * 1000, "bitrate") < 0 ||
        set_encoder_control(fd, V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_HIGH, "profile") < 0 ||
        set_encoder_control(fd, V4L2_CID_MPEG_VIDEO_H264_LEVEL, V4L2_MPEG_VIDEO_H264_LEVEL_4_1, "level") < 0 ||
        set_encoder_control(fd, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, (int32_t)p->config.frame_rate, "I period") < 0 ||
        set_encoder_control(fd, V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER, 1, "repeat headers") < 0)
        return -1;

    struct v4l2_format format = {};
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    format.fmt.pix_mp.width = stream.size.width;
    format.fmt.pix_mp.height = stream.size.height;
    format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
    format.fmt.pix_mp.field = V4L2_FIELD_ANY;
    format.fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
    format.fmt.pix_mp.num_planes = 1;
    format.fmt.pix_mp.plane_fmt[0].bytesperline = stream.stride;
    if (xioctl(fd, VIDIOC_S_FMT, &format) < 0) {
        ca_log("RPi encoder input format failed: %s", strerror(errno));
        return -1;
    }
    format = {};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    format.fmt.pix_mp.width = stream.size.width;
    format.fmt.pix_mp.height = stream.size.height;
    format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    format.fmt.pix_mp.field = V4L2_FIELD_ANY;
    format.fmt.pix_mp.colorspace = V4L2_COLORSPACE_DEFAULT;
    format.fmt.pix_mp.num_planes = 1;
    format.fmt.pix_mp.plane_fmt[0].sizeimage = RPI_ENCODED_BUFFER_SIZE;
    if (xioctl(fd, VIDIOC_S_FMT, &format) < 0) {
        ca_log("RPi encoder output format failed: %s", strerror(errno));
        return -1;
    }
    struct v4l2_streamparm parm = {};
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    parm.parm.output.timeperframe.numerator = 1;
    parm.parm.output.timeperframe.denominator = p->config.frame_rate;
    (void)xioctl(fd, VIDIOC_S_PARM, &parm);

    struct v4l2_requestbuffers buffers = {};
    buffers.count = (unsigned)p->requests.size();
    buffers.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buffers.memory = V4L2_MEMORY_DMABUF;
    if (xioctl(fd, VIDIOC_REQBUFS, &buffers) < 0 || buffers.count < p->requests.size()) {
        ca_log("RPi encoder input buffers unavailable: %s", strerror(errno));
        return -1;
    }
    buffers = {};
    buffers.count = RPI_ENCODED_BUFFERS;
    buffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buffers.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &buffers) < 0 || buffers.count < RPI_ENCODED_BUFFERS) {
        ca_log("RPi encoder output buffers unavailable: %s", strerror(errno));
        return -1;
    }
    for (unsigned i = 0; i < RPI_ENCODED_BUFFERS; i++) {
        struct v4l2_plane plane = {};
        struct v4l2_buffer v = {};
        v.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        v.memory = V4L2_MEMORY_MMAP;
        v.index = i;
        v.length = 1;
        v.m.planes = &plane;
        if (xioctl(fd, VIDIOC_QUERYBUF, &v) < 0) return -1;
        void *data = mmap(NULL, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, plane.m.mem_offset);
        if (data == MAP_FAILED) return -1;
        p->encoded[i] = data;
        p->encoded_length[i] = plane.length;
        if (xioctl(fd, VIDIOC_QBUF, &v) < 0) return -1;
    }
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) return -1;
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) return -1;
    p->encoder_started = true;
    return 0;
}

/* Configure the acquired camera with one stream, allocate and map its
 * buffers and create one request per buffer (cookie = buffer index). */
static int configure_camera(struct ca_rpi_pipeline *p, StreamRole role, unsigned width, unsigned height,
                            unsigned buffer_count, const ColorSpace &colour)
{
    p->camera_config = p->camera->generateConfiguration({role});
    if (!p->camera_config || p->camera_config->empty()) { errno = ENOTSUP; return -1; }
    StreamConfiguration &stream = p->camera_config->at(0);
    stream.pixelFormat = formats::YUV420;
    stream.size = Size(width, height);
    stream.bufferCount = buffer_count;
    stream.colorSpace = colour;
    p->camera_config->orientation = p->config.inverted ? Orientation::Rotate180 : Orientation::Rotate0;
    if (p->camera_config->validate() == CameraConfiguration::Invalid) {
        ca_log("RPi camera configuration invalid"); errno = EINVAL; return -1;
    }
    if (stream.size != Size(width, height) || stream.pixelFormat != formats::YUV420) {
        ca_log("RPi camera cannot provide %ux%u YUV420 (got %s)", width, height, stream.toString().c_str());
        errno = ENOTSUP; return -1;
    }
    if (p->camera->configure(p->camera_config.get()) < 0) { ca_log("RPi camera configure failed"); errno = EIO; return -1; }
    p->stream = stream.stream();
    ca_log("RPi camera %s configured %s", p->camera->id().c_str(), stream.toString().c_str());

    p->allocator = std::make_unique<FrameBufferAllocator>(p->camera);
    if (p->allocator->allocate(p->stream) < 0) { errno = ENOMEM; return -1; }
    const auto &buffers = p->allocator->buffers(p->stream);
    for (unsigned i = 0; i < buffers.size(); i++) {
        std::unique_ptr<Request> request = p->camera->createRequest(i);
        if (!request || request->addBuffer(p->stream, buffers[i].get()) < 0) { errno = ENOMEM; return -1; }
        p->requests.push_back(std::move(request));
        /* All YUV420 planes share one dmabuf; map it for still copies. */
        const auto planes = buffers[i]->planes();
        const size_t length = planes.back().offset + planes.back().length;
        void *data = mmap(NULL, length, PROT_READ, MAP_SHARED, planes[0].fd.get(), 0);
        p->mapped.push_back(data == MAP_FAILED ? nullptr : static_cast<const uint8_t *>(data));
        p->mapped_length.push_back(data == MAP_FAILED ? 0 : length);
        if (data == MAP_FAILED) ca_log("RPi cannot map camera buffer %u for stills: %s", i, strerror(errno));
    }
    return 0;
}

/* Free a stopped camera's requests and buffers. */
static void release_buffers(struct ca_rpi_pipeline *p)
{
    p->requests.clear();
    for (size_t i = 0; i < p->mapped.size(); i++)
        if (p->mapped[i]) munmap(const_cast<uint8_t *>(p->mapped[i]), p->mapped_length[i]);
    p->mapped.clear();
    p->mapped_length.clear();
    if (p->allocator && p->stream) (void)p->allocator->free(p->stream);
    p->allocator.reset();
    p->stream = nullptr;
}

static int open_camera(struct ca_rpi_pipeline *p)
{
    p->manager = std::make_unique<CameraManager>();
    int result = p->manager->start();
    if (result < 0) { ca_log("RPi libcamera start failed: %s", strerror(-result)); errno = -result; return -1; }
    const char *id = getenv("CAMERA_APP_RPI_CAMERA");
    if (id && *id) p->camera = p->manager->get(id);
    else if (!p->manager->cameras().empty()) p->camera = p->manager->cameras()[0];
    if (!p->camera) { ca_log("RPi no libcamera camera found"); errno = ENODEV; return -1; }
    if (p->camera->acquire() < 0) { ca_log("RPi camera %s is busy", p->camera->id().c_str()); errno = EBUSY; return -1; }
    p->acquired = true;
    return configure_camera(p, StreamRole::VideoRecording, p->config.width, p->config.height,
                            RPI_CAMERA_BUFFERS, ColorSpace::Rec709);
}

/* Reopen the camera from a fresh CameraManager for video. libcamera's
 * Raspberry Pi pipeline keeps a queue of pending ISP ("immediate") controls,
 * keyed by request sequence, that stop() does not clear: after a restart
 * entries left from the old stream hold back every later brightness,
 * contrast, saturation or AWB mode change for as many frames as the old
 * stream had run. A new CameraManager starts that state, and the IPA, again. */
static int reopen_camera(struct ca_rpi_pipeline *p)
{
    p->camera->requestCompleted.disconnect(p);
    p->camera_config.reset();
    if (p->acquired) (void)p->camera->release();
    p->acquired = false;
    p->camera.reset();
    p->manager->stop();
    p->manager.reset();
    if (open_camera(p) < 0) return -1;
    p->camera->requestCompleted.connect(p, [p](Request *request) { request_complete(p, request); });
    return 0;
}

/* Start a configured video stream with the current image settings. */
static int start_video(struct ca_rpi_pipeline *p)
{
    ControlList initial(controls::controls);
    const int64_t frame_us = 1000000 / (int64_t)p->config.frame_rate;
    initial.set(controls::FrameDurationLimits, {frame_us, frame_us});
    pthread_mutex_lock(&p->lock);
    const struct ca_config image = p->config.image;
    p->pending.clear();
    p->have_pending = false;
    pthread_mutex_unlock(&p->lock);
    struct ca_rpi_image_controls mapped;
    image_control_list(&image, initial, &mapped);
    /* A still capture locks exposure and white balance, and the IPA keeps
     * that across a restart: hand both back to the algorithms. Manual ISO
     * and shutter settings still apply through image_control_list(). */
    initial.set(controls::AwbEnable, true);
#if !RPI_LIBCAMERA_AT_LEAST(0, 5)
    initial.set(controls::AeEnable, true);
#endif
    pthread_mutex_lock(&p->lock);
    p->image = mapped;
    pthread_mutex_unlock(&p->lock);
    if (p->camera->start(&initial) < 0) { ca_log("RPi camera start failed"); errno = EIO; return -1; }
    p->camera_started = true;
    return 0;
}

static int queue_requests(struct ca_rpi_pipeline *p)
{
    for (auto &request : p->requests) {
        if (p->camera->queueRequest(request.get()) < 0) { errno = EIO; return -1; }
    }
    return 0;
}

static int start(struct ca_rpi_pipeline *p)
{
    if (open_camera(p) < 0 || open_encoder(p, p->camera_config->at(0)) < 0) return -1;
    p->camera->requestCompleted.connect(p, [p](Request *request) { request_complete(p, request); });
    if (start_video(p) < 0) return -1;
    int error = pthread_create(&p->thread, NULL, encoder_thread, p);
    if (error) { errno = error; return -1; }
    p->thread_started = true;
    return queue_requests(p);
}

int ca_rpi_pipeline_open(struct ca_rpi_pipeline **out, const struct ca_rpi_pipeline_config *config)
{
    if (!out || !config || !config->width || !config->height || !config->frame_rate) { errno = EINVAL; return -1; }
    auto *p = new (std::nothrow) ca_rpi_pipeline();
    if (!p) { errno = ENOMEM; return -1; }
    p->config = *config;
    atomic_init(&p->stop, false);
    atomic_init(&p->still_wanted, false);
    atomic_init(&p->paused, false);
    atomic_init(&p->still_mode, false);
    atomic_init(&p->encoder_queued, 0);
    pthread_mutex_init(&p->lock, NULL);
    pthread_mutex_init(&p->video_lock, NULL);
    pthread_condattr_t condattr;
    pthread_condattr_init(&condattr);
    pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC);
    pthread_cond_init(&p->still_ready, &condattr);
    pthread_condattr_destroy(&condattr);
    if (start(p) < 0) {
        int saved = errno ? errno : EIO;
        ca_rpi_pipeline_close(p);
        errno = saved;
        return -1;
    }
    *out = p;
    return 0;
}

int ca_rpi_pipeline_set_image(struct ca_rpi_pipeline *p, const struct ca_config *settings)
{
    if (!p || !settings) { errno = EINVAL; return -1; }
    ControlList list(controls::controls);
    struct ca_rpi_image_controls mapped;
    image_control_list(settings, list, &mapped);
    pthread_mutex_lock(&p->lock);
    /* Each list holds every image control, so the newest replaces any change
     * not yet queued; merge() would keep the older values. */
    p->pending = std::move(list);
    p->have_pending = true;
    p->image = mapped;
    ca_config_copy_image(&p->config.image, settings);
    pthread_mutex_unlock(&p->lock);
    return 0;
}

int ca_rpi_pipeline_set_bitrate(struct ca_rpi_pipeline *p, unsigned bit_rate_kbps)
{
    if (!p || !bit_rate_kbps) { errno = EINVAL; return -1; }
    if (bit_rate_kbps == p->config.bit_rate_kbps) return 0;
    if (set_encoder_control(p->encoder, V4L2_CID_MPEG_VIDEO_BITRATE, (int32_t)bit_rate_kbps * 1000, "bitrate") < 0) {
        errno = EIO;
        return -1;
    }
    p->config.bit_rate_kbps = bit_rate_kbps;
    ca_log("RPi encoder bitrate %u kbps", bit_rate_kbps);
    return 0;
}

void ca_rpi_pipeline_close(struct ca_rpi_pipeline *p)
{
    if (!p) return;
    atomic_store(&p->stop, true);
    if (p->thread_started) pthread_join(p->thread, NULL);
    if (p->camera_started) (void)p->camera->stop();
    if (p->camera) p->camera->requestCompleted.disconnect(p);
    if (p->encoder >= 0) {
        if (p->encoder_started) {
            int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            (void)xioctl(p->encoder, VIDIOC_STREAMOFF, &type);
            type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            (void)xioctl(p->encoder, VIDIOC_STREAMOFF, &type);
        }
        for (unsigned i = 0; i < RPI_ENCODED_BUFFERS; i++)
            if (p->encoded[i]) munmap(p->encoded[i], p->encoded_length[i]);
        close(p->encoder);
    }
    release_buffers(p);
    p->camera_config.reset();
    if (p->acquired) (void)p->camera->release();
    p->camera.reset();
    if (p->manager) p->manager->stop();
    p->manager.reset();
    pthread_cond_destroy(&p->still_ready);
    pthread_mutex_destroy(&p->video_lock);
    pthread_mutex_destroy(&p->lock);
    delete p;
}

int ca_rpi_pipeline_grab_still(struct ca_rpi_pipeline *p, struct ca_rpi_still_frame *frame, unsigned timeout_ms)
{
    if (!p || !frame || !p->camera_config) { errno = EINVAL; return -1; }
    const StreamConfiguration &stream = p->camera_config->at(0);
    *frame = {};
    frame->width = stream.size.width;
    frame->height = stream.size.height;
    frame->stride = stream.stride;
    const size_t chroma = (size_t)(frame->stride / 2U) * ((frame->height + 1U) / 2U);
    /* Padding: libjpeg reads chroma rows rounded up to whole 8-pixel blocks. */
    frame->data = static_cast<uint8_t *>(malloc((size_t)frame->stride * frame->height + 2U * chroma + 64U));
    if (!frame->data) { errno = ENOMEM; return -1; }

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000U;
    deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&p->lock);
    p->still = frame;
    p->still_done = false;
    atomic_store(&p->still_wanted, true);
    int error = 0;
    while (!p->still_done && error == 0) error = pthread_cond_timedwait(&p->still_ready, &p->lock, &deadline);
    const bool done = p->still_done;
    p->still = nullptr;
    atomic_store(&p->still_wanted, false);
    pthread_mutex_unlock(&p->lock);
    if (!done) {
        free(frame->data);
        frame->data = nullptr;
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

/* Exposure and white balance locked to the last video frame, so a still
 * needs no convergence; image settings (brightness etc.) still apply. */
static void still_control_list(struct ca_rpi_pipeline *p, ControlList &list)
{
    pthread_mutex_lock(&p->lock);
    const struct ca_config image = p->config.image;
    const int32_t exposure_us = p->last_exposure_us;
    const float gain = p->last_gain, red = p->last_colour[0], blue = p->last_colour[1];
    pthread_mutex_unlock(&p->lock);
    image_control_list(&image, list, NULL);
    if (exposure_us > 0 && gain > 0.0f) {
#if RPI_LIBCAMERA_AT_LEAST(0, 5)
        list.set(controls::ExposureTimeMode, controls::ExposureTimeModeManual);
        list.set(controls::AnalogueGainMode, controls::AnalogueGainModeManual);
#else
        list.set(controls::AeEnable, false);
#endif
        list.set(controls::ExposureTime, exposure_us);
        list.set(controls::AnalogueGain, gain);
    }
    if (red > 0.0f && blue > 0.0f) {
        list.set(controls::AwbEnable, false);
        list.set(controls::ColourGains, {red, blue});
    }
}

int ca_rpi_pipeline_capture_still(struct ca_rpi_pipeline *p, unsigned width, unsigned height,
                                  struct ca_rpi_still_frame *frame, unsigned timeout_ms)
{
    if (!p || !frame || !width || !height || !p->camera) { errno = EINVAL; return -1; }
    *frame = {};
    const uint64_t started = monotonic_us();

    /* Pause video and wait for the encoder to return every camera buffer
     * (normally a few milliseconds). Under video_lock no hand-off to the
     * encoder or requeue to the camera is part-way through. */
    pthread_mutex_lock(&p->video_lock);
    atomic_store(&p->paused, true);
    pthread_mutex_unlock(&p->video_lock);
    for (unsigned i = 0; i < 50U && atomic_load(&p->encoder_queued) > 0; i++) usleep(5000);
    bool encoder_ok = true;
    if (atomic_load(&p->encoder_queued) > 0) {
        /* Take the buffers back by restarting the encoder's input queue, so
         * no late dequeue can reach a freed or reused request. */
        pthread_mutex_lock(&p->video_lock);
        const int held = atomic_load(&p->encoder_queued);
        if (held > 0) {
            ca_log("RPi encoder still holds %d camera buffers; resetting its input", held);
            int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            if (xioctl(p->encoder, VIDIOC_STREAMOFF, &type) < 0)
                ca_log("RPi encoder input stop failed: %s", strerror(errno));
            if (xioctl(p->encoder, VIDIOC_STREAMON, &type) < 0) {
                ca_log("RPi encoder input restart failed: %s", strerror(errno));
                encoder_ok = false;
            }
            atomic_store(&p->encoder_queued, 0);
        }
        pthread_mutex_unlock(&p->video_lock);
    }
    if (p->camera_started) (void)p->camera->stop();
    p->camera_started = false;
    release_buffers(p);
    const uint64_t paused = monotonic_us();

    int result = -1, error = ETIMEDOUT;
    if (configure_camera(p, StreamRole::StillCapture, width, height, 2U, ColorSpace::Sycc) == 0) {
        const StreamConfiguration &stream = p->camera_config->at(0);
        frame->width = stream.size.width;
        frame->height = stream.size.height;
        frame->stride = stream.stride;
        frame->jpeg_colour = stream.colorSpace && *stream.colorSpace == ColorSpace::Sycc;
        const size_t chroma = (size_t)(frame->stride / 2U) * ((frame->height + 1U) / 2U);
        /* Padding: libjpeg reads chroma rows rounded up to whole 8-pixel blocks. */
        frame->data = static_cast<uint8_t *>(malloc((size_t)frame->stride * frame->height + 2U * chroma + 64U));
        ControlList locked(controls::controls);
        still_control_list(p, locked);
        pthread_mutex_lock(&p->lock);
        p->still = frame->data ? frame : nullptr;
        p->still_done = false;
        p->still_frames = 0;
        pthread_mutex_unlock(&p->lock);
        atomic_store(&p->still_mode, true);
        if (!frame->data) {
            error = ENOMEM;
        } else if (p->camera->start(&locked) < 0) {
            error = EIO;
        } else {
            p->camera_started = true;
            struct timespec deadline;
            clock_gettime(CLOCK_MONOTONIC, &deadline);
            deadline.tv_sec += timeout_ms / 1000U;
            deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
            if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
            if (queue_requests(p) == 0) {
                pthread_mutex_lock(&p->lock);
                int wait = 0;
                while (!p->still_done && wait == 0) wait = pthread_cond_timedwait(&p->still_ready, &p->lock, &deadline);
                pthread_mutex_unlock(&p->lock);
            }
            (void)p->camera->stop();
            p->camera_started = false;
        }
        atomic_store(&p->still_mode, false);
        pthread_mutex_lock(&p->lock);
        if (p->still_done) result = 0;
        p->still = nullptr;
        pthread_mutex_unlock(&p->lock);
    } else {
        error = errno;
    }
    release_buffers(p);
    const uint64_t captured = monotonic_us();

    /* Resume video; the encoder keeps its state, so streams and recordings
     * continue after a gap. A key frame lets viewers resynchronise. */
    bool resumed = reopen_camera(p) == 0 && start_video(p) == 0;
    if (resumed) {
        pthread_mutex_lock(&p->video_lock);
        atomic_store(&p->paused, false);
        pthread_mutex_unlock(&p->video_lock);
        (void)set_encoder_control(p->encoder, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1, "key frame");
        /* A dead encoder input is reported, so the caller sees the failure;
         * the next photo's reset tries to restart it again. */
        resumed = queue_requests(p) == 0 && encoder_ok;
    }
    const uint64_t finished = monotonic_us();
    ca_log("RPi still %ux%u: pause %llu ms, capture %llu ms, resume %llu ms", width, height,
           (unsigned long long)(paused - started) / 1000U, (unsigned long long)(captured - paused) / 1000U,
           (unsigned long long)(finished - captured) / 1000U);
    if (!resumed) {
        ca_log("RPi video could not be restarted after a still capture");
        free(frame->data);
        frame->data = nullptr;
        errno = EIO;
        return -1;
    }
    if (result < 0) {
        free(frame->data);
        frame->data = nullptr;
        errno = error;
        return -1;
    }
    return 0;
}
