#ifndef CAMERA_APP_A8_PIPELINE_H
#define CAMERA_APP_A8_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "camera_app/config.h"
#include "camera_app/image_tracker.h"
bool ca_a8_tracking_available();
bool ca_a8_tracking_frame(ca_tracking_frame &frame);
#include "apcam/target.h"

/* SIYI A8 mini: SigmaStar SSC8836 (Mercury6) with a Sony IMX678.
 * One sensor -> VIF -> ISP -> SCL channel with one output port per encoder. */
#define CA_A8_SENSOR_WIDTH APCAM_LENS1_WIDTH
#define CA_A8_SENSOR_HEIGHT APCAM_LENS1_HEIGHT
#define CA_A8_FRAME_RATE APCAM_FRAME_RATE
#define CA_A8_MAX_ZOOM APCAM_ZOOM_MAX

/* Frame-buffered ISP output feeds SCL device 1. Digital zoom can upscale
 * beyond the sensor's pixel rate, which overruns the realtime SCL path. */
#define CA_A8_SCL_DEV 1

#define CA_A8_MAIN_VENC 0U
#define CA_A8_SUB_VENC 1U
#define CA_A8_RECORD_VENC 2U
#define CA_A8_VENC_COUNT 3U

struct ca_a8_stream {
    unsigned width;
    unsigned height;
    enum ca_video_codec codec;
    unsigned bit_rate_kbps;
};

struct ca_a8_pipeline_config {
    struct ca_a8_stream streams[CA_A8_VENC_COUNT];
    unsigned jpeg_quality;
};

int ca_a8_pipeline_open(const struct ca_a8_pipeline_config *config);
void ca_a8_pipeline_close(void);

/* poll()able descriptor of an encoder channel */
int ca_a8_venc_fd(unsigned channel);
/* returns 1 with a malloc()ed Annex-B access unit, 0 when nothing is ready */
int ca_a8_venc_get(unsigned channel, uint8_t **data, size_t *length,
                   uint64_t *pts_us);
int ca_a8_venc_request_idr(unsigned channel);

/* one JPEG from the main stream; the caller frees *jpeg */
int ca_a8_capture_jpeg(uint8_t **jpeg, size_t *length);

/* only valid once frames are flowing, see mi-pipeline notes */
int ca_a8_load_isp_bin(const char *path);
int ca_a8_set_zoom(float ratio);
int ca_a8_set_inverted(bool inverted);

#endif
