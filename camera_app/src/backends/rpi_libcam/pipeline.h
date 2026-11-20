#ifndef CAMERA_APP_RPI_PIPELINE_H
#define CAMERA_APP_RPI_PIPELINE_H

#include "camera_app/config.h"
#include "camera_app/exposure.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* libcamera capture of the CSI camera into the Pi 4 / CM4 V4L2 H.264 encoder.
 * Callbacks run on the pipeline's encoder thread; frame data is only valid
 * during the callback. Image settings apply to the next queued request, so
 * changes take effect without restarting the stream. */
struct ca_rpi_pipeline;

typedef void (*ca_rpi_frame_fn)(void *opaque, const uint8_t *annex_b, size_t length,
                                uint64_t pts_us, bool key_frame);
typedef void (*ca_rpi_exposure_fn)(void *opaque, const struct ca_exposure *sample);

struct ca_rpi_pipeline_config {
    unsigned width, height, frame_rate, bit_rate_kbps;
    bool inverted;
    struct ca_config image;
    ca_rpi_frame_fn frame;
    ca_rpi_exposure_fn exposure;
    void *opaque;
};

int ca_rpi_pipeline_open(struct ca_rpi_pipeline **pipeline,
                         const struct ca_rpi_pipeline_config *config);
/* Change the H.264 bitrate while streaming. */
int ca_rpi_pipeline_set_bitrate(struct ca_rpi_pipeline *pipeline, unsigned bit_rate_kbps);
int ca_rpi_pipeline_set_image(struct ca_rpi_pipeline *pipeline,
                              const struct ca_config *settings);
void ca_rpi_pipeline_close(struct ca_rpi_pipeline *pipeline);

/* A copy of one camera frame: planar YUV420 (I420), U and V planes at half
 * the luma stride. Video frames use limited-range Rec.709; frames from a
 * still capture are already full-range BT.601 (JPEG colour). */
struct ca_rpi_still_frame {
    uint8_t *data; /* malloc()ed; Y, then U, then V */
    unsigned width, height, stride;
    uint64_t timestamp_us;
    bool jpeg_colour;
};

/* Copy the next camera frame without interrupting video. The caller frees
 * frame->data. Returns -1 with errno ETIMEDOUT if no frame arrives. */
int ca_rpi_pipeline_grab_still(struct ca_rpi_pipeline *pipeline,
                               struct ca_rpi_still_frame *frame, unsigned timeout_ms);

/* Capture one frame at a larger size by switching the sensor mode: video
 * pauses, the camera runs a still configuration with exposure and white
 * balance locked to the last video frame, then video resumes. Encoded video
 * and recording continue across the gap. Fails with errno EIO if video could
 * not be restarted. The caller frees frame->data. */
int ca_rpi_pipeline_capture_still(struct ca_rpi_pipeline *pipeline, unsigned width, unsigned height,
                                  struct ca_rpi_still_frame *frame, unsigned timeout_ms);

/* libcamera control values for the IMG_* settings, independent of libcamera
 * so they can be tested on the host. Zero gain/exposure means automatic. */
struct ca_rpi_image_controls {
    float brightness;      /* -1..1, 0 is neutral */
    float contrast;        /* 0..2, 1 is neutral */
    float saturation;      /* 0..2, 1 is neutral */
    float exposure_value;  /* stops */
    float analogue_gain;   /* 0 = automatic */
    int32_t exposure_us;   /* 0 = automatic */
    enum ca_exposure_mode mode;
};

static inline void ca_rpi_image_controls(const struct ca_config *settings,
                                         struct ca_rpi_image_controls *out)
{
    static const int32_t shutter_us[] = {
        0, 33333, 20000, 10000, 4000, 2000, 1333, 1000, 500,
    };
    out->brightness = ((float)settings->brightness - 50.0f) / 50.0f;
    out->contrast = (float)settings->contrast / 50.0f;
    out->saturation = (float)settings->saturation / 50.0f;
    out->exposure_value = (float)settings->exposure_compensation / 10.0f;
    out->analogue_gain = settings->iso == CA_ISO_AUTO ? 0.0f
        : (float)(1U << ((unsigned)settings->iso - 1U));
    out->exposure_us = (unsigned)settings->shutter < sizeof(shutter_us) / sizeof(shutter_us[0])
        ? shutter_us[settings->shutter] : 0;
    const bool fixed_gain = out->analogue_gain > 0.0f, fixed_shutter = out->exposure_us > 0;
    out->mode = fixed_gain && fixed_shutter ? CA_AE_MANUAL
        : fixed_gain ? CA_AE_GAIN_PRIORITY
        : fixed_shutter ? CA_AE_SHUTTER_PRIORITY : CA_AE_AUTO;
}

#endif
