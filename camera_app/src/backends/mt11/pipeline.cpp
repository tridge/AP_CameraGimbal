#define _POSIX_C_SOURCE 200809L
#include "pipeline.h"
#include "thermal.h"

#include "camera_app/log.h"

#include "ot_scene_setparam.h"
#include "scene_loadparam.h"
#include "ss_mpi_sys.h"
#include "ss_mpi_vb.h"
#include "ss_mpi_vpss.h"

#include <stdlib.h>
#include <math.h>
#include "apcam/atomic.h"
#include <string.h>
#include <time.h>

#define ZOOM_MIPI_DEV 2
#define ZOOM_VI_DEV 2
#define ZOOM_I2C_BUS 3
#define ZOOM_CLOCK_RESET 1
#define WIDE_MIPI_DEV 0
#define WIDE_VI_DEV 0
#define WIDE_I2C_BUS 2
#define WIDE_CLOCK_RESET 0

static ot_scene_param scene_param;
static ot_scene_video_mode scene_video_mode;
static td_bool scene_started;
static atomic_uint thermal_time_ref;
static atomic_bool output_inverted;

static td_s32 add_vb_pool(ot_vb_cfg *vb_cfg, td_u32 pool, td_u32 width,
                          td_u32 height, ot_pixel_format pixel_format,
                          ot_data_bit_width bit_width,
                          ot_compress_mode compress_mode, td_u32 count)
{
    ot_pic_buf_attr attr = {};
    ot_vb_calc_cfg calc = {};

    attr.width = width;
    attr.height = height;
    attr.align = OT_DEFAULT_ALIGN;
    attr.pixel_format = pixel_format;
    attr.bit_width = bit_width;
    attr.compress_mode = compress_mode;
    ot_common_get_pic_buf_cfg(&attr, &calc);
    if (calc.vb_size == 0U) return TD_FAILURE;
    vb_cfg->common_pool[pool].blk_size = calc.vb_size;
    vb_cfg->common_pool[pool].blk_cnt = count;
    vb_cfg->common_pool[pool].remap_mode = OT_VB_REMAP_MODE_NOCACHE;
    ca_log("VB pool %u: %ux%u format=%d compress=%d size=%llu count=%u",
           pool, width, height, pixel_format, compress_mode,
           (unsigned long long)calc.vb_size, count);
    return TD_SUCCESS;
}

td_s32 ca_mt11_system_init(void)
{
    ot_vb_cfg vb_cfg = {};
    td_s32 result;

    vb_cfg.max_pool_cnt = 5;
    result = add_vb_pool(&vb_cfg, 0, CA_MT11_SENSOR_WIDTH,
                         CA_MT11_SENSOR_HEIGHT,
                         OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420,
                         OT_DATA_BIT_WIDTH_8, OT_COMPRESS_MODE_NONE, 12);
    if (result != TD_SUCCESS) return result;
    /* B090 uses the 8-bit container selector for this RAW12 line buffer. */
    result = add_vb_pool(&vb_cfg, 1, CA_MT11_SENSOR_WIDTH,
                         CA_MT11_SENSOR_HEIGHT,
                         OT_PIXEL_FORMAT_RGB_BAYER_12BPP,
                         OT_DATA_BIT_WIDTH_8, OT_COMPRESS_MODE_LINE, 12);
    if (result != TD_SUCCESS) return result;
    result = add_vb_pool(&vb_cfg, 2, CA_MT11_SENSOR_WIDTH,
                         CA_MT11_SENSOR_HEIGHT,
                         OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420,
                         OT_DATA_BIT_WIDTH_8, OT_COMPRESS_MODE_SEG, 12);
    if (result != TD_SUCCESS) return result;
    result = add_vb_pool(&vb_cfg, 3, CA_MT11_THERMAL_WIDTH,
                         CA_MT11_THERMAL_HEIGHT,
                         OT_PIXEL_FORMAT_YUV_SEMIPLANAR_420,
                         OT_DATA_BIT_WIDTH_8, OT_COMPRESS_MODE_NONE, 12);
    if (result != TD_SUCCESS) return result;
    /* Three simultaneous scaled thermal outputs need their own right-sized
     * uncompressed blocks.  Falling back to the 4K pool exhausts its twelve
     * blocks and silently stalls every output in the group. */
    result = add_vb_pool(&vb_cfg, 4, CA_MT11_THERMAL_ENCODE_WIDTH,
                         CA_MT11_THERMAL_ENCODE_HEIGHT,
                         OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420,
                         OT_DATA_BIT_WIDTH_8, OT_COMPRESS_MODE_NONE, 12);
    if (result != TD_SUCCESS) return result;
    result = sample_comm_sys_init_with_vb_supplement(
        &vb_cfg, OT_VB_SUPPLEMENT_BNR_MOT_MASK);
    if (result != TD_SUCCESS) return result;
    return sample_comm_vi_set_vi_vpss_mode(OT_VI_OFFLINE_VPSS_OFFLINE,
                                            OT_VI_VIDEO_MODE_NORM);
}

void ca_mt11_configure_vi(sample_vi_cfg *vi_cfg, td_bool wide)
{
    const ot_vi_dev dev = wide ? WIDE_VI_DEV : ZOOM_VI_DEV;
    const ot_vi_pipe pipe = wide ? CA_MT11_WIDE_PIPE : CA_MT11_ZOOM_PIPE;
    const td_s8 bus = wide ? WIDE_I2C_BUS : ZOOM_I2C_BUS;
    const td_u32 clock_reset = wide ? WIDE_CLOCK_RESET : ZOOM_CLOCK_RESET;

    sample_comm_vi_get_default_vi_cfg(SONY_IMX678_MIPI_8M_30FPS_12BIT,
                                      vi_cfg);
    vi_cfg->sns_info.bus_id = bus;
    vi_cfg->sns_info.sns_clk_src = clock_reset;
    vi_cfg->sns_info.sns_rst_src = clock_reset;
    sample_comm_vi_get_mipi_info_by_dev_id(SONY_IMX678_MIPI_8M_30FPS_12BIT,
                                            dev,
                                            &vi_cfg->mipi_info);
    vi_cfg->mipi_info.divide_mode = LANE_DIVIDE_MODE_1;
    vi_cfg->mipi_info.combo_dev_attr.devno = dev;
    vi_cfg->mipi_info.combo_dev_attr.img_rect.x = 0;
    vi_cfg->mipi_info.combo_dev_attr.img_rect.y = 0;
    vi_cfg->mipi_info.combo_dev_attr.img_rect.width = CA_MT11_SENSOR_WIDTH;
    vi_cfg->mipi_info.combo_dev_attr.img_rect.height = CA_MT11_SENSOR_HEIGHT;
    vi_cfg->mipi_info.ext_data_type_attr.devno = dev;

    vi_cfg->dev_info.vi_dev = dev;
    vi_cfg->dev_info.dev_attr.in_size.width = CA_MT11_SENSOR_WIDTH;
    vi_cfg->dev_info.dev_attr.in_size.height = CA_MT11_SENSOR_HEIGHT;
    vi_cfg->bind_pipe.pipe_num = 1;
    vi_cfg->bind_pipe.pipe_id[0] = pipe;
    vi_cfg->grp_info.grp_num = 1;
    vi_cfg->grp_info.fusion_grp[0] = pipe;
    vi_cfg->grp_info.fusion_grp_attr[0].pipe_id[0] = pipe;
    vi_cfg->grp_info.fusion_grp_attr[0].cache_line = CA_MT11_SENSOR_HEIGHT;

    vi_cfg->pipe_info[0].isp_info.isp_pub_attr.wnd_rect.x = 0;
    vi_cfg->pipe_info[0].isp_info.isp_pub_attr.wnd_rect.y = 0;
    vi_cfg->pipe_info[0].isp_info.isp_pub_attr.wnd_rect.width =
        CA_MT11_SENSOR_WIDTH;
    vi_cfg->pipe_info[0].isp_info.isp_pub_attr.wnd_rect.height =
        CA_MT11_SENSOR_HEIGHT;
    vi_cfg->pipe_info[0].isp_info.isp_pub_attr.sns_size.width =
        CA_MT11_SENSOR_WIDTH;
    vi_cfg->pipe_info[0].isp_info.isp_pub_attr.sns_size.height =
        CA_MT11_SENSOR_HEIGHT;
    vi_cfg->pipe_info[0].isp_info.isp_pub_attr.frame_rate = 30.0f;
    vi_cfg->pipe_info[0].chn_info[0].chn_attr.depth = 2;
}

td_s32 ca_mt11_vpss_start(ot_vpss_grp group,
                          const struct ca_mt11_output_sizes *sizes)
{
    ot_vpss_grp_attr group_attr;
    ot_vpss_chn_attr channels[OT_VPSS_MAX_PHYS_CHN_NUM];
    td_bool enabled[OT_VPSS_MAX_PHYS_CHN_NUM] = {
        TD_TRUE, TD_TRUE, TD_TRUE, TD_FALSE
    };

    memset(channels, 0, sizeof(channels));
    sample_comm_vpss_get_default_grp_attr(&group_attr);
    group_attr.max_width = CA_MT11_SENSOR_WIDTH;
    group_attr.max_height = CA_MT11_SENSOR_HEIGHT;
    for (unsigned channel = 0; channel < 3U; channel++) {
        sample_comm_vpss_get_default_chn_attr(&channels[channel]);
        channels[channel].width = sizes->width[channel];
        channels[channel].height = sizes->height[channel];
        channels[channel].depth = channel == 0U ? 2U : channel == 1U ? 1U : 0U;
        if (channel != 0U) {
            /* SS928 auxiliary VPSS outputs do not accept segment
             * compression; only physical channel 0 does. */
            channels[channel].compress_mode = OT_COMPRESS_MODE_NONE;
        }
    }
    return sample_common_vpss_start(group, enabled, &group_attr, channels,
                                    OT_VPSS_MAX_PHYS_CHN_NUM);
}

void ca_mt11_vpss_stop(ot_vpss_grp group)
{
    td_bool enabled[OT_VPSS_MAX_PHYS_CHN_NUM] = {
        TD_TRUE, TD_TRUE, TD_TRUE, TD_FALSE
    };
    (void)sample_common_vpss_stop(group, enabled, OT_VPSS_MAX_PHYS_CHN_NUM);
}

td_s32 ca_mt11_scene_start(const char *directory)
{
    td_s32 result;

    if (directory == NULL || *directory == '\0') return TD_FAILURE;
    set_dir_name(directory);
    result = ot_scene_create_param(directory, &scene_param, &scene_video_mode);
    if (result != TD_SUCCESS) return result;
    result = ot_scene_init(&scene_param);
    if (result != TD_SUCCESS) return result;
    scene_started = TD_TRUE;
    result = ot_scene_set_scene_mode(&scene_video_mode.video_mode[0]);
    if (result != TD_SUCCESS) {
        ca_mt11_scene_stop();
        return result;
    }
    return TD_SUCCESS;
}

void ca_mt11_scene_stop(void)
{
    if (scene_started) {
        (void)ot_scene_deinit();
        scene_started = TD_FALSE;
    }
}

td_s32 ca_mt11_set_digital_zoom(ot_vpss_grp group, float ratio)
{
    ot_vpss_crop_info crop = {(td_bool)(0)};

    if (!isfinite(ratio) || ratio < 1.0f || ratio > 10.0f) return TD_FAILURE;
    if (ratio <= 1.001f) {
        crop.enable = TD_FALSE;
        /* SS928 validates the rectangle even when disabling group crop. */
        crop.crop_mode = OT_COORD_ABS;
        crop.crop_rect.width = CA_MT11_SENSOR_WIDTH;
        crop.crop_rect.height = CA_MT11_SENSOR_HEIGHT;
    } else {
        td_u32 width = ((td_u32)((float)CA_MT11_SENSOR_WIDTH / ratio)) & ~3U;
        td_u32 height = ((td_u32)((float)CA_MT11_SENSOR_HEIGHT / ratio)) & ~3U;
        crop.enable = TD_TRUE;
        crop.crop_mode = OT_COORD_ABS;
        crop.crop_rect.width = width;
        crop.crop_rect.height = height;
        crop.crop_rect.x = (td_s32)((CA_MT11_SENSOR_WIDTH - width) / 2U) & ~1;
        crop.crop_rect.y = (td_s32)((CA_MT11_SENSOR_HEIGHT - height) / 2U) & ~1;
    }
    return ss_mpi_vpss_set_grp_crop(group, &crop);
}

void ca_mt11_video_attributes(ot_venc_chn_attr *attr,
                              enum ca_video_codec codec, td_u32 width,
                              td_u32 height, td_u32 frame_rate,
                              td_u32 bit_rate_kbps)
{
    memset(attr, 0, sizeof(*attr));
    attr->venc_attr.type = codec == CA_VIDEO_H265 ? OT_PT_H265 : OT_PT_H264;
    attr->venc_attr.max_pic_width = width;
    attr->venc_attr.max_pic_height = height;
    attr->venc_attr.pic_width = width;
    attr->venc_attr.pic_height = height;
    attr->venc_attr.buf_size = OT_ALIGN_UP(
        width * height * 3U / 4U, 64U);
    attr->venc_attr.profile = 0;
    attr->venc_attr.is_by_frame = TD_TRUE;
    if (codec == CA_VIDEO_H265) {
        attr->venc_attr.h265_attr.rcn_ref_share_buf_en = TD_FALSE;
        attr->venc_attr.h265_attr.frame_buf_ratio = 70;
        attr->rc_attr.rc_mode = OT_VENC_RC_MODE_H265_CBR;
        attr->rc_attr.h265_cbr.gop = frame_rate;
        attr->rc_attr.h265_cbr.stats_time = 1;
        attr->rc_attr.h265_cbr.src_frame_rate = frame_rate;
        attr->rc_attr.h265_cbr.dst_frame_rate = frame_rate;
        attr->rc_attr.h265_cbr.bit_rate = bit_rate_kbps;
    } else {
        attr->venc_attr.h264_attr.rcn_ref_share_buf_en = TD_FALSE;
        attr->venc_attr.h264_attr.frame_buf_ratio = 70;
        attr->rc_attr.rc_mode = OT_VENC_RC_MODE_H264_CBR;
        attr->rc_attr.h264_cbr.gop = frame_rate;
        attr->rc_attr.h264_cbr.stats_time = 1;
        attr->rc_attr.h264_cbr.src_frame_rate = frame_rate;
        attr->rc_attr.h264_cbr.dst_frame_rate = frame_rate;
        attr->rc_attr.h264_cbr.bit_rate = bit_rate_kbps;
    }
    attr->gop_attr.gop_mode = OT_VENC_GOP_MODE_NORMAL_P;
    attr->gop_attr.normal_p.ip_qp_delta = 2;
}

td_s32 ca_mt11_thermal_vpss_start(
    const struct ca_mt11_output_sizes *sizes)
{
    ot_vpss_grp_attr group_attr;
    ot_vpss_chn_attr channels[OT_VPSS_MAX_PHYS_CHN_NUM];
    td_bool enabled[OT_VPSS_MAX_PHYS_CHN_NUM] = {
        TD_FALSE, TD_TRUE, TD_FALSE, TD_FALSE
    };
    td_s32 result;

    (void)sizes;
    memset(channels, 0, sizeof(channels));
    sample_comm_vpss_get_default_grp_attr(&group_attr);
    /* Unlike the sensor-backed groups, the thermal group's input is only
     * 640x512.  Giving this group the RGB sensor's 4K maximum causes the
     * SS928 VPSS to accept frames but never produce output. */
    group_attr.max_width = CA_MT11_THERMAL_ENCODE_WIDTH;
    group_attr.max_height = CA_MT11_THERMAL_ENCODE_HEIGHT;
    for (unsigned channel = 0; channel < 3U; channel++) {
        sample_comm_vpss_get_default_chn_attr(&channels[channel]);
        channels[channel].width = CA_MT11_THERMAL_ENCODE_WIDTH;
        channels[channel].height = CA_MT11_THERMAL_ENCODE_HEIGHT;
        channels[channel].depth = 2;
        if (channel != 0U) {
            channels[channel].compress_mode = OT_COMPRESS_MODE_NONE;
        }
    }
    result = sample_common_vpss_start(CA_MT11_THERMAL_GROUP, enabled,
                                      &group_attr, channels,
                                      OT_VPSS_MAX_PHYS_CHN_NUM);
    return result;
}

void ca_mt11_thermal_vpss_stop(void)
{
    td_bool enabled[OT_VPSS_MAX_PHYS_CHN_NUM] = {
        TD_FALSE, TD_TRUE, TD_FALSE, TD_FALSE
    };
    (void)sample_common_vpss_stop(CA_MT11_THERMAL_GROUP, enabled,
                                  OT_VPSS_MAX_PHYS_CHN_NUM);
}

td_s32 ca_mt11_thermal_send_yuyv(const td_u8 *yuyv)
{
    ot_pic_buf_attr buffer_attr = {
        .width = CA_MT11_THERMAL_WIDTH,
        .height = CA_MT11_THERMAL_HEIGHT,
        .align = OT_DEFAULT_ALIGN,
        .bit_width = OT_DATA_BIT_WIDTH_8,
        .pixel_format = OT_PIXEL_FORMAT_YUV_SEMIPLANAR_420,
        .compress_mode = OT_COMPRESS_MODE_NONE,
    };
    ot_vb_calc_cfg calc = {};
    ot_video_frame_info frame = {};
    ot_vb_blk block;
    td_phys_addr_t physical;
    td_u8 *mapping;
    struct timespec now;
    td_s32 result;

    if (yuyv == NULL) return TD_FAILURE;
    ot_common_get_pic_buf_cfg(&buffer_attr, &calc);
    block = ss_mpi_vb_get_blk(OT_VB_INVALID_POOL_ID, calc.vb_size, TD_NULL);
    if (block == OT_VB_INVALID_HANDLE) return TD_FAILURE;
    physical = ss_mpi_vb_handle_to_phys_addr(block);
    mapping = physical != 0 ? (td_u8 *) ss_mpi_sys_mmap(physical, calc.vb_size) : NULL;
    if (mapping == NULL) {
        (void)ss_mpi_vb_release_blk(block);
        return TD_FAILURE;
    }
    frame.mod_id = OT_ID_VGS;
    frame.pool_id = ss_mpi_vb_handle_to_pool_id(block);
    frame.video_frame.phys_addr[0] = physical + calc.head_size;
    frame.video_frame.phys_addr[1] = frame.video_frame.phys_addr[0] +
                                      calc.main_y_size;
    frame.video_frame.virt_addr[0] = mapping + calc.head_size;
    frame.video_frame.virt_addr[1] = (uint8_t *)frame.video_frame.virt_addr[0] +
                                      calc.main_y_size;
    frame.video_frame.stride[0] = calc.main_stride;
    frame.video_frame.stride[1] = calc.main_stride;
    frame.video_frame.width = buffer_attr.width;
    frame.video_frame.height = buffer_attr.height;
    frame.video_frame.dynamic_range = OT_DYNAMIC_RANGE_SDR8;
    frame.video_frame.compress_mode = buffer_attr.compress_mode;
    frame.video_frame.video_format = OT_VIDEO_FORMAT_LINEAR;
    frame.video_frame.field = OT_VIDEO_FIELD_FRAME;
    frame.video_frame.pixel_format = buffer_attr.pixel_format;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    frame.video_frame.pts = (td_u64)now.tv_sec * 1000000ULL +
                            (td_u64)now.tv_nsec / 1000ULL;
    /* SS928's 50 Hz media timebase represents each 25 fps source frame twice. */
    frame.video_frame.time_ref = atomic_fetch_add(&thermal_time_ref, 1U) * 2U;
    if (atomic_load(&output_inverted)) {
        ca_mt11_yuyv_to_nv12(
            yuyv, CA_MT11_THERMAL_WIDTH, CA_MT11_THERMAL_HEIGHT,
            (uint8_t *)frame.video_frame.virt_addr[0], calc.main_stride,
            (uint8_t *)frame.video_frame.virt_addr[1], calc.main_stride);
    } else {
        ca_mt11_yuyv_to_nv12_rotated_180(
            yuyv, CA_MT11_THERMAL_WIDTH, CA_MT11_THERMAL_HEIGHT,
            (uint8_t *)frame.video_frame.virt_addr[0], calc.main_stride,
            (uint8_t *)frame.video_frame.virt_addr[1], calc.main_stride);
    }
    result = ss_mpi_vpss_send_frame(CA_MT11_THERMAL_GROUP, &frame, 100);
    (void)ss_mpi_sys_munmap(mapping, calc.vb_size);
    (void)ss_mpi_vb_release_blk(block);
    return result;
}

bool ca_mt11_single_sensor(void)
{
    static const bool single = getenv("CA_MT11_SINGLE_SENSOR") != NULL &&
                               atoi(getenv("CA_MT11_SINGLE_SENSOR")) != 0;
    return single;
}

td_s32 ca_mt11_set_inverted(td_bool inverted, ot_vpss_grp record_group)
{
    for (ot_vpss_grp group = CA_MT11_FIRST_PIPE;
         group <= CA_MT11_WIDE_GROUP; group++) {
        for (ot_vpss_chn channel = 0; channel < 3;
             channel++) {
            ot_vpss_chn_attr attr;
            td_s32 result = ss_mpi_vpss_disable_chn(group, channel);
            if (result != TD_SUCCESS) return result;
            result = ss_mpi_vpss_get_chn_attr(group, channel, &attr);
            if (result == TD_SUCCESS) {
                attr.mirror_en = inverted;
                attr.flip_en = inverted;
                result = ss_mpi_vpss_set_chn_attr(group, channel, &attr);
            }
            // the record channel stays disabled unless recording on this group
            td_s32 enable_result = channel == CA_MT11_RECORD_CHN &&
                                           group != record_group
                                       ? TD_SUCCESS
                                       : ss_mpi_vpss_enable_chn(group, channel);
            if (result != TD_SUCCESS) return result;
            if (enable_result != TD_SUCCESS) return enable_result;
        }
    }
    atomic_store(&output_inverted, inverted != TD_FALSE);
    return TD_SUCCESS;
}
