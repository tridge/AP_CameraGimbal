#ifndef CAMERA_APP_MT11_PIPELINE_H
#define CAMERA_APP_MT11_PIPELINE_H

#include "sample_comm.h"
#include "ss_mpi_venc.h"
#include "ot_scene.h"
#include "camera_app/config.h"
#include "apcam/target.h"

#define CA_MT11_SENSOR_WIDTH APCAM_LENS1_WIDTH
#define CA_MT11_SENSOR_HEIGHT APCAM_LENS1_HEIGHT
#define CA_MT11_ENCODE_WIDTH 1920U
#define CA_MT11_ENCODE_HEIGHT 1080U
#define CA_MT11_ZOOM_PIPE 0
#define CA_MT11_WIDE_PIPE 1
#define CA_MT11_ZOOM_GROUP 0
#define CA_MT11_WIDE_GROUP 1
#define CA_MT11_THERMAL_GROUP 2
#define CA_MT11_THERMAL_VENC 1
#define CA_MT11_THERMAL_ENCODE_WIDTH APCAM_THERMAL_STREAM_WIDTH
#define CA_MT11_THERMAL_ENCODE_HEIGHT APCAM_THERMAL_STREAM_HEIGHT
#define CA_MT11_THERMAL_FRAME_RATE APCAM_THERMAL_FRAME_RATE

#define CA_MT11_RTSP_MAIN_CHN 0
#define CA_MT11_RTSP_SUB_CHN 1
#define CA_MT11_RECORD_CHN 2

struct ca_mt11_output_sizes {
    td_u32 width[3];
    td_u32 height[3];
};

// experiment: CA_MT11_SINGLE_SENSOR=1 runs only the wide RGB sensor
bool ca_mt11_single_sensor(void);
#define CA_MT11_FIRST_PIPE (ca_mt11_single_sensor() ? CA_MT11_WIDE_PIPE : CA_MT11_ZOOM_PIPE)
td_s32 ca_mt11_system_init(void);
void ca_mt11_configure_vi(sample_vi_cfg *vi_cfg, td_bool wide);
td_s32 ca_mt11_vpss_start(ot_vpss_grp group,
                          const struct ca_mt11_output_sizes *sizes);
void ca_mt11_vpss_stop(ot_vpss_grp group);
td_s32 ca_mt11_scene_start(const char *directory);
void ca_mt11_scene_stop(void);
td_s32 ca_mt11_set_digital_zoom(ot_vpss_grp group, float ratio);
void ca_mt11_video_attributes(ot_venc_chn_attr *attr,
                              enum ca_video_codec codec, td_u32 width,
                              td_u32 height, td_u32 frame_rate,
                              td_u32 bit_rate_kbps);
td_s32 ca_mt11_thermal_vpss_start(
    const struct ca_mt11_output_sizes *sizes);
void ca_mt11_thermal_vpss_stop(void);
td_s32 ca_mt11_thermal_send_yuyv(const td_u8 *yuyv);
td_s32 ca_mt11_set_inverted(td_bool inverted, ot_vpss_grp record_group);

#endif
