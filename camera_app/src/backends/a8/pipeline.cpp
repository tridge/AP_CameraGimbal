#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pipeline.h"
#include "../siyi/tracking_frame.h"

#include "camera_app/log.h"

#include "mi/star/m6_isp.h"
#include "mi/star/m6_scl.h"
#include "mi/star/m6_snr.h"
#include "mi/star/m6_sys.h"
#include "mi/star/m6_venc.h"
#include "mi/star/m6_vif.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

/* ISP port 1 and SCL device 1 provide the buffered digital zoom path. */
#define SNR_PAD 0
#define VIF_GRP 0
#define VIF_DEV 0
#define VIF_PORT 0
#define ISP_DEV 0
#define ISP_CHN 0
#define ISP_PORT 1
#define SCL_DEV CA_A8_SCL_DEV
#define SCL_CHN 0
#define JPEG_SCL_PORT CA_A8_MAIN_VENC
#define JPEG_VENC_CHN 0
#define ISP_BIN_KEY 1234U

enum stage {
    STAGE_NONE,
    STAGE_SYS,
    STAGE_SENSOR,
    STAGE_VIF,
    STAGE_ISP,
    STAGE_SCL,
    STAGE_BOUND,
    STAGE_PORTS,
    STAGE_VENC,
};

struct a8_pipeline {
    m6_sys_impl sys;
    m6_snr_impl snr;
    m6_vif_impl vif;
    m6_isp_impl isp;
    m6_scl_impl scl;
    m6_venc_impl venc;
    int (*isp_load_bin)(unsigned device, unsigned channel, char *path,
                        unsigned key);
    void *libm;
    void *libpthread;
    enum stage stage;
    m6_snr_pad pad;
    m6_snr_plane plane;
    struct ca_a8_pipeline_config config;
    bool venc_created[CA_A8_VENC_COUNT];
    bool venc_bound[CA_A8_VENC_COUNT];
    bool port_enabled[CA_A8_VENC_COUNT+1];
    int venc_fd[CA_A8_VENC_COUNT];
    bool jpeg_created;
    bool jpeg_bound;
    bool inverted;
    m6_common_rect zoom_crop; /* zero = full sensor */
};

static struct a8_pipeline pipe_state;
static CA_MI_TrackingFrames tracking_frames;
bool ca_a8_tracking_available() { return tracking_frames.available(); }
bool ca_a8_tracking_frame(ca_tracking_frame &frame) { return tracking_frames.read(frame); }

#define MI_CHECK(call)                                                      \
    do {                                                                    \
        int mi_ret_ = (call);                                               \
        if (mi_ret_ != 0) {                                                 \
            ca_log("%s failed: 0x%x", #call, (unsigned)mi_ret_);            \
            errno = EIO;                                                    \
            return -1;                                                      \
        }                                                                   \
    } while (0)

static m6_common_pixfmt bayer_pixfmt(const m6_snr_plane *plane)
{
    /* the vendor app's formula; the sensor's own pixFmt field is not what
     * VIF wants on this SDK build */
    if (plane->bayer > M6_BAYER_END) return plane->pixFmt;
    return (m6_common_pixfmt)(M6_PIXFMT_RGB_BAYER +
                              plane->precision * M6_BAYER_END + plane->bayer);
}

static int load_libraries(struct a8_pipeline *p)
{
    /* libispalgo.so imports log2/pow but only declares libc */
    p->libm = dlopen("libm.so.6", RTLD_NOW | RTLD_GLOBAL);
    p->libpthread = dlopen("libpthread.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (p->libm == NULL || p->libpthread == NULL) {
        ca_log("MI dependency preload failed: %s", dlerror());
        errno = ENOENT;
        return -1;
    }
    if (m6_sys_load(&p->sys) || m6_snr_load(&p->snr) ||
        m6_vif_load(&p->vif) || m6_isp_load(&p->isp) ||
        m6_scl_load(&p->scl) || m6_venc_load(&p->venc)) {
        errno = ENOENT;
        return -1;
    }
    p->isp_load_bin = (int (*)(unsigned int, unsigned int, char*, unsigned int))(dlsym(p->isp.handle, "MI_ISP_API_CmdLoadBinFile"));
    if (p->isp_load_bin == NULL) {
        ca_log("MI symbol lookup failed: %s", dlerror());
        errno = ENOENT;
        return -1;
    }
    return 0;
}

static int sensor_start(struct a8_pipeline *p)
{
    unsigned count = 0;
    int profile = -1;

    MI_CHECK(p->snr.fnInitDevice(NULL));
    MI_CHECK(p->snr.fnSetPlaneMode(SNR_PAD, 0));
    MI_CHECK(p->snr.fnGetResolutionCount(SNR_PAD, &count));
    for (unsigned i = 0; i < count; i++) {
        m6_snr_res res;
        memset(&res, 0, sizeof(res));
        if (p->snr.fnGetResolution(SNR_PAD, (unsigned char)i, &res) != 0) continue;
        if (res.crop.width == CA_A8_SENSOR_WIDTH &&
            res.crop.height == CA_A8_SENSOR_HEIGHT &&
            res.maxFps == CA_A8_FRAME_RATE) {
            profile = (int)i;
            break;
        }
    }
    if (profile < 0) {
        ca_log("sensor has no %ux%u@%u mode", CA_A8_SENSOR_WIDTH,
               CA_A8_SENSOR_HEIGHT, CA_A8_FRAME_RATE);
        errno = ENODEV;
        return -1;
    }
    MI_CHECK(p->snr.fnSetResolution(SNR_PAD, (unsigned char)profile));
    MI_CHECK(p->snr.fnEnable(SNR_PAD));
    p->stage = STAGE_SENSOR;
    MI_CHECK(p->snr.fnSetFramerate(SNR_PAD, CA_A8_FRAME_RATE));
    MI_CHECK(p->snr.fnGetPadInfo(SNR_PAD, &p->pad));
    MI_CHECK(p->snr.fnGetPlaneInfo(SNR_PAD, 0, &p->plane));
    ca_log("sensor %s mode %d %ux%u@%u", p->plane.sensName, profile,
           p->plane.capt.width, p->plane.capt.height, CA_A8_FRAME_RATE);
    return 0;
}

static int vif_start(struct a8_pipeline *p)
{
    m6_vif_grp group;
    m6_vif_dev device;
    m6_vif_port port;

    memset(&group, 0, sizeof(group));
    group.intf = p->pad.intf;
    group.work = M6_VIF_WORK_1MULTIPLEX;
    group.hdr = M6_HDR_OFF;
    group.edge = group.intf == M6_INTF_BT656 ? p->pad.intfAttr.bt656.edge
                                             : M6_EDGE_DOUBLE;
    group.grpStitch = 1U << VIF_GRP;
    MI_CHECK(p->vif.fnCreateGroup(VIF_GRP, &group));
    p->stage = STAGE_VIF;
    memset(&device, 0, sizeof(device));
    device.pixFmt = bayer_pixfmt(&p->plane);
    device.crop = p->plane.capt;
    MI_CHECK(p->vif.fnSetDeviceConfig(VIF_DEV, &device));
    MI_CHECK(p->vif.fnEnableDevice(VIF_DEV));
    memset(&port, 0, sizeof(port));
    port.capt = p->plane.capt;
    port.dest.width = p->plane.capt.width;
    port.dest.height = p->plane.capt.height;
    port.pixFmt = bayer_pixfmt(&p->plane);
    port.frate = M6_VIF_FRATE_FULL;
    MI_CHECK(p->vif.fnSetPortConfig(VIF_DEV, VIF_PORT, &port));
    MI_CHECK(p->vif.fnEnablePort(VIF_DEV, VIF_PORT));
    return 0;
}

static int isp_start(struct a8_pipeline *p)
{
    unsigned combo = 1;
    m6_isp_chn channel;
    m6_isp_para param;
    m6_isp_port port;

    MI_CHECK(p->isp.fnCreateDevice(ISP_DEV, &combo));
    p->stage = STAGE_ISP;
    memset(&channel, 0, sizeof(channel));
    channel.sensorId = 1U << SNR_PAD;
    MI_CHECK(p->isp.fnCreateChannel(ISP_DEV, ISP_CHN, &channel));
    memset(&param, 0, sizeof(param));
    param.hdr = M6_HDR_OFF;
    param.level3DNR = 1;
    MI_CHECK(p->isp.fnSetChannelParam(ISP_DEV, ISP_CHN, &param));
    MI_CHECK(p->isp.fnStartChannel(ISP_DEV, ISP_CHN));
    memset(&port, 0, sizeof(port));
    port.pixFmt = M6_PIXFMT_YUV420SP;
    MI_CHECK(p->isp.fnSetPortConfig(ISP_DEV, ISP_CHN, ISP_PORT, &port));
    MI_CHECK(p->isp.fnEnablePort(ISP_DEV, ISP_CHN, ISP_PORT));
    return 0;
}

static int scl_start(struct a8_pipeline *p)
{
    unsigned binds = 0xfU;
    unsigned reserved = 0;
    int rotate = 0;

    MI_CHECK(p->scl.fnCreateDevice(SCL_DEV, &binds));
    p->stage = STAGE_SCL;
    MI_CHECK(p->scl.fnCreateChannel(SCL_DEV, SCL_CHN, &reserved));
    MI_CHECK(p->scl.fnAdjustChannelRotation(SCL_DEV, SCL_CHN, &rotate));
    MI_CHECK(p->scl.fnStartChannel(SCL_DEV, SCL_CHN));
    return 0;
}

static int bind_front_end(struct a8_pipeline *p)
{
    m6_sys_bind vif = {M6_SYS_MOD_VIF, VIF_DEV, VIF_PORT, 0};
    m6_sys_bind isp_input = {M6_SYS_MOD_ISP, ISP_DEV, ISP_CHN, 0};
    m6_sys_bind isp_output = {M6_SYS_MOD_ISP, ISP_DEV, ISP_CHN, ISP_PORT};
    m6_sys_bind scl = {M6_SYS_MOD_SCL, SCL_DEV, SCL_CHN, 0};

    MI_CHECK(p->sys.fnBindExt(0, &vif, &isp_input, CA_A8_FRAME_RATE,
                              CA_A8_FRAME_RATE, M6_SYS_LINK_REALTIME, 0));
    p->stage = STAGE_BOUND;
    MI_CHECK(p->sys.fnBindExt(0, &isp_output, &scl, CA_A8_FRAME_RATE,
                              CA_A8_FRAME_RATE, M6_SYS_LINK_FRAMEBASE, 0));
    return 0;
}

static int scl_port_config(struct a8_pipeline *p, unsigned port)
{
    m6_scl_port config;

    memset(&config, 0, sizeof(config));
    config.crop = p->zoom_crop;
    config.output.width = port==3 ? 320 : (unsigned short)p->config.streams[port].width;
    config.output.height = port==3 ? 180 : (unsigned short)p->config.streams[port].height;
    config.mirror = p->inverted;
    config.flip = p->inverted;
    config.pixFmt = M6_PIXFMT_YUV420SP;
    config.compress = M6_COMPR_NONE;
    MI_CHECK(p->scl.fnSetPortConfig(SCL_DEV, SCL_CHN, (int)port, &config));
    return 0;
}

static int scl_ports_start(struct a8_pipeline *p)
{
    p->stage = STAGE_PORTS;
    for (unsigned port = 0; port < CA_A8_VENC_COUNT; port++) {
        if (scl_port_config(p, port) < 0) return -1;
        MI_CHECK(p->scl.fnEnablePort(SCL_DEV, SCL_CHN, (int)port));
        p->port_enabled[port] = true;
    }
    return 0;
}

static void h26x_attributes(m6_venc_chn *channel,
                            const struct ca_a8_stream *stream)
{
    m6_venc_rate_h26xcbr cbr = {
        .gop = CA_A8_FRAME_RATE,
        .statTime = 1,
        .fpsNum = CA_A8_FRAME_RATE,
        .fpsDen = 1,
        .bitrate = stream->bit_rate_kbps << 10,
        .avgLvl = 1,
    };
    m6_venc_attr_h26x *attr;

    memset(channel, 0, sizeof(*channel));
    if (stream->codec == CA_VIDEO_H265) {
        channel->attrib.codec = M6_VENC_CODEC_H265;
        channel->rate.mode = M6_VENC_RATEMODE_H265CBR;
        channel->rate.h265Cbr = cbr;
        attr = &channel->attrib.h265;
    } else {
        channel->attrib.codec = M6_VENC_CODEC_H264;
        channel->rate.mode = M6_VENC_RATEMODE_H264CBR;
        channel->rate.h264Cbr = cbr;
        attr = &channel->attrib.h264;
    }
    attr->maxWidth = stream->width;
    attr->maxHeight = stream->height;
    attr->width = stream->width;
    attr->height = stream->height;
    attr->bufSize = stream->width * stream->height;
    attr->profile = 1; /* main */
    attr->byFrame = 1;
    attr->refNum = 1;
}

static int bind_scl_venc(struct a8_pipeline *p, unsigned port,
                         unsigned device, unsigned channel)
{
    m6_sys_bind scl = {M6_SYS_MOD_SCL, SCL_DEV, SCL_CHN, port};
    m6_sys_bind venc = {M6_SYS_MOD_VENC, device, channel, 0};

    /* this SDK build only accepts frame-based links into the encoders */
    return p->sys.fnBindExt(0, &scl, &venc, CA_A8_FRAME_RATE,
                            CA_A8_FRAME_RATE, M6_SYS_LINK_FRAMEBASE, 0);
}

static void unbind_scl_venc(struct a8_pipeline *p, unsigned port,
                            unsigned device, unsigned channel)
{
    m6_sys_bind scl = {M6_SYS_MOD_SCL, SCL_DEV, SCL_CHN, port};
    m6_sys_bind venc = {M6_SYS_MOD_VENC, device, channel, 0};

    (void)p->sys.fnUnbind(0, &scl, &venc);
}

static int venc_start(struct a8_pipeline *p)
{
    unsigned max_width = 0, max_height = 0;
    m6_venc_init init;

    p->stage = STAGE_VENC;
    for (unsigned i = 0; i < CA_A8_VENC_COUNT; i++) {
        if (p->config.streams[i].width > max_width) {
            max_width = p->config.streams[i].width;
        }
        if (p->config.streams[i].height > max_height) {
            max_height = p->config.streams[i].height;
        }
        p->venc_fd[i] = -1;
    }
    init.maxWidth = max_width;
    init.maxHeight = max_height;
    MI_CHECK(p->venc.fnCreateDevice(M6_VENC_DEV_H26X_0, &init));
    for (unsigned i = 0; i < CA_A8_VENC_COUNT; i++) {
        m6_venc_chn channel;
        h26x_attributes(&channel, &p->config.streams[i]);
        MI_CHECK(p->venc.fnCreateChannel(M6_VENC_DEV_H26X_0, i, &channel));
        p->venc_created[i] = true;
        MI_CHECK(p->venc.fnStartReceiving(M6_VENC_DEV_H26X_0, i));
        MI_CHECK(bind_scl_venc(p, i, M6_VENC_DEV_H26X_0, i));
        p->venc_bound[i] = true;
        p->venc_fd[i] = p->venc.fnGetDescriptor(M6_VENC_DEV_H26X_0, i);
        if (p->venc_fd[i] < 0) {
            ca_log("VENC channel %u descriptor unavailable", i);
            errno = EIO;
            return -1;
        }
    }
    {
        /* still-image channel on the MJPEG device, fed from the main port */
        const struct ca_a8_stream *still = &p->config.streams[CA_A8_MAIN_VENC];
        m6_venc_chn channel;
        m6_venc_init jpeg_init = {.maxWidth = CA_A8_SENSOR_WIDTH,
                                  .maxHeight = CA_A8_SENSOR_HEIGHT};
        memset(&channel, 0, sizeof(channel));
        channel.attrib.codec = M6_VENC_CODEC_MJPG;
        channel.rate.mode = M6_VENC_RATEMODE_MJPGQP;
        channel.rate.mjpgQp.fpsNum = 1;
        channel.rate.mjpgQp.fpsDen = 1;
        channel.rate.mjpgQp.quality = p->config.jpeg_quality;
        channel.attrib.mjpg.maxWidth = ALIGN_UP(still->width, 8U);
        channel.attrib.mjpg.maxHeight = ALIGN_UP(still->height, 2U);
        channel.attrib.mjpg.width = channel.attrib.mjpg.maxWidth;
        channel.attrib.mjpg.height = channel.attrib.mjpg.maxHeight;
        channel.attrib.mjpg.bufSize = channel.attrib.mjpg.maxWidth *
                                      channel.attrib.mjpg.maxHeight;
        channel.attrib.mjpg.byFrame = 1;
        MI_CHECK(p->venc.fnCreateDevice(M6_VENC_DEV_MJPG_0, &jpeg_init));
        MI_CHECK(p->venc.fnCreateChannel(M6_VENC_DEV_MJPG_0, JPEG_VENC_CHN,
                                         &channel));
        p->jpeg_created = true;
        MI_CHECK(bind_scl_venc(p, JPEG_SCL_PORT, M6_VENC_DEV_MJPG_0,
                               JPEG_VENC_CHN));
        p->jpeg_bound = true;
    }
    return 0;
}

int ca_a8_pipeline_open(const struct ca_a8_pipeline_config *config)
{
    struct a8_pipeline *p = &pipe_state;

    if (config == NULL || p->stage != STAGE_NONE) {
        errno = EINVAL;
        return -1;
    }
    for (unsigned i = 0; i < CA_A8_VENC_COUNT; i++) {
        if (config->streams[i].width == 0U ||
            config->streams[i].width > CA_A8_SENSOR_WIDTH ||
            config->streams[i].height == 0U ||
            config->streams[i].height > CA_A8_SENSOR_HEIGHT) {
            errno = EINVAL;
            return -1;
        }
    }
    memset(p, 0, sizeof(*p));
    p->config = *config;
    if (load_libraries(p) < 0) return -1;
    MI_CHECK(p->sys.fnInit(0));
    p->stage = STAGE_SYS;
    {
        m6_sys_ver version;
        memset(&version, 0, sizeof(version));
        if (p->sys.fnGetVersion(0, &version) == 0) {
            ca_log("MI %s", version.version);
        }
    }
    if (sensor_start(p) < 0 || vif_start(p) < 0 || isp_start(p) < 0 ||
        scl_start(p) < 0 || bind_front_end(p) < 0 ||
        scl_ports_start(p) < 0 || venc_start(p) < 0) {
        int saved_errno = errno;
        ca_a8_pipeline_close();
        errno = saved_errno;
        return -1;
    }
    if(scl_port_config(p,3)==0 && p->scl.fnEnablePort(SCL_DEV,SCL_CHN,3)==0) {
        p->port_enabled[3]=true;
        if(!tracking_frames.open(M6_SYS_MOD_SCL,SCL_DEV,true)) ca_log("A8 tracking frame depth unavailable");
    } else ca_log("A8 tracking scaler output unavailable");
    return 0;
}

void ca_a8_pipeline_close(void)
{
    tracking_frames={};
    struct a8_pipeline *p = &pipe_state;
    m6_sys_bind source, destination;

    if (p->stage == STAGE_NONE) return;
    if (p->stage >= STAGE_VENC) {
        for (unsigned i = 0; i < CA_A8_VENC_COUNT; i++) {
            if (p->venc_fd[i] >= 0) {
                (void)p->venc.fnFreeDescriptor(M6_VENC_DEV_H26X_0, i);
            }
            if (p->venc_created[i]) {
                (void)p->venc.fnStopReceiving(M6_VENC_DEV_H26X_0, i);
            }
            if (p->venc_bound[i]) {
                unbind_scl_venc(p, i, M6_VENC_DEV_H26X_0, i);
            }
            if (p->venc_created[i]) {
                (void)p->venc.fnDestroyChannel(M6_VENC_DEV_H26X_0, i);
            }
        }
        if (p->jpeg_bound) {
            unbind_scl_venc(p, JPEG_SCL_PORT, M6_VENC_DEV_MJPG_0,
                            JPEG_VENC_CHN);
        }
        if (p->jpeg_created) {
            (void)p->venc.fnDestroyChannel(M6_VENC_DEV_MJPG_0, JPEG_VENC_CHN);
            (void)p->venc.fnDestroyDevice(M6_VENC_DEV_MJPG_0);
        }
        (void)p->venc.fnDestroyDevice(M6_VENC_DEV_H26X_0);
    }
    if (p->stage >= STAGE_PORTS) {
        for (unsigned port = 0; port <= CA_A8_VENC_COUNT; port++) {
            if (p->port_enabled[port]) {
                (void)p->scl.fnDisablePort(SCL_DEV, SCL_CHN, (int)port);
            }
        }
    }
    if (p->stage >= STAGE_BOUND) {
        source = (m6_sys_bind){M6_SYS_MOD_ISP, ISP_DEV, ISP_CHN, ISP_PORT};
        destination = (m6_sys_bind){M6_SYS_MOD_SCL, SCL_DEV, SCL_CHN, 0};
        (void)p->sys.fnUnbind(0, &source, &destination);
        source = (m6_sys_bind){M6_SYS_MOD_VIF, VIF_DEV, VIF_PORT, 0};
        destination = (m6_sys_bind){M6_SYS_MOD_ISP, ISP_DEV, ISP_CHN, 0};
        (void)p->sys.fnUnbind(0, &source, &destination);
    }
    if (p->stage >= STAGE_SCL) {
        (void)p->scl.fnStopChannel(SCL_DEV, SCL_CHN);
        (void)p->scl.fnDestroyChannel(SCL_DEV, SCL_CHN);
        (void)p->scl.fnDestroyDevice(SCL_DEV);
    }
    if (p->stage >= STAGE_ISP) {
        (void)p->isp.fnDisablePort(ISP_DEV, ISP_CHN, ISP_PORT);
        (void)p->isp.fnStopChannel(ISP_DEV, ISP_CHN);
        (void)p->isp.fnDestroyChannel(ISP_DEV, ISP_CHN);
        (void)p->isp.fnDestroyDevice(ISP_DEV);
    }
    if (p->stage >= STAGE_VIF) {
        (void)p->vif.fnDisablePort(VIF_DEV, VIF_PORT);
        (void)p->vif.fnDisableDevice(VIF_DEV);
        (void)p->vif.fnDestroyGroup(VIF_GRP);
    }
    if (p->stage >= STAGE_SENSOR) (void)p->snr.fnDisable(SNR_PAD);
    (void)p->sys.fnExit(0);
    m6_vif_unload(&p->vif);
    m6_venc_unload(&p->venc);
    m6_snr_unload(&p->snr);
    m6_scl_unload(&p->scl);
    m6_isp_unload(&p->isp);
    m6_sys_unload(&p->sys);
    if (p->libpthread != NULL) dlclose(p->libpthread);
    if (p->libm != NULL) dlclose(p->libm);
    memset(p, 0, sizeof(*p));
}

int ca_a8_venc_fd(unsigned channel)
{
    struct a8_pipeline *p = &pipe_state;

    if (p->stage < STAGE_VENC || channel >= CA_A8_VENC_COUNT) return -1;
    return p->venc_fd[channel];
}

static int read_stream(struct a8_pipeline *p, unsigned device,
                       unsigned channel, uint8_t **data, size_t *length,
                       uint64_t *pts_us)
{
    m6_venc_stat status;
    m6_venc_strm stream;
    m6_venc_pack packs[16];
    size_t total = 0;
    uint8_t *frame;
    int result;

    memset(&status, 0, sizeof(status));
    result = p->venc.fnQuery(device, channel, &status);
    if (result != 0) {
        ca_log("VENC %u/%u query failed: 0x%x", device, channel,
               (unsigned)result);
        errno = EIO;
        return -1;
    }
    if (status.curPacks == 0U) return 0;
    if (status.curPacks > 128U) {
        ca_log("VENC %u/%u returned invalid pack count=%u", device, channel,
               status.curPacks);
        errno = EIO;
        return -1;
    }
    memset(&stream, 0, sizeof(stream));
    stream.packet = status.curPacks > 16U
                        ? (m6_venc_pack *)calloc(status.curPacks, sizeof(*packs))
                        : packs;
    if (stream.packet == NULL) return -1;
    stream.count = status.curPacks;
    result = p->venc.fnGetStream(device, channel, &stream, 40);
    if (result != 0) {
        if (stream.packet != packs) free(stream.packet);
        ca_log("VENC %u/%u get stream failed: 0x%x", device, channel,
               (unsigned)result);
        errno = EIO;
        return -1;
    }
    for (unsigned i = 0; i < stream.count; i++) {
        const m6_venc_pack *pack = &stream.packet[i];
        if (pack->offset <= pack->length) total += pack->length - pack->offset;
    }
    frame = (uint8_t*)(total != 0U ? malloc(total) : NULL);
    if (frame != NULL) {
        size_t offset = 0;
        for (unsigned i = 0; i < stream.count; i++) {
            const m6_venc_pack *pack = &stream.packet[i];
            if (pack->offset > pack->length) continue;
            memcpy(frame + offset, pack->data + pack->offset,
                   pack->length - pack->offset);
            offset += pack->length - pack->offset;
            if (pts_us != NULL) *pts_us = pack->timestamp;
        }
    }
    (void)p->venc.fnFreeStream(device, channel, &stream);
    if (stream.packet != packs) free(stream.packet);
    if (frame == NULL) {
        errno = total == 0U ? ENODATA : ENOMEM;
        return -1;
    }
    *data = frame;
    *length = total;
    return 1;
}

int ca_a8_venc_get(unsigned channel, uint8_t **data, size_t *length,
                   uint64_t *pts_us)
{
    struct a8_pipeline *p = &pipe_state;

    if (p->stage < STAGE_VENC || channel >= CA_A8_VENC_COUNT ||
        data == NULL || length == NULL) {
        errno = EINVAL;
        return -1;
    }
    return read_stream(p, M6_VENC_DEV_H26X_0, channel, data, length, pts_us);
}

int ca_a8_venc_request_idr(unsigned channel)
{
    struct a8_pipeline *p = &pipe_state;

    if (p->stage < STAGE_VENC || channel >= CA_A8_VENC_COUNT) {
        errno = EINVAL;
        return -1;
    }
    MI_CHECK(p->venc.fnRequestIdr(M6_VENC_DEV_H26X_0, channel, 1));
    return 0;
}

int ca_a8_capture_jpeg(uint8_t **jpeg, size_t *length)
{
    struct a8_pipeline *p = &pipe_state;
    int count = 1;
    int fd;
    int result = -1;

    if (p->stage < STAGE_VENC || jpeg == NULL || length == NULL) {
        errno = EINVAL;
        return -1;
    }
    MI_CHECK(p->venc.fnStartReceivingEx(M6_VENC_DEV_MJPG_0, JPEG_VENC_CHN,
                                        &count));
    fd = p->venc.fnGetDescriptor(M6_VENC_DEV_MJPG_0, JPEG_VENC_CHN);
    if (fd < 0) {
        (void)p->venc.fnStopReceiving(M6_VENC_DEV_MJPG_0, JPEG_VENC_CHN);
        errno = EIO;
        return -1;
    }
    for (unsigned attempt = 0; attempt < 3U; attempt++) {
        fd_set readable;
        struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        if (select(fd + 1, &readable, NULL, NULL, &timeout) <= 0) continue;
        result = read_stream(p, M6_VENC_DEV_MJPG_0, JPEG_VENC_CHN, jpeg,
                             length, NULL);
        if (result != 0) break;
    }
    (void)p->venc.fnFreeDescriptor(M6_VENC_DEV_MJPG_0, JPEG_VENC_CHN);
    (void)p->venc.fnStopReceiving(M6_VENC_DEV_MJPG_0, JPEG_VENC_CHN);
    if (result == 1) return 0;
    if (result == 0) errno = ETIMEDOUT;
    return -1;
}

int ca_a8_load_isp_bin(const char *path)
{
    struct a8_pipeline *p = &pipe_state;
    int result;

    if (p->stage < STAGE_VENC || path == NULL) {
        errno = EINVAL;
        return -1;
    }
    result = p->isp_load_bin(ISP_DEV, ISP_CHN, (char *)path, ISP_BIN_KEY);
    if (result != 0) {
        ca_log("ISP tuning %s failed: 0x%x", path, (unsigned)result);
        errno = EIO;
        return -1;
    }
    return 0;
}

int ca_a8_set_zoom(float ratio)
{
    struct a8_pipeline *p = &pipe_state;
    m6_common_rect crop;
    unsigned width, height;

    if (p->stage < STAGE_VENC || !(ratio >= 1.0f) || ratio > CA_A8_MAX_ZOOM) {
        errno = EINVAL;
        return -1;
    }
    memset(&crop, 0, sizeof(crop));
    if (ratio > 1.0f) {
        /* digital zoom: crop the ISP output on every scaler port */
        width = (unsigned)((float)p->plane.capt.width / ratio) & ~3U;
        height = (unsigned)((float)p->plane.capt.height / ratio) & ~3U;
        crop.x = (unsigned short)(((p->plane.capt.width - width) / 2U) & ~1U);
        crop.y = (unsigned short)(((p->plane.capt.height - height) / 2U) & ~1U);
        crop.width = (unsigned short)width;
        crop.height = (unsigned short)height;
    }
    p->zoom_crop = crop;
    for (unsigned port = 0; port <= CA_A8_VENC_COUNT; port++) {
        if (p->port_enabled[port] && scl_port_config(p, port) < 0) return -1;
    }
    return 0;
}

int ca_a8_set_inverted(bool inverted)
{
    struct a8_pipeline *p = &pipe_state;

    if (p->stage < STAGE_VENC) {
        errno = EINVAL;
        return -1;
    }
    p->inverted = inverted;
    for (unsigned port = 0; port <= CA_A8_VENC_COUNT; port++) {
        if (p->port_enabled[port] && scl_port_config(p, port) < 0) return -1;
    }
    return 0;
}
