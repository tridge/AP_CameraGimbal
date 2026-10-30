#ifndef CAMERA_APP_VIDEO_METADATA_H
#define CAMERA_APP_VIDEO_METADATA_H

#include "camera_app/config.h"
#include "camera_app/metadata.h"

#define CA_VIDEO_METADATA_JSON_MAX 1024U
#define CA_VIDEO_METADATA_SEI_MAX 1600U

#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t ca_video_metadata_uuid[16];

/* Process-wide selection is fixed before media workers start. A parameter
 * change takes effect on app restart, preserving active stream/file formats. */
void ca_video_metadata_set_format(enum ca_video_telemetry_format format);
enum ca_video_telemetry_format ca_video_metadata_format(void);
const char *ca_video_recording_extension(void);

/* Bounded UTF-8 JSON, excluding the terminating NUL. pts90k identifies the
 * video sample (or RTP timestamp); UTC is the telemetry snapshot time. */
size_t ca_video_metadata_json(const struct ca_metadata *metadata,
                               const struct timespec *utc, uint64_t pts90k,
                               char *output, size_t capacity);
/* Annex-B user_data_unregistered SEI, including start code and escaping. */
size_t ca_video_metadata_sei(enum ca_video_codec codec, const char *json,
                              size_t length, uint8_t *output, size_t capacity);
/* Locate the next Annex-B NAL, excluding start code/trailing_zero_8bits. */
bool ca_annexb_next(const uint8_t *data, size_t length, size_t *offset,
                    size_t *nal, size_t *nal_length);
bool ca_video_nal_is_vcl(enum ca_video_codec codec, uint8_t header);
/* Insert immediately before the first VCL NAL of a single access unit.
 * Returns 1 and malloc-owned output, 0 for no VCL, or -1 on failure. */
int ca_video_metadata_insert(enum ca_video_codec codec,
                              const uint8_t *data, size_t length, uint64_t pts90k, float hfov_deg,
                              uint8_t **output, size_t *output_length);

#ifdef __cplusplus
}
#endif
#endif
