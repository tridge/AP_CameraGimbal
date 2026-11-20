#ifndef CAMERA_APP_SUPPORT_VIDEO_H
#define CAMERA_APP_SUPPORT_VIDEO_H
#include "camera_app/config.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct ca_support_video;
int ca_support_video_open(struct ca_support_video **result,
                           const struct ca_support_config *config,
                           unsigned stream, enum ca_video_codec codec);
// Stream 3: an immutable Matroska header followed by independent FFV1 clusters.
int ca_support_video_open_matroska(struct ca_support_video **result,
                                  const struct ca_support_config *config,
                                  const uint8_t *header, size_t length);
void ca_support_video_push(struct ca_support_video *publisher, const uint8_t *data,
                            size_t length, uint32_t timestamp, bool key_frame);
void ca_support_video_close(struct ca_support_video *publisher);
#ifdef __cplusplus
}
#endif
#endif
