#ifndef CAMERA_APP_ZR10_PIPELINE_H
#define CAMERA_APP_ZR10_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "camera_app/config.h"
#include "camera_app/image_tracker.h"
bool ca_zr10_tracking_available();
bool ca_zr10_tracking_frame(ca_tracking_frame &frame);
#include "apcam/target.h"

/* SIYI ZR10: SigmaStar Infinity6B0 with a GC4663.
 * One sensor -> VIF -> VPE with one output per video encoder.
 * Transient JPEG capture shares the main video output. */
#define CA_ZR10_SENSOR_WIDTH APCAM_LENS1_WIDTH
#define CA_ZR10_SENSOR_HEIGHT APCAM_LENS1_HEIGHT
#define CA_ZR10_FRAME_RATE APCAM_FRAME_RATE
#define CA_ZR10_MAX_ZOOM APCAM_ZOOM_MAX

#define CA_ZR10_MAIN_VENC 0U
#define CA_ZR10_SUB_VENC 1U
#define CA_ZR10_RECORD_VENC 2U
#define CA_ZR10_VENC_COUNT 3U

struct ca_zr10_stream {
    unsigned width;
    unsigned height;
    enum ca_video_codec codec;
    unsigned bit_rate_kbps;
};

struct ca_zr10_pipeline_config {
    struct ca_zr10_stream streams[CA_ZR10_VENC_COUNT];
    unsigned jpeg_quality;
};

int ca_zr10_pipeline_open(const struct ca_zr10_pipeline_config *config);
void ca_zr10_pipeline_close(void);

/* poll()able descriptor of an encoder channel */
int ca_zr10_venc_fd(unsigned channel);
/* returns 1 with a malloc()ed Annex-B access unit, 0 when nothing is ready */
int ca_zr10_venc_get(unsigned channel, uint8_t **data, size_t *length,
                   uint64_t *pts_us);
int ca_zr10_venc_request_idr(unsigned channel);

/* one JPEG from the main stream; the caller frees *jpeg */
int ca_zr10_capture_jpeg(uint8_t **jpeg, size_t *length);

/* only valid once frames are flowing, see mi-pipeline notes */
int ca_zr10_load_isp_bin(const char *path);
int ca_zr10_set_inverted(bool inverted);
void ca_zr10_sample_focus(void);
int ca_zr10_take_focus(uint8_t payload[27]);

#endif
